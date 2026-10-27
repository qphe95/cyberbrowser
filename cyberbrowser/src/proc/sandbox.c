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

#include "coord_core.h"

static bool g_on;
static PSECURITY_DESCRIPTOR g_object_sd, g_label_sd;
static PACL g_label;
static SECURITY_ATTRIBUTES g_object_sa;
static PSID g_user;                     /* this process's user */
static PSID g_tab_sids[COORD_MAX_TABS];
static wchar_t g_tab_names[COORD_MAX_TABS][64];  /* their profiles, deleted when the tab ends */
static unsigned g_tab_serial;
static PSID g_store_sid, g_ui_sid;
static union { ACL acl; char b[512]; } g_tab_dacl[COORD_MAX_TABS];

#define TAB_MEMORY_LIMIT   (4ull << 30)
#define NET_MEMORY_LIMIT   (2ull << 30)
#define STORE_MEMORY_LIMIT (512ull << 20)

/* The AppContainer SID of profile `name`, registering the profile the first
 * time: CreateProcess refuses an AppContainer SID with no profile. */
static PSID profile_sid(const wchar_t *name) {
    PSID sid = NULL;
    HRESULT hr = CreateAppContainerProfile(name, name, L"cyberbrowser sandbox", NULL, 0, &sid);
    if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS))
        hr = DeriveAppContainerSidFromAppContainerName(name, &sid);
    if (FAILED(hr)) {
        fprintf(stderr, "[sandbox] profile %ls: %lx\n", name, (unsigned long)hr);
        return NULL;
    }
    return sid;
}

/* A profile of its own for the tab process about to start in `slot`,
 * "cyberbrowser.tab.<coordinator pid>.<n>": its SID is no other process's,
 * ever, and what the tab leaves in the profile's folder and registry
 * storage goes with it (sandbox_tab_ended). */
static PSID new_tab_sid(int slot) {
    if (slot < 0 || slot >= COORD_MAX_TABS) return NULL;
    sandbox_tab_ended(slot);
    _snwprintf(g_tab_names[slot], 64, L"cyberbrowser.tab.%lu.%u", GetCurrentProcessId(), ++g_tab_serial);
    g_tab_sids[slot] = profile_sid(g_tab_names[slot]);
    if (!g_tab_sids[slot]) g_tab_names[slot][0] = 0;
    return g_tab_sids[slot];
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
    for (int i = 0; i < n; i++) {
        unsigned long pid = wcstoul(names[i] + 17, NULL, 10);
        HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
        bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) CloseHandle(p);
        if (!alive) DeleteAppContainerProfile(names[i]);
    }
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
    g_on = g_store_sid && g_ui_sid;
    return g_on;
}

bool sandbox_enabled(void) {
    return g_on;
}

/* The low-integrity, write-restricted token of the network process: it may
 * read what the user may, but writes need one of the restricting SIDs, which
 * the user's files, other processes and the tabs' objects don't grant. */
static HANDLE net_token(void) {
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
    bool ok = CreateRestrictedToken(self, DISABLE_MAX_PRIVILEGE | WRITE_RESTRICTED, 1, deny, 0, NULL, 4, rs, &tok);
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
                           : kind == SB_STORE ? STORE_MEMORY_LIMIT : 0;
    if (mem) {
        lim.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        lim.ProcessMemoryLimit = (SIZE_T)mem;
    }
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui;
    ui.UIRestrictionsClass = JOB_OBJECT_UILIMIT_DESKTOP | JOB_OBJECT_UILIMIT_DISPLAYSETTINGS |
                             JOB_OBJECT_UILIMIT_EXITWINDOWS | JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS;
    if (kind != SB_UI)
        ui.UIRestrictionsClass |= JOB_OBJECT_UILIMIT_GLOBALATOMS | JOB_OBJECT_UILIMIT_HANDLES |
                                  JOB_OBJECT_UILIMIT_READCLIPBOARD | JOB_OBJECT_UILIMIT_WRITECLIPBOARD;
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
    if (kind != SB_UI) m |= PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON;
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
         * the tab itself, and nothing the network process's restricting
         * SIDs match, so it can't write to them. */
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
    case SB_STORE:
        sb->appcontainer = g_store_sid;
        sb->lpac = true;
        break;
    case SB_UI:
        sb->appcontainer = g_ui_sid;
        break;
    case SB_NET:
        sb->token = net_token();
        if (!sb->token) return false;
        break;
    }
    if (kind != SB_NET && !sb->appcontainer) return false;
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

bool sandbox_grant_folder(const wchar_t *dir) {
    if (!g_on) return true;
    PACL old = NULL, acl = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    if (GetNamedSecurityInfoW(dir, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, NULL, NULL, &old, NULL, &sd) !=
        ERROR_SUCCESS)
        return false;
    EXPLICIT_ACCESSW ea;
    memset(&ea, 0, sizeof(ea));
    ea.grfAccessPermissions = GENERIC_ALL;
    ea.grfAccessMode = GRANT_ACCESS;
    ea.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
    ea.Trustee.ptstrName = (LPWSTR)g_store_sid;
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
