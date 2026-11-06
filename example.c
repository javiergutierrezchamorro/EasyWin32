/* ==================================================================
 * example.c - EasyWin32 demonstration
 * ------------------------------------------------------------------
 * A plain old-school text program (main + conio + stdio) compiled
 * as a Windows GUI application. The box characters (0xC9, 0xCD...)
 * are CP437, which is the window's default codepage, so they look
 * exactly like they did on DOS.
 *
 * Build it (see README), for example with MSVC:
 *   cl /nologo example.c easywin32.c /link /subsystem:windows user32.lib gdi32.lib
 * ================================================================== */
#include "easywin32.h"
#include <stdio.h>

/* double-line box using DOS (CP437) box-drawing characters */
static void caja(int x, int y, int w, int h)
{
    int i;

    gotoxy(x, y);
    putch(0xC9);
    for (i = 0; i < w - 2; i++) putch(0xCD);
    putch(0xBB);
    for (i = 1; i < h - 1; i++) {
        gotoxy(x, y + i);          putch(0xBA);
        gotoxy(x + w - 1, y + i);  putch(0xBA);
    }
    gotoxy(x, y + h - 1);
    putch(0xC8);
    for (i = 0; i < w - 2; i++) putch(0xCD);
    putch(0xBC);
}

int main(int argc, char *argv[])
{
    char name[64];
    int  i, key;

    (void)argc; (void)argv;
    ew_settitle("EasyWin32 demo");

    /* blue screen, DOS style */
    textbackground(BLUE);
    textcolor(LIGHTGRAY);
    clrscr();
    caja(1, 1, 80, 25);

    textcolor(YELLOW);
    gotoxy(34, 3);  cprintf("EASYWIN32  DEMO");
    textcolor(LIGHTCYAN);
    gotoxy(22, 4);  cprintf("A modern EasyWin for Win32 and Win64");

    /* plain printf redirected to the window */
    textcolor(LIGHTGRAY);
    gotoxy(4, 6);
    printf("Standard output (printf/puts/fprintf/cout) is redirected.\n");
    printf("Standard input (scanf/cin) reads from the window's keyboard.\n");
    printf("This is EasyWin32 v%s.\n", EASYWIN32_VERSION);
    printf("Drag the mouse to select text (auto-copied), right-click for the menu.\n");

    /* show all 16 foreground/background colors */
    textcolor(WHITE);
    gotoxy(4, 9);  cprintf("CGA colors: ");
    for (i = 0; i < 16; i++) {
        textattr((i | (i << 4)));
        cprintf(" %02X ", i);
    }
    textattr(LIGHTGRAY | (BLUE << 4));

    /* sub-window with its own scrolling: same as DOS window() */
    window(30, 12, 78, 22);
    textbackground(BLACK);
    textcolor(LIGHTGREEN);
    clrscr();
    printf("Sub-window with window(): 49 columns x 11 rows.\n");
    for (i = 1; i <= 15; i++)
        printf("Line %2d (automatic scrolling inside the sub-window).\n", i);
    window(1, 1, 80, 25);
    textbackground(BLUE);

    /* input through scanf (with automatic echo as you type) */
    textcolor(YELLOW);
    gotoxy(4, 23); cprintf("What is your name? ");
    textcolor(WHITE);
    fflush(stdout);
    if (scanf("%63s", name) != 1)
        name[0] = 0;
    textcolor(LIGHTMAGENTA);
    cprintf("\nHello, %s!", name);

    /* raw and extended keys: arrows, Home, F1..F12... */
    textcolor(LIGHTGRAY);
    gotoxy(4, 24);
    cprintf("Press arrows / F1..F12 / letters. ENTER or ESC to continue.");
    for (;;) {
        key = getch();
        if (key == '\r' || key == 27)
            break;
        if (key == 0) {                     /* extended key */
            key = getch();
            textcolor(LIGHTRED);
            cprintf("[ext %d]  ", key);
        } else {
            textcolor(LIGHTCYAN);
            cprintf("[%d '%c']  ", key, (key >= ' ') ? key : '?');
        }
    }

    /* beep and pause, DOS style */
    clreol();
    sound(800);
    delay(250);
    nosound();

    gotoxy(4, 24);
    textcolor(YELLOW);
    cprintf("End of the demo. Press any key to exit...");
    getch();
    return 0;
}
