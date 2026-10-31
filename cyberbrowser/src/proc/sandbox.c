/* sandbox.c - the coordinator's side of the sandboxes (sandbox.h). */
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#include "sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <userenv.h>
#include <sddl.h>
#include <aclapi.h>
#include <bcrypt.h>

#include "coord_core.h"

static bool g_on;
static PSECURITY_DESCRIPTOR g_object_sd, g_label_sd;
static PACL g_label;
static SECURITY_ATTRIBUTES g_object_sa;
static PSID g_user;                     /* this process's user */
static PSID g_tab_sids[COORD_MAX_TABS];
static wchar_t g_tab_names[COORD_MAX_TABS][64];  /* their profiles, deleted when the tab ends */
static wchar_t g_tab_slot_names[COORD_MAX_TABS][64];   /* each slot's name, for this coordinator's life */
static PSID g_store_sid, g_ui_sid, g_net_sid;
static union { SID s; char b[SECURITY_MAX_SID_SIZE]; } g_net_caps[2];
static SID_AND_ATTRIBUTES g_net_cap_list[8];
static DWORD g_net_ncaps;
static union { ACL acl; char b[512]; } g_tab_dacl[COORD_MAX_TABS];
static PSID g_webgpu_sids[COORD_MAX_WEBGPU];
static wchar_t g_webgpu_names[COORD_MAX_WEBGPU][64];
static wchar_t g_webgpu_slot_names[COORD_MAX_WEBGPU][64];
static union { ACL acl; char b[512]; } g_webgpu_dacl[COORD_MAX_WEBGPU];

#define TAB_MEMORY_LIMIT   (4ull << 30)
#define NET_MEMORY_LIMIT   (2ull << 30)
#define STORE_MEMORY_LIMIT (512ull << 20)
#define LOOP_MEMORY_LIMIT  (256ull << 20)
#define GPU_MEMORY_LIMIT   (8ull << 30)
#define WEBGPU_MEMORY_LIMIT (4ull << 30)

/* The AppContainer SID of profile `name`, registering the profile the first
 * time: CreateProcess refuses an AppContainer SID with no profile. */
static PSID profile_sid(const wchar_t *name) {
    PSID sid = NULL;
    HRESULT hr = E_FAIL;
    /* (again a few times: browsers starting together, test runs side by
     * side, registering one profile at once may see E_UNEXPECTED) */
    for (int tries = 0; tries < 10; tries++) {
        hr = CreateAppContainerProfile(name, name, L"cyberbrowser sandbox", NULL, 0, &sid);
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
            hr = DeriveAppContainerSidFromAppContainerName(name, &sid);
        if (SUCCEEDED(hr)) break;
        Sleep(20 + 20 * tries);
    }
    if (FAILED(hr)) {
        fprintf(stderr, "[sandbox] profile %ls: %lx\n", name, (unsigned long)hr);
        return NULL;
    }
    return sid;
}

/* A slot's profile name, "cyberbrowser.<kind>.<coordinator pid>.<64 random
 * bits>", made the first time the slot is used and kept for this
 * coordinator's life.  Random, not a count: a process can read its own, and
 * a count would tell it how many (of any site) came before it.  Kept, not
 * new for each process: Windows keeps something for every AppContainer SID
 * a session has seen, and a new one per tab (a test run's thousands) ran it
 * out (CreateProcess: ERROR_NO_SYSTEM_RESOURCES) until the user signed out. */
