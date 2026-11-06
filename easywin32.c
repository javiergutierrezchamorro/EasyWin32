/* ==================================================================
 * EasyWin32.c - implementation
 * ------------------------------------------------------------------
 * Architecture (pure Win32, ANSI C; nothing compiler-specific):
 *
 *  - WinMain (from this library) starts the window on its own GUI
 *    thread, redirects stdout/stderr/stdin to pipes (dup2 first,
 *    with a verified named-pipe + freopen fallback for the modern
 *    UCRT, where GUI executables are born without std handles) and
 *    calls the user's main().
 *
 *  - A reader thread drains the stdout pipe and pours the bytes
 *    into a grid of cells (char + color attribute), with scrolling,
 *    conio attributes and basic ANSI sequence decoding.
 *
 *  - The keyboard feeds a single queue: kbhit/getch/getche read
 *    from it raw, and on Enter the pending line is also delivered
 *    to stdin (scanf/gets/cin).
 *
 *  - The conio functions (gotoxy, textcolor, cprintf...) write
 *    directly to the grid with the same cursor printf uses,
 *    draining the pipe first so the DOS ordering is preserved.
 *
 *  - QuickWin-style extras: menu bar (File/Edit/View/Help), mouse
 *    text selection with automatic copy to the clipboard, Copy /
 *    Paste / Select All / Clear Screen, context menu and Ctrl+C /
 *    Ctrl+V. EasyWin never had these; QuickWin did.
 *
 *  - Identity: if the app links easywin32.rc (icon + VERSIONINFO
 *    1.04), the window picks up the EasyWin32 icon automatically
 *    (ID 1, or Borland-style "A"; standard icon otherwise), and
 *    Help/About shows version, compiler, platform and build date.
 *    ew_version() returns EASYWIN32_VERSION ("1.04").
 *
 *  - Robustness: g_lock is initialized lazily (never used raw) and
 *    any conio call made before ew_init() self-bootstraps the
 *    window. This keeps working even if the program was built with
 *    the console subsystem (where WinMain never runs), if ew_init()
 *    was forgotten in EASYWIN32_NO_ENTRY mode, or from C++ global
 *    constructors.
 * ================================================================== */

#define _CRT_NONSTDC_NO_WARNINGS 1
#define _CRT_SECURE_NO_WARNINGS 1

#include "easywin32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <io.h>
#include <errno.h>

#if defined(_MSC_VER) && _MSC_VER < 1400
#define EW_VSNPRINTF _vsnprintf
#else
#define EW_VSNPRINTF vsnprintf
#endif

#ifdef _WIN64
#define EW_OSFH(h) ((long long)(h))
#else
#define EW_OSFH(h) ((long)(h))
#endif

 /* Cast for AppendMenu's popup-menu HMENU argument. USER handles are
  * guaranteed to fit in 32 bits, but MSVC x64 still warns (C4311) on
  * a plain (UINT) cast, so use the real UINT_PTR type when the
  * headers have it (basetsd.h); old Borland 5.x headers lack it. */
#if defined(_UINT_PTR_DEFINED)
#define EW_UPTR UINT_PTR
#elif defined(_WIN64)
#define EW_UPTR unsigned long long
#else
#define EW_UPTR unsigned long
#endif

#define EW_MAXCOLS 256
#define EW_MAXROWS 128
#define EW_KQSIZE  1024
#define EW_KQMASK  (EW_KQSIZE - 1)
#define EW_LINEMAX 1024
#define EW_NLINES  32
#define EW_READBUF 4096

#define EW_WM_PAINTQ (WM_APP + 1)
#define EW_WM_RESIZE (WM_APP + 2)
#define EW_WM_FONT   (WM_APP + 3)
#define EW_WM_CARET  (WM_APP + 4)

  /* menu command IDs (QuickWin-style menu bar) */
#define EW_M_EXIT     100
#define EW_M_COPY     101
#define EW_M_PASTE    102
#define EW_M_SELALL   103
#define EW_M_CLEAR    104
#define EW_M_FONT_S   110
#define EW_M_FONT_M   111
#define EW_M_FONT_L   112
#define EW_M_CP437    113
#define EW_M_CP850    114
#define EW_M_CP1252   115
#define EW_M_ABOUT    120

typedef struct { unsigned char ch, attr; } EWCELL;

/* ---------------- text terminal state ---------------- */
static EWCELL        g_grid[EW_MAXROWS][EW_MAXCOLS];
static int           g_cols = 80, g_rows = 25;
static int           g_x = 0, g_y = 0;               /* absolute cursor, 0-based */
static unsigned char g_attr = 0x07;
static unsigned char g_normattr = 0x07;
static int           g_wx0 = 0, g_wy0 = 0, g_wx1 = 79, g_wy1 = 24;

static CRITICAL_SECTION g_lock;
static volatile LONG g_lockState = 0;   /* 0 = new, 1 = initializing, 2 = ready */
static volatile LONG g_bootTried = 0;  /* self-bootstrap attempted once */

/* ---------------- window and font ---------------- */
static HWND  g_hwnd = NULL;
static HFONT g_font = NULL;
static int   g_cw = 9, g_chh = 16;
static char  g_title[80] = "EasyWin32";
static char  g_fontface[64] = "Lucida Console";
static int   g_fontheight = 16;
static UINT  g_codepage = 437;     /* DOS codepage by default */
static HICON g_hIcon = NULL;       /* custom icon from easywin32.rc, if any */
static HICON g_hIconS = NULL;      /* 16x16 variant for the title bar */
static int   g_caretkind = 2;      /* same values as _setcursortype: 0/1/2 */
static int   g_echo = 1;

/* ---------------- mouse text selection ---------------- */
static int g_selOn = 0;
static int g_selax = 0, g_selay = 0;                 /* anchor cell */
static int g_selx0 = 0, g_sely0 = 0, g_selx1 = 0, g_sely1 = 0;  /* rect */

/* ---------------- pipes and threads ---------------- */
static HANDLE g_hOutR = NULL, g_hOutW = NULL;   /* write end handed to the CRT */
static HANDLE g_hInR = NULL, g_hInW = NULL;   /* read end handed to the CRT  */
static HANDLE g_hErrR = NULL;                  /* stderr pipe, named-pipe mode */
static HANDLE g_semKeys = NULL, g_semLines = NULL, g_evtReady = NULL;
static HANDLE g_threadGui = NULL, g_threadReader = NULL, g_threadFeeder = NULL;
static HANDLE g_threadErr = NULL;              /* 2nd reader (stderr pipe) */
static DWORD  g_tidGui = 0;

/* stdio transport actually in use:
 * 0 = not initialized, 1 = dup2 on anonymous pipes,
 * 2 = named pipes + freopen, 3 = broken (see g_diag) */
static int    g_stdMode = 0;
static char   g_diag[160] = "";                /* why 3 (broken) */
static volatile LONG g_diagShown = 0;
static volatile LONG g_run = 0;
static volatile LONG g_shutdownDone = 0;
static volatile LONG g_readerIdle = 1;

/* ---------------- keyboard ---------------- */
static unsigned char g_kq[EW_KQSIZE];
static int           g_kqHead = 0, g_kqTail = 0;
static char          g_pend[EW_LINEMAX];
static int           g_pendLen = 0;
static char          g_lines[EW_NLINES][EW_LINEMAX];
static int           g_lineLen[EW_NLINES];
static int           g_lineHead = 0, g_lineCount = 0;

/* ---------------- command line ---------------- */
static char  g_cmdBuf[4096];
static char* g_argv[512];
static int   g_argc = 0;

/* ---------------- speaker ---------------- */
static volatile LONG g_sndOn = 0;
static volatile LONG g_sndFreq = 440;
static HANDLE g_sndThread = NULL;

/* ---------------- ANSI decoder ---------------- */
static int  g_ansiState = 0;
static char g_ansiBuf[32];
static int  g_ansiLen = 0;

static COLORREF g_pal[16] = {
    RGB(0,0,0),       RGB(0,0,170),     RGB(0,170,0),     RGB(0,170,170),
    RGB(170,0,0),     RGB(170,0,170),   RGB(170,85,0),    RGB(170,170,170),
    RGB(85,85,85),    RGB(85,85,255),   RGB(85,255,85),   RGB(85,255,255),
    RGB(255,85,85),   RGB(255,85,255),  RGB(255,255,85),  RGB(255,255,255)
};

