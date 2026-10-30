/* windows.h - the Windows API as Frama-C/Eva sees it (SANDBOX_DESIGN.md
 * section 8.7.3, step 5).  Used only by tools/prove.sh's eva targets, which
 * put proofs/eva/include ahead of every other include directory and define
 * FRAMA_C; the build never sees this file.
 *
 * TRUSTED.  Every contract below is an assumption about Windows (or the C
 * runtime MinGW links), part of section 8.4's trusted base: Eva checks the
 * shims' calls against the `requires` clauses and believes the `assigns` and
 * `ensures` clauses.  They follow Microsoft's documentation:
 *   - buffers: a call requires what the documentation says it reads or
 *     writes to be valid for the sizes passed (bytes or characters, as the
 *     API counts them), and writes at most there;
 *   - results: a handle may be NULL (or INVALID_HANDLE_VALUE, for the calls
 *     that fail with it); a count returned is at most the room given; any
 *     call may fail, unless the documentation says it can't;
 *   - effects on the system (processes, files, the registry, events) are
 *     an assignment to __fc_win_state, which every result may depend on,
 *     so nothing the shims learn from Windows is taken as known.
 * Types have their Windows sizes under the LLP64 machdep
 * (machdep_mingw_x86_64.yaml): DWORD and LONG are 32 bits, WCHAR 16.
 *
 * A few functions also have a body (win_models.c), where a contract alone
 * would lose what Eva needs: memory that is mapped (the peer's bytes,
 * unknown), the wide printf of the C runtime (so its %ls arguments are
 * read as strings), and window procedures that Windows calls back. */
#ifndef FC_WINDOWS_H
#define FC_WINDOWS_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "__fc_builtin.h"
#include "fc_atomic.h"

/* What Windows holds outside this process: files, other processes, the
 * clock, the last error.  Read and written by the calls that touch it. */
extern int __fc_win_state;

#define WINAPI
#define CALLBACK
#define APIENTRY
#define WINAPIV
#define __stdcall
#define __cdecl
#define DECLSPEC_NORETURN __attribute__((noreturn))
#define VOID void
#define CONST const
#define FAR
#define NEAR
#define IN
#define OUT
#define OPTIONAL

/* ---- base types -------------------------------------------------------------- */

typedef int BOOL;
typedef unsigned char BOOLEAN;
typedef unsigned char BYTE, UCHAR, *PUCHAR, *PBYTE, *LPBYTE;
typedef unsigned short WORD, USHORT;
typedef unsigned long DWORD, ULONG, *PDWORD, *LPDWORD, *PULONG;
typedef long LONG, *PLONG, *LPLONG;
typedef int INT;
typedef unsigned int UINT;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG, DWORD64, DWORDLONG;
typedef long long LONG_PTR, INT_PTR, SSIZE_T;
typedef unsigned long long ULONG_PTR, UINT_PTR, SIZE_T, DWORD_PTR, *PSIZE_T;
typedef ULONG_PTR WPARAM;
typedef LONG_PTR LPARAM, LRESULT;
typedef LONG HRESULT;
typedef DWORD COLORREF;
typedef WORD ATOM;
typedef DWORD ACCESS_MASK, SECURITY_INFORMATION;
typedef wchar_t WCHAR, *PWCHAR, *LPWSTR, *PWSTR;
typedef const wchar_t *LPCWSTR, *PCWSTR;
typedef char CHAR, *LPSTR, *PSTR;
typedef const char *LPCSTR, *PCSTR;
typedef void *PVOID, *LPVOID, *HANDLE, *PSID, *PSECURITY_DESCRIPTOR, *HGDIOBJ;
typedef const void *LPCVOID;
typedef HANDLE *PHANDLE, *LPHANDLE;
typedef long long (*FARPROC)(void);

/* Opaque: never dereferenced.  (MinGW's struct holds an int; a char here,
 * so that Eva doesn't take a handle's value for a pointer's alignment.) */
#define DECLARE_HANDLE(n) typedef struct n##__ { char unused; } *n
DECLARE_HANDLE(HWND);
DECLARE_HANDLE(HDC);
DECLARE_HANDLE(HBRUSH);
DECLARE_HANDLE(HFONT);
DECLARE_HANDLE(HBITMAP);
DECLARE_HANDLE(HINSTANCE);
DECLARE_HANDLE(HKEY);
DECLARE_HANDLE(HMONITOR);
DECLARE_HANDLE(HICON);
DECLARE_HANDLE(HMENU);
DECLARE_HANDLE(HRGN);
DECLARE_HANDLE(HPEN);
typedef HINSTANCE HMODULE;
typedef HICON HCURSOR;

#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFul
#define INVALID_HANDLE_VALUE ((HANDLE)(LONG_PTR)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define MAXIMUM_WAIT_OBJECTS 64

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; } u;
    LONGLONG QuadPart;
} LARGE_INTEGER, *PLARGE_INTEGER;

typedef struct _FILETIME { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;

typedef struct _SECURITY_ATTRIBUTES {
    DWORD nLength;
    LPVOID lpSecurityDescriptor;
    BOOL bInheritHandle;
} SECURITY_ATTRIBUTES, *PSECURITY_ATTRIBUTES, *LPSECURITY_ATTRIBUTES;

/* ---- errors -------------------------------------------------------------------- */

#define ERROR_SUCCESS 0L
#define NO_ERROR 0L
#define ERROR_FILE_NOT_FOUND 2L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_INSUFFICIENT_BUFFER 122L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_NO_MORE_ITEMS 259L
#define S_OK ((HRESULT)0)
#define FACILITY_WIN32 7
#define HRESULT_FROM_WIN32(x) \
    ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT)(((x) & 0x0000FFFF) | (FACILITY_WIN32 << 16) | 0x80000000)))
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define STATUS_DLL_INIT_FAILED ((DWORD)0xC0000142L)

/*@ assigns \result \from __fc_win_state; */
DWORD GetLastError(void);

/* ---- handles, events, waits ------------------------------------------------------ */

#define WAIT_OBJECT_0 0x00000000ul
#define WAIT_ABANDONED_0 0x00000080ul
#define WAIT_TIMEOUT 0x00000102ul
#define WAIT_FAILED 0xFFFFFFFFul
#define HANDLE_FLAG_INHERIT 0x00000001
#define HANDLE_FLAG_PROTECT_FROM_CLOSE 0x00000002
#define DUPLICATE_CLOSE_SOURCE 0x00000001
#define DUPLICATE_SAME_ACCESS 0x00000002
#define SYNCHRONIZE 0x00100000L
#define DELETE 0x00010000L
#define WRITE_OWNER 0x00080000L
#define GENERIC_READ 0x80000000ul
#define GENERIC_WRITE 0x40000000ul
#define GENERIC_EXECUTE 0x20000000ul
#define GENERIC_ALL 0x10000000ul

/* Any value: closing a handle that isn't one is an error Windows reports
 * (or, with strict handle checks, ends the process), not undefined. */
/*@ assigns __fc_win_state \from __fc_win_state, hObject;
    assigns \result \from __fc_win_state, hObject; */
BOOL CloseHandle(HANDLE hObject);

/*@ requires sa: lpEventAttributes == \null || \valid_read(lpEventAttributes);
    requires name: lpName == \null || valid_read_wstring(lpName);
    assigns __fc_win_state \from __fc_win_state, bManualReset, bInitialState;
    assigns \result \from __fc_win_state; */
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES lpEventAttributes, BOOL bManualReset, BOOL bInitialState,
                    LPCWSTR lpName);

/*@ assigns __fc_win_state \from __fc_win_state, hEvent;
    assigns \result \from __fc_win_state, hEvent; */
BOOL SetEvent(HANDLE hEvent);

/*@ assigns __fc_win_state \from __fc_win_state, hHandle, dwMilliseconds;
    assigns \result \from __fc_win_state, hHandle, dwMilliseconds;
    ensures result: \result == WAIT_OBJECT_0 || \result == WAIT_ABANDONED_0 || \result == WAIT_TIMEOUT ||
                    \result == WAIT_FAILED; */
DWORD WaitForSingleObject(HANDLE hHandle, DWORD dwMilliseconds);

/*@ requires count: 1 <= nCount <= MAXIMUM_WAIT_OBJECTS;
    requires handles: \valid_read(lpHandles + (0 .. nCount - 1));
    requires handles_set: \initialized(lpHandles + (0 .. nCount - 1));
    assigns __fc_win_state \from __fc_win_state, lpHandles[0 .. nCount - 1], dwMilliseconds;
    assigns \result \from __fc_win_state, lpHandles[0 .. nCount - 1], nCount, bWaitAll, dwMilliseconds;
    ensures result: \result < WAIT_OBJECT_0 + nCount ||
                    (WAIT_ABANDONED_0 <= \result < WAIT_ABANDONED_0 + nCount) ||
                    \result == WAIT_TIMEOUT || \result == WAIT_FAILED; */
DWORD WaitForMultipleObjects(DWORD nCount, const HANDLE *lpHandles, BOOL bWaitAll, DWORD dwMilliseconds);

