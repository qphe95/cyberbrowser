/* win_models.c - bodies for the few Windows functions whose contract alone
 * (include/windows.h) would lose what Eva needs.  TRUSTED, like the
 * contracts: each body does what the contract says and no more, in a form
 * Eva can follow.
 *   - Memory Windows hands out (a mapped view, a DIB section's pixels, the
 *     strings and descriptors the security calls allocate) is allocated
 *     here, so Eva knows its size and sees it freed (UnmapViewOfFile,
 *     LocalFree, FreeSid).  What a mapped view holds is the other
 *     process's to write: unknown bytes.
 *   - The C runtime's wide printf, so its %ls and %hs arguments are read
 *     as strings, and its truncation is Microsoft's (no NUL, -1).
 *   - Window procedures: RegisterClassW and SetWindowLongPtrW remember
 *     them, DispatchMessageW calls one with the message it is given.
 *   - getenv (the C library's): Frama-C's model returns strings Eva can't
 *     follow; here NULL or any NUL-terminated string. */
#include <windows.h>
#include <stdlib.h>
#include "__fc_builtin.h"

int __fc_win_state;
const struct in6_addr in6addr_loopback = { { { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 } } };

/* ---- memory ------------------------------------------------------------------ */

#define FC_VIEW_KNOWN 4096

/* The C runtime's environment (getenv is the C library's, but Frama-C's
 * model returns strings Eva can't follow): NULL, or a NUL-terminated string
 * of any content, here of at most 4095 bytes. */
char *getenv(const char *name) {
    static char value[4096];
    (void)strlen(name);
    if (Frama_C_nondet(0, 1)) return NULL;
    Frama_C_make_unknown(value, sizeof(value) - 1);
    value[sizeof(value) - 1] = 0;
    return value;
}

LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                     DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap) {
    (void)hFileMappingObject;
    (void)dwDesiredAccess;
    (void)dwFileOffsetHigh;
    (void)dwFileOffsetLow;
    if (Frama_C_nondet(0, 1)) return NULL;          /* no such section, too large, no memory */
    /* calloc: the view's bytes are all set (Windows maps zeroed or written
     * pages), also to Eva when the block is a summary of several views. */
    char *p = (char *)calloc(1, dwNumberOfBytesToMap);
    if (!p) return NULL;
    /* Any bytes.  Eva can make bytes unknown only over a range of known
     * size: a view larger than FC_VIEW_KNOWN bytes (a channel's) gets its
     * first FC_VIEW_KNOWN unknown, which hold every header the shims read
     * (ChanShared, 448 bytes); after that are the rings, which only
     * chan_ring_put and chan_ring_take touch, as their contracts
     * (chan_ring_model.c: any message, whatever the bytes).  A smaller view
     * is unknown whole. */
    if (dwNumberOfBytesToMap >= FC_VIEW_KNOWN) Frama_C_make_unknown(p, FC_VIEW_KNOWN);
    else Frama_C_make_unknown(p, dwNumberOfBytesToMap);
    return p;
}

BOOL UnmapViewOfFile(LPCVOID lpBaseAddress) {
    free((void *)lpBaseAddress);
    return Frama_C_nondet(0, 1);
}

HANDLE LocalFree(HANDLE hMem) {
    free(hMem);
    return NULL;
}

PVOID FreeSid(PSID pSid) {
    free(pSid);
    return NULL;
}

/* A SID of 8 to SECURITY_MAX_SID_SIZE bytes, made by Windows. */
static PSID new_sid(void) {
    SID *s = (SID *)malloc(SECURITY_MAX_SID_SIZE);
    if (!s) return NULL;
    Frama_C_make_unknown((char *)s, SECURITY_MAX_SID_SIZE);
    s->SubAuthorityCount = (BYTE)Frama_C_interval(0, 15);
    return s;
}