/* ---------------- internal prototypes ---------------- */
static void    ew_ensure_lock(void);
static void    ew_lock(void);
static void    ew_unlock(void);
static void    ew_bootstrap(void);
static void    ew_request_paint(void);
static void    ew_drain(void);
static void    ew_emit(const char* s, int len);
static void    ew_putc_locked(int c);
static void    ew_newline_locked(void);
static void    ew_scroll_locked(void);
static void    ew_consume_byte(int c);
static void    ew_ansi_exec(int final);
static DWORD WINAPI ew_reader(LPVOID p);
static DWORD WINAPI ew_feeder(LPVOID p);
static DWORD WINAPI ew_gui_thread(LPVOID p);
static int    ew_setup_stdio(void);
static void   ew_report_stdio(void);
static const char* ew_stdmode_text(void);
static DWORD WINAPI ew_snd_thread(LPVOID p);
static void    ew_feed_char(unsigned ch);
static void    ew_paste(void);
static void    ew_on_keydown(WPARAM vk);
static int     ew_kq_push_locked(int c);
static int     ew_kq_pop_locked(int* c);
static int     ew_getch_impl(int echo);
static void    ew_make_font(void);
static void    ew_measure_font(void);
static void    ew_fit_window(void);
static void    ew_paint(HWND h);
static void    ew_update_caret(void);
static void    ew_create_caret(HWND h);
static void    ew_parse_cmdline(void);
static void    ew_sel_set(int x, int y);      /* lock required */
static void    ew_copy_selection(void);
static HMENU   ew_build_menu(void);
static void    ew_command(HWND h, int id);
static const char* ew_compiler(void);
static const char* ew_platform(void);

/* ==================================================================
 * Lazy lock and self-bootstrap
 * ------------------------------------------------------------------
 * NEVER call EnterCriticalSection on g_lock directly: using a
 * zero-initialized CRITICAL_SECTION (before InitializeCriticalSection
 * ran) crashes inside ntdll with 0xC0000005 writing near NULL.
 * This can happen if a conio function runs before ew_init() (a
 * console-subsystem build where WinMain never runs, a forgotten
 * ew_init() in EASYWIN32_NO_ENTRY mode, a C++ global constructor).
 *
 * ew_ensure_lock() initializes it on first use with a portable
 * interlocked state machine (Vista's InitOnceExecuteOnce is missing
 * from Borland 5.x / Open Watcom headers), and ew_bootstrap() starts
 * the whole window on demand so those situations keep working.
 * ================================================================== */

static void ew_ensure_lock(void)
{
    for (;;) {
        LONG st = InterlockedCompareExchange(&g_lockState, 1, 0);
        if (st == 0) {
            InitializeCriticalSection(&g_lock);
            InterlockedExchange(&g_lockState, 2);
            return;
        }
        if (st == 2)
            return;
        Sleep(0);                   /* another thread is initializing it */
    }
}

static void ew_lock(void)
{
    ew_ensure_lock();
    EnterCriticalSection(&g_lock);
}

static void ew_unlock(void)
{
    LeaveCriticalSection(&g_lock);
}

/* First conio call before ew_init()? Start everything now, once. */
static void ew_bootstrap(void)
{
    if (!g_run && !g_shutdownDone &&
        InterlockedExchange(&g_bootTried, 1) == 0) {
        (void)ew_init();
    }
}

/* ==================================================================
 * Text grid (all _locked functions require the lock held)
 * ================================================================== */

static void ew_scroll_locked(void)
{
    int r, c, wbytes;
    EWCELL fill;

    wbytes = (g_wx1 - g_wx0 + 1) * (int)sizeof(EWCELL);
    for (r = g_wy0; r < g_wy1; r++)
        memcpy(&g_grid[r][g_wx0], &g_grid[r + 1][g_wx0], (size_t)wbytes);
    fill.ch = ' ';
    fill.attr = g_attr;
    for (c = g_wx0; c <= g_wx1; c++)
        g_grid[g_wy1][c] = fill;
}

static void ew_newline_locked(void)
{
    if (g_y >= g_wy1) {
        if (g_y == g_wy1)
            ew_scroll_locked();
    }
    else if (g_y < g_rows - 1) {
        g_y++;
    }
}

static void ew_putc_locked(int c)
{
    if (c == '\n') {
        g_x = g_wx0;
        ew_newline_locked();
    }
    else if (c == '\r') {
        g_x = g_wx0;
    }
    else if (c == 8) {
        if (g_x > g_wx0)
            g_x--;
    }
    else if (c == 7) {
        MessageBeep(MB_OK);
    }
    else if (c == '\t') {
        int guard = 16;   /* safety stop if the window is very narrow */
        do {
            ew_putc_locked(' ');
        } while (((g_x - g_wx0) % 8) != 0 && --guard > 0);
    }
    else {
        g_grid[g_y][g_x].ch = (unsigned char)c;
        g_grid[g_y][g_x].attr = g_attr;
        g_x++;
        if (g_x > g_wx1) {
            g_x = g_wx0;
            ew_newline_locked();
        }
    }
}

/* ==================================================================
 * Output: stdout/stderr pipe -> grid
 * ================================================================== */

static void ew_request_paint(void)
{
    if (g_hwnd != NULL && g_run)
        PostMessageA(g_hwnd, EW_WM_PAINTQ, 0, 0);
}

/* Waits until the reader thread has poured everything pending from
 * the pipe. The conio functions call this before touching the
 * screen, so "printf(...); gotoxy(...)" keeps the DOS ordering. */
static void ew_drain(void)
{
    DWORD start;

    if (g_hOutR == NULL || !g_run)
        return;
    start = GetTickCount();
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_hOutR, NULL, 0, NULL, &avail, NULL))
            return;
        if (avail == 0 && g_readerIdle)
            return;
        if (GetTickCount() - start > 10000)  /* safety net */
            return;
        Sleep(0);
    }
}

static void ew_emit(const char* s, int len)
{
    int i;

    if (!g_run || s == NULL || len <= 0)
        return;
    ew_lock();
    for (i = 0; i < len; i++)
        ew_putc_locked((unsigned char)s[i]);
    ew_unlock();
    ew_request_paint();
}

/* -------- basic ANSI (CSI) sequences: an extra over EasyWin ------- */

static void ew_ansi_exec(int final)
{
    int vals[8];
    int n = 0, v = 0, i;

    vals[0] = 0;
    for (i = 0; g_ansiBuf[i] != 0; i++) {
        char ch = g_ansiBuf[i];
        if (ch >= '0' && ch <= '9')
            v = v * 10 + (ch - '0');
        else {
            if (n < 8) vals[n++] = v;
            v = 0;
        }
    }
    if (n < 8) vals[n++] = v;

    switch (final) {
    case 'm':
        for (i = 0; i < n; i++) {
            int a = vals[i];
            if (a == 0)
                g_attr = g_normattr;
            else if (a == 1)
                g_attr |= 0x08;
            else if (a == 2)
                g_attr &= 0xF7;
            else if (a >= 30 && a <= 37)
                g_attr = (unsigned char)((g_attr & 0xF0) | (a - 30));
            else if (a == 39)
                g_attr = (unsigned char)((g_attr & 0xF0) | 0x07);
            else if (a >= 40 && a <= 47)
                g_attr = (unsigned char)((g_attr & 0x0F) | ((a - 40) << 4));
            else if (a == 49)
                g_attr = (unsigned char)(g_attr & 0x0F);
        }
        break;
    case 'A':
        g_y -= (vals[0] > 0) ? vals[0] : 1;
        if (g_y < 0) g_y = 0;
        break;
    case 'B':
        g_y += (vals[0] > 0) ? vals[0] : 1;
        if (g_y > g_rows - 1) g_y = g_rows - 1;
        break;
    case 'C':
        g_x += (vals[0] > 0) ? vals[0] : 1;
        if (g_x > g_cols - 1) g_x = g_cols - 1;
        break;
    case 'D':
        g_x -= (vals[0] > 0) ? vals[0] : 1;
        if (g_x < 0) g_x = 0;
        break;
    case 'H':
    case 'f': {
        int row = (vals[0] > 0) ? vals[0] : 1;
        int col = (n >= 2 && vals[1] > 0) ? vals[1] : 1;
        g_y = row - 1;
        g_x = col - 1;
        if (g_y > g_rows - 1) g_y = g_rows - 1;
        if (g_x > g_cols - 1) g_x = g_cols - 1;
        break;
    }
    case 'J':
        if (vals[0] == 2 || vals[0] == 3) {
            int r, c;
            for (r = 0; r < g_rows; r++)
                for (c = 0; c < g_cols; c++) {
                    g_grid[r][c].ch = ' ';
                    g_grid[r][c].attr = g_attr;
                }
        }
        break;
    case 'K': {
        int c;
        for (c = g_x; c < g_cols; c++) {
            g_grid[g_y][c].ch = ' ';
            g_grid[g_y][c].attr = g_attr;
        }
        break;
    }
    default:
        break;
    }
}

