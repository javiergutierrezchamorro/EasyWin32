/* ==================================================================
 * EasyWin32.h - a modern "EasyWin" for Win32 and Win64
 * ------------------------------------------------------------------
 * Recreates the idea of Borland C++'s EasyWin library (Win16):
 * compile DOS-style text applications as Windows GUI applications,
 * with the output shown in an 80x25 color text window. It also
 * includes the extras Microsoft's QuickWin used to provide: a menu
 * bar (File/Edit/View/Help), mouse text selection, clipboard
 * copy/paste (Ctrl+C / Ctrl+V) and a right-click context menu.
 *
 * Works with MSVC (x86/x64), Borland C++ 5.x, Open Watcom and
 * MinGW. Clean-room implementation: it contains no original code
 * from Borland or Microsoft QuickWin; it only replicates the
 * documented Turbo C conio API and stdio behavior.
 *
 * USAGE:
 *   - Add easywin32.c and easywin32.h to your project.
 *   - Compile as a GUI application (subsystem WINDOWS).
 *   - Write your program as always: int main(int, char**).
 *     The library provides WinMain, which creates the window and
 *     calls your main(). Do NOT define your own WinMain (or compile
 *     easywin32.c with -DEASYWIN32_NO_ENTRY).
 *   - printf/puts/fprintf/std::cout go to the window.
 *     scanf/gets/std::cin read from the window's keyboard.
 *   - Do NOT include your compiler's <conio.h> in the same
 *     project; easywin32.h already declares its own conio.
 *   - Optional: add easywin32.rc (with easywin32.ico beside it)
 *     to your project to get the EasyWin32 icon in the title bar
 *     and taskbar, plus the version resource in the properties.
 * ================================================================== */
#ifndef EASYWIN32_H
#define EASYWIN32_H

/* ------------------------------------------------------------------
 * MSVC deprecates the classic DOS-era CRT functions (scanf, strcpy,
 * getch...) with warning C4996. DOS-style code needs them, so:
 *  - if easywin32.h is included before the CRT headers, the two
 *    _CRT_*_NO_WARNINGS defines below remove the deprecation;
 *  - the pragma also works if <stdio.h> was included earlier,
 *    because C4996 fires at the call site.
 * (If you dislike pragmas, define _CRT_SECURE_NO_WARNINGS project-
 * wide instead; then easywin32.h changes nothing for you.)
 * ------------------------------------------------------------------ */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#endif
#if defined(_MSC_VER)
#pragma warning(disable : 4996)   /* deprecated CRT functions */
#endif

#include <windows.h>

/* Library version, also shown in Help/About and in the .rc */
#define EASYWIN32_VERSION "1.04"

#ifdef __cplusplus
extern "C" {
#endif

/* --- conio colors (classic Turbo C / Borland values) --- */
#ifndef EASYWIN32_NO_COLOR_NAMES
#ifndef BLACK
#define BLACK          0
#define BLUE           1
#define GREEN          2
#define CYAN           3
#define RED            4
#define MAGENTA        5
#define BROWN          6
#define LIGHTGRAY      7
#define DARKGRAY       8
#define LIGHTBLUE      9
#define LIGHTGREEN     10
#define LIGHTCYAN      11
#define LIGHTRED       12
#define LIGHTMAGENTA   13
#define YELLOW         14
#define WHITE          15
#endif /* BLACK */
#endif /* EASYWIN32_NO_COLOR_NAMES */

/* --- Cursor types (same as Borland/MSVC conio.h) --- */
#if !defined(_NOCURSOR) && !defined(_INC_CONIO) && !defined(__CONIO_H)
#define _NOCURSOR      0
#define _SOLIDCURSOR   1
#define _NORMALCURSOR  2
#endif

/* --- struct text_info (compatible with Borland's) --- */
#if !defined(__BORLANDC__)
struct text_info {
    unsigned char  winleft;       /* active window, 1-based          */
    unsigned char  wintop;
    unsigned char  winright;
    unsigned char  winbottom;
    unsigned char  attribute;     /* current text attribute          */
    unsigned char  normattr;      /* startup attribute               */
    unsigned char  curx;          /* cursor inside the window        */
    unsigned char  cury;
    unsigned short screenheight;  /* screen height in cells           */
    unsigned short screenwidth;   /* screen width in cells           */
};
#endif

/* ==================================================================
 * Classic conio (gotoxy, colors, text windows...)
 * 1-based coordinates, relative to the active window, as in DOS.
 * ================================================================== */
void clrscr(void);
void clreol(void);
void gotoxy(int x, int y);
int  wherex(void);
int  wherey(void);
void window(int left, int top, int right, int bottom);
void textcolor(int color);
void textbackground(int color);
void textattr(int attr);
void highvideo(void);
void lowvideo(void);
void normvideo(void);
void delline(void);
void insline(void);
int  gettext(int left, int top, int right, int bottom, void *dest);
int  puttext(int left, int top, int right, int bottom, const void *src);
int  movetext(int left, int top, int right, int bottom, int dx, int dy);
void gettextinfo(struct text_info *info);
void _setcursortype(int cursor_type);

int  cprintf(const char *fmt, ...);   /* printf using current attribute */
int  cputs(const char *s);            /* fputs to the window            */
int  putch(int c);

int  kbhit(void);
int  getch(void);                     /* wait for a key                */
int  getche(void);                    /* getch with echo (see ew_setecho) */

void delay(unsigned milliseconds);
void sound(unsigned frequency);       /* tone until nosound()          */
void nosound(void);

/* ==================================================================
 * EasyWin32 extensions
 * ================================================================== */
int  ew_init(void);                   /* create window + redirect stdio     */
void ew_shutdown(void);               /* close window and clean up (idempotent) */
int  ew_resize(int cols, int rows);    /* 80x25 default; max 256x128 (clears) */
int  ew_setfont(const char *facename, int height); /* e.g. "Lucida Console", 16 */
void ew_setcodepage(unsigned codepage); /* render codepage: 437 default (DOS), 1252, 850, 65001... */
void ew_settitle(const char *title);
void ew_setecho(int on);              /* keyboard echo: 1 default; 0 for passwords */
HWND ew_hwnd(void);
const char *ew_version(void);          /* library version, e.g. "1.04" */

#ifdef __cplusplus
}
#endif

#endif /* EASYWIN32_H */