#define QS_ALLINPUT 0x04FF
#define MWMO_INPUTAVAILABLE 0x0004
/*@ requires count: nCount <= MAXIMUM_WAIT_OBJECTS - 1;
    requires handles: nCount > 0 ==> \valid_read(pHandles + (0 .. nCount - 1));
    requires handles_set: nCount > 0 ==> \initialized(pHandles + (0 .. nCount - 1));
    assigns __fc_win_state \from __fc_win_state, pHandles[0 .. nCount - 1], dwMilliseconds;
    assigns \result \from __fc_win_state, pHandles[0 .. nCount - 1], nCount, dwMilliseconds, dwWakeMask, dwFlags;
    ensures result: \result <= WAIT_OBJECT_0 + nCount ||
                    (WAIT_ABANDONED_0 <= \result < WAIT_ABANDONED_0 + nCount) ||
                    \result == WAIT_TIMEOUT || \result == WAIT_FAILED; */
DWORD MsgWaitForMultipleObjectsEx(DWORD nCount, const HANDLE *pHandles, DWORD dwMilliseconds, DWORD dwWakeMask,
                                  DWORD dwFlags);

/*@ assigns __fc_win_state \from __fc_win_state, hObject, dwMask, dwFlags;
    assigns \result \from __fc_win_state; */
BOOL SetHandleInformation(HANDLE hObject, DWORD dwMask, DWORD dwFlags);

/* With DUPLICATE_CLOSE_SOURCE and no target process, the target handle may
 * be NULL: only the source is closed. */
/*@ requires target: lpTargetHandle == \null || \valid(lpTargetHandle);
    requires needs_target: hTargetProcessHandle != \null ==> lpTargetHandle != \null;
    assigns __fc_win_state \from __fc_win_state, hSourceProcessHandle, hSourceHandle, hTargetProcessHandle,
                                 dwDesiredAccess, bInheritHandle, dwOptions;
    assigns \result \from __fc_win_state;
    behavior with_target:
      assumes lpTargetHandle != \null;
      assigns __fc_win_state \from __fc_win_state, hSourceProcessHandle, hSourceHandle, hTargetProcessHandle,
                                   dwDesiredAccess, bInheritHandle, dwOptions;
      assigns \result \from __fc_win_state;
      assigns *lpTargetHandle \from __fc_win_state;
    behavior no_target:
      assumes lpTargetHandle == \null;
      assigns __fc_win_state \from __fc_win_state, hSourceProcessHandle, hSourceHandle, dwOptions;
      assigns \result \from __fc_win_state;
    complete behaviors;
    disjoint behaviors; */
BOOL DuplicateHandle(HANDLE hSourceProcessHandle, HANDLE hSourceHandle, HANDLE hTargetProcessHandle,
                     LPHANDLE lpTargetHandle, DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwOptions);

/* The pseudo-handle (HANDLE)-1, always. */
/*@ assigns \result \from \nothing;
    ensures \result == INVALID_HANDLE_VALUE; */
HANDLE GetCurrentProcess(void);

/*@ assigns \result \from __fc_win_state; */
DWORD GetCurrentProcessId(void);

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
DWORD GetTickCount(void);

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
ULONGLONG GetTickCount64(void);

/*@ assigns __fc_win_state \from __fc_win_state, dwMilliseconds; */
void Sleep(DWORD dwMilliseconds);

#define YieldProcessor() ((void)0)

/* ---- shared memory ---------------------------------------------------------- */

#define PAGE_READONLY 0x02
#define PAGE_READWRITE 0x04
#define FILE_MAP_WRITE 0x0002
#define FILE_MAP_READ 0x0004
#define FILE_MAP_ALL_ACCESS 0x000F001F

/*@ requires sa: lpFileMappingAttributes == \null || \valid_read(lpFileMappingAttributes);
    requires name: lpName == \null || valid_read_wstring(lpName);
    assigns __fc_win_state \from __fc_win_state, hFile, flProtect, dwMaximumSizeHigh, dwMaximumSizeLow;
    assigns \result \from __fc_win_state; */
HANDLE CreateFileMappingW(HANDLE hFile, LPSECURITY_ATTRIBUTES lpFileMappingAttributes, DWORD flProtect,
                          DWORD dwMaximumSizeHigh, DWORD dwMaximumSizeLow, LPCWSTR lpName);

/* A view of dwNumberOfBytesToMap bytes (the shims always pass a size), or
 * NULL.  Its bytes are the section's: written by whichever process shares
 * it, so unknown here (win_models.c; which marks only the first 4096 bytes
 * of a large view as set, see there).  0 bytes would map the whole
 * section, of a size unknown here: the shims never ask for that. */
/*@ requires sized: dwNumberOfBytesToMap > 0;
    allocates \result;
    assigns __fc_heap_status \from __fc_heap_status, dwNumberOfBytesToMap;
    assigns \result \from __fc_heap_status, __fc_win_state, hFileMappingObject, dwNumberOfBytesToMap;
    ensures view: \result == \null || \fresh(\result, dwNumberOfBytesToMap); */
LPVOID MapViewOfFile(HANDLE hFileMappingObject, DWORD dwDesiredAccess, DWORD dwFileOffsetHigh,
                     DWORD dwFileOffsetLow, SIZE_T dwNumberOfBytesToMap);

/* Only a view MapViewOfFile returned (else an error, which the shims never
 * cause); the view is gone afterwards. */
/*@ requires view: lpBaseAddress != \null && \freeable(lpBaseAddress);
    frees lpBaseAddress;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns \result \from __fc_win_state; */
BOOL UnmapViewOfFile(LPCVOID lpBaseAddress);

/* ---- processes, threads, jobs ------------------------------------------------- */

typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID lpThreadParameter);

/* The thread is not run here: Eva has one thread.  The drivers call each
 * thread's function themselves, in the state it starts in (see the notes). */
/*@ requires sa: lpThreadAttributes == \null || \valid_read(lpThreadAttributes);
    requires tid: lpThreadId == \null || \valid(lpThreadId);
    assigns __fc_win_state \from __fc_win_state, lpStartAddress, lpParameter;
    assigns \result \from __fc_win_state;
    behavior id:
      assumes lpThreadId != \null;
      assigns __fc_win_state \from __fc_win_state, lpStartAddress, lpParameter;
      assigns \result \from __fc_win_state;
      assigns *lpThreadId \from __fc_win_state;
    behavior no_id:
      assumes lpThreadId == \null;
      assigns __fc_win_state \from __fc_win_state, lpStartAddress, lpParameter;
      assigns \result \from __fc_win_state;
    complete behaviors;
    disjoint behaviors; */
HANDLE CreateThread(LPSECURITY_ATTRIBUTES lpThreadAttributes, SIZE_T dwStackSize,
                    LPTHREAD_START_ROUTINE lpStartAddress, LPVOID lpParameter, DWORD dwCreationFlags,
                    LPDWORD lpThreadId);

/*@ assigns __fc_win_state \from __fc_win_state, hThread;
    assigns \result \from __fc_win_state; */
DWORD ResumeThread(HANDLE hThread);

/*@ terminates \false;
    assigns \nothing;
    ensures \false; */
DECLSPEC_NORETURN void ExitProcess(UINT uExitCode);

/*@ assigns __fc_win_state \from __fc_win_state, hProcess, uExitCode;
    assigns \result \from __fc_win_state; */
BOOL TerminateProcess(HANDLE hProcess, UINT uExitCode);

/*@ requires code: \valid(lpExitCode);
    assigns *lpExitCode \from __fc_win_state, hProcess;
    assigns \result \from __fc_win_state, hProcess; */
BOOL GetExitCodeProcess(HANDLE hProcess, LPDWORD lpExitCode);

/*@ assigns __fc_win_state \from __fc_win_state, dwDesiredAccess, dwProcessId;
    assigns \result \from __fc_win_state, dwProcessId; */
HANDLE OpenProcess(DWORD dwDesiredAccess, BOOL bInheritHandle, DWORD dwProcessId);

typedef void (*WAITORTIMERCALLBACK)(PVOID, BOOLEAN);
#define WT_EXECUTEONLYONCE 0x00000008
/* The callback runs later on a pool thread: not here (see CreateThread). */
/*@ requires out: \valid(phNewWaitObject);
    assigns *phNewWaitObject \from __fc_win_state;
    assigns __fc_win_state \from __fc_win_state, hObject, Callback, Context, dwMilliseconds, dwFlags;
    assigns \result \from __fc_win_state; */
BOOL RegisterWaitForSingleObject(PHANDLE phNewWaitObject, HANDLE hObject, WAITORTIMERCALLBACK Callback,
                                 PVOID Context, ULONG dwMilliseconds, ULONG dwFlags);

/*@ assigns __fc_win_state \from __fc_win_state, WaitHandle, CompletionEvent;
    assigns \result \from __fc_win_state; */
BOOL UnregisterWaitEx(HANDLE WaitHandle, HANDLE CompletionEvent);

typedef struct _PROCESS_INFORMATION {
    HANDLE hProcess;
    HANDLE hThread;
    DWORD dwProcessId;
    DWORD dwThreadId;
} PROCESS_INFORMATION, *LPPROCESS_INFORMATION;

