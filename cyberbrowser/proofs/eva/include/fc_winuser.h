/* fc_winuser.h - windows, painting and input (user32, gdi32), for Eva.
 * Part of windows.h's model: TRUSTED, see there.  Included by it only.
 *
 * Windows calls a window procedure back from DispatchMessageW (and from
 * CreateWindowExW, SendMessageW, DestroyWindow ...).  The model makes the
 * calls from DispatchMessageW alone (win_models.c): each window procedure
 * registered with RegisterClassW or SetWindowLongPtrW, with any message.
 * The others would be the same procedures with messages the UI sends
 * itself, nested; Eva can't follow the recursion, and those messages are a
 * subset of what DispatchMessageW already gives them. */
#ifndef FC_WINUSER_H
#define FC_WINUSER_H

typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);

typedef struct tagRECT { LONG left, top, right, bottom; } RECT, *LPRECT;
typedef const RECT *LPCRECT;
typedef struct tagPOINT { LONG x, y; } POINT, *LPPOINT;
typedef struct tagMSG {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
} MSG, *LPMSG;
typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore, fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT, *LPPAINTSTRUCT;
typedef struct tagWNDCLASSW {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra, cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
} WNDCLASSW;
typedef struct tagMONITORINFO {
    DWORD cbSize;
    RECT rcMonitor, rcWork;
    DWORD dwFlags;
} MONITORINFO, *LPMONITORINFO;

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG biWidth, biHeight;
    WORD biPlanes, biBitCount;
    DWORD biCompression, biSizeImage;
    LONG biXPelsPerMeter, biYPelsPerMeter;
    DWORD biClrUsed, biClrImportant;
} BITMAPINFOHEADER;
typedef struct tagRGBQUAD { BYTE rgbBlue, rgbGreen, rgbRed, rgbReserved; } RGBQUAD;
typedef struct tagBITMAPINFO { BITMAPINFOHEADER bmiHeader; RGBQUAD bmiColors[1]; } BITMAPINFO;
#pragma pack(push, 2)
typedef struct tagBITMAPFILEHEADER {
    WORD bfType;
    DWORD bfSize;
    WORD bfReserved1, bfReserved2;
    DWORD bfOffBits;
} BITMAPFILEHEADER;
#pragma pack(pop)

#define MAKEWORD(a, b) ((WORD)(((BYTE)((DWORD_PTR)(a) & 0xff)) | ((WORD)((BYTE)((DWORD_PTR)(b) & 0xff))) << 8))
#define LOWORD(l) ((WORD)((DWORD_PTR)(l) & 0xffff))
#define HIWORD(l) ((WORD)((DWORD_PTR)(l) >> 16))
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#define GET_WHEEL_DELTA_WPARAM(wParam) ((short)HIWORD(wParam))
#define WHEEL_DELTA 120
#define RGB(r, g, b) ((COLORREF)(((BYTE)(r) | ((WORD)((BYTE)(g)) << 8)) | (((DWORD)(BYTE)(b)) << 16)))
#define MAKEINTRESOURCEW(i) ((LPWSTR)((ULONG_PTR)((WORD)(i))))

