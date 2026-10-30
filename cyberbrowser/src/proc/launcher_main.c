/* launcher_main.c - cyberbrowser.exe, the browser a user opens: it starts the
 * coordinator (cb_coord.exe, from this executable's folder) with the same
 * arguments (start URLs), with no console window, and waits for it.  The
 * coordinator starts the rest (SANDBOX_DESIGN.md section 2). */
#include <windows.h>
#include <wchar.h>

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show) {
    (void)inst; (void)prev; (void)show;
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 1;
    wchar_t *slash = wcsrchr(exe, L'\\');
    if (!slash || (size_t)(slash + 1 - exe) + wcslen(L"cb_coord.exe") >= MAX_PATH) return 1;
    wcscpy(slash + 1, L"cb_coord.exe");

    /* "<exe>" and this program's own arguments, as they were given. */
    size_t len = wcslen(exe) + wcslen(args) + 4;
    wchar_t *cmd = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!cmd) return 1;
    _snwprintf(cmd, len, L"\"%ls\"%ls%ls", exe, args[0] ? L" " : L"", args);
    cmd[len - 1] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        wchar_t msg[MAX_PATH + 64];
        _snwprintf(msg, MAX_PATH + 64, L"Could not start %ls (error %lu).", exe, GetLastError());
        msg[MAX_PATH + 63] = 0;
        MessageBoxW(NULL, msg, L"CyberBrowser", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