typedef struct _STARTUPINFOW {
    DWORD cb;
    LPWSTR lpReserved, lpDesktop, lpTitle;
    DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
    WORD wShowWindow, cbReserved2;
    LPBYTE lpReserved2;
    HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW, *LPSTARTUPINFOW;

typedef struct _PROC_THREAD_ATTRIBUTE_LIST *PPROC_THREAD_ATTRIBUTE_LIST, *LPPROC_THREAD_ATTRIBUTE_LIST;
typedef struct _STARTUPINFOEXW {
    STARTUPINFOW StartupInfo;
    LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;
} STARTUPINFOEXW;

#define STARTF_USESTDHANDLES 0x00000100
#define CREATE_SUSPENDED 0x00000004
#define DETACHED_PROCESS 0x00000008
#define EXTENDED_STARTUPINFO_PRESENT 0x00080000

/* lpCommandLine is writable (CreateProcessW may change it in place),
 * NUL-terminated, at most 32,767 characters. */
/*@ requires app: lpApplicationName == \null || valid_read_wstring(lpApplicationName);
    requires cmd: lpCommandLine == \null || valid_wstring(lpCommandLine);
    requires cmd_len: lpCommandLine != \null ==> wcslen(lpCommandLine) < 32767;
    requires psa: lpProcessAttributes == \null || \valid_read(lpProcessAttributes);
    requires tsa: lpThreadAttributes == \null || \valid_read(lpThreadAttributes);
    requires dir: lpCurrentDirectory == \null || valid_read_wstring(lpCurrentDirectory);
    requires si: \valid_read(lpStartupInfo) && \initialized(&lpStartupInfo->cb);
    requires si_ex: (dwCreationFlags & EXTENDED_STARTUPINFO_PRESENT) != 0 ==>
                    \valid_read((STARTUPINFOEXW *)lpStartupInfo);
    requires pi: \valid(lpProcessInformation);
    assigns __fc_win_state \from __fc_win_state, lpApplicationName[0 ..], lpCommandLine[0 ..], dwCreationFlags,
                                 bInheritHandles, *lpStartupInfo, lpEnvironment;
    assigns lpCommandLine[0 ..] \from lpCommandLine[0 ..];
    assigns *lpProcessInformation \from __fc_win_state;
    assigns \result \from __fc_win_state; */
BOOL CreateProcessW(LPCWSTR lpApplicationName, LPWSTR lpCommandLine, LPSECURITY_ATTRIBUTES lpProcessAttributes,
                    LPSECURITY_ATTRIBUTES lpThreadAttributes, BOOL bInheritHandles, DWORD dwCreationFlags,
                    LPVOID lpEnvironment, LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                    LPPROCESS_INFORMATION lpProcessInformation);

/*@ requires app: lpApplicationName == \null || valid_read_wstring(lpApplicationName);
    requires cmd: lpCommandLine == \null || valid_wstring(lpCommandLine);
    requires cmd_len: lpCommandLine != \null ==> wcslen(lpCommandLine) < 32767;
    requires psa: lpProcessAttributes == \null || \valid_read(lpProcessAttributes);
    requires tsa: lpThreadAttributes == \null || \valid_read(lpThreadAttributes);
    requires dir: lpCurrentDirectory == \null || valid_read_wstring(lpCurrentDirectory);
    requires si: \valid_read(lpStartupInfo) && \initialized(&lpStartupInfo->cb);
    requires si_ex: (dwCreationFlags & EXTENDED_STARTUPINFO_PRESENT) != 0 ==>
                    \valid_read((STARTUPINFOEXW *)lpStartupInfo);
    requires pi: \valid(lpProcessInformation);
    assigns __fc_win_state \from __fc_win_state, hToken, lpApplicationName[0 ..], lpCommandLine[0 ..],
                                 dwCreationFlags, bInheritHandles, *lpStartupInfo, lpEnvironment;
    assigns lpCommandLine[0 ..] \from lpCommandLine[0 ..];
    assigns *lpProcessInformation \from __fc_win_state;
    assigns \result \from __fc_win_state; */
BOOL CreateProcessAsUserW(HANDLE hToken, LPCWSTR lpApplicationName, LPWSTR lpCommandLine,
                          LPSECURITY_ATTRIBUTES lpProcessAttributes, LPSECURITY_ATTRIBUTES lpThreadAttributes,
                          BOOL bInheritHandles, DWORD dwCreationFlags, LPVOID lpEnvironment,
                          LPCWSTR lpCurrentDirectory, LPSTARTUPINFOW lpStartupInfo,
                          LPPROCESS_INFORMATION lpProcessInformation);

#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002ull
#define PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY 0x00020007ull
#define PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES 0x00020009ull
#define PROC_THREAD_ATTRIBUTE_ALL_APPLICATION_PACKAGES_POLICY 0x0002000Full
#define PROCESS_CREATION_ALL_APPLICATION_PACKAGES_OPT_OUT 0x01
#define PROCESS_CREATION_MITIGATION_POLICY_HEAP_TERMINATE_ALWAYS_ON (1ull << 12)
#define PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON (1ull << 16)
#define PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON (1ull << 20)
#define PROCESS_CREATION_MITIGATION_POLICY_STRICT_HANDLE_CHECKS_ALWAYS_ON (1ull << 24)
#define PROCESS_CREATION_MITIGATION_POLICY_WIN32K_SYSTEM_CALL_DISABLE_ALWAYS_ON (1ull << 28)
#define PROCESS_CREATION_MITIGATION_POLICY_EXTENSION_POINT_DISABLE_ALWAYS_ON (1ull << 32)
#define PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON (1ull << 36)
#define PROCESS_CREATION_MITIGATION_POLICY_FONT_DISABLE_ALWAYS_ON (1ull << 48)
#define PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_REMOTE_ALWAYS_ON (1ull << 52)
#define PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_NO_LOW_LABEL_ALWAYS_ON (1ull << 56)
#define PROCESS_CREATION_MITIGATION_POLICY_IMAGE_LOAD_PREFER_SYSTEM32_ALWAYS_ON (1ull << 60)

/* With no list, it tells the size one needs (and fails).  With a list, it
 * needs *lpSize bytes there; the size a list of that many attributes needs
 * is FC_ATTR_LIST_BYTES, the most this model ever answers. */
#define FC_ATTR_LIST_BYTES(n) (48 + 32 * (n))
/*@ requires size: \valid(lpSize);
    requires count: 1 <= dwAttributeCount <= 16;
    requires room: lpAttributeList != \null ==> \valid((char *)lpAttributeList + (0 .. *lpSize - 1));
    assigns *lpSize \from dwAttributeCount, *lpSize;
    assigns ((char *)lpAttributeList)[0 .. *lpSize - 1] \from dwAttributeCount;
    assigns \result \from __fc_win_state, *lpSize, dwAttributeCount, lpAttributeList;
    behavior query:
      assumes lpAttributeList == \null;
      assigns *lpSize \from dwAttributeCount;
      assigns \result \from \nothing;
      ensures needed: *lpSize == FC_ATTR_LIST_BYTES(dwAttributeCount);
      ensures fails: \result == FALSE;
    behavior init:
      assumes lpAttributeList != \null;
      assigns ((char *)lpAttributeList)[0 .. *lpSize - 1] \from dwAttributeCount;
      assigns *lpSize \from *lpSize, dwAttributeCount;
      assigns \result \from __fc_win_state, *lpSize, dwAttributeCount;
      ensures enough: \result != FALSE ==> \old(*lpSize) >= FC_ATTR_LIST_BYTES(dwAttributeCount);
    complete behaviors;
    disjoint behaviors; */
BOOL InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwAttributeCount,
                                       DWORD dwFlags, PSIZE_T lpSize);

/* lpValue must stay valid until the list is deleted (it is read by
 * CreateProcess, not copied): not checked here. */
/*@ requires list: lpAttributeList != \null;
    requires value: \valid_read((char *)lpValue + (0 .. cbSize - 1));
    requires value_set: \initialized((char *)lpValue + (0 .. cbSize - 1));
    requires prev: lpPreviousValue == \null;
    requires ret: lpReturnSize == \null;
    assigns __fc_win_state \from __fc_win_state, Attribute, ((char *)lpValue)[0 .. cbSize - 1];
    assigns \result \from __fc_win_state, Attribute, cbSize; */
BOOL UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList, DWORD dwFlags, DWORD_PTR Attribute,
                               PVOID lpValue, SIZE_T cbSize, PVOID lpPreviousValue, PSIZE_T lpReturnSize);

/*@ requires list: lpAttributeList != \null;
    assigns \nothing; */
void DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList);

typedef enum _JOBOBJECTINFOCLASS {
    JobObjectBasicUIRestrictions = 4,
    JobObjectExtendedLimitInformation = 9,
} JOBOBJECTINFOCLASS;

typedef struct _IO_COUNTERS {
    ULONGLONG ReadOperationCount, WriteOperationCount, OtherOperationCount;
    ULONGLONG ReadTransferCount, WriteTransferCount, OtherTransferCount;
} IO_COUNTERS;