#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SETFONT 0x0030
#define WM_NCHITTEST 0x0084
#define WM_KEYDOWN 0x0100
#define WM_CHAR 0x0102
#define WM_TIMER 0x0113
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_MOUSEWHEEL 0x020A
#define WM_CAPTURECHANGED 0x0215
#define WM_PRINT 0x0317
#define EM_SETSEL 0x00B1
#define WA_INACTIVE 0
#define HTTRANSPARENT (-1)
#define PRF_NONCLIENT 0x00000002L
#define PRF_CLIENT 0x00000004L
#define PRF_ERASEBKGND 0x00000008L
#define PM_REMOVE 0x0001
#define GWLP_WNDPROC (-4)
#define GA_ROOT 2
#define SW_HIDE 0
#define SW_SHOW 5
#define SW_SHOWNA 8
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOACTIVATE 0x0010
#define HWND_TOPMOST ((HWND)(LONG_PTR)-1)
#define HWND_NOTOPMOST ((HWND)(LONG_PTR)-2)
#define CW_USEDEFAULT ((int)0x80000000)
#define WS_OVERLAPPEDWINDOW 0x00CF0000L
#define WS_CHILD 0x40000000L
#define WS_VISIBLE 0x10000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_BORDER 0x00800000L
#define ES_AUTOHSCROLL 0x0080L
#define MONITOR_DEFAULTTONEAREST 0x00000002
#define IDC_ARROW MAKEINTRESOURCEW(32512)
#define IDC_SIZEALL MAKEINTRESOURCEW(32646)
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_CONTROL 0x11
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_UP 0x26
#define VK_DOWN 0x28
#define DT_CENTER 0x00000001
#define DT_VCENTER 0x00000004
#define DT_SINGLELINE 0x00000020
#define DT_NOPREFIX 0x00000800
#define DT_END_ELLIPSIS 0x00008000
#define TRANSPARENT 1
#define SRCCOPY 0x00CC0020ul
#define DIB_RGB_COLORS 0
#define BI_RGB 0L
#define WHITE_BRUSH 0
#define FW_NORMAL 400
#define DEFAULT_CHARSET 1
#define CLEARTYPE_QUALITY 5

/* ---- windows -------------------------------------------------------------------- */

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
BOOL SetProcessDPIAware(void);

/* Remembers the class's procedure for DispatchMessageW (win_models.c). */
/*@ requires cls: \valid_read(lpWndClass);
    requires name: valid_read_wstring(lpWndClass->lpszClassName); */
ATOM RegisterClassW(const WNDCLASSW *lpWndClass);

/*@ requires cls: valid_read_wstring(lpClassName);
    requires title: lpWindowName == \null || valid_read_wstring(lpWindowName);
    assigns __fc_win_state \from __fc_win_state, lpClassName[0 ..], lpWindowName[0 ..], dwStyle, X, Y, nWidth,
                                 nHeight, hWndParent;
    assigns \result \from __fc_win_state; */
HWND CreateWindowExW(DWORD dwExStyle, LPCWSTR lpClassName, LPCWSTR lpWindowName, DWORD dwStyle, int X, int Y,
                     int nWidth, int nHeight, HWND hWndParent, HMENU hMenu, HINSTANCE hInstance, LPVOID lpParam);
#define CreateWindowW(c, t, s, x, y, w, h, p, m, i, l) CreateWindowExW(0, c, t, s, x, y, w, h, p, m, i, l)

/*@ assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state; */
BOOL DestroyWindow(HWND hWnd);

/*@ requires r: \valid(lpRect) && \initialized(lpRect);
    assigns *lpRect \from *lpRect, dwStyle, bMenu;
    assigns \result \from \nothing; */
BOOL AdjustWindowRect(LPRECT lpRect, DWORD dwStyle, BOOL bMenu);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, nCmdShow;
    assigns \result \from __fc_win_state, hWnd; */
BOOL ShowWindow(HWND hWnd, int nCmdShow);

/*@ assigns \result \from __fc_win_state, hWnd; */
BOOL IsWindowVisible(HWND hWnd);

/*@ assigns \result \from __fc_win_state, hWnd; */
BOOL IsWindow(HWND hWnd);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, X, Y, nWidth, nHeight;
    assigns \result \from __fc_win_state; */
BOOL MoveWindow(HWND hWnd, int X, int Y, int nWidth, int nHeight, BOOL bRepaint);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, hWndInsertAfter, X, Y, cx, cy, uFlags;
    assigns \result \from __fc_win_state; */
BOOL SetWindowPos(HWND hWnd, HWND hWndInsertAfter, int X, int Y, int cx, int cy, UINT uFlags);

/*@ requires text: valid_read_wstring(lpString);
    assigns __fc_win_state \from __fc_win_state, hWnd, lpString[0 ..];
    assigns \result \from __fc_win_state; */
BOOL SetWindowTextW(HWND hWnd, LPCWSTR lpString);

/* Copies at most nMaxCount - 1 characters and a NUL. */
/*@ requires room: nMaxCount > 0;
    requires buf: \valid(lpString + (0 .. nMaxCount - 1));
    assigns lpString[0 .. nMaxCount - 1] \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd, nMaxCount;
    ensures range: 0 <= \result < nMaxCount;
    ensures terminated: lpString[\result] == 0;
    ensures string: valid_read_wstring(lpString); */