static bool slot_name(wchar_t *name, const wchar_t *kind) {
    if (name[0]) return true;
    unsigned long long r = 0;
    if (BCryptGenRandom(NULL, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    _snwprintf(name, 64, L"cyberbrowser.%ls.%lu.%016llx", kind, GetCurrentProcessId(), r);
    return true;
}

/* A profile for the tab process about to start in `slot`, under the
 * slot's name: the profile of the slot's last tab is deleted first (its
 * folder and registry storage with it, sandbox_tab_ended) and made again,
 * so nothing the last one left there reaches this one; its SID is the
 * slot's (a SID follows from the name). */
static PSID new_tab_sid(int slot) {
    if (slot < 0 || slot >= COORD_MAX_TABS) return NULL;
    sandbox_tab_ended(slot);
    if (!slot_name(g_tab_slot_names[slot], L"tab")) return NULL;
    memcpy(g_tab_names[slot], g_tab_slot_names[slot], sizeof(g_tab_names[slot]));
    g_tab_sids[slot] = profile_sid(g_tab_names[slot]);
    if (!g_tab_sids[slot]) g_tab_names[slot][0] = 0;
    return g_tab_sids[slot];
}

/* The same for the web-GPU process about to start in `slot`. */
static PSID new_webgpu_sid(int slot) {
    if (slot < 0 || slot >= COORD_MAX_WEBGPU) return NULL;
    sandbox_webgpu_ended(slot);
    if (!slot_name(g_webgpu_slot_names[slot], L"webgpu")) return NULL;
    memcpy(g_webgpu_names[slot], g_webgpu_slot_names[slot], sizeof(g_webgpu_names[slot]));
    g_webgpu_sids[slot] = profile_sid(g_webgpu_names[slot]);
    if (!g_webgpu_sids[slot]) g_webgpu_names[slot][0] = 0;
    return g_webgpu_sids[slot];
}

void sandbox_webgpu_ended(int slot) {
    if (slot < 0 || slot >= COORD_MAX_WEBGPU || !g_webgpu_names[slot][0]) return;
    DeleteAppContainerProfile(g_webgpu_names[slot]);
    FreeSid(g_webgpu_sids[slot]);
    g_webgpu_sids[slot] = NULL;
    g_webgpu_names[slot][0] = 0;
}

/* What a process of AppContainer `sid` creates: this user, SYSTEM and
 * itself, and nothing the loopback relay's restricting SIDs match. */
static ACL *own_dacl(ACL *a, DWORD size, PSID sid) {
    union { SID s; char b[SECURITY_MAX_SID_SIZE]; } system;
    DWORD n = sizeof(system);
    CreateWellKnownSid(WinLocalSystemSid, NULL, &system, &n);
    InitializeAcl(a, size, ACL_REVISION);
    AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, g_user);
    AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, &system);
    AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, sid);
    return a;
}

void sandbox_tab_ended(int slot) {
    if (slot < 0 || slot >= COORD_MAX_TABS || !g_tab_names[slot][0]) return;
    DeleteAppContainerProfile(g_tab_names[slot]);
    FreeSid(g_tab_sids[slot]);
    g_tab_sids[slot] = NULL;
    g_tab_names[slot][0] = 0;
}

void sandbox_end(void) {
    for (int i = 0; i < COORD_MAX_TABS; i++) sandbox_tab_ended(i);
    for (int i = 0; i < COORD_MAX_WEBGPU; i++) sandbox_webgpu_ended(i);
}

/* The monikers of the registered profiles that start with `prefix`. */
static int list_profiles(const wchar_t *prefix, wchar_t (*out)[64], int cap) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                      L"AppContainer\\Mappings",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    int n = 0;
    size_t plen = wcslen(prefix);
    wchar_t sub[256], moniker[256];
    for (DWORD i = 0; n < cap; i++) {
        DWORD len = 256;
        if (RegEnumKeyExW(k, i, sub, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        DWORD bytes = sizeof(moniker);
        if (RegGetValueW(k, sub, L"Moniker", RRF_RT_REG_SZ, NULL, moniker, &bytes) == ERROR_SUCCESS &&
            !wcsncmp(moniker, prefix, plen) && wcslen(moniker) < 64)
            wcscpy(out[n++], moniker);
    }
    RegCloseKey(k);
    return n;
}

/* Delete the tab profiles of coordinators that are gone (one that crashed
 * couldn't delete its own). */
static void sweep_tab_profiles(void) {
    static wchar_t names[512][64];
    int n = list_profiles(L"cyberbrowser.tab.", names, 512);
    n += list_profiles(L"cyberbrowser.webgpu.", names + n, 512 - n);
    for (int i = 0; i < n; i++) {
        const wchar_t *pidp = wcschr(names[i] + 13, L'.');
        unsigned long pid = pidp ? wcstoul(pidp + 1, NULL, 10) : 0;
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) CloseHandle(p);
        if (!alive) DeleteAppContainerProfile(names[i]);
    }
}

/* The SID of a named capability ("registryRead"), as AppContainers carry
 * it; kept for the process's life. */
static PSID named_capability(const wchar_t *name) {
    typedef BOOL(WINAPI * Derive)(LPCWSTR, PSID **, DWORD *, PSID **, DWORD *);
    Derive derive = (Derive)(void *)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"),
                                                   "DeriveCapabilitySidsFromName");
    PSID *groups = NULL, *sids = NULL;
    DWORD ngroups = 0, nsids = 0;
    if (!derive || !derive(name, &groups, &ngroups, &sids, &nsids)) return NULL;
    PSID sid = nsids ? sids[0] : NULL;
    for (DWORD i = 0; i < ngroups; i++) LocalFree(groups[i]);
    for (DWORD i = 1; i < nsids; i++) LocalFree(sids[i]);
    LocalFree(groups);
    LocalFree(sids);
    return sid;
}