typedef struct _JOBOBJECT_BASIC_LIMIT_INFORMATION {
    LARGE_INTEGER PerProcessUserTimeLimit;
    LARGE_INTEGER PerJobUserTimeLimit;
    DWORD LimitFlags;
    SIZE_T MinimumWorkingSetSize;
    SIZE_T MaximumWorkingSetSize;
    DWORD ActiveProcessLimit;
    ULONG_PTR Affinity;
    DWORD PriorityClass;
    DWORD SchedulingClass;
} JOBOBJECT_BASIC_LIMIT_INFORMATION;

typedef struct _JOBOBJECT_EXTENDED_LIMIT_INFORMATION {
    JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
    IO_COUNTERS IoInfo;
    SIZE_T ProcessMemoryLimit;
    SIZE_T JobMemoryLimit;
    SIZE_T PeakProcessMemoryUsed;
    SIZE_T PeakJobMemoryUsed;
} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;

typedef struct _JOBOBJECT_BASIC_UI_RESTRICTIONS {
    DWORD UIRestrictionsClass;
} JOBOBJECT_BASIC_UI_RESTRICTIONS;

#define JOB_OBJECT_LIMIT_PROCESS_MEMORY 0x00000100
#define JOB_OBJECT_LIMIT_ACTIVE_PROCESS 0x00000008
#define JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION 0x00000400
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 0x00002000
#define JOB_OBJECT_UILIMIT_HANDLES 0x00000001
#define JOB_OBJECT_UILIMIT_READCLIPBOARD 0x00000002
#define JOB_OBJECT_UILIMIT_WRITECLIPBOARD 0x00000004
#define JOB_OBJECT_UILIMIT_SYSTEMPARAMETERS 0x00000008
#define JOB_OBJECT_UILIMIT_DISPLAYSETTINGS 0x00000010
#define JOB_OBJECT_UILIMIT_GLOBALATOMS 0x00000020
#define JOB_OBJECT_UILIMIT_DESKTOP 0x00000040
#define JOB_OBJECT_UILIMIT_EXITWINDOWS 0x00000080

/*@ requires sa: lpJobAttributes == \null || \valid_read(lpJobAttributes);
    requires name: lpName == \null || valid_read_wstring(lpName);
    assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
HANDLE CreateJobObjectW(LPSECURITY_ATTRIBUTES lpJobAttributes, LPCWSTR lpName);

/*@ requires info: \valid_read((char *)lpJobObjectInformation + (0 .. cbJobObjectInformationLength - 1));
    requires info_set: \initialized((char *)lpJobObjectInformation + (0 .. cbJobObjectInformationLength - 1));
    requires size: JobObjectInformationClass == JobObjectExtendedLimitInformation ==>
                   cbJobObjectInformationLength == sizeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION);
    requires size_ui: JobObjectInformationClass == JobObjectBasicUIRestrictions ==>
                      cbJobObjectInformationLength == sizeof(JOBOBJECT_BASIC_UI_RESTRICTIONS);
    assigns __fc_win_state \from __fc_win_state, hJob, JobObjectInformationClass,
                                 ((char *)lpJobObjectInformation)[0 .. cbJobObjectInformationLength - 1];
    assigns \result \from __fc_win_state; */
BOOL SetInformationJobObject(HANDLE hJob, JOBOBJECTINFOCLASS JobObjectInformationClass,
                             LPVOID lpJobObjectInformation, DWORD cbJobObjectInformationLength);

/*@ assigns __fc_win_state \from __fc_win_state, hJob, hProcess;
    assigns \result \from __fc_win_state; */
BOOL AssignProcessToJobObject(HANDLE hJob, HANDLE hProcess);

/* ---- modules, environment ------------------------------------------------------ */

/* Returns the length copied (less than nSize, NUL-terminated), or nSize
 * when the name was cut short (then terminated at nSize - 1), or 0. */
/*@ requires buf: \valid(lpFilename + (0 .. nSize - 1));
    requires size: nSize > 0;
    assigns lpFilename[0 .. nSize - 1] \from __fc_win_state, hModule;
    assigns \result \from __fc_win_state, hModule, nSize;
    ensures range: 0 <= \result <= nSize;
    ensures terminated: \result < nSize ==> lpFilename[\result] == 0;
    ensures terminated_cut: \result == nSize ==> lpFilename[nSize - 1] == 0;
    ensures string: valid_read_wstring(lpFilename); */
DWORD GetModuleFileNameW(HMODULE hModule, LPWSTR lpFilename, DWORD nSize);

/*@ requires name: lpModuleName == \null || valid_read_wstring(lpModuleName);
    assigns \result \from __fc_win_state, lpModuleName[0 ..]; */
HMODULE GetModuleHandleW(LPCWSTR lpModuleName);

/* A function of the module, or NULL: what it is is the caller's cast. */
/*@ requires name: valid_read_string(lpProcName);
    assigns \result \from __fc_win_state, hModule, lpProcName[0 ..]; */
FARPROC GetProcAddress(HMODULE hModule, LPCSTR lpProcName);

/* hMem: NULL, or memory Windows allocated with LocalAlloc for us (by the
 * calls documented to need LocalFree).  The model allocates such memory
 * with malloc, so free's rules apply. */
/*@ requires mem: hMem == \null || \freeable(hMem);
    frees hMem;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns \result \from \nothing;
    ensures freed: \result == \null; */
HANDLE LocalFree(HANDLE hMem);

/* Returns 0 (none), the length copied (< nSize, terminated), or the size
 * needed (>= nSize, buffer unchanged, which the shims check). */
/*@ requires name: valid_read_wstring(lpName);
    requires buf: nSize > 0 ==> \valid(lpBuffer + (0 .. nSize - 1));
    assigns lpBuffer[0 .. nSize - 1] \from __fc_win_state, lpName[0 ..];
    assigns \result \from __fc_win_state, lpName[0 ..], nSize;
    ensures fits: 0 < \result < nSize ==> lpBuffer[\result] == 0;
    ensures fits_string: 0 < \result < nSize ==> valid_read_wstring(lpBuffer); */
DWORD GetEnvironmentVariableW(LPCWSTR lpName, LPWSTR lpBuffer, DWORD nSize);

/*@ requires name: valid_read_wstring(lpName);
    requires value: lpValue == \null || valid_read_wstring(lpValue);
    assigns __fc_win_state \from __fc_win_state, lpName[0 ..], lpValue[0 ..];
    assigns \result \from __fc_win_state; */
BOOL SetEnvironmentVariableW(LPCWSTR lpName, LPCWSTR lpValue);

/* ---- files ------------------------------------------------------------------- */

#define FILE_SHARE_READ 0x00000001
#define FILE_SHARE_WRITE 0x00000002
#define FILE_SHARE_DELETE 0x00000004
#define CREATE_NEW 1
#define CREATE_ALWAYS 2
#define OPEN_EXISTING 3
#define OPEN_ALWAYS 4
#define FILE_ATTRIBUTE_READONLY 0x00000001
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010
#define FILE_ATTRIBUTE_NORMAL 0x00000080
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000
#define FILE_NAME_NORMALIZED 0x0
#define VOLUME_NAME_DOS 0x0
#define MOVEFILE_REPLACE_EXISTING 0x00000001
#define MOVEFILE_WRITE_THROUGH 0x00000008

/* A path longer than MAX_PATH - 1 without the \\?\ prefix fails; not
 * undefined either way. */
/*@ requires name: valid_read_wstring(lpFileName);
    requires sa: lpSecurityAttributes == \null || \valid_read(lpSecurityAttributes);
    assigns __fc_win_state \from __fc_win_state, lpFileName[0 ..], dwDesiredAccess, dwShareMode,
                                 dwCreationDisposition, dwFlagsAndAttributes;
    assigns \result \from __fc_win_state, lpFileName[0 ..], dwCreationDisposition; */
HANDLE CreateFileW(LPCWSTR lpFileName, DWORD dwDesiredAccess, DWORD dwShareMode,
                   LPSECURITY_ATTRIBUTES lpSecurityAttributes, DWORD dwCreationDisposition,
                   DWORD dwFlagsAndAttributes, HANDLE hTemplateFile);

/*@ requires name: valid_read_wstring(lpPathName);
    requires sa: lpSecurityAttributes == \null || \valid_read(lpSecurityAttributes);
    assigns __fc_win_state \from __fc_win_state, lpPathName[0 ..];
    assigns \result \from __fc_win_state, lpPathName[0 ..]; */
BOOL CreateDirectoryW(LPCWSTR lpPathName, LPSECURITY_ATTRIBUTES lpSecurityAttributes);

/*@ requires name: valid_read_wstring(lpFileName);
    assigns __fc_win_state \from __fc_win_state, lpFileName[0 ..];
    assigns \result \from __fc_win_state, lpFileName[0 ..]; */
BOOL DeleteFileW(LPCWSTR lpFileName);

/*@ requires from: valid_read_wstring(lpExistingFileName);
    requires to: lpNewFileName == \null || valid_read_wstring(lpNewFileName);
    assigns __fc_win_state \from __fc_win_state, lpExistingFileName[0 ..], lpNewFileName[0 ..], dwFlags;
    assigns \result \from __fc_win_state; */