static void ew_consume_byte(int c)
{
    if (g_ansiState == 0) {
        if (c == 27) {
            g_ansiState = 1;
            g_ansiLen = 0;
            return;
        }
        ew_putc_locked(c);
    }
    else if (g_ansiState == 1) {
        g_ansiState = (c == '[') ? 2 : 0;
        if (g_ansiState == 0 && c != 27)
            ew_putc_locked(c);   /* lone ESC: print the byte */
    }
    else {
        if ((c >= '0' && c <= '9') || c == ';' || c == '?') {
            if (g_ansiLen < (int)sizeof(g_ansiBuf) - 1)
                g_ansiBuf[g_ansiLen++] = (char)c;
        }
        else if (c == 27) {
            g_ansiState = 1;
            g_ansiLen = 0;
        }
        else {
            g_ansiBuf[g_ansiLen] = 0;
            g_ansiState = 0;
            ew_ansi_exec(c);
        }
    }
}

static DWORD WINAPI ew_reader(LPVOID p)
{
    char   buf[EW_READBUF];
    DWORD  n;
    HANDLE h = (HANDLE)p;   /* NULL: stdout pipe; else a fixed handle */

    while (1) {
        DWORD avail = 0;
        HANDLE cur = (h != NULL) ? h : g_hOutR;

        if (cur == NULL)
            break;
        if (!PeekNamedPipe(cur, NULL, 0, NULL, &avail, NULL))
            break;
        if (avail == 0) {
            InterlockedExchange(&g_readerIdle, 1);
            Sleep(1);
            continue;
        }
        InterlockedExchange(&g_readerIdle, 0);
        if (!ReadFile(cur, buf,
            (avail < (DWORD)EW_READBUF) ? avail : (DWORD)EW_READBUF,
            &n, NULL) || n == 0)
            break;
        ew_lock();
        {
            DWORD i;
            for (i = 0; i < n; i++)
                ew_consume_byte((unsigned char)buf[i]);
        }
        ew_unlock();
        ew_request_paint();
    }
    InterlockedExchange(&g_readerIdle, 1);
    return 0;
}

/* ==================================================================
 * Input: keyboard -> raw queue (getch/kbhit) + stdin (scanf/gets)
 * ================================================================== */

static int ew_kq_push_locked(int c)
{
    int nt = (g_kqTail + 1) & EW_KQMASK;
    if (nt == g_kqHead)
        return 0;           /* full: drop, like the BIOS buffer did */
    g_kq[g_kqTail] = (unsigned char)c;
    g_kqTail = nt;
    return 1;
}

static int ew_kq_pop_locked(int* c)
{
    if (g_kqHead == g_kqTail)
        return 0;
    *c = g_kq[g_kqHead];
    g_kqHead = (g_kqHead + 1) & EW_KQMASK;
    return 1;
}

/* Called on the GUI thread with every typed character.
 * - echoes it to the grid (if ew_setecho(1)),
 * - pushes it into the raw queue for kbhit/getch,
 * - and accumulates it into the pending line for stdin. */
static void ew_feed_char(unsigned ch)
{
    int pushed = 0;
    int linequeued = 0;

    ew_lock();
    if (ch == '\r') {
        ew_putc_locked('\r');
        ew_putc_locked('\n');
        pushed += ew_kq_push_locked('\r');
        if (g_pendLen > 0 && g_lineCount < EW_NLINES && g_pendLen < EW_LINEMAX) {
            int idx = (g_lineHead + g_lineCount) % EW_NLINES;
            memcpy(g_lines[idx], g_pend, (size_t)g_pendLen);
            g_lines[idx][g_pendLen] = '\n';
            g_lineLen[idx] = g_pendLen + 1;
            g_lineCount++;
            linequeued = 1;
        }
        g_pendLen = 0;
    }
    else if (ch == 8) {
        if (g_pendLen > 0) {
            g_pendLen--;
            ew_putc_locked(8);
            ew_putc_locked(' ');
            ew_putc_locked(8);
        }
        pushed += ew_kq_push_locked(8);
    }
    else if (ch == '\t') {
        ew_putc_locked('\t');
        pushed += ew_kq_push_locked('\t');
    }
    else if (ch >= ' ') {
        ew_putc_locked((int)ch);
        pushed += ew_kq_push_locked((int)ch);
        if (g_pendLen < EW_LINEMAX)
            g_pend[g_pendLen++] = (unsigned char)ch;
    }
    else {
        /* raw control characters (Esc, Ctrl+letter...) for getch */
        pushed += ew_kq_push_locked((int)ch);
    }
    ew_unlock();

    if (linequeued)
        ReleaseSemaphore(g_semLines, 1, NULL);
    if (pushed > 0)
        ReleaseSemaphore(g_semKeys, (LONG)pushed, NULL);
    ew_request_paint();
}