int GetWindowTextW(HWND hWnd, LPWSTR lpString, int nMaxCount);

/*@ requires r: \valid(lpRect);
    assigns *lpRect \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd;
    ensures set: \initialized(lpRect); */
BOOL GetWindowRect(HWND hWnd, LPRECT lpRect);

/*@ requires pts: cPoints > 0 ==> \valid(lpPoints + (0 .. cPoints - 1));
    requires pts_set: cPoints > 0 ==> \initialized(lpPoints + (0 .. cPoints - 1));
    assigns lpPoints[0 .. cPoints - 1] \from lpPoints[0 .. cPoints - 1], __fc_win_state, hWndFrom, hWndTo;
    assigns \result \from __fc_win_state, hWndFrom, hWndTo; */
int MapWindowPoints(HWND hWndFrom, HWND hWndTo, LPPOINT lpPoints, UINT cPoints);

/*@ requires p: \valid(lpPoint) && \initialized(lpPoint);
    assigns *lpPoint \from *lpPoint, __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd; */
BOOL ClientToScreen(HWND hWnd, LPPOINT lpPoint);

/*@ requires p: \valid(lpPoint) && \initialized(lpPoint);
    assigns *lpPoint \from *lpPoint, __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd; */
BOOL ScreenToClient(HWND hWnd, LPPOINT lpPoint);

/*@ assigns \result \from __fc_win_state, Point.x, Point.y; */
HWND WindowFromPoint(POINT Point);

/*@ assigns \result \from __fc_win_state, hwnd, gaFlags; */
HWND GetAncestor(HWND hwnd, UINT gaFlags);

/*@ assigns \result \from __fc_win_state, hWnd; */
HWND GetParent(HWND hWnd);

/*@ assigns \result \from __fc_win_state; */
HWND GetFocus(void);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state; */
HWND SetFocus(HWND hWnd);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state; */
HWND SetCapture(HWND hWnd);

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
BOOL ReleaseCapture(void);

/*@ assigns \result \from __fc_win_state, nVirtKey; */
short GetKeyState(int nVirtKey);

/*@ assigns \result \from __fc_win_state, pt.x, pt.y, dwFlags; */
HMONITOR MonitorFromPoint(POINT pt, DWORD dwFlags);

/*@ requires mi: \valid(lpmi) && \initialized(&lpmi->cbSize) && lpmi->cbSize == sizeof(MONITORINFO);
    assigns lpmi->rcMonitor, lpmi->rcWork, lpmi->dwFlags \from __fc_win_state, hMonitor;
    assigns \result \from __fc_win_state, hMonitor; */
BOOL GetMonitorInfoW(HMONITOR hMonitor, LPMONITORINFO lpmi);

/* Any message, at the UI's own request: the edit control or a window
 * procedure handles it; not followed here (see above). */
/*@ assigns __fc_win_state \from __fc_win_state, hWnd, Msg, wParam, lParam;
    assigns \result \from __fc_win_state, hWnd, Msg, wParam, lParam; */
LRESULT SendMessageW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

/* Remembers a new window procedure for DispatchMessageW (win_models.c);
 * returns the old one. */
LONG_PTR SetWindowLongPtrW(HWND hWnd, int nIndex, LONG_PTR dwNewLong);

/*@ requires proc: lpPrevWndFunc != \null;
    assigns __fc_win_state \from __fc_win_state, lpPrevWndFunc, hWnd, Msg, wParam, lParam;
    assigns \result \from __fc_win_state, hWnd, Msg, wParam, lParam; */