BOOL MoveFileExW(LPCWSTR lpExistingFileName, LPCWSTR lpNewFileName, DWORD dwFlags);

typedef struct _OVERLAPPED *LPOVERLAPPED;

/* Writes at most nNumberOfBytesToRead bytes, and says how many in *read. */
/*@ requires buf: \valid((char *)lpBuffer + (0 .. nNumberOfBytesToRead - 1));
    requires count: lpNumberOfBytesRead != \null && \valid(lpNumberOfBytesRead);
    requires sync: lpOverlapped == \null;
    assigns ((char *)lpBuffer)[0 .. nNumberOfBytesToRead - 1] \from __fc_win_state, hFile;
    assigns *lpNumberOfBytesRead \from __fc_win_state, hFile, nNumberOfBytesToRead;
    assigns __fc_win_state \from __fc_win_state, hFile, nNumberOfBytesToRead;
    assigns \result \from __fc_win_state;
    ensures read: *lpNumberOfBytesRead <= nNumberOfBytesToRead; */
BOOL ReadFile(HANDLE hFile, LPVOID lpBuffer, DWORD nNumberOfBytesToRead, LPDWORD lpNumberOfBytesRead,
              LPOVERLAPPED lpOverlapped);

/*@ requires buf: \valid_read((char *)lpBuffer + (0 .. nNumberOfBytesToWrite - 1));
    requires buf_set: \initialized((char *)lpBuffer + (0 .. nNumberOfBytesToWrite - 1));
    requires count: lpNumberOfBytesWritten != \null && \valid(lpNumberOfBytesWritten);
    requires sync: lpOverlapped == \null;
    assigns *lpNumberOfBytesWritten \from __fc_win_state, hFile, nNumberOfBytesToWrite;
    assigns __fc_win_state \from __fc_win_state, hFile, ((char *)lpBuffer)[0 .. nNumberOfBytesToWrite - 1];
    assigns \result \from __fc_win_state;
    ensures written: *lpNumberOfBytesWritten <= nNumberOfBytesToWrite; */
BOOL WriteFile(HANDLE hFile, LPCVOID lpBuffer, DWORD nNumberOfBytesToWrite, LPDWORD lpNumberOfBytesWritten,
               LPOVERLAPPED lpOverlapped);

/*@ requires name: valid_read_wstring(lpFileName);
    assigns \result \from __fc_win_state, lpFileName[0 ..]; */
DWORD GetFileAttributesW(LPCWSTR lpFileName);

/*@ requires name: valid_read_string(lpFileName);
    assigns \result \from __fc_win_state, lpFileName[0 ..]; */
DWORD GetFileAttributesA(LPCSTR lpFileName);

typedef enum _GET_FILEEX_INFO_LEVELS { GetFileExInfoStandard, GetFileExMaxInfoLevel } GET_FILEEX_INFO_LEVELS;
typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

/*@ requires name: valid_read_wstring(lpFileName);
    requires level: fInfoLevelId == GetFileExInfoStandard;
    requires out: \valid((WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation);
    assigns *(WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation \from __fc_win_state, lpFileName[0 ..];
    assigns \result \from __fc_win_state, lpFileName[0 ..]; */
BOOL GetFileAttributesExW(LPCWSTR lpFileName, GET_FILEEX_INFO_LEVELS fInfoLevelId, LPVOID lpFileInformation);

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow;
} BY_HANDLE_FILE_INFORMATION, *LPBY_HANDLE_FILE_INFORMATION;

/*@ requires out: \valid(lpFileInformation);
    assigns *lpFileInformation \from __fc_win_state, hFile;
    assigns \result \from __fc_win_state, hFile; */
BOOL GetFileInformationByHandle(HANDLE hFile, LPBY_HANDLE_FILE_INFORMATION lpFileInformation);

/*@ requires out: \valid(lpFileSize);
    assigns *lpFileSize \from __fc_win_state, hFile;
    assigns \result \from __fc_win_state, hFile;
    ensures size: \result != 0 ==> lpFileSize->QuadPart >= 0; */
BOOL GetFileSizeEx(HANDLE hFile, PLARGE_INTEGER lpFileSize);

/* Returns the length (< cchFilePath, terminated), the size needed
 * (>= cchFilePath: buffer unspecified), or 0. */
/*@ requires buf: \valid(lpszFilePath + (0 .. cchFilePath - 1));
    assigns lpszFilePath[0 .. cchFilePath - 1] \from __fc_win_state, hFile;
    assigns \result \from __fc_win_state, hFile, cchFilePath;
    ensures fits: 0 < \result < cchFilePath ==> lpszFilePath[\result] == 0;
    ensures fits_string: 0 < \result < cchFilePath ==> valid_read_wstring(lpszFilePath); */
DWORD GetFinalPathNameByHandleW(HANDLE hFile, LPWSTR lpszFilePath, DWORD cchFilePath, DWORD dwFlags);

typedef struct _WIN32_FIND_DATAW {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
    WCHAR cFileName[MAX_PATH];
    WCHAR cAlternateFileName[14];
} WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

/* The names it returns are terminated within their arrays. */
/*@ requires name: valid_read_wstring(lpFileName);
    requires out: \valid(lpFindFileData);
    assigns *lpFindFileData \from __fc_win_state, lpFileName[0 ..];
    assigns \result \from __fc_win_state, lpFileName[0 ..];
    ensures names: \result != INVALID_HANDLE_VALUE ==>
                   (\exists integer i; 0 <= i < MAX_PATH && lpFindFileData->cFileName[i] == 0); */
HANDLE FindFirstFileW(LPCWSTR lpFileName, LPWIN32_FIND_DATAW lpFindFileData);

/*@ requires out: \valid(lpFindFileData);
    assigns *lpFindFileData \from __fc_win_state, hFindFile;
    assigns __fc_win_state \from __fc_win_state, hFindFile;
    assigns \result \from __fc_win_state, hFindFile;
    ensures names: \result != 0 ==> (\exists integer i; 0 <= i < MAX_PATH && lpFindFileData->cFileName[i] == 0); */
BOOL FindNextFileW(HANDLE hFindFile, LPWIN32_FIND_DATAW lpFindFileData);

/*@ assigns __fc_win_state \from __fc_win_state, hFindFile;
    assigns \result \from __fc_win_state; */
BOOL FindClose(HANDLE hFindFile);

/* ---- text -------------------------------------------------------------------- */

#define CP_ACP 0
#define CP_UTF8 65001
#define MB_ERR_INVALID_CHARS 0x00000008

/* cbMultiByte -1: the input is a NUL-terminated string, and the output
 * (and the count) include the terminator.  cchWideChar 0: a size query.
 * Otherwise writes at most cchWideChar characters and returns how many
 * (0 on failure, including not enough room). */
/*@ requires in_string: cbMultiByte == -1 ==> valid_read_string(lpMultiByteStr);
    requires in_bytes: cbMultiByte > 0 ==> \valid_read(lpMultiByteStr + (0 .. cbMultiByte - 1));
    requires in_size: cbMultiByte == -1 || cbMultiByte > 0;
    requires out_size: cchWideChar >= 0;
    requires out: cchWideChar > 0 ==> \valid(lpWideCharStr + (0 .. cchWideChar - 1));
    assigns \result \from CodePage, dwFlags, lpMultiByteStr[0 ..], cbMultiByte, cchWideChar;
    assigns lpWideCharStr[0 .. cchWideChar - 1] \from CodePage, dwFlags, lpMultiByteStr[0 ..], cbMultiByte;
    ensures range: \result >= 0;
    ensures fits: cchWideChar > 0 ==> \result <= cchWideChar;
    ensures terminated: cbMultiByte == -1 && cchWideChar > 0 && \result > 0 ==> lpWideCharStr[\result - 1] == 0; */
int MultiByteToWideChar(UINT CodePage, DWORD dwFlags, LPCSTR lpMultiByteStr, int cbMultiByte,
                        LPWSTR lpWideCharStr, int cchWideChar);

/*@ requires in_string: cchWideChar == -1 ==> valid_read_wstring(lpWideCharStr);
    requires in_chars: cchWideChar > 0 ==> \valid_read(lpWideCharStr + (0 .. cchWideChar - 1));
    requires in_size: cchWideChar == -1 || cchWideChar > 0;
    requires out_size: cbMultiByte >= 0;
    requires out: cbMultiByte > 0 ==> \valid(lpMultiByteStr + (0 .. cbMultiByte - 1));
    requires utf8: lpDefaultChar == \null && lpUsedDefaultChar == \null;
    assigns \result \from CodePage, dwFlags, lpWideCharStr[0 ..], cchWideChar, cbMultiByte;
    assigns lpMultiByteStr[0 .. cbMultiByte - 1] \from CodePage, dwFlags, lpWideCharStr[0 ..], cchWideChar;
    ensures range: \result >= 0;
    ensures fits: cbMultiByte > 0 ==> \result <= cbMultiByte;
    ensures terminated: cchWideChar == -1 && cbMultiByte > 0 && \result > 0 ==> lpMultiByteStr[\result - 1] == 0; */