bool sandbox_init(void) {
    const char *off = getenv("CYBER_NO_SANDBOX");
    if (off && off[0] && off[0] != '0') {
        fprintf(stderr, "[sandbox] off (CYBER_NO_SANDBOX)\n");
        return false;
    }
    HANDLE tok;
    static union { TOKEN_USER u; char b[256]; } user;
    DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    bool got = GetTokenInformation(tok, TokenUser, &user, sizeof(user), &n);
    CloseHandle(tok);
    if (!got) return false;
    g_user = user.u.User.Sid;
    /* Process and thread objects: this user and SYSTEM only, and a medium
     * label, so no process at low integrity (every sandboxed one) can open
     * them, whatever the DACL says. */
    wchar_t *usid = NULL, sddl[256];
    if (!ConvertSidToStringSidW(g_user, &usid)) return false;
    _snwprintf(sddl, 256, L"D:P(A;;GA;;;SY)(A;;GA;;;%ls)", usid);
    LocalFree(usid);
    BOOL present, defaulted;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &g_object_sd, NULL) ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"S:(ML;;NWNRNX;;;ME)", SDDL_REVISION_1, &g_label_sd,
                                                              NULL) ||
        !GetSecurityDescriptorSacl(g_label_sd, &present, &g_label, &defaulted) || !present)
        return false;
    g_object_sa.nLength = sizeof(g_object_sa);
    g_object_sa.lpSecurityDescriptor = g_object_sd;
    g_object_sa.bInheritHandle = FALSE;
    sweep_tab_profiles();
    g_store_sid = profile_sid(L"cyberbrowser.store");
    g_ui_sid = profile_sid(L"cyberbrowser.ui");
    g_net_sid = profile_sid(L"cyberbrowser.net");
    /* The network process's capabilities: the internet, and the local
     * network (routers, printers, development machines). */
    static const WELL_KNOWN_SID_TYPE caps[2] = {
        WinCapabilityInternetClientSid, WinCapabilityPrivateNetworkClientServerSid,
    };
    for (int i = 0; i < 2; i++) {
        DWORD n = sizeof(g_net_caps[i]);
        if (!CreateWellKnownSid(caps[i], NULL, &g_net_caps[i], &n)) return false;
        g_net_cap_list[i].Sid = &g_net_caps[i];
        g_net_cap_list[i].Attributes = SE_GROUP_ENABLED;
    }
    g_net_ncaps = 2;
    /* And the named ones LPAC needs to start Winsock and check
     * certificates: reading the registry (the Winsock catalog), and the
     * cryptographic services. */
    static const wchar_t *named[] = { L"registryRead", L"lpacCryptoServices" };
    for (int i = 0; i < (int)(sizeof(named) / sizeof(named[0])); i++) {
        PSID sid = named_capability(named[i]);
        if (!sid) {
            fprintf(stderr, "[sandbox] no capability %ls\n", named[i]);
            return false;
        }
        g_net_cap_list[g_net_ncaps].Sid = sid;
        g_net_cap_list[g_net_ncaps++].Attributes = SE_GROUP_ENABLED;
    }
    g_on = g_store_sid && g_ui_sid && g_net_sid;
    return g_on;
}

bool sandbox_enabled(void) {
    return g_on;
}

/* The low-integrity, restricted token of the loopback relay (an
 * AppContainer can't connect to loopback addresses).  Every access needs one
 * of the restricting SIDs (restricted code, Everyone, Users, Authenticated
 * Users) as well as the user's: the system's files grant them reading, so
 * DLLs load and Winsock starts, but the user's profile, other processes and
 * the tabs' objects don't, so it can neither read nor write them. */