BOOL ConvertSidToStringSidW(PSID Sid, LPWSTR *StringSid) {
    (void)Sid;
    if (Frama_C_nondet(0, 1)) return FALSE;
    size_t n = (size_t)Frama_C_interval(5, 184);    /* "S-1-..." */
    wchar_t *s = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!s) return FALSE;
    for (size_t i = 0; i < n; i++) s[i] = (wchar_t)Frama_C_interval(1, 0xffff);
    s[n] = 0;
    *StringSid = s;
    return TRUE;
}

BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR StringSecurityDescriptor, DWORD StringSDRevision,
                                                         PSECURITY_DESCRIPTOR *SecurityDescriptor,
                                                         PULONG SecurityDescriptorSize) {
    (void)StringSDRevision;
    size_t n = wcslen(StringSecurityDescriptor);    /* it is read */
    (void)n;
    if (Frama_C_nondet(0, 1)) return FALSE;
    char *sd = (char *)malloc(256);
    if (!sd) return FALSE;
    Frama_C_make_unknown(sd, 256);
    *SecurityDescriptor = sd;
    if (SecurityDescriptorSize) *SecurityDescriptorSize = 256;
    return TRUE;
}

BOOL GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL *lpbSaclPresent, PACL *pSacl,
                               BOOL *lpbSaclDefaulted) {
    *lpbSaclPresent = Frama_C_nondet(0, 1);
    *lpbSaclDefaulted = Frama_C_nondet(0, 1);
    *pSacl = *lpbSaclPresent ? (PACL)((char *)pSecurityDescriptor + 20) : NULL;    /* inside it */
    return Frama_C_nondet(0, 1);
}

DWORD GetNamedSecurityInfoW(LPCWSTR pObjectName, SE_OBJECT_TYPE ObjectType, SECURITY_INFORMATION SecurityInfo,
                            PSID *ppsidOwner, PSID *ppsidGroup, PACL *ppDacl, PACL *ppSacl,
                            PSECURITY_DESCRIPTOR *ppSecurityDescriptor) {
    (void)ObjectType;
    (void)SecurityInfo;
    (void)ppsidOwner;
    (void)ppsidGroup;
    (void)ppSacl;
    size_t n = wcslen(pObjectName);
    (void)n;
    if (Frama_C_nondet(0, 1)) return ERROR_ACCESS_DENIED;
    char *sd = (char *)malloc(512);
    if (!sd) return 8;                              /* ERROR_NOT_ENOUGH_MEMORY */
    Frama_C_make_unknown(sd, 512);
    *ppSecurityDescriptor = sd;
    *ppDacl = Frama_C_nondet(0, 1) ? NULL : (PACL)(sd + 20);
    return ERROR_SUCCESS;
}

DWORD SetEntriesInAclW(ULONG cCountOfExplicitEntries, PEXPLICIT_ACCESSW pListOfExplicitEntries, PACL OldAcl,
                       PACL *NewAcl) {
    (void)OldAcl;
    for (ULONG i = 0; i < cCountOfExplicitEntries; i++) {
        EXPLICIT_ACCESSW e = pListOfExplicitEntries[i];  /* it is read */
        (void)e;
    }
    if (Frama_C_nondet(0, 1)) return ERROR_INVALID_PARAMETER;
    char *a = (char *)malloc(512);
    if (!a) return 8;
    Frama_C_make_unknown(a, 512);
    *NewAcl = (PACL)a;
    return ERROR_SUCCESS;
}

HRESULT CreateAppContainerProfile(PCWSTR pszAppContainerName, PCWSTR pszDisplayName, PCWSTR pszDescription,
                                  PSID_AND_ATTRIBUTES pCapabilities, DWORD dwCapabilityCount,
                                  PSID *ppSidAppContainerSid) {
    (void)pCapabilities;
    (void)dwCapabilityCount;
    size_t n = wcslen(pszAppContainerName) + wcslen(pszDisplayName) + wcslen(pszDescription);
    (void)n;
    __fc_win_state = Frama_C_interval(0, 1000);
    int r = Frama_C_interval(0, 2);
    if (r == 0) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
    if (r == 1) return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
    PSID s = new_sid();
    if (!s) return (HRESULT)0x8007000EL;            /* E_OUTOFMEMORY */
    *ppSidAppContainerSid = s;
    return S_OK;
}