int WideCharToMultiByte(UINT CodePage, DWORD dwFlags, LPCWSTR lpWideCharStr, int cchWideChar,
                        LPSTR lpMultiByteStr, int cbMultiByte, LPCSTR lpDefaultChar, BOOL *lpUsedDefaultChar);

/* ---- the C runtime's Microsoft functions -------------------------------------------- */

/* _snwprintf (msvcrt): formats at most count characters into buffer and
 * returns how many, NUL-terminated only if there was room for the NUL; if
 * the text didn't fit, returns -1 with buffer filled and NOT terminated.
 * %ls reads a wide string, %hs a narrow one (win_models.c checks both). */
/*@ requires room: count > 0 && \valid(buffer + (0 .. count - 1));
    requires format: valid_read_wstring(format); */
int __fc_snwprintf(wchar_t *buffer, size_t count, const wchar_t *format, ...);
#define _snwprintf __fc_snwprintf
#define snwprintf __fc_snwprintf

/* fwprintf to stderr: its arguments are read (win_models.c). */
int __fc_fwprintf(FILE *stream, const wchar_t *format, ...);
#define fwprintf __fc_fwprintf

/* Declared for headers that wrap them (win32_compat.h); the shims don't
 * call them.  MinGW has no gmtime_r or localtime_r (win32_compat.h makes
 * them); Frama-C's POSIX time.h does: renamed here so both can be. */
#include <time.h>
#define gmtime_r fc_win_gmtime_r
#define localtime_r fc_win_localtime_r
struct tm;
int gmtime_s(struct tm *tm, const long long *t);
int localtime_s(struct tm *tm, const long long *t);
long long _mkgmtime(struct tm *tm);
int _mkdir(const char *path);
int _access(const char *path, int mode);

/*@ requires a: valid_read_nwstring(s1, n) || valid_read_wstring(s1);
    requires b: valid_read_nwstring(s2, n) || valid_read_wstring(s2);
    assigns \result \from s1[0 .. n - 1], s2[0 .. n - 1], n; */
int _wcsnicmp(const wchar_t *s1, const wchar_t *s2, size_t n);

/*@ requires a: valid_read_nstring((char *)s1, n) || valid_read_string(s1);
    requires b: valid_read_nstring((char *)s2, n) || valid_read_string(s2);
    assigns \result \from s1[0 .. n - 1], s2[0 .. n - 1], n; */
int _strnicmp(const char *s1, const char *s2, size_t n);

/*@ requires a: valid_read_string(s1);
    requires b: valid_read_string(s2);
    assigns \result \from s1[0 ..], s2[0 ..]; */
int _stricmp(const char *s1, const char *s2);

/* ---- synchronization --------------------------------------------------------------- */

typedef struct _RTL_CRITICAL_SECTION {
    PVOID DebugInfo;
    LONG LockCount, RecursionCount;
    HANDLE OwningThread, LockSemaphore;
    ULONG_PTR SpinCount;
} CRITICAL_SECTION, *LPCRITICAL_SECTION, *PCRITICAL_SECTION;
typedef struct _RTL_SRWLOCK { PVOID Ptr; } SRWLOCK, *PSRWLOCK;
typedef struct _RTL_CONDITION_VARIABLE { PVOID Ptr; } CONDITION_VARIABLE, *PCONDITION_VARIABLE;

/* A lock's memory must be valid; its use from several threads isn't
 * modeled (Eva has one thread). */
/*@ requires \valid(cs);
    assigns *cs \from \nothing;
    ensures \initialized(cs); */
void InitializeCriticalSection(LPCRITICAL_SECTION cs);
/*@ requires \valid(cs) && \initialized(cs);
    assigns *cs \from *cs; */
void DeleteCriticalSection(LPCRITICAL_SECTION cs);
/*@ requires \valid(cs) && \initialized(cs);
    assigns *cs \from *cs; */
void EnterCriticalSection(LPCRITICAL_SECTION cs);
/*@ requires \valid(cs) && \initialized(cs);
    assigns *cs \from *cs; */
void LeaveCriticalSection(LPCRITICAL_SECTION cs);
/*@ requires \valid(l);
    assigns *l \from \nothing;
    ensures \initialized(l); */
void InitializeSRWLock(PSRWLOCK l);
/*@ requires \valid(l) && \initialized(l);
    assigns *l \from *l; */
void AcquireSRWLockShared(PSRWLOCK l);
/*@ requires \valid(l) && \initialized(l);
    assigns *l \from *l; */
void ReleaseSRWLockShared(PSRWLOCK l);
/*@ requires \valid(l) && \initialized(l);
    assigns *l \from *l; */
void AcquireSRWLockExclusive(PSRWLOCK l);
/*@ requires \valid(l) && \initialized(l);
    assigns *l \from *l; */
void ReleaseSRWLockExclusive(PSRWLOCK l);
/*@ requires \valid(cv);
    assigns *cv \from \nothing;
    ensures \initialized(cv); */
void InitializeConditionVariable(PCONDITION_VARIABLE cv);
/*@ requires \valid(cv) && \initialized(cv);
    requires \valid(cs) && \initialized(cs);
    assigns *cv \from *cv;
    assigns *cs \from *cs;
    assigns __fc_win_state \from __fc_win_state, dwMilliseconds;
    assigns \result \from __fc_win_state; */
BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD dwMilliseconds);
/*@ requires \valid(cv) && \initialized(cv);
    assigns *cv \from *cv; */
void WakeConditionVariable(PCONDITION_VARIABLE cv);
/*@ requires \valid(cv) && \initialized(cv);
    assigns *cv \from *cv; */
void WakeAllConditionVariable(PCONDITION_VARIABLE cv);

/* Atomic, and wrapping (the hardware's add): no overflow. */
/*@ requires \valid(p);
    assigns *p \from *p;
    assigns \result \from *p;
    ensures \result == *p; */
LONG InterlockedIncrement(volatile LONG *p);
/*@ requires \valid(p);
    assigns *p \from *p;
    assigns \result \from *p;
    ensures \result == *p; */
LONG InterlockedDecrement(volatile LONG *p);
/*@ requires \valid(p);
    assigns *p \from v;
    assigns \result \from *p;
    ensures *p == v;
    ensures \result == \old(*p); */
LONG InterlockedExchange(volatile LONG *p, LONG v);

/* ---- security ------------------------------------------------------------------ */

typedef struct _SID_IDENTIFIER_AUTHORITY { BYTE Value[6]; } SID_IDENTIFIER_AUTHORITY;
typedef struct _SID {
    BYTE Revision;
    BYTE SubAuthorityCount;
    SID_IDENTIFIER_AUTHORITY IdentifierAuthority;
    DWORD SubAuthority[1];
} SID;
typedef struct _SID_AND_ATTRIBUTES { PSID Sid; DWORD Attributes; } SID_AND_ATTRIBUTES, *PSID_AND_ATTRIBUTES;
typedef struct _ACL { BYTE AclRevision, Sbz1; WORD AclSize, AceCount, Sbz2; } ACL, *PACL;
typedef struct _TOKEN_USER { SID_AND_ATTRIBUTES User; } TOKEN_USER;
typedef struct _TOKEN_MANDATORY_LABEL { SID_AND_ATTRIBUTES Label; } TOKEN_MANDATORY_LABEL;
typedef struct _TOKEN_DEFAULT_DACL { PACL DefaultDacl; } TOKEN_DEFAULT_DACL;
typedef struct _SECURITY_CAPABILITIES {
    PSID AppContainerSid;
    PSID_AND_ATTRIBUTES Capabilities;
    DWORD CapabilityCount;
    DWORD Reserved;
} SECURITY_CAPABILITIES;

#define SECURITY_MAX_SID_SIZE 68
#define ACL_REVISION 2
#define SE_GROUP_ENABLED 0x00000004L
#define SE_GROUP_INTEGRITY 0x00000020L
#define DISABLE_MAX_PRIVILEGE 0x1
#define TOKEN_ASSIGN_PRIMARY 0x0001
#define TOKEN_DUPLICATE 0x0002
#define TOKEN_QUERY 0x0008
#define TOKEN_ADJUST_DEFAULT 0x0080
#define OWNER_SECURITY_INFORMATION 0x00000001L
#define GROUP_SECURITY_INFORMATION 0x00000002L
#define DACL_SECURITY_INFORMATION 0x00000004L
#define SACL_SECURITY_INFORMATION 0x00000008L
#define LABEL_SECURITY_INFORMATION 0x00000010L
#define SDDL_REVISION_1 1

typedef enum _TOKEN_INFORMATION_CLASS {
    TokenUser = 1,
    TokenDefaultDacl = 6,
    TokenIntegrityLevel = 25,
} TOKEN_INFORMATION_CLASS;

typedef enum _WELL_KNOWN_SID_TYPE {
    WinWorldSid = 1,
    WinRestrictedCodeSid = 11,
    WinAuthenticatedUserSid = 17,
    WinLocalSystemSid = 22,
    WinBuiltinAdministratorsSid = 26,
    WinBuiltinUsersSid = 27,
    WinLowLabelSid = 66,
    WinCapabilityInternetClientSid = 85,
    WinCapabilityPrivateNetworkClientServerSid = 87,
    WinBuiltinAnyPackageSid = 84,
} WELL_KNOWN_SID_TYPE;

