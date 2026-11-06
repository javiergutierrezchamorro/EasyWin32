# EasyWin32: a modern "EasyWin" for Win32 and Win64

Version **1.04**.

It recreates the idea of Borland C++'s **EasyWin** library (which only ever worked on Win16): recompile DOS-style text applications as Windows **GUI** applications, with the output painted into an 80×25 (resizable) text window with colors, cursor and keyboard.

It also adds the extras Microsoft's **QuickWin** used to provide over EasyWin: a classic menu bar (File / Edit / View / Help), mouse text selection and clipboard copy/paste.

It is **pure ANSI C on top of the Win32 API**, with no compiler-specific extensions, so it compiles for 32 and 64 bits with:

| Compiler | Command |
| --- | --- |
| MSVC x86/x64 | `cl /nologo app.c easywin32.c /link /subsystem:windows user32.lib gdi32.lib kernel32.lib` |
| Borland C++ 5.x / C++Builder | `bcc32 -tW app.c easywin32.c` |
| Open Watcom (x86) | `wcl386 -bt=nt -l=nt_win app.c easywin32.c user32 gdi32` |
| MinGW-w64 x86/x64 | `gcc -mwindows -o app.exe app.c easywin32.c -lgdi32 -luser32` |

> Legal note: it contains no code from Borland or from Microsoft QuickWin, both of which are proprietary (EasyWin's sources were never published). This is a clean-room implementation that replicates the documented Turbo C `conio` API and stdio behavior.

## Package contents

| File | Purpose |
| --- | --- |
| `easywin32.h` / `easywin32.c` | the library |
| `easywin32.rc` / `easywin32.ico` | optional: EasyWin32 icon (a DOS window with a `C:\>` prompt, 16×16 + 32×32) and the 1.04 version resource |
| `example.c` | demo program |
| `README.md` | this file |

## How it works

1. The library provides `WinMain`: it creates the window on its own GUI thread, redirects **stdout, stderr and stdin** into pipes (descriptor-level `_dup2`, with a verified `freopen` + named-pipe fallback) and calls your `main(argc, argv)`.
2. A reader thread pours the pipe into a grid of cells (character + attribute) with scrolling, colors and basic **ANSI sequence** decoding (an extra over EasyWin).
3. The keyboard feeds a single queue: `kbhit`/`getch`/`getche` read it raw (with 0 + scan-code extended codes for arrows, Home, End, PgUp, PgDn, Ins, Del and F1-F12), and on Enter the pending line is also delivered to `stdin` (`scanf`, `gets`, `std::cin`).
4. The conio functions (`gotoxy`, `textcolor`, `cprintf`...) write to the same grid with the same cursor `printf` uses, draining the pipe first to preserve DOS ordering.
5. A QuickWin-style **menu bar** (File / Edit / View / Help) offers Copy, Paste, Select All, Clear Screen, font sizes and the rendering codepage. Dragging with the mouse selects text (shown inverted, auto-copied to the clipboard on release), and right-click opens a context menu with the Edit commands.

## How to use it

- Add `easywin32.c` and `easywin32.h` to the project and compile as a **GUI application** (subsystem WINDOWS).
- Recommended: also add `easywin32.rc` to the project (with `easywin32.ico` beside it). It gives the app the EasyWin32 icon (title bar, taskbar, Alt+Tab) and the version resource in the file properties. It is optional: without it the app runs with the standard icon. Per-compiler: Visual Studio compiles `.rc` files automatically; command line MSVC `rc easywin32.rc` then link `easywin32.res`; MinGW `windres easywin32.rc -O coff -o easywin32.res`; Borland `brc32 -fo easywin32.res easywin32.rc`; Watcom `wrc easywin32.rc`.
- Write your program as always: `int main(int argc, char **argv)` or `int main(void)`. Do not define your own `WinMain`.
- Include only `easywin32.h`; **do not include** your compiler's `<conio.h>` (symbol collisions).
- If your app already has a `WinMain`, compile `easywin32.c` with `-DEASYWIN32_NO_ENTRY` and call `ew_init()` / `ew_shutdown()` yourself.

## Classic conio API

`clrscr`, `clreol`, `gotoxy`, `wherex`, `wherey`, `window`, `textcolor`, `textbackground`, `textattr`, `highvideo`, `lowvideo`, `normvideo`, `delline`, `insline`, `gettext`, `puttext`, `movetext`, `gettextinfo`, `_setcursortype`, `cprintf`, `cputs`, `putch`, `kbhit`, `getch`, `getche`, `delay`, `sound`, `nosound`, plus the color constants (`BLACK` ... `WHITE`).

All with DOS semantics: 1-based coordinates relative to the active window, `gettext`/`puttext` in character+attribute pairs, startup attribute 0x07.

## ew_* extensions

| Function | Description |
| --- | --- |
| `ew_init()` / `ew_shutdown()` | manual startup/shutdown (`EASYWIN32_NO_ENTRY` mode) |
| `ew_resize(cols, rows)` | change the grid (80×25 default, 256×128 max); clears the screen |
| `ew_setfont(face, height)` | monospaced font (Lucida Console 16 by default) |
| `ew_setcodepage(cp)` | rendering codepage: **437 by default (DOS)**; 1252 for modern literals with accents, 850, 65001 (UTF-8)... |
| `ew_settitle(t)` | window title |
| `ew_setecho(0/1)` | keyboard echo (1 by default; set it to 0 for secrets and passwords) |
| `ew_version()` | library version string, currently "1.04" |
| `ew_hwnd()` | the window's HWND, in case you want to hang your own controls on it |

## Behavior and notes

- **Keyboard echo**: by default what you type shows up instantly (needed for `scanf`/`cin`). For passwords use `ew_setecho(0)`, and then `getche()` does the echoing.
- **Codepage**: with CP437 (default), the DOS box-drawing characters (`0xB3`, `0xC9`, `0xDB`...) and the accents of ported DOS programs look exactly right. For new code with modern accented literals, call `ew_setcodepage(1252)` at the start (or compile with `/utf-8` and use 65001).
- **Clipboard**: Ctrl+V pastes the clipboard as if it were typed. Drag the mouse to select text: it is copied to the clipboard automatically when you release the button (Edit → Copy and Ctrl+C do the same). With no active selection, Ctrl+C reaches the program as the 0x03 control character, exactly like on DOS.
- **Closing the window** does not kill the process (the app keeps running headless). If you prefer real-console behavior, call `exit(0)` in response to `WM_CLOSE` in your own loop.
- **Help → About** reports the library version (1.04), the compiler that built the program, the platform (Win32/Win64), the stdio transport in use and the build date/time, plus the author credit.
- DOS blink does not exist (modern Windows text carets don't blink).
- `sound()` plays the tone with `Beep()` in 45 ms slices: it sounds slightly pulsated, but `sound`/`nosound`/`delay` work.
- A normal exit (return from `main` or `exit()`) shows everything pending (conio calls drain the pipe, and `atexit` closes the window last).

## Troubleshooting

- **Only conio text (cprintf/gotoxy) shows; printf/cout output is missing, or `scanf` returns instantly** — the stdio redirection is not working. In GUI executables the standard streams are born without real handles, and on some UCRT builds (Visual C++ 2015+ x64) `_dup2` can fail or silently do nothing. EasyWin32 handles this itself: every step of the redirect is checked, verified with a live probe (one byte is really pushed through `stdout` and must arrive in the pipe) and, if the descriptor path fails, it retries automatically with named pipes plus `freopen` — the one redirect the UCRT officially supports for GUI programs. The About box shows the winning transport in the `I/O:` line (`anonymous pipes (dup2)` or `named pipes (freopen)`). If both transports fail (it should not happen), a one-time warning names the exact failing step, error number included: conio still works, printf/scanf do not. Also make sure you compile the shipped easywin32.c **and** easywin32.h wholesale.
- **Exception 0xC0000008 (invalid handle) in CloseHandle while running under the debugger** — a pipe handle was closed twice: once by the CRT (`_close` of a descriptor whose `_dup2` failed) and once by EasyWin32's teardown. Fixed with strict handle ownership: once `_dup2` succeeds the handle belongs to the CRT (freopen closes it later, never us), and any handle closed by `_close` is nulled before the teardown so it cannot be closed again.

- **MSVC C4311: "truncamiento de puntero" warnings on the AppendMenuA lines (HMENU to UINT)** — harmless (USER handles fit in 32 bits), but the current file avoids them entirely with a `UINT_PTR`-compatible cast, so seeing them means you are compiling an **old copy of easywin32.c**; replace it.
- **A black console window appears next to the EasyWin32 window** — the program was built with the **console subsystem** (for example a Visual Studio "Console Application" project). Windows itself creates that console: EasyWin32 never calls `AllocConsole`. It also means the library's `WinMain` is not being used as the entry point, and your `main()` is running on the console CRT startup (the EasyWin32 window still appears because the library self-bootstraps). Fix: build with subsystem WINDOWS — Visual Studio: Project properties → Linker → System → SubSystem = **Window (/SUBSYSTEM:WINDOWS)**; command line: `/link /subsystem:windows user32.lib gdi32.lib kernel32.lib`.

- **MSVC C4996: "scanf: This function or variable may be unsafe"** — MSVC deprecates the classic DOS-era CRT calls. `easywin32.h` takes care of it: it defines `_CRT_SECURE_NO_WARNINGS` and disables warning C4996 with `#pragma warning(disable: 4996)` for the rest of the translation unit, so `scanf`, `strcpy`, etc. compile clean. Just include `easywin32.h` in each file that uses them (include order does not matter thanks to the pragma). Alternatively define `_CRT_SECURE_NO_WARNINGS` project-wide and remove the pragma if you prefer.

- **Crash: 0xC0000005 access violation in ntdll at `EnterCriticalSection(&g_lock)` (write to a near-NULL address such as 0x24)** — this means a conio function ran before `ew_init()`, when the library's lock had not been created yet. Typical causes: the program was built with the **console subsystem** (for example a Visual Studio "Console Application" project), so the library's `WinMain` never runs; `ew_init()` was forgotten in `EASYWIN32_NO_ENTRY` mode; or a C++ global constructor calls conio before startup. EasyWin32 now initializes the lock lazily and **self-bootstraps**: the first conio call starts the window on demand, so this situation no longer crashes. For the real EasyWin behavior (GUI application, no console window) build with subsystem WINDOWS anyway: in Visual Studio set Project properties → Linker → System → SubSystem = **Window (/SUBSYSTEM:WINDOWS)**, or on the command line add `/link /subsystem:windows user32.lib gdi32.lib kernel32.lib`.

## Limitations compared to real DOS

- No hardware access: `int86`, `inportb`, `outportb`, `peekb`/`pokeb`, real-time clock interrupts or BGI graphics. Those parts must be ported to the Win32 API.
- `textmode()` is not implemented (use `clrscr()` + `window()`).
- If you mix `getch()` and `scanf()` over the same typed keys, the same keypress can end up in both the raw queue and stdin (DOS had a single queue; this is a reasonable approximation).
- The default size is 80×25, non-resizable by dragging; use `ew_resize`/`ew_setfont` to change it.