HRESULT DeriveAppContainerSidFromAppContainerName(PCWSTR pszAppContainerName, PSID *ppsidAppContainerSid) {
    size_t n = wcslen(pszAppContainerName);
    (void)n;
    if (Frama_C_nondet(0, 1)) return HRESULT_FROM_WIN32(ERROR_INVALID_PARAMETER);
    PSID s = new_sid();
    if (!s) return (HRESULT)0x8007000EL;
    *ppsidAppContainerSid = s;
    return S_OK;
}

/* kernelbase.dll's DeriveCapabilitySidsFromName, for GetProcAddress:
 * arrays of SIDs it allocates, each to LocalFree, and the arrays too. */
static BOOL fc_DeriveCapabilitySidsFromName(LPCWSTR CapName, PSID **CapabilityGroupSids,
                                           DWORD *CapabilityGroupSidCount, PSID **CapabilitySids,
                                           DWORD *CapabilitySidCount) {
    size_t n = wcslen(CapName);
    (void)n;
    if (Frama_C_nondet(0, 1)) return FALSE;
    DWORD ng = (DWORD)Frama_C_interval(0, 2), ns = (DWORD)Frama_C_interval(0, 2);
    PSID *g = (PSID *)malloc((ng ? ng : 1) * sizeof(PSID)), *s = (PSID *)malloc((ns ? ns : 1) * sizeof(PSID));
    if (!g || !s) { free(g); free(s); return FALSE; }
    for (DWORD i = 0; i < ng; i++) g[i] = new_sid();
    for (DWORD i = 0; i < ns; i++) s[i] = new_sid();
    *CapabilityGroupSids = g;
    *CapabilityGroupSidCount = ng;
    *CapabilitySids = s;
    *CapabilitySidCount = ns;
    return TRUE;
}

FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    (void)hModule;
    if (Frama_C_nondet(0, 1)) return NULL;
    if (!strcmp(lpProcName, "DeriveCapabilitySidsFromName")) return (FARPROC)(void *)fc_DeriveCapabilitySidsFromName;
    return NULL;
}

/* The pixels of a DIB section: rows of the width's bytes, padded to 4. */
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO *pbmi, UINT usage, void **ppvBits, HANDLE hSection,
                         DWORD offset) {
    (void)hdc;
    (void)usage;
    (void)hSection;
    (void)offset;
    *ppvBits = NULL;
    if (Frama_C_nondet(0, 1)) return NULL;
    long long w = pbmi->bmiHeader.biWidth, h = pbmi->bmiHeader.biHeight;
    if (h < 0) h = -h;
    size_t stride = (size_t)(((w * pbmi->bmiHeader.biBitCount + 31) / 32) * 4);
    void *bits = malloc(stride * (size_t)h);
    if (!bits) return NULL;
    *ppvBits = bits;
    return (HBITMAP)(uintptr_t)Frama_C_interval(1, 0x7fffffff);
}

/* ---- window procedures ------------------------------------------------------------ */

#define FC_MAX_PROCS 8
static WNDPROC fc_procs[FC_MAX_PROCS];
static int fc_nprocs;

static void remember(WNDPROC p) {
    for (int i = 0; i < fc_nprocs; i++)
        if (fc_procs[i] == p) return;
    if (p && fc_nprocs < FC_MAX_PROCS) fc_procs[fc_nprocs++] = p;
}

ATOM RegisterClassW(const WNDCLASSW *lpWndClass) {
    size_t n = wcslen(lpWndClass->lpszClassName);
    (void)n;
    remember(lpWndClass->lpfnWndProc);
    __fc_win_state = Frama_C_interval(0, 1000);
    return (ATOM)Frama_C_interval(0, 0xffff);
}