typedef enum _SE_OBJECT_TYPE { SE_UNKNOWN_OBJECT_TYPE, SE_FILE_OBJECT, SE_SERVICE, SE_PRINTER,
                               SE_REGISTRY_KEY, SE_LMSHARE, SE_KERNEL_OBJECT } SE_OBJECT_TYPE;

/*@ requires out: \valid(TokenHandle);
    assigns *TokenHandle \from __fc_win_state, ProcessHandle, DesiredAccess;
    assigns __fc_win_state \from __fc_win_state, ProcessHandle, DesiredAccess;
    assigns \result \from __fc_win_state; */
BOOL OpenProcessToken(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);

/* Fills at most TokenInformationLength bytes; the SIDs it returns point
 * into that buffer. */
/*@ requires buf: \valid((char *)TokenInformation + (0 .. TokenInformationLength - 1));
    requires ret: \valid(ReturnLength);
    requires user: TokenInformationClass == TokenUser ==> TokenInformationLength >= sizeof(TOKEN_USER);
    assigns ((char *)TokenInformation)[0 .. TokenInformationLength - 1] \from __fc_win_state, TokenHandle,
                                                                          TokenInformationClass;
    assigns *ReturnLength \from __fc_win_state, TokenHandle, TokenInformationClass;
    assigns \result \from __fc_win_state; */
BOOL GetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
                         LPVOID TokenInformation, DWORD TokenInformationLength, PDWORD ReturnLength);

/*@ requires info: \valid_read((char *)TokenInformation + (0 .. TokenInformationLength - 1));
    requires info_set: \initialized((char *)TokenInformation + (0 .. TokenInformationLength - 1));
    assigns __fc_win_state \from __fc_win_state, TokenHandle, TokenInformationClass,
                                 ((char *)TokenInformation)[0 .. TokenInformationLength - 1];
    assigns \result \from __fc_win_state; */
BOOL SetTokenInformation(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass,
                         LPVOID TokenInformation, DWORD TokenInformationLength);

/* *StringSid: a string Windows allocated (LocalFree it). */
/*@ requires sid: Sid != \null;
    requires out: \valid(StringSid);
    allocates *StringSid;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns *StringSid \from __fc_heap_status, Sid;
    assigns \result \from __fc_win_state, Sid;
    ensures string: \result != 0 ==> valid_read_wstring(*StringSid) && \freeable(*StringSid); */
BOOL ConvertSidToStringSidW(PSID Sid, LPWSTR *StringSid);

/*@ requires sddl: valid_read_wstring(StringSecurityDescriptor);
    requires out: \valid(SecurityDescriptor);
    requires size: SecurityDescriptorSize == \null || \valid(SecurityDescriptorSize);
    allocates *SecurityDescriptor;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns *SecurityDescriptor \from __fc_heap_status, StringSecurityDescriptor[0 ..];
    assigns \result \from __fc_win_state, StringSecurityDescriptor[0 ..];
    ensures made: \result != 0 ==> \freeable(*SecurityDescriptor); */
BOOL ConvertStringSecurityDescriptorToSecurityDescriptorW(LPCWSTR StringSecurityDescriptor,
                                                         DWORD StringSDRevision,
                                                         PSECURITY_DESCRIPTOR *SecurityDescriptor,
                                                         PULONG SecurityDescriptorSize);

/* *pSacl points into the descriptor. */
/*@ requires sd: pSecurityDescriptor != \null;
    requires out: \valid(lpbSaclPresent) && \valid(pSacl) && \valid(lpbSaclDefaulted);
    assigns *lpbSaclPresent, *pSacl, *lpbSaclDefaulted \from pSecurityDescriptor;
    assigns \result \from pSecurityDescriptor; */
BOOL GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOL *lpbSaclPresent, PACL *pSacl,
                               BOOL *lpbSaclDefaulted);

/* Writes at most *cbSid bytes, and the size it needs in *cbSid. */
/*@ requires domain: DomainSid == \null;
    requires size: \valid(cbSid);
    requires out: \valid((char *)pSid + (0 .. *cbSid - 1));
    assigns ((char *)pSid)[0 .. *cbSid - 1] \from WellKnownSidType;
    assigns *cbSid \from WellKnownSidType, *cbSid;
    assigns \result \from WellKnownSidType, *cbSid; */
BOOL CreateWellKnownSid(WELL_KNOWN_SID_TYPE WellKnownSidType, PSID DomainSid, PSID pSid, DWORD *cbSid);

/*@ requires acl: \valid((char *)pAcl + (0 .. nAclLength - 1));
    requires size: nAclLength >= sizeof(ACL);
    assigns ((char *)pAcl)[0 .. nAclLength - 1] \from nAclLength, dwAclRevision;
    assigns \result \from nAclLength;
    ensures header: \result != 0 ==> \initialized(pAcl); */
BOOL InitializeAcl(PACL pAcl, DWORD nAclLength, DWORD dwAclRevision);

/* Appends within the ACL's own AclSize (it fails if there is no room). */
/*@ requires acl: \valid(pAcl) && \initialized(pAcl);
    requires room: \valid((char *)pAcl + (0 .. pAcl->AclSize - 1));
    requires sid: pSid != \null;
    assigns ((char *)pAcl)[0 .. pAcl->AclSize - 1] \from ((char *)pAcl)[0 .. pAcl->AclSize - 1], AccessMask,
                                                       indirect: pSid;
    assigns \result \from pAcl->AclSize, indirect: pSid; */
BOOL AddAccessAllowedAce(PACL pAcl, DWORD dwAceRevision, DWORD AccessMask, PSID pSid);

/*@ requires deny: DisableSidCount > 0 ==> \valid_read(SidsToDisable + (0 .. DisableSidCount - 1));
    requires privs: DeletePrivilegeCount == 0;
    requires restrict: RestrictedSidCount > 0 ==> \valid_read(SidsToRestrict + (0 .. RestrictedSidCount - 1));
    requires out: \valid(NewTokenHandle);
    assigns *NewTokenHandle \from __fc_win_state, ExistingTokenHandle;
    assigns __fc_win_state \from __fc_win_state, ExistingTokenHandle, Flags,
                                 SidsToDisable[0 .. DisableSidCount - 1], SidsToRestrict[0 .. RestrictedSidCount - 1];
    assigns \result \from __fc_win_state; */
BOOL CreateRestrictedToken(HANDLE ExistingTokenHandle, DWORD Flags, DWORD DisableSidCount,
                           PSID_AND_ATTRIBUTES SidsToDisable, DWORD DeletePrivilegeCount, void *PrivilegesToDelete,
                           DWORD RestrictedSidCount, PSID_AND_ATTRIBUTES SidsToRestrict, PHANDLE NewTokenHandle);

/*@ requires sid: \valid_read((SID *)pSid);
    assigns \result \from ((SID *)pSid)->SubAuthorityCount;
    ensures size: 8 <= \result <= SECURITY_MAX_SID_SIZE; */
DWORD GetLengthSid(PSID pSid);

/* A SID that AllocateAndInitializeSid or the AppContainer calls made. */
/*@ requires sid: pSid == \null || \freeable(pSid);
    frees pSid;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns \result \from \nothing; */
PVOID FreeSid(PSID pSid);

/*@ requires sd_ok: psidOwner == \null && psidGroup == \null && pDacl == \null;
    requires sacl: pSacl != \null;
    assigns __fc_win_state \from __fc_win_state, handle, ObjectType, SecurityInfo, indirect: pSacl;
    assigns \result \from __fc_win_state; */
DWORD SetSecurityInfo(HANDLE handle, SE_OBJECT_TYPE ObjectType, SECURITY_INFORMATION SecurityInfo, PSID psidOwner,
                      PSID psidGroup, PACL pDacl, PACL pSacl);

/* *ppDacl points into *ppSecurityDescriptor, which the caller LocalFrees. */
/*@ requires name: valid_read_wstring(pObjectName);
    requires dacl: \valid(ppDacl) && \valid(ppSecurityDescriptor);
    requires others: ppsidOwner == \null && ppsidGroup == \null && ppSacl == \null;
    allocates *ppSecurityDescriptor;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns *ppSecurityDescriptor \from __fc_heap_status, pObjectName[0 ..];
    assigns *ppDacl \from *ppSecurityDescriptor;
    assigns \result \from __fc_win_state, pObjectName[0 ..];
    ensures sd: \result == ERROR_SUCCESS ==> \freeable(*ppSecurityDescriptor);
    ensures no_sd: \result != ERROR_SUCCESS ==> *ppSecurityDescriptor == \old(*ppSecurityDescriptor) &&
                                                 *ppDacl == \old(*ppDacl); */
DWORD GetNamedSecurityInfoW(LPCWSTR pObjectName, SE_OBJECT_TYPE ObjectType, SECURITY_INFORMATION SecurityInfo,
                            PSID *ppsidOwner, PSID *ppsidGroup, PACL *ppDacl, PACL *ppSacl,
                            PSECURITY_DESCRIPTOR *ppSecurityDescriptor);