LRESULT CallWindowProcW(WNDPROC lpPrevWndFunc, HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, Msg, wParam, lParam;
    assigns \result \from __fc_win_state, hWnd, Msg, wParam, lParam; */
LRESULT DefWindowProcW(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam);

/*@ requires msg: \valid(lpMsg);
    assigns *lpMsg \from __fc_win_state;
    assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state;
    ensures got: \result != 0 ==> \initialized(lpMsg); */
BOOL PeekMessageW(LPMSG lpMsg, HWND hWnd, UINT wMsgFilterMin, UINT wMsgFilterMax, UINT wRemoveMsg);

/*@ requires msg: \valid_read(lpMsg) && \initialized(lpMsg);
    assigns __fc_win_state \from __fc_win_state, *lpMsg;
    assigns \result \from __fc_win_state; */
BOOL TranslateMessage(const MSG *lpMsg);

/* Calls a window procedure (win_models.c). */
/*@ requires msg: \valid_read(lpMsg) && \initialized(lpMsg); */
LRESULT DispatchMessageW(const MSG *lpMsg);

/*@ assigns __fc_win_state \from __fc_win_state, nExitCode; */
void PostQuitMessage(int nExitCode);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, nIDEvent, uElapse;
    assigns \result \from __fc_win_state; */
UINT_PTR SetTimer(HWND hWnd, UINT_PTR nIDEvent, UINT uElapse, void *lpTimerFunc);

/*@ requires r: lpRect == \null || \valid_read(lpRect);
    assigns __fc_win_state \from __fc_win_state, hWnd, bErase;
    assigns \result \from __fc_win_state; */
BOOL InvalidateRect(HWND hWnd, const RECT *lpRect, BOOL bErase);

/*@ requires r: lpRect == \null || \valid_read(lpRect);
    assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state; */
BOOL ValidateRect(HWND hWnd, const RECT *lpRect);

/*@ requires r: \valid_read(lprc) && \initialized(lprc);
    assigns \result \from *lprc, pt.x, pt.y; */
BOOL PtInRect(const RECT *lprc, POINT pt);

/*@ requires name: lpCursorName != \null;
    assigns \result \from __fc_win_state, hInstance, lpCursorName; */
HCURSOR LoadCursorW(HINSTANCE hInstance, LPCWSTR lpCursorName);
#define LoadCursor LoadCursorW

/*@ assigns __fc_win_state \from __fc_win_state, hCursor;
    assigns \result \from __fc_win_state; */
HCURSOR SetCursor(HCURSOR hCursor);

/* ---- painting ----------------------------------------------------------------- */

/*@ requires ps: \valid(lpPaint);
    assigns *lpPaint \from __fc_win_state, hWnd;
    assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd; */
HDC BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint);

/*@ requires ps: \valid_read(lpPaint) && \initialized(lpPaint);
    assigns __fc_win_state \from __fc_win_state, hWnd, *lpPaint;
    assigns \result \from __fc_win_state; */
BOOL EndPaint(HWND hWnd, const PAINTSTRUCT *lpPaint);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd;
    assigns \result \from __fc_win_state, hWnd; */
HDC GetDC(HWND hWnd);

/*@ assigns __fc_win_state \from __fc_win_state, hWnd, hDC;
    assigns \result \from __fc_win_state; */
int ReleaseDC(HWND hWnd, HDC hDC);

/*@ assigns __fc_win_state \from __fc_win_state, hdc;
    assigns \result \from __fc_win_state, hdc; */
HDC CreateCompatibleDC(HDC hdc);

/*@ assigns __fc_win_state \from __fc_win_state, hdc;
    assigns \result \from __fc_win_state; */
BOOL DeleteDC(HDC hdc);

/*@ assigns __fc_win_state \from __fc_win_state, hdc, cx, cy;
    assigns \result \from __fc_win_state, hdc, cx, cy; */
HBITMAP CreateCompatibleBitmap(HDC hdc, int cx, int cy);

/* *ppvBits: the bitmap's pixels, Windows's memory, valid until the bitmap
 * is deleted: biHeight rows of biWidth * biBitCount / 8 bytes, each padded
 * to 4 bytes.  The model allocates it (win_models.c). */
/*@ requires bmi: \valid_read(pbmi) && \initialized(&pbmi->bmiHeader);
    requires bits: \valid(ppvBits);
    requires section: hSection == \null;
    requires usage: usage == DIB_RGB_COLORS;
    requires dims: pbmi->bmiHeader.biWidth > 0 && pbmi->bmiHeader.biHeight != 0; */
HBITMAP CreateDIBSection(HDC hdc, const BITMAPINFO *pbmi, UINT usage, void **ppvBits, HANDLE hSection,
                         DWORD offset);

/*@ assigns __fc_win_state \from __fc_win_state, ho;
    assigns \result \from __fc_win_state; */