/* An edit control's own procedure, which SetWindowLongPtrW hands back. */
static LRESULT fc_edit_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)h; (void)m; (void)w; (void)l;
    return Frama_C_interval(-1000, 1000);
}

LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong) {
    (void)hWnd;
    if (nIndex == GWLP_WNDPROC) {
        remember((WNDPROC)dwNewLong);
        return Frama_C_nondet(0, 1) ? 0 : (LONG_PTR)fc_edit_proc;
    }
    return 0;
}

LRESULT DispatchMessageW(const MSG *lpMsg) {
    if (fc_nprocs == 0) return 0;
    int k = Frama_C_interval(0, fc_nprocs - 1);
    return fc_procs[k](lpMsg->hwnd, lpMsg->message, lpMsg->wParam, lpMsg->lParam);
}

/* ---- the C runtime's wide printf ----------------------------------------------------- */

/* Formats into buffer[0 .. count - 1] when buffer isn't NULL, else only
 * reads the arguments.  Numbers come out as 1 to 20 unknown digits (at
 * least the width).  Returns the length the text has. */
static size_t fc_wformat(wchar_t *buffer, size_t count, const wchar_t *format, va_list ap) {
    size_t o = 0;
    //@ loop unroll 64;
    for (const wchar_t *p = format; *p; p++) {
        if (*p != L'%') {
            if (buffer && o < count) buffer[o] = *p;
            o++;
            continue;
        }
        p++;
        if (*p == L'%') {
            if (buffer && o < count) buffer[o] = L'%';
            o++;
            continue;
        }
        int width = 0, len = 0;                     /* len: -1 h, 1 l, 2 ll */
        //@ loop unroll 4;
        while (*p == L'-' || *p == L'+' || *p == L' ' || *p == L'#' || *p == L'0') p++;
        //@ loop unroll 4;
        while (*p >= L'0' && *p <= L'9') { width = width * 10 + (*p - L'0'); p++; }
        if (*p == L'h') { len = -1; p++; }
        else if (*p == L'l') { len = 1; p++; if (*p == L'l') { len = 2; p++; } }
        if (*p == L's' && len < 0) {
            const char *s = va_arg(ap, const char *);
            for (size_t i = 0; s[i]; i++) {
                if (buffer && o < count) buffer[o] = (wchar_t)(unsigned char)s[i];
                o++;
            }
        } else if (*p == L's') {
            const wchar_t *s = va_arg(ap, const wchar_t *);
            for (size_t i = 0; s[i]; i++) {
                if (buffer && o < count) buffer[o] = s[i];
                o++;
            }
        } else {
            /* d i u x X o c */
            if (len == 2) { long long v = va_arg(ap, long long); (void)v; }
            else if (len == 1) { long v = va_arg(ap, long); (void)v; }
            else { int v = va_arg(ap, int); (void)v; }
            int digits = Frama_C_interval(width > 1 ? width : 1, width > 20 ? width : 20);
            for (int i = 0; i < digits; i++) {
                if (buffer && o < count) buffer[o] = (wchar_t)Frama_C_interval(L'0', L'f');
                o++;
            }
        }
        if (!*p) break;
    }
    return o;
}

int __fc_snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...) {
    va_list ap;
    va_start(ap, format);
    size_t n = fc_wformat(buffer, count, format, ap);
    va_end(ap);
    if (n < count) {
        buffer[n] = 0;
        return (int)n;
    }
    return n == count ? (int)n : -1;                /* no NUL */
}

int __fc_fwprintf(FILE *stream, const wchar_t *format, ...) {
    (void)stream;
    va_list ap;
    va_start(ap, format);
    size_t n = fc_wformat(NULL, 0, format, ap);
    va_end(ap);
    return n > 0x7fffffff ? -1 : (int)n;
}