/*@ requires name: valid_read_wstring(pObjectName);
    assigns __fc_win_state \from __fc_win_state, pObjectName[0 ..], ObjectType, SecurityInfo, indirect: pDacl;
    assigns \result \from __fc_win_state; */
DWORD SetNamedSecurityInfoW(LPWSTR pObjectName, SE_OBJECT_TYPE ObjectType, SECURITY_INFORMATION SecurityInfo,
                            PSID psidOwner, PSID psidGroup, PACL pDacl, PACL pSacl);

typedef enum _ACCESS_MODE { NOT_USED_ACCESS, GRANT_ACCESS, SET_ACCESS, DENY_ACCESS, REVOKE_ACCESS } ACCESS_MODE;
typedef enum _MULTIPLE_TRUSTEE_OPERATION { NO_MULTIPLE_TRUSTEE } MULTIPLE_TRUSTEE_OPERATION;
typedef enum _TRUSTEE_FORM { TRUSTEE_IS_SID, TRUSTEE_IS_NAME } TRUSTEE_FORM;
typedef enum _TRUSTEE_TYPE { TRUSTEE_IS_UNKNOWN, TRUSTEE_IS_USER, TRUSTEE_IS_GROUP, TRUSTEE_IS_DOMAIN,
                             TRUSTEE_IS_ALIAS, TRUSTEE_IS_WELL_KNOWN_GROUP } TRUSTEE_TYPE;
typedef struct _TRUSTEE_W {
    struct _TRUSTEE_W *pMultipleTrustee;
    MULTIPLE_TRUSTEE_OPERATION MultipleTrusteeOperation;
    TRUSTEE_FORM TrusteeForm;
    TRUSTEE_TYPE TrusteeType;
    LPWSTR ptstrName;
} TRUSTEE_W;
typedef struct _EXPLICIT_ACCESS_W {
    DWORD grfAccessPermissions;
    ACCESS_MODE grfAccessMode;
    DWORD grfInheritance;
    TRUSTEE_W Trustee;
} EXPLICIT_ACCESSW, *PEXPLICIT_ACCESSW;
#define SUB_CONTAINERS_AND_OBJECTS_INHERIT 0x3

/* *NewAcl: allocated by Windows (LocalFree it). */
/*@ requires entries: \valid_read(pListOfExplicitEntries + (0 .. cCountOfExplicitEntries - 1));
    requires out: \valid(NewAcl);
    allocates *NewAcl;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns *NewAcl \from __fc_heap_status, pListOfExplicitEntries[0 .. cCountOfExplicitEntries - 1], OldAcl;
    assigns \result \from __fc_win_state;
    ensures made: \result == ERROR_SUCCESS ==> \freeable(*NewAcl);
    ensures not_made: \result != ERROR_SUCCESS ==> *NewAcl == \old(*NewAcl); */
DWORD SetEntriesInAclW(ULONG cCountOfExplicitEntries, PEXPLICIT_ACCESSW pListOfExplicitEntries, PACL OldAcl,
                       PACL *NewAcl);

/* *ppSidAppContainerSid: FreeSid it. */
/*@ requires name: valid_read_wstring(pszAppContainerName);
    requires display: valid_read_wstring(pszDisplayName);
    requires desc: valid_read_wstring(pszDescription);
    requires caps: dwCapabilityCount == 0;
    requires out: \valid(ppSidAppContainerSid);
    allocates *ppSidAppContainerSid;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns __fc_win_state \from __fc_win_state, pszAppContainerName[0 ..];
    assigns *ppSidAppContainerSid \from __fc_heap_status, pszAppContainerName[0 ..];
    assigns \result \from __fc_win_state, pszAppContainerName[0 ..];
    ensures made: SUCCEEDED(\result) ==> \freeable(*ppSidAppContainerSid);
    ensures not_made: FAILED(\result) ==> *ppSidAppContainerSid == \old(*ppSidAppContainerSid); */
HRESULT CreateAppContainerProfile(PCWSTR pszAppContainerName, PCWSTR pszDisplayName, PCWSTR pszDescription,
                                  PSID_AND_ATTRIBUTES pCapabilities, DWORD dwCapabilityCount,
                                  PSID *ppSidAppContainerSid);

/*@ requires name: valid_read_wstring(pszAppContainerName);
    requires out: \valid(ppsidAppContainerSid);
    allocates *ppsidAppContainerSid;
    assigns __fc_heap_status \from __fc_heap_status;
    assigns *ppsidAppContainerSid \from __fc_heap_status, pszAppContainerName[0 ..];
    assigns \result \from __fc_win_state, pszAppContainerName[0 ..];
    ensures made: SUCCEEDED(\result) ==> \freeable(*ppsidAppContainerSid);
    ensures not_made: FAILED(\result) ==> *ppsidAppContainerSid == \old(*ppsidAppContainerSid); */
HRESULT DeriveAppContainerSidFromAppContainerName(PCWSTR pszAppContainerName, PSID *ppsidAppContainerSid);

/*@ requires name: valid_read_wstring(pszAppContainerName);
    assigns __fc_win_state \from __fc_win_state, pszAppContainerName[0 ..];
    assigns \result \from __fc_win_state; */
HRESULT DeleteAppContainerProfile(PCWSTR pszAppContainerName);

typedef LONG NTSTATUS;
#define BCRYPT_USE_SYSTEM_PREFERRED_RNG 0x00000002
/*@ requires alg: hAlgorithm == \null;
    requires buf: \valid(pbBuffer + (0 .. cbBuffer - 1));
    assigns pbBuffer[0 .. cbBuffer - 1] \from __fc_win_state;
    assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
NTSTATUS BCryptGenRandom(void *hAlgorithm, PUCHAR pbBuffer, ULONG cbBuffer, ULONG dwFlags);

/* ---- the registry -------------------------------------------------------------- */

#define HKEY_CURRENT_USER ((HKEY)(ULONG_PTR)((LONG)0x80000001))
#define KEY_READ 0x20019
#define RRF_RT_REG_SZ 0x00000002
typedef LONG LSTATUS;

/*@ requires sub: lpSubKey == \null || valid_read_wstring(lpSubKey);
    requires out: \valid(phkResult);
    assigns *phkResult \from __fc_win_state, hKey, lpSubKey[0 ..];
    assigns \result \from __fc_win_state, hKey, lpSubKey[0 ..]; */
LSTATUS RegOpenKeyExW(HKEY hKey, LPCWSTR lpSubKey, DWORD ulOptions, DWORD samDesired, HKEY *phkResult);

/* *lpcchName: the room in characters; on success the length, without the
 * NUL that follows it. */
/*@ requires len: \valid(lpcchName) && *lpcchName > 0;
    requires name: \valid(lpName + (0 .. *lpcchName - 1));
    requires rest: lpReserved == \null && lpClass == \null && lpcchClass == \null && lpftLastWriteTime == \null;
    assigns lpName[0 .. *lpcchName - 1] \from __fc_win_state, hKey, dwIndex;
    assigns *lpcchName \from __fc_win_state, hKey, dwIndex, *lpcchName;
    assigns \result \from __fc_win_state, hKey, dwIndex, *lpcchName;
    ensures fits: \result == ERROR_SUCCESS ==> *lpcchName < \old(*lpcchName) && lpName[*lpcchName] == 0;
    ensures string: \result == ERROR_SUCCESS ==> valid_read_wstring(lpName); */
LSTATUS RegEnumKeyExW(HKEY hKey, DWORD dwIndex, LPWSTR lpName, LPDWORD lpcchName, LPDWORD lpReserved,
                      LPWSTR lpClass, LPDWORD lpcchClass, FILETIME *lpftLastWriteTime);

/* With RRF_RT_REG_SZ the value is NUL-terminated within *pcbData bytes. */
/*@ requires sub: lpSubKey == \null || valid_read_wstring(lpSubKey);
    requires value: lpValue == \null || valid_read_wstring(lpValue);
    requires type: pdwType == \null;
    requires size: \valid(pcbData);
    requires data: \valid((char *)pvData + (0 .. *pcbData - 1));
    assigns ((char *)pvData)[0 .. *pcbData - 1] \from __fc_win_state, hKey, lpSubKey[0 ..], lpValue[0 ..];
    assigns *pcbData \from __fc_win_state, hKey, lpSubKey[0 ..], lpValue[0 ..], *pcbData;
    assigns \result \from __fc_win_state, hKey, lpSubKey[0 ..], lpValue[0 ..], *pcbData;
    ensures fits: \result == ERROR_SUCCESS ==> *pcbData <= \old(*pcbData);
    ensures string: \result == ERROR_SUCCESS && dwFlags == RRF_RT_REG_SZ ==> valid_read_wstring((wchar_t *)pvData); */
LSTATUS RegGetValueW(HKEY hKey, LPCWSTR lpSubKey, LPCWSTR lpValue, DWORD dwFlags, LPDWORD pdwType, PVOID pvData,
                     LPDWORD pcbData);

/*@ assigns __fc_win_state \from __fc_win_state, hKey;
    assigns \result \from __fc_win_state; */
LSTATUS RegCloseKey(HKEY hKey);

#include "fc_winuser.h"
#include "fc_winsock.h"

#endif