/* Ctrl+V: paste clipboard text as if it were typed */
static void ew_paste(void)
{
    HANDLE h;

    if (!OpenClipboard(g_hwnd))
        return;
    h = GetClipboardData(CF_TEXT);
    if (h != NULL) {
        const char* p = (const char*)GlobalLock(h);
        if (p != NULL) {
            for (; *p != 0; p++) {
                if (*p == '\r')
                    continue;
                if (*p == '\n')
                    ew_feed_char('\r');
                else
                    ew_feed_char((unsigned char)*p);
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

/* Extended keys: getch() returns 0 and then the scan code
 * (just like DOS: arrows 72/80/75/77, Home 71, End 79...). */
static void ew_on_keydown(WPARAM vk)
{
    int sc = 0;

    switch (vk) {
    case VK_UP:     sc = 72; break;
    case VK_DOWN:   sc = 80; break;
    case VK_LEFT:   sc = 75; break;
    case VK_RIGHT:  sc = 77; break;
    case VK_HOME:   sc = 71; break;
    case VK_END:    sc = 79; break;
    case VK_PRIOR:  sc = 73; break;   /* PgUp */
    case VK_NEXT:   sc = 81; break;   /* PgDn */
    case VK_INSERT: sc = 82; break;
    case VK_DELETE: sc = 83; break;
    default:
        if (vk >= VK_F1 && vk <= VK_F10)
            sc = 59 + ((int)vk - VK_F1);
        else if (vk == VK_F11)
            sc = 84;
        else if (vk == VK_F12)
            sc = 85;
        break;
    }
    if (sc != 0) {
        int pushed = 0;
        ew_lock();
        pushed += ew_kq_push_locked(0);
        pushed += ew_kq_push_locked(sc);
        ew_unlock();
        if (pushed > 0)
            ReleaseSemaphore(g_semKeys, (LONG)pushed, NULL);
    }
}

/* ==================================================================
 * QuickWin-style extras: mouse text selection, clipboard and a
 * classic menu bar (File / Edit / View / Help). Real EasyWin only
 * had a bare text window; Microsoft's QuickWin added menus and
 * copy/paste, and so does this library.
 * ================================================================== */

 /* Recompute the selection rect from the anchor and the current cell.
  * Called with g_lock held. */
static void ew_sel_set(int x, int y)
{
    g_selx0 = (x < g_selax) ? x : g_selax;
    g_selx1 = (x < g_selax) ? g_selax : x;
    g_sely0 = (y < g_selay) ? y : g_selay;
    g_sely1 = (y < g_selay) ? g_selay : y;
}

static void ew_copy_selection(void)
{
    static char buf[EW_MAXCOLS * EW_MAXROWS + EW_MAXROWS * 2 + 1];
    int x0, y0, x1, y1, y, len = 0;
    HGLOBAL hg;
    char* p;

    ew_lock();
    if (!g_selOn) {
        ew_unlock();
        return;
    }
    x0 = g_selx0; y0 = g_sely0; x1 = g_selx1; y1 = g_sely1;
    for (y = y0; y <= y1; y++) {
        int x;
        for (x = x0; x <= x1; x++)
            buf[len++] = (char)g_grid[y][x].ch;
        if (y != y1) {
            buf[len++] = '\r';
            buf[len++] = '\n';
        }
    }
    ew_unlock();
    if (len == 0 || !OpenClipboard(g_hwnd))
        return;
    EmptyClipboard();
    hg = GlobalAlloc(GMEM_MOVEABLE, (DWORD)len + 1);
    if (hg != NULL) {
        p = (char*)GlobalLock(hg);
        if (p != NULL) {
            memcpy(p, buf, (size_t)len);
            p[len] = 0;
            GlobalUnlock(hg);
            if (SetClipboardData(CF_TEXT, hg) == NULL)
                GlobalFree(hg);
        }
        else {
            GlobalFree(hg);
        }
    }
    CloseClipboard();
}

static HMENU ew_build_menu(void)
{
    HMENU bar = CreateMenu();
    HMENU mFile = CreatePopupMenu();
    HMENU mEdit = CreatePopupMenu();
    HMENU mView = CreatePopupMenu();
    HMENU mHelp = CreatePopupMenu();

    AppendMenuA(mFile, MF_STRING, EW_M_EXIT, "E&xit\tAlt+F4");

    AppendMenuA(mEdit, MF_STRING, EW_M_COPY, "&Copy\tCtrl+C");
    AppendMenuA(mEdit, MF_STRING, EW_M_PASTE, "&Paste\tCtrl+V");
    AppendMenuA(mEdit, MF_STRING, EW_M_SELALL, "Select &All");
    AppendMenuA(mEdit, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mEdit, MF_STRING, EW_M_CLEAR, "C&lear Screen");

    AppendMenuA(mView, MF_STRING, EW_M_FONT_S, "&Small font");
    AppendMenuA(mView, MF_STRING, EW_M_FONT_M, "&Medium font");
    AppendMenuA(mView, MF_STRING, EW_M_FONT_L, "&Large font");
    AppendMenuA(mView, MF_SEPARATOR, 0, NULL);
    AppendMenuA(mView, MF_STRING, EW_M_CP437, "Codepage &437 (US DOS)");
    AppendMenuA(mView, MF_STRING, EW_M_CP850, "Codepage &850 (EU DOS)");
    AppendMenuA(mView, MF_STRING, EW_M_CP1252, "Codepage 125&2 (Windows)");

    AppendMenuA(mHelp, MF_STRING, EW_M_ABOUT, "&About EasyWin32...");

    AppendMenuA(bar, MF_POPUP, (EW_UPTR)mFile, "&File");
    AppendMenuA(bar, MF_POPUP, (EW_UPTR)mEdit, "&Edit");
    AppendMenuA(bar, MF_POPUP, (EW_UPTR)mView, "&View");
    AppendMenuA(bar, MF_POPUP | MF_HELP, (EW_UPTR)mHelp, "&Help");
    return bar;
}

/* Turn a preprocessor number into a string ("stringify") */
#define EW_STR2(x) #x
#define EW_STR(x)  EW_STR2(x)

/* Which compiler built this program (for the About box) */
static const char* ew_compiler(void)
{
#if defined(_MSC_VER)
    return "Microsoft Visual C++ " EW_STR(_MSC_VER);
#elif defined(__BORLANDC__)
    return "Borland C++ " EW_STR(__BORLANDC__);
#elif defined(__WATCOMC__)
    return "Open Watcom C/C++ " EW_STR(__WATCOMC__);
#elif defined(__clang__)
    return "Clang " EW_STR(__clang_major__) "." EW_STR(__clang_minor__);
#elif defined(__GNUC__) && (defined(__MINGW32__) || defined(__MINGW64__))
    return "GCC (MinGW) " EW_STR(__GNUC__) "." EW_STR(__GNUC_MINOR__);
#elif defined(__GNUC__)
    return "GCC " EW_STR(__GNUC__) "." EW_STR(__GNUC_MINOR__);
#else
    return "Unknown compiler";
#endif
}

/* Which platform this program was built for (for the About box) */
static const char* ew_platform(void)
{
#if defined(_WIN64)
# if defined(_M_ARM64) || defined(__aarch64__)
    return "Win64 (ARM64)";
# else
    return "Win64 (x64)";
# endif
#else
    return "Win32 (x86)";
#endif
}

/* Runs on the GUI thread (WM_COMMAND) */
static void ew_command(HWND h, int id)
{
    switch (id) {
    case EW_M_EXIT:
        PostMessageA(h, WM_CLOSE, 0, 0);
        break;
    case EW_M_COPY:
        ew_copy_selection();
        break;
    case EW_M_PASTE:
        ew_paste();
        break;
    case EW_M_SELALL:
        ew_lock();
        g_selOn = 1;
        g_selax = 0;
        g_selay = 0;
        ew_sel_set(g_cols - 1, g_rows - 1);
        ew_unlock();
        InvalidateRect(h, NULL, FALSE);
        break;
    case EW_M_CLEAR: {
        int r, c;
        ew_lock();
        for (r = 0; r < g_rows; r++)
            for (c = 0; c < g_cols; c++) {
                g_grid[r][c].ch = ' ';
                g_grid[r][c].attr = g_attr;
            }
        g_x = 0;
        g_y = 0;
        g_selOn = 0;
        ew_unlock();
        InvalidateRect(h, NULL, FALSE);
        break;
    }
    case EW_M_FONT_S:
        g_fontheight = 12;
        PostMessageA(h, EW_WM_FONT, 0, 0);
        break;
    case EW_M_FONT_M:
        g_fontheight = 16;
        PostMessageA(h, EW_WM_FONT, 0, 0);
        break;
    case EW_M_FONT_L:
        g_fontheight = 20;
        PostMessageA(h, EW_WM_FONT, 0, 0);
        break;
    case EW_M_CP437:
        g_codepage = 437;
        InvalidateRect(h, NULL, TRUE);
        break;
    case EW_M_CP850:
        g_codepage = 850;
        InvalidateRect(h, NULL, TRUE);
        break;
    case EW_M_CP1252:
        g_codepage = 1252;
        InvalidateRect(h, NULL, TRUE);
        break;
    case EW_M_ABOUT: {
        char msg[512];
        wsprintfA(msg,
            "EasyWin32 %s - EasyWin-style text window for Win32/Win64.\n\n"
            "Clean-room recreation of Borland's EasyWin concept with\n"
            "QuickWin-style menus, text selection and clipboard.\n\n"
            "Compiler: %s\nPlatform: %s\nI/O: %s\nBuilt: %s %s\n\n"
            "Javier Guti\351rrez Chamorro (Guti)",
            EASYWIN32_VERSION, ew_compiler(), ew_platform(),
            ew_stdmode_text(), __DATE__, __TIME__);
        MessageBoxA(h, msg, "About EasyWin32", MB_OK | MB_ICONINFORMATION);
        break;
    }
    default:
        break;
    }
}

/* Thread that delivers complete lines to the stdin pipe */
static DWORD WINAPI ew_feeder(LPVOID p)
{
    char loc[EW_LINEMAX];

    (void)p;
    for (;;) {
        int   len;
        DWORD n;

        if (WaitForSingleObject(g_semLines, 250) != WAIT_OBJECT_0) {
            if (!g_run)
                break;
            continue;
        }
        ew_lock();
        len = g_lineLen[g_lineHead];
        memcpy(loc, g_lines[g_lineHead], (size_t)len);
        g_lineHead = (g_lineHead + 1) % EW_NLINES;
        g_lineCount--;
        ew_unlock();
        if (!WriteFile(g_hInW, loc, (DWORD)len, &n, NULL)) {
            /* nobody is reading stdin: the line is lost */
        }
    }
    return 0;
}

/* ==================================================================
 * Speaker (sound/nosound with Beep in slices, DOS style)
 * ================================================================== */

static DWORD WINAPI ew_snd_thread(LPVOID p)
{
    (void)p;
    while (g_sndOn)
        Beep((DWORD)g_sndFreq, 45);
    return 0;
}

void sound(unsigned frequency)
{
    g_sndFreq = (LONG)frequency;
    if (!g_sndOn && frequency >= 37) {
        g_sndOn = 1;
        if (g_sndThread != NULL)
            CloseHandle(g_sndThread);
        g_sndThread = CreateThread(NULL, 0, ew_snd_thread, NULL, 0, NULL);
    }
}

void nosound(void)
{
    g_sndOn = 0;
}

void delay(unsigned milliseconds)
{
    Sleep((DWORD)milliseconds);
}

/* ==================================================================
 * GUI: font, window, painting and cursor
 * ================================================================== */

static void ew_make_font(void)
{
    HFONT f;

    if (g_font != NULL)
        DeleteObject(g_font);
    f = CreateFontA(-g_fontheight, 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, g_fontface);
    if (f == NULL)
        f = (HFONT)GetStockObject(SYSTEM_FIXED_FONT);
    g_font = f;
}

static void ew_measure_font(void)
{
    HDC dc = GetDC(NULL);
    HFONT oldf;
    TEXTMETRICA tm;

    if (dc == NULL)
        return;
    oldf = (HFONT)SelectObject(dc, g_font);
    GetTextMetricsA(dc, &tm);
    g_cw = tm.tmAveCharWidth;
    g_chh = tm.tmHeight;
    SelectObject(dc, oldf);
    ReleaseDC(NULL, dc);
    if (g_cw < 4)  g_cw = 8;
    if (g_chh < 6) g_chh = 16;
}

static void ew_fit_window(void)
{
    RECT  r;
    DWORD style;

    if (g_hwnd == NULL)
        return;
    style = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);
    r.left = 0;
    r.top = 0;
    r.right = g_cols * g_cw;
    r.bottom = g_rows * g_chh;
    AdjustWindowRect(&r, style, FALSE);
    r.bottom += GetSystemMetrics(SM_CYMENU);   /* AdjustWindowRect ignores the menu bar */
    SetWindowPos(g_hwnd, NULL, 0, 0,
        r.right - r.left, r.bottom - r.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, NULL, TRUE);
}

static void ew_paint(HWND h)
{
    PAINTSTRUCT ps;
    HDC  dc;
    HFONT oldf;
    int  y, y0, y1;
    char  sb[EW_MAXCOLS];
    WCHAR wb[EW_MAXCOLS];

    dc = BeginPaint(h, &ps);
    if (dc == NULL)
        return;
    oldf = (HFONT)SelectObject(dc, g_font);
    SetBkMode(dc, OPAQUE);

    y0 = ps.rcPaint.top / g_chh;
    if (y0 < 0) y0 = 0;
    y1 = (ps.rcPaint.bottom + g_chh - 1) / g_chh;
    if (y1 > g_rows) y1 = g_rows;

    ew_lock();
    for (y = y0; y < y1; y++) {
        int xs = 0;
        int selrow = g_selOn && y >= g_sely0 && y <= g_sely1;
        while (xs < g_cols) {
            int len = 1, i, sel;
            unsigned char a = g_grid[y][xs].attr;
            sel = selrow && xs >= g_selx0 && xs <= g_selx1;
            while (xs + len < g_cols && g_grid[y][xs + len].attr == a &&
                (selrow && (xs + len) >= g_selx0 && (xs + len) <= g_selx1)
                == (sel != 0))
                len++;
            for (i = 0; i < len; i++)
                sb[i] = (char)g_grid[y][xs + i].ch;
            /* CP437/CP850: DOS box characters render correctly */
            MultiByteToWideChar((UINT)g_codepage, 0, sb, len, wb, len);
            if (sel) {           /* selection: invert colors */
                SetTextColor(dc, g_pal[(a >> 4) & 0x0F]);
                SetBkColor(dc, g_pal[a & 0x0F]);
            }
            else {
                SetTextColor(dc, g_pal[a & 0x0F]);
                SetBkColor(dc, g_pal[(a >> 4) & 0x0F]);
            }
            TextOutW(dc, xs * g_cw, y * g_chh, wb, len);
            xs += len;
        }
    }
    ew_unlock();

    SelectObject(dc, oldf);
    EndPaint(h, &ps);
}

static void ew_update_caret(void)
{
    int x, y;

    if (g_hwnd == NULL || g_caretkind == _NOCURSOR)
        return;
    ew_lock();
    x = g_x;
    y = g_y;
    ew_unlock();
    if (g_caretkind == _SOLIDCURSOR)
        SetCaretPos(x * g_cw, y * g_chh);
    else
        SetCaretPos(x * g_cw, y * g_chh + (g_chh - 2));
}

static void ew_create_caret(HWND h)
{
    if (g_caretkind == _NOCURSOR)
        return;
    if (g_caretkind == _SOLIDCURSOR)
        CreateCaret(h, NULL, g_cw, g_chh);
    else
        CreateCaret(h, NULL, g_cw, 2);
    ew_update_caret();
    ShowCaret(h);
}

static LRESULT CALLBACK ew_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CHAR:
        if ((int)wp == 0x16)                 /* Ctrl+V = paste */
            ew_paste();
        else if ((int)wp == 3 && g_selOn)    /* Ctrl+C = copy selection */
            ew_copy_selection();
        else
            ew_feed_char((unsigned)wp);      /* (Ctrl+C reaches the app
                                                when there is no selection) */
        return 0;
    case WM_KEYDOWN:
        ew_on_keydown(wp);
        return 0;
    case WM_COMMAND:
        ew_command(h, (int)LOWORD(wp));
        return 0;
    case WM_LBUTTONDOWN: {
        int cx = (int)(short)LOWORD(lp) / g_cw;
        int cy = (int)(short)HIWORD(lp) / g_chh;
        if (cx < 0) cx = 0;
        if (cx > g_cols - 1) cx = g_cols - 1;
        if (cy < 0) cy = 0;
        if (cy > g_rows - 1) cy = g_rows - 1;
        ew_lock();
        g_selOn = 1;                     /* anchor and start dragging */
        g_selax = cx;
        g_selay = cy;
        ew_sel_set(cx, cy);
        ew_unlock();
        SetCapture(h);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if ((wp & MK_LBUTTON) != 0 && g_selOn) {
            int cx = (int)(short)LOWORD(lp) / g_cw;
            int cy = (int)(short)HIWORD(lp) / g_chh;
            if (cx < 0) cx = 0;
            if (cx > g_cols - 1) cx = g_cols - 1;
            if (cy < 0) cy = 0;
            if (cy > g_rows - 1) cy = g_rows - 1;
            ew_lock();
            ew_sel_set(cx, cy);
            ew_unlock();
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_selOn) {
            ReleaseCapture();
            if (g_selx0 == g_selx1 && g_sely0 == g_sely1) {
                ew_lock();                /* a click without dragging: */
                g_selOn = 0;             /* deselect, don't copy */
                ew_unlock();
            }
            else {
                ew_copy_selection();     /* terminal style: auto-copy */
            }
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_CONTEXTMENU: {
        HMENU bar = GetMenu(h);
        HMENU edit = (bar != NULL) ? GetSubMenu(bar, 1) : NULL;
        if (edit != NULL)
            TrackPopupMenu(edit, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                (int)(short)LOWORD(lp), (int)(short)HIWORD(lp),
                0, h, NULL);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        ew_paint(h);
        return 0;
    case WM_TIMER:
        ew_update_caret();
        return 0;
    case WM_SETFOCUS:
        ew_create_caret(h);
        return 0;
    case WM_KILLFOCUS:
        HideCaret(h);
        DestroyCaret();
        return 0;
    case EW_WM_PAINTQ:
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case EW_WM_RESIZE:
        ew_fit_window();
        return 0;
    case EW_WM_FONT:
        ew_make_font();
        ew_measure_font();
        ew_fit_window();
        return 0;
    case EW_WM_CARET:
        if (GetFocus() == h) {
            HideCaret(h);
            DestroyCaret();
            ew_create_caret(h);
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ew_gui_thread(LPVOID p)
{
    WNDCLASSA wc;
    MSG  msg;
    RECT r;
    DWORD style;
    HINSTANCE hI = GetModuleHandleA(NULL);

    (void)p;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ew_wndproc;
    wc.hInstance = hI;
    /* Custom EasyWin32 icon from easywin32.rc if compiled in
     * (ID 1, or the Borland-style name "A"); standard icon otherwise */
    g_hIcon = LoadIconA(hI, MAKEINTRESOURCEA(1));
    if (g_hIcon == NULL)
        g_hIcon = LoadIconA(hI, "A");
    wc.hIcon = (g_hIcon != NULL) ? g_hIcon
        : LoadIconA(NULL, (LPCSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = "EasyWin32Wnd";
    RegisterClassA(&wc);

    ew_make_font();
    ew_measure_font();

    style = (WS_OVERLAPPEDWINDOW & ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX));
    r.left = 0;
    r.top = 0;
    r.right = g_cols * g_cw;
    r.bottom = g_rows * g_chh;
    AdjustWindowRect(&r, style, FALSE);
    r.bottom += GetSystemMetrics(SM_CYMENU);   /* room for the menu bar */

    g_hwnd = CreateWindowExA(0, "EasyWin32Wnd", g_title, style,
        CW_USEDEFAULT, CW_USEDEFAULT,
        r.right - r.left, r.bottom - r.top,
        NULL, NULL, hI, NULL);
    if (g_hwnd != NULL) {
        SetMenu(g_hwnd, ew_build_menu());   /* QuickWin-style menu bar */
        if (g_hIcon != NULL) {              /* apply the .rc icon */
            g_hIconS = (HICON)LoadImageA(hI, MAKEINTRESOURCEA(1),
                IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            if (g_hIconS == NULL)
                g_hIconS = (HICON)LoadImageA(hI, "A",
                    IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
            SendMessageA(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)g_hIcon);
            if (g_hIconS != NULL)
                SendMessageA(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)g_hIconS);
        }
        ShowWindow(g_hwnd, SW_SHOW);
        SetTimer(g_hwnd, 1, 60, NULL);
    }
    SetEvent(g_evtReady);
    if (g_hwnd == NULL)
        return 0;

    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}

/* ==================================================================
 * Classic conio API
 * (each public function self-bootstraps: ew_bootstrap())
 * ================================================================== */

void gotoxy(int x, int y)
{
    ew_bootstrap();
    ew_drain();
    ew_lock();
    if (x < 1) x = 1;
    if (y < 1) y = 1;
    g_x = g_wx0 + x - 1;
    if (g_x > g_wx1) g_x = g_wx1;
    g_y = g_wy0 + y - 1;
    if (g_y > g_wy1) g_y = g_wy1;
    ew_unlock();
}

int wherex(void)
{
    int v;
    ew_bootstrap();
    ew_lock();
    v = g_x - g_wx0 + 1;
    ew_unlock();
    return v;
}

int wherey(void)
{
    int v;
    ew_bootstrap();
    ew_lock();
    v = g_y - g_wy0 + 1;
    ew_unlock();
    return v;
}

void clrscr(void)
{
    int r, c;
    ew_bootstrap();
    ew_drain();
    ew_lock();
    for (r = g_wy0; r <= g_wy1; r++)
        for (c = g_wx0; c <= g_wx1; c++) {
            g_grid[r][c].ch = ' ';
            g_grid[r][c].attr = g_attr;
        }
    g_x = g_wx0;
    g_y = g_wy0;
    ew_unlock();
    ew_request_paint();
}

void clreol(void)
{
    int c;
    ew_bootstrap();
    ew_drain();
    ew_lock();
    for (c = g_x; c <= g_wx1; c++) {
        g_grid[g_y][c].ch = ' ';
        g_grid[g_y][c].attr = g_attr;
    }
    ew_unlock();
    ew_request_paint();
}

void window(int left, int top, int right, int bottom)
{
    ew_bootstrap();
    ew_drain();
    ew_lock();
    if (left < 1) left = 1;
    if (top < 1) top = 1;
    if (right > g_cols) right = g_cols;
    if (bottom > g_rows) bottom = g_rows;
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    g_wx0 = left - 1;
    g_wy0 = top - 1;
    g_wx1 = right - 1;
    g_wy1 = bottom - 1;
    g_x = g_wx0;
    g_y = g_wy0;
    ew_unlock();
    ew_request_paint();
}

void textcolor(int color)
{
    ew_bootstrap();
    ew_drain();
    ew_lock();
    g_attr = (unsigned char)((g_attr & 0xF0) | (color & 0x0F));
    ew_unlock();
}

void textbackground(int color)
{
    ew_bootstrap();
    ew_drain();
    ew_lock();
    g_attr = (unsigned char)((g_attr & 0x0F) | ((color & 0x07) << 4));
    ew_unlock();
}

void textattr(int attr)
{
    ew_bootstrap();
    ew_drain();
    ew_lock();
    g_attr = (unsigned char)(attr & 0xFF);
    ew_unlock();
}

void highvideo(void)
{
    ew_bootstrap();
    ew_lock();
    g_attr |= 0x08;
    ew_unlock();
}

void lowvideo(void)
{
    ew_bootstrap();
    ew_lock();
    g_attr &= 0xF7;
    ew_unlock();
}

void normvideo(void)
{
    ew_bootstrap();
    ew_lock();
    g_attr = g_normattr;
    ew_unlock();
}

void delline(void)
{
    int r, c, wbytes;
    EWCELL fill;

    ew_bootstrap();
    ew_drain();
    ew_lock();
    wbytes = (g_wx1 - g_wx0 + 1) * (int)sizeof(EWCELL);
    for (r = g_y; r < g_wy1; r++)
        memcpy(&g_grid[r][g_wx0], &g_grid[r + 1][g_wx0], (size_t)wbytes);
    fill.ch = ' ';
    fill.attr = g_attr;
    for (c = g_wx0; c <= g_wx1; c++)
        g_grid[g_wy1][c] = fill;
    ew_unlock();
    ew_request_paint();
}

void insline(void)
{
    int r, c, wbytes;
    EWCELL fill;

    ew_bootstrap();
    ew_drain();
    ew_lock();
    wbytes = (g_wx1 - g_wx0 + 1) * (int)sizeof(EWCELL);
    for (r = g_wy1; r > g_y; r--)
        memcpy(&g_grid[r][g_wx0], &g_grid[r - 1][g_wx0], (size_t)wbytes);
    fill.ch = ' ';
    fill.attr = g_attr;
    for (c = g_wx0; c <= g_wx1; c++)
        g_grid[g_y][c] = fill;
    ew_unlock();
    ew_request_paint();
}

/* gettext/puttext use Turbo C's format: pairs of character byte +
 * attribute byte, row by row. Absolute screen coordinates
 * (1-based). */

int gettext(int left, int top, int right, int bottom, void* dest)
{
    unsigned char* d = (unsigned char*)dest;
    int i, j;

    ew_bootstrap();
    if (dest == NULL || left < 1 || top < 1 ||
        right > g_cols || bottom > g_rows ||
        left > right || top > bottom)
        return 0;
    ew_lock();
    for (j = top; j <= bottom; j++)
        for (i = left; i <= right; i++) {
            *d++ = g_grid[j - 1][i - 1].ch;
            *d++ = g_grid[j - 1][i - 1].attr;
        }
    ew_unlock();
    return 1;
}

int puttext(int left, int top, int right, int bottom, const void* src)
{
    const unsigned char* s = (const unsigned char*)src;
    int i, j;

    ew_bootstrap();
    if (src == NULL || left < 1 || top < 1 ||
        right > g_cols || bottom > g_rows ||
        left > right || top > bottom)
        return 0;
    ew_lock();
    for (j = top; j <= bottom; j++)
        for (i = left; i <= right; i++) {
            g_grid[j - 1][i - 1].ch = *s++;
            g_grid[j - 1][i - 1].attr = *s++;
        }
    ew_unlock();
    ew_request_paint();
    return 1;
}

int movetext(int left, int top, int right, int bottom, int dx, int dy)
{
    static unsigned char tmp[EW_MAXCOLS * EW_MAXROWS * 2];

    ew_bootstrap();
    if (gettext(left, top, right, bottom, tmp) == 0)
        return 0;
    if (left + dx < 1 || top + dy < 1 ||
        right + dx > g_cols || bottom + dy > g_rows)
        return 0;
    return puttext(left + dx, top + dy, right + dx, bottom + dy, tmp);
}

void gettextinfo(struct text_info* info)
{
    if (info == NULL)
        return;
    ew_bootstrap();
    ew_lock();
    info->winleft = (unsigned char)(g_wx0 + 1);
    info->wintop = (unsigned char)(g_wy0 + 1);
    info->winright = (unsigned char)(g_wx1 + 1);
    info->winbottom = (unsigned char)(g_wy1 + 1);
    info->attribute = g_attr;
    info->normattr = g_normattr;
    info->curx = (unsigned char)(g_x - g_wx0 + 1);
    info->cury = (unsigned char)(g_y - g_wy0 + 1);
    info->screenheight = (unsigned short)g_rows;
    info->screenwidth = (unsigned short)g_cols;
    ew_unlock();
}

void _setcursortype(int cursor_type)
{
    g_caretkind = cursor_type;
    if (g_hwnd != NULL)
        PostMessageA(g_hwnd, EW_WM_CARET, 0, 0);
}

int cprintf(const char* fmt, ...)
{
    char    buf[2048];
    va_list ap;
    int     n;

    va_start(ap, fmt);
    n = EW_VSNPRINTF(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0)
        return n;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    ew_bootstrap();
    ew_drain();
    ew_emit(buf, n);
    return n;
}

int cputs(const char* s)
{
    int n;

    if (s == NULL)
        return 0;
    n = (int)strlen(s);
    ew_bootstrap();
    ew_drain();
    ew_emit(s, n);
    return (n > 0) ? (int)(unsigned char)s[n - 1] : 0;
}

int putch(int c)
{
    char t = (char)c;
    ew_bootstrap();
    ew_drain();
    ew_emit(&t, 1);
    return (int)(unsigned char)c;
}

int kbhit(void)
{
    int n;

    if (!g_run)
        return 0;
    ew_lock();
    n = (g_kqTail - g_kqHead) & EW_KQMASK;
    ew_unlock();
    return n != 0;
}

static int ew_getch_impl(int echo)
{
    int c = 0;

    if (!g_run)
        return -1;
    ew_drain();
    for (;;) {
        if (WaitForSingleObject(g_semKeys, INFINITE) != WAIT_OBJECT_0)
            return -1;
        ew_lock();
        if (ew_kq_pop_locked(&c)) {
            ew_unlock();
            break;
        }
        ew_unlock();
    }
    /* the echo already happened while typing; getche only adds one
     * if it was disabled with ew_setecho(0) */
    if (echo && !g_echo) {
        char t = (char)c;
        ew_drain();
        ew_emit(&t, 1);
    }
    return (int)(unsigned char)c;
}

int getch(void)
{
    return ew_getch_impl(0);
}

int getche(void)
{
    return ew_getch_impl(1);
}

/* ==================================================================
 * ew_* extensions
 * ================================================================== */

void ew_settitle(const char* title)
{
    if (title == NULL)
        return;
    strncpy(g_title, title, sizeof(g_title) - 1);
    g_title[sizeof(g_title) - 1] = 0;
    if (g_hwnd != NULL)
        PostMessageA(g_hwnd, WM_SETTEXT, 0, (LPARAM)g_title);
}

int ew_resize(int cols, int rows)
{
    int r, c;

    if (cols < 8 || rows < 4)
        return 0;
    if (cols > EW_MAXCOLS) cols = EW_MAXCOLS;
    if (rows > EW_MAXROWS) rows = EW_MAXROWS;
    ew_lock();
    g_cols = cols;
    g_rows = rows;
    g_wx0 = 0; g_wy0 = 0;
    g_wx1 = cols - 1; g_wy1 = rows - 1;
    g_x = 0; g_y = 0;
    for (r = 0; r < EW_MAXROWS; r++)
        for (c = 0; c < EW_MAXCOLS; c++) {
            g_grid[r][c].ch = ' ';
            g_grid[r][c].attr = g_attr;
        }
    ew_unlock();
    if (g_hwnd != NULL)
        PostMessageA(g_hwnd, EW_WM_RESIZE, 0, 0);
    ew_request_paint();
    return 1;
}

int ew_setfont(const char* facename, int height)
{
    if (facename != NULL) {
        strncpy(g_fontface, facename, sizeof(g_fontface) - 1);
        g_fontface[sizeof(g_fontface) - 1] = 0;
    }
    if (height > 0)
        g_fontheight = height;
    if (g_hwnd != NULL)
        PostMessageA(g_hwnd, EW_WM_FONT, 0, 0);
    return 1;
}

void ew_setcodepage(unsigned codepage)
{
    g_codepage = (UINT)codepage;
    ew_request_paint();
}

void ew_setecho(int on)
{
    g_echo = on ? 1 : 0;
}

HWND ew_hwnd(void)
{
    return g_hwnd;
}

const char* ew_version(void)
{
    return EASYWIN32_VERSION;
}

/* ==================================================================
 * Startup and shutdown
 * ================================================================== */

 /* ------------------------------------------------------------------
  * stdio plumbing. Two independent strategies, both pure ANSI C plus
  * plain Win32:
  *
  *   1) anonymous pipes + _open_osfhandle/_dup2 - the classic fd-level
  *      trick, accepted by every compiler's CRT since forever;
  *   2) named pipes + freopen - the only stdout redirect the modern
  *      UCRT officially supports for GUI executables, where the
  *      standard streams are born without real handles and _dup2 can
  *      quietly fail (that is exactly what happens with Visual C++
  *      2015 and later x64 GUI builds on some machines).
  *
  * Both are proven with a live probe: one byte is pushed through the
  * real stdout and must arrive in the pipe the reader thread will
  * drain. A strategy that "succeeds" but still swallows printf
  * output is therefore impossible; if both fail, a one-time warning
  * says exactly which step broke, and the About box shows which path
  * won when everything is fine.
  * ------------------------------------------------------------------ */

  /* Close *ph if it is a live handle and clear it, so a handle can
   * never be closed twice: with a debugger attached, CloseHandle on
   * an already-closed handle raises STATUS_INVALID_HANDLE. */
static void ew_close_h(HANDLE* ph)
{
    HANDLE h = *ph;

    if (h != NULL && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    *ph = NULL;
}

/* Push one harmless byte through the real stdout and check that it
 * lands in pipe h, then discard it. Called on a fresh, empty screen,
 * so a stray CR would be invisible anyway. */
static int ew_probe_stdout(HANDLE h)
{
    char  c;
    DWORD avail = 0, n = 0;
    int   i;

    if (fwrite("\r", 1, 1, stdout) != 1 || fflush(stdout) != 0)
        return 0;
    for (i = 0; i < 60; i++) {              /* up to ~300 ms */
        avail = 0;
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
            return 0;
        if (avail > 0)
            break;
        Sleep(5);
    }
    if (avail == 0)
        return 0;
    if (!ReadFile(h, &c, 1, &n, NULL) || n != 1)
        return 0;
    return 1;
}

/* One-time warning when printf/scanf could not be connected at all */
static void ew_report_stdio(void)
{
    char msg[512];

    if (InterlockedExchange(&g_diagShown, 1) != 0)
        return;
    if (g_diag[0] == 0)
        wsprintfA(g_diag, "unknown failure (GetLastError %lu)",
            (unsigned long)GetLastError());
    wsprintfA(msg,
        "EasyWin32 could not connect printf/scanf to the window.\n\n"
        "Reason: %s\n\n"
        "The window and the conio functions still work, but printf,\n"
        "scanf and the other stdio functions do not.\n\n"
        "Please report this message to the author.", g_diag);
    MessageBoxA(g_hwnd, msg, "EasyWin32 - stdio warning",
        MB_OK | MB_ICONWARNING);
}

/* What the About box shows for the transport in use */
static const char* ew_stdmode_text(void)
{
    switch (g_stdMode) {
    case 1:  return "anonymous pipes (dup2)";
    case 2:  return "named pipes (freopen)";
    case 3:  return "stdio not connected";
    default: return "not initialized";
    }
}

/* Creates the pipes and redirects stdout/stderr/stdin. Returns 1 and
 * leaves g_hOutR/g_hErrR/g_hInW pointing at the winning pipes. On
 * failure everything is closed and the handles are NULL again. */
static int ew_setup_stdio(void)
{
    SECURITY_ATTRIBUTES sa;
    char  pname[64];
    unsigned long pid;
    FILE* fo, * fe, * fi;
    int   fdOut, fdIn, stdOutOk, stdInOk;

    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = FALSE;

    /* ---- strategy 1: anonymous pipes + _open_osfhandle/_dup2 ---- */
    g_hOutR = NULL; g_hOutW = NULL; g_hInR = NULL; g_hInW = NULL;
    if (CreatePipe(&g_hOutR, &g_hOutW, &sa, 65536))
        (void)CreatePipe(&g_hInR, &g_hInW, &sa, 65536);

    if (g_hOutR != NULL && g_hOutW != NULL &&
        g_hInR != NULL && g_hInW != NULL) {
        fdOut = fdIn = -1;
        stdOutOk = stdInOk = 0;

        /* Ownership rule that keeps CloseHandle happy: once _dup2
         * succeeds, the CRT owns the OS handle (refcounted by the
         * fd table) and we must never CloseHandle it ourselves; a
         * later freopen closes the stream and the handle cleanly.
         * When _dup2 fails, _close(fd) is what really closes the
         * handle, so the variable is nulled right away. */
        fdOut = _open_osfhandle(EW_OSFH(g_hOutW), O_WRONLY | O_BINARY);
        if (fdOut < 0) {
            wsprintfA(g_diag, "_open_osfhandle(stdout) failed, errno %d",
                (int)errno);
        }
        else if (_dup2(fdOut, 1) != 0) {
            wsprintfA(g_diag, "_dup2(fd, 1) failed, errno %d", (int)errno);
            _close(fdOut);              /* fd 1 never took the handle */
            g_hOutW = NULL;
        }
        else {
            stdOutOk = 1;                /* fd 1 owns g_hOutW now */
            (void)_dup2(fdOut, 2);       /* stderr: best effort */
            _close(fdOut);               /* refcount keeps it open */
        }

        if (stdOutOk) {
            fdIn = _open_osfhandle(EW_OSFH(g_hInR), O_RDONLY | O_BINARY);
            if (fdIn < 0) {
                wsprintfA(g_diag,
                    "_open_osfhandle(stdin) failed, errno %d", (int)errno);
            }
            else if (_dup2(fdIn, 0) != 0) {
                wsprintfA(g_diag, "_dup2(fd, 0) failed, errno %d",
                    (int)errno);
                _close(fdIn);            /* fd 0 never took the handle */
                g_hInR = NULL;
            }
            else {
                stdInOk = 1;              /* fd 0 owns g_hInR now */
                _close(fdIn);             /* refcount keeps it open */
            }
        }

        if (stdOutOk && stdInOk) {
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            if (ew_probe_stdout(g_hOutR)) {
                g_stdMode = 1;
                return 1;
            }
            wsprintfA(g_diag,
                "stdout probe failed, errno %d, GetLastError %lu",
                (int)errno, (unsigned long)GetLastError());
        }
        /* Tear down what the CRT does not own. Handles the CRT
         * already owns stay open: freopen in strategy 2 closes
         * them properly when it rebinds the streams. */
        ew_close_h(&g_hOutR);            /* reader end: never CRT's */
        ew_close_h(&g_hInW);             /* feeder end: never CRT's */
        if (!stdOutOk)
            ew_close_h(&g_hOutW);
        if (!stdInOk)
            ew_close_h(&g_hInR);
    }
    else {
        wsprintfA(g_diag, "CreatePipe failed, GetLastError %lu",
            (unsigned long)GetLastError());
        ew_close_h(&g_hOutR); ew_close_h(&g_hOutW);
        ew_close_h(&g_hInR);  ew_close_h(&g_hInW);
    }

    /* ---- strategy 2: named pipes + freopen (UCRT-safe) ----
     * Each stream gets its own pipe back to EasyWin32: the reader
     * thread drains stdout and stderr, the feeder thread pushes
     * typed lines into stdin. freopen is the redirect the UCRT
     * documents for executables without a console. */
    pid = (unsigned long)GetCurrentProcessId();

    wsprintfA(pname, "\\\\.\\pipe\\easyw32o_%lu", pid);
    g_hOutR = CreateNamedPipeA(pname, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
        PIPE_WAIT, 1, 0, 65536, 0, NULL);
    wsprintfA(pname, "\\\\.\\pipe\\easyw32e_%lu", pid);
    g_hErrR = CreateNamedPipeA(pname, PIPE_ACCESS_INBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
        PIPE_WAIT, 1, 0, 65536, 0, NULL);
    wsprintfA(pname, "\\\\.\\pipe\\easyw32i_%lu", pid);
    g_hInW = CreateNamedPipeA(pname, PIPE_ACCESS_OUTBOUND,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
        PIPE_WAIT, 1, 65536, 0, 0, NULL);

    if (g_hOutR == INVALID_HANDLE_VALUE || g_hOutR == NULL ||
        g_hInW == INVALID_HANDLE_VALUE || g_hInW == NULL) {
        wsprintfA(g_diag, "CreateNamedPipe failed, GetLastError %lu",
            (unsigned long)GetLastError());
    }
    else {
        if (g_hErrR == INVALID_HANDLE_VALUE || g_hErrR == NULL)
            g_hErrR = NULL;               /* stderr stays direct, no drama */

        wsprintfA(pname, "\\\\.\\pipe\\easyw32o_%lu", pid);
        fo = freopen(pname, "wb", stdout);
        wsprintfA(pname, "\\\\.\\pipe\\easyw32e_%lu", pid);
        fe = (g_hErrR != NULL) ? freopen(pname, "wb", stderr) : NULL;
        wsprintfA(pname, "\\\\.\\pipe\\easyw32i_%lu", pid);
        fi = freopen(pname, "rb", stdin);

        if (fo == NULL) {
            wsprintfA(g_diag,
                "freopen(stdout to named pipe) failed, errno %d",
                (int)errno);
        }
        else if (fi == NULL) {
            wsprintfA(g_diag,
                "freopen(stdin to named pipe) failed, errno %d",
                (int)errno);
        }
        else {
            if (fe == NULL && g_hErrR != NULL) {
                CloseHandle(g_hErrR);
                g_hErrR = NULL;
            }
            setvbuf(stdout, NULL, _IONBF, 0);
            setvbuf(stderr, NULL, _IONBF, 0);
            if (ew_probe_stdout(g_hOutR)) {
                g_stdMode = 2;
                return 1;
            }
            wsprintfA(g_diag,
                "stdout probe (named pipe) failed, GetLastError %lu",
                (unsigned long)GetLastError());
        }
    }

    /* total failure: close everything and let the caller report it */
    ew_close_h(&g_hOutR);
    ew_close_h(&g_hErrR);
    ew_close_h(&g_hInW);
    return 0;
}

int ew_init(void)
{
    if (g_run)
        return 1;

    ew_ensure_lock();
    g_semKeys = CreateSemaphoreA(NULL, 0, 4096, NULL);
    g_semLines = CreateSemaphoreA(NULL, 0, EW_NLINES, NULL);
    g_evtReady = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_semKeys == NULL || g_semLines == NULL || g_evtReady == NULL)
        return 0;

    g_threadGui = CreateThread(NULL, 0, ew_gui_thread, NULL, 0, &g_tidGui);
    if (g_threadGui == NULL)
        return 0;
    WaitForSingleObject(g_evtReady, INFINITE);
    if (g_hwnd == NULL)
        return 0;

    if (!ew_setup_stdio()) {
        g_stdMode = 3;
        ew_report_stdio();   /* printf/scanf dead; conio still works */
    }

    InterlockedExchange(&g_readerIdle, 0);
    if (g_hOutR != NULL)
        g_threadReader = CreateThread(NULL, 0, ew_reader, NULL, 0, NULL);
    if (g_hErrR != NULL)
        g_threadErr = CreateThread(NULL, 0, ew_reader, (LPVOID)g_hErrR,
            0, NULL);
    if (g_hInW != NULL)
        g_threadFeeder = CreateThread(NULL, 0, ew_feeder, NULL, 0, NULL);
    g_run = 1;

    if ((g_hOutR != NULL && g_threadReader == NULL) ||
        (g_hInW != NULL && g_threadFeeder == NULL)) {
        g_stdMode = 3;
        if (g_diag[0] == 0)
            wsprintfA(g_diag, "CreateThread failed, GetLastError %lu",
                (unsigned long)GetLastError());
        ew_report_stdio();
    }
    return 1;
}

void ew_shutdown(void)
{
    HANDLE h;

    if (InterlockedExchange(&g_shutdownDone, 1) != 0)
        return;
    fflush(stdout);
    fflush(stderr);
    ew_drain();
    g_run = 0;

    if (g_hwnd != NULL)
        PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    h = g_hOutR;
    g_hOutR = NULL;                 /* wakes up the reader thread */
    if (h != NULL)
        CloseHandle(h);
    h = g_hErrR;
    g_hErrR = NULL;                 /* wakes up the stderr reader */
    if (h != NULL)
        CloseHandle(h);

    if (g_threadReader != NULL) {
        WaitForSingleObject(g_threadReader, 2000);
        CloseHandle(g_threadReader);
        g_threadReader = NULL;
    }
    if (g_threadErr != NULL) {
        WaitForSingleObject(g_threadErr, 2000);
        CloseHandle(g_threadErr);
        g_threadErr = NULL;
    }
    if (g_threadFeeder != NULL) {
        ReleaseSemaphore(g_semLines, 1, NULL);
        if (WaitForSingleObject(g_threadFeeder, 500) == WAIT_OBJECT_0) {
            CloseHandle(g_threadFeeder);
            g_threadFeeder = NULL;
        }
    }
    if (g_threadGui != NULL) {
        WaitForSingleObject(g_threadGui, 2000);
        CloseHandle(g_threadGui);
        g_threadGui = NULL;
    }
}

/* argc/argv from GetCommandLineA */
static void ew_parse_cmdline(void)
{
    char* p = g_cmdBuf;
    const char* src = GetCommandLineA();
    size_t n = strlen(src);

    if (n > sizeof(g_cmdBuf) - 1)
        n = sizeof(g_cmdBuf) - 1;
    memcpy(g_cmdBuf, src, n);
    g_cmdBuf[n] = 0;

    g_argc = 0;
    while (*p != 0 && g_argc < 512 - 1) {
        char* start;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == 0)
            break;
        start = p;
        if (*p == '"') {
            p++;
            start = p;
            while (*p != 0 && *p != '"')
                p++;
        }
        else {
            while (*p != 0 && *p != ' ' && *p != '\t')
                p++;
        }
        if (*p != 0) {
            *p = 0;
            p++;
        }
        g_argv[g_argc++] = start;
    }
    g_argv[g_argc] = NULL;
}

/* ------------------------------------------------------------------
 * Library entry point: compiled unless you define
 * EASYWIN32_NO_ENTRY (for example, if your app already has a
 * WinMain and calls ew_init()/ew_shutdown() manually).
 * ------------------------------------------------------------------ */
#ifndef EASYWIN32_NO_ENTRY

extern int main(int argc, char* argv[]);

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int nShow)
{
    (void)hInst; (void)hPrev; (void)cmd; (void)nShow;

    if (!ew_init())
        return 1;
    atexit(ew_shutdown);          /* also covers the user's exit() */
    ew_parse_cmdline();
    return main(g_argc, g_argv);
}

#endif /* EASYWIN32_NO_ENTRY */