static HANDLE loop_token(void) {
    HANDLE self = NULL, tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_DEFAULT,
                          &self))
        return NULL;
    static const WELL_KNOWN_SID_TYPE restricting[] = {
        WinRestrictedCodeSid, WinWorldSid, WinBuiltinUsersSid, WinAuthenticatedUserSid,
    };
    union { SID s; char b[SECURITY_MAX_SID_SIZE]; } sids[4], admins;
    SID_AND_ATTRIBUTES rs[4], deny[1];
    DWORD n;
    for (int i = 0; i < 4; i++) {
        n = sizeof(sids[i]);
        CreateWellKnownSid(restricting[i], NULL, &sids[i], &n);
        rs[i].Sid = &sids[i];
        rs[i].Attributes = 0;
    }
    n = sizeof(admins);
    CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, &admins, &n);
    deny[0].Sid = &admins;
    deny[0].Attributes = 0;
    bool ok = CreateRestrictedToken(self, DISABLE_MAX_PRIVILEGE, 1, deny, 0, NULL, 4, rs, &tok);
    CloseHandle(self);
    if (!ok) return NULL;
    /* Low integrity. */
    union { SID s; char b[SECURITY_MAX_SID_SIZE]; } low;
    n = sizeof(low);
    CreateWellKnownSid(WinLowLabelSid, NULL, &low, &n);
    TOKEN_MANDATORY_LABEL tml;
    tml.Label.Sid = &low;
    tml.Label.Attributes = SE_GROUP_INTEGRITY;
    /* Its own objects: this user, SYSTEM, and the restricted-code SID it
     * writes with. */
    union { ACL acl; char b[512]; } dacl;
    InitializeAcl(&dacl.acl, sizeof(dacl), ACL_REVISION);
    AddAccessAllowedAce(&dacl.acl, ACL_REVISION, GENERIC_ALL, g_user);
    union { SID s; char b[SECURITY_MAX_SID_SIZE]; } system;
    n = sizeof(system);
    CreateWellKnownSid(WinLocalSystemSid, NULL, &system, &n);
    AddAccessAllowedAce(&dacl.acl, ACL_REVISION, GENERIC_ALL, &system);
    AddAccessAllowedAce(&dacl.acl, ACL_REVISION, GENERIC_ALL, &sids[0]);
    TOKEN_DEFAULT_DACL dd = { &dacl.acl };
    if (!SetTokenInformation(tok, TokenIntegrityLevel, &tml, sizeof(tml) + GetLengthSid(&low)) ||
        !SetTokenInformation(tok, TokenDefaultDacl, &dd, sizeof(dd))) {
        CloseHandle(tok);
        return NULL;
    }
    return tok;
}

/* A job of the child's own, nested in the coordinator's: one process, a
 * memory limit, and no reach into other processes' windows, the clipboard,
 * global atoms or system settings. */
static HANDLE child_job(SandboxKind kind) {
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (!job) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof(lim));
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    lim.BasicLimitInformation.ActiveProcessLimit = 1;
    unsigned long long mem = kind == SB_TAB ? TAB_MEMORY_LIMIT : kind == SB_NET ? NET_MEMORY_LIMIT
                           : kind == SB_STORE ? STORE_MEMORY_LIMIT : kind == SB_LOOP ? LOOP_MEMORY_LIMIT
                           : kind == SB_GPU ? GPU_MEMORY_LIMIT : kind == SB_WEBGPU ? WEBGPU_MEMORY_LIMIT : 0;
    if (mem) {
        lim.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        lim.ProcessMemoryLimit = (SIZE_T)mem;
    }
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    if (kind != SB_UI)
        ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_READCLIPBOARD |
                                  JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
    /* The GPU process presents into the UI's window: a USER handle from
     * outside its job. */
    if (kind != SB_UI && kind != SB_GPU) ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_HANDLES;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim)) ||
        !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui))) {
        CloseHandle(job);
        return NULL;
    }
    return job;
}

bool sandbox_for(SandboxKind kind, int slot, SpawnSandbox *sb) {
    memset(sb, 0, sizeof(*sb));
    if (!g_on) return true;
    DWORD64 m = PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON |
                PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON;
    if (kind != SB_UI && kind != SB_GPU && kind != SB_WEBGPU)
        m |= PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON;
    if (kind == SB_TAB) m |= PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON;
    sb->mitigations[0] = m;
    /* No child processes: the job allows one process.  (The child process
     * policy would say it again, but with it our processes fail their DLL
     * initialization, STATUS_DLL_INIT_FAILED.) */
    sb->object_sa = &g_object_sa;
    sb->label = g_label;
    switch (kind) {
    case SB_TAB:
        sb->appcontainer = new_tab_sid(slot);
        sb->lpac = true;
        /* What the tab creates (its threads, say): this user, SYSTEM and
         * the tab itself, and nothing the loopback relay's restricting
         * SIDs match, so it can't open them. */
        if (sb->appcontainer) {
            ACL *a = &g_tab_dacl[slot].acl;
            union { SID s; char b[SECURITY_MAX_SID_SIZE]; } system;
            DWORD n = sizeof(system);
            CreateWellKnownSid(WinLocalSystemSid, NULL, &system, &n);
            InitializeAcl(a, sizeof(g_tab_dacl[slot]), ACL_REVISION);
            AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, g_user);
            AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, &system);
            AddAccessAllowedAce(a, ACL_REVISION, GENERIC_ALL, sb->appcontainer);
            sb->default_dacl = a;
        }
        break;
    case SB_WEBGPU:
        /* Not LPAC: the graphics drivers' user-mode parts need what an
         * AppContainer has (the GPU process runs in one too). */
        sb->appcontainer = new_webgpu_sid(slot);
        if (sb->appcontainer)
            sb->default_dacl = own_dacl(&g_webgpu_dacl[slot].acl, sizeof(g_webgpu_dacl[slot]), sb->appcontainer);
        break;
    case SB_STORE:
        sb->appcontainer = g_store_sid;
        sb->lpac = true;
        break;
    case SB_UI:
        sb->appcontainer = g_ui_sid;
        break;
    case SB_GPU:
        /* The UI's AppContainer: its windows are the only thing that gives
         * (presenting into them); its job is its own. */
        sb->appcontainer = g_ui_sid;
        break;
    case SB_NET:
        /* LPAC with the network: it reads no files but those it is given
         * (sandbox_grant_folder: the replays), and reaches loopback only
         * through the relay. */
        sb->appcontainer = g_net_sid;
        sb->lpac = true;
        sb->caps = g_net_cap_list;
        sb->ncaps = g_net_ncaps;
        break;
    case SB_LOOP:
        sb->token = loop_token();
        if (!sb->token) return false;
        break;
    }
    if (kind != SB_LOOP && !sb->appcontainer) return false;
    sb->job = child_job(kind);
    if (!sb->job) {
        sandbox_release(sb);
        return false;
    }
    return true;
}