BOOL DeleteObject(HGDIOBJ ho);

/*@ assigns __fc_win_state \from __fc_win_state, hdc, h;
    assigns \result \from __fc_win_state, hdc; */
HGDIOBJ SelectObject(HDC hdc, HGDIOBJ h);

/*@ assigns \result \from __fc_win_state, i; */
HGDIOBJ GetStockObject(int i);

/*@ assigns __fc_win_state \from __fc_win_state, color;
    assigns \result \from __fc_win_state; */
HBRUSH CreateSolidBrush(COLORREF color);

/*@ requires face: pszFaceName == \null || valid_read_wstring(pszFaceName);
    assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
HFONT CreateFontW(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic,
                  DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
                  DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName);

/*@ requires r: \valid_read(lprc) && \initialized(lprc);
    assigns __fc_win_state \from __fc_win_state, hDC, *lprc, hbr;
    assigns \result \from __fc_win_state; */
int FillRect(HDC hDC, const RECT *lprc, HBRUSH hbr);

/* cchText -1: a NUL-terminated string.  DT_MODIFYSTRING isn't used, so
 * the text is only read. */
/*@ requires text: cchText == -1 ==> valid_read_wstring(lpchText);
    requires chars: cchText >= 0 ==> \valid_read(lpchText + (0 .. cchText - 1));
    requires r: \valid(lprc) && \initialized(lprc);
    assigns __fc_win_state \from __fc_win_state, hdc, lpchText[0 ..], *lprc, format;
    assigns \result \from __fc_win_state; */
int DrawTextW(HDC hdc, LPCWSTR lpchText, int cchText, LPRECT lprc, UINT format);

/*@ assigns __fc_win_state \from __fc_win_state, hdc, mode;
    assigns \result \from __fc_win_state; */
int SetBkMode(HDC hdc, int mode);

/*@ assigns __fc_win_state \from __fc_win_state, hdc, color;
    assigns \result \from __fc_win_state; */
COLORREF SetTextColor(HDC hdc, COLORREF color);

/*@ requires old: lppt == \null || \valid(lppt);
    assigns __fc_win_state \from __fc_win_state, hdc, x, y;
    assigns \result \from __fc_win_state; */
BOOL SetViewportOrgEx(HDC hdc, int x, int y, LPPOINT lppt);

/*@ assigns __fc_win_state \from __fc_win_state, hdc, x, y, cx, cy, hdcSrc, x1, y1, rop;
    assigns \result \from __fc_win_state; */
BOOL BitBlt(HDC hdc, int x, int y, int cx, int cy, HDC hdcSrc, int x1, int y1, DWORD rop);

/*@ assigns __fc_win_state \from __fc_win_state;
    assigns \result \from __fc_win_state; */
BOOL GdiFlush(void);

/* Reads cLines rows, starting at row StartScan, of lpbmi's bitmap from
 * lpvBits: 32 or 24 bits a pixel, each row padded to 4 bytes.  The shims
 * pass StartScan 0 and a top-down bitmap (negative height) of at least
 * cLines rows, or a bottom-up one of exactly that many. */
/*@ requires bmi: \valid_read(lpbmi) && \initialized(&lpbmi->bmiHeader);
    requires start: StartScan == 0;
    requires rows: cLines <= (lpbmi->bmiHeader.biHeight < 0 ? -lpbmi->bmiHeader.biHeight
                                                              : lpbmi->bmiHeader.biHeight);
    requires bits: \valid_read((char *)lpvBits +
                               (0 .. cLines * (((lpbmi->bmiHeader.biWidth * lpbmi->bmiHeader.biBitCount + 31) / 32) * 4)
                                     - 1));
    assigns __fc_win_state \from __fc_win_state, hdc, xDest, yDest, w, h, xSrc, ySrc, cLines, *lpbmi,
                                 ((char *)lpvBits)[0 ..];
    assigns \result \from __fc_win_state, cLines; */
int SetDIBitsToDevice(HDC hdc, int xDest, int yDest, DWORD w, DWORD h, int xSrc, int ySrc, UINT StartScan,
                      UINT cLines, const void *lpvBits, const BITMAPINFO *lpbmi, UINT ColorUse);

#endif