void sandbox_release(SpawnSandbox *sb) {
    /* The job lives on while its process does. */
    if (sb->job) CloseHandle(sb->job);
    if (sb->token) CloseHandle(sb->token);
    sb->job = sb->token = NULL;
}

bool sandbox_grant_folder(const wchar_t *dir, SandboxKind kind, bool write) {
    if (!g_on) return true;
    PSID sid = kind == SB_STORE ? g_store_sid : kind == SB_NET ? g_net_sid : kind == SB_UI ? g_ui_sid : NULL;
    if (!sid) return false;
    PACL old = NULL, acl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd) !=
        ERROR_SUCCESS)
        return false;
    /* Already given (an inherited grant to everything in it): setting it
     * again would rewrite what everything in the folder inherits, which
     * at the same time as another process doing it (test runs side by
     * side) can leave a file with none of it. */
    GENERIC_MAPPING fmap = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    ACCESS_MASK need = write ? GENERIC_ALL : GENERIC_READ | GENERIC_EXECUTE;
    MapGenericMask(&need, &fmap);
    for (DWORD i = 0; old && i < old->AceCount; i++) {
        ACCESS_ALLOWED_ACE *ace = NULL;
        if (!GetAce(old, i, (void **)&ace) || ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE) continue;
        if ((ace->Header.AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) !=
                (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE) ||
            (ace->Header.AceFlags & INHERIT_ONLY_ACE))
            continue;
        ACCESS_MASK have = ace->Mask;
        MapGenericMask(&have, &fmap);
        if ((have & need) == need && EqualSid((PSID)&ace->SidStart, sid)) {
            LocalFree(sd);
            return true;
        }
    }
    EXPLICIT_ACCESSW ea;
    memset(&ea, 0, sizeof(ea));
    ea.grfAccessPermissions = write ? GENERIC_ALL : GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)sid;
    bool ok = SetEntriesInAclW(1, &ea, old, &acl) == ERROR_SUCCESS &&
              SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL) ==
                  ERROR_SUCCESS;
    if (acl) LocalFree(acl);
    LocalFree(sd);
    return ok;
}

bool sandbox_grant_packages_read(const wchar_t *dir) {
    if (!g_on) return true;
    union { SID s; char b[SECURITY_MAX_SID_SIZE]; } any;
    DWORD n = sizeof(any);
    if (!CreateWellKnownSid(WinBuiltinAnyPackageSid, NULL, &any, &n)) return false;
    PACL old = NULL, acl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd) !=
        ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESSW ea;
    memset(&ea, 0, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_READ | GENERIC_EXECUTE;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)&any;
    bool ok = SetEntriesInAclW(1, &ea, old, &acl) == ERROR_SUCCESS &&
              SetNamedSecurityInfoW((LPWSTR)dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, acl, NULL) ==
                  ERROR_SUCCESS;
    if (acl) LocalFree(acl);
    LocalFree(sd);
    return ok;
}

int sandbox_remove_profiles(void) {
    static wchar_t names[512][64];
    int n = list_profiles(L"cyberbrowser.", names, 512), removed = 0;
    for (int i = 0; i < n; i++)
        if (SUCCEEDED(DeleteAppContainerProfile(names[i]))) removed++;
    return removed;
}
