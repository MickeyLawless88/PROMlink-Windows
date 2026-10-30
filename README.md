# PROMlink for Windows (PL.EXE)

**Ported/Patched by Mickey White Lawless**

A native Win32 build of Data I/O's **PROMlink 2.01** (`PL.EXE`, the PC-side link
software for Data I/O PROM programmers) that runs on current 32-bit and 64-bit
Windows and talks to the programmer through an ordinary Windows COM port,
USB-serial adapters included.

The original `PL.EXE` is **not modified**. Its exact bytes are embedded inside the
new `PL.EXE` and run on a small built-in emulated PC (8086 CPU, DOS and BIOS
services, 8250 serial chip, 8259 interrupt controller, text-mode screen).

PROMlink is Data I/O Corporation's software (© 1984, 1985 Data I/O Corp.). This
project provides only the machine it runs on. No Data I/O code was altered.

---

## Contents

1. [Quick start](#1-quick-start)
2. [Status and what has been tested](#2-status-and-what-has-been-tested)
3. [Why the original cannot run on Windows](#3-why-the-original-cannot-run-on-windows)
4. [How this build works](#4-how-this-build-works)
5. [Installation and files](#5-installation-and-files)
6. [Running it](#6-running-it)
7. [Serial port handling](#7-serial-port-handling)
8. [Window, screen and keyboard](#8-window-screen-and-keyboard)
9. [Diagnostics: `/LOG`](#9-diagnostics-log)
10. [Troubleshooting](#10-troubleshooting)
11. [Technical reference](#11-technical-reference)
12. [Change log: problems found and fixed](#12-change-log-problems-found-and-fixed)
13. [Building from source](#13-building-from-source)
14. [How it was tested](#14-how-it-was-tested)
15. [Known limitations](#15-known-limitations)
16. [Verifying the embedded original](#16-verifying-the-embedded-original)
17. [Legal and credits](#17-legal-and-credits)

---

## 1. Quick start

1. Copy `PL.EXE` into your PROMlink folder, next to `PLMENUS.STR`, `PLDEVLST.DAT`,
   `PLERROR.STR`, `INIT.CFN` and the `.BIN` files. Overwrite the old `PL.EXE`
   after closing any running copy (Windows will not replace a running program).
2. Connect the programmer to **COM1** (or COM2). With a USB-serial adapter, set
   its port number in Device Manager to COM1.
3. Double-click `PL.EXE`.

The title screen appears; when the programmer answers you get
`Contact with programmer established.` Press **F1** for the main menu.

If it does not start, see [Troubleshooting](#10-troubleshooting).

## 2. Status and what has been tested

| Area | Status |
|---|---|
| Boot, title screen, menus, help | Working |
| Main menu, Program/Verify, File functions, Edit/View RAM screens | Working |
| Directory listing, change directory, delete/rename file calls | Working (change directory had a bug, fixed; see [12](#12-change-log-problems-found-and-fixed)) |
| Serial transmit and receive, interrupt-driven receive | Working in the test harness and with a real programmer ("Contact with programmer established") |
| Resizable window that fills at any size | Working |
| Long sessions of programmer traffic | **Not fully verified.** One report of `Error #FC` while paging through RAM; the cause is not yet known (see [10](#10-troubleshooting) and [9](#9-diagnostics-log)) |
| 64-bit Windows | Built and starts under Wine; the 32-bit build is the one distributed and used |

Be aware: the author does not own every programmer model. Only parts of the
program have been exercised against real hardware. Please report differences from
DOS behavior with a `/LOG` capture attached.

## 3. Why the original cannot run on Windows

`PL.EXE` is a **16-bit real-mode DOS program** (MZ executable, built with
Lattice C 2.1). Three separate things stop it on a modern PC:

1. **No 16-bit subsystem.** 64-bit Windows has no NTVDM, so a 16-bit DOS
   executable cannot even be loaded. (32-bit Windows had NTVDM, but it is optional
   and removed in current versions.)
2. **Direct hardware access.** PROMlink does not use a serial API. It programs
   the PC's **8250 UART directly** with port I/O at `3F8h` (COM1) or `2F8h` (COM2),
   sets the baud divisor, line and modem control registers itself, installs its
   own **IRQ4 interrupt handler** (vector 0Ch), and talks to the **8259 interrupt
   controller** (`20h`/`21h`). Windows does not let programs touch I/O ports or
   hook hardware interrupts.
3. **Direct screen memory.** Text is written straight into video memory at
   `B800h:0000` and the cursor is moved through the video chip registers.

So patching a few bytes was never an option. The program expects an entire PC
underneath it, and this project provides one.

## 4. How this build works

`PL.EXE` here is one Win32 executable containing:

```
+---------------------------------------------------------------+
| Win32 window (GDI), keyboard, timers, COM port                |
+----------------------+----------------------+-----------------+
| DOS / BIOS services  | 8250 UART + 8259 PIC | Text video      |
| (INT 21h,10h,14h,    | (ports 3F8/2F8,      | (B800h memory,  |
|  16h,11h,1Ah,...)    |  20h/21h, IRQ4)      |  CRTC cursor)   |
+----------------------+----------------------+-----------------+
| 8086 CPU emulator                                             |
+---------------------------------------------------------------+
| 1 MB emulated memory, with the ORIGINAL PL.EXE loaded as DOS  |
| would load it (PSP, environment, relocations, stack)          |
+---------------------------------------------------------------+
```

### 4.1 8086 CPU core

A complete 8086 instruction interpreter: all addressing modes, segment overrides,
prefixes (`REP`/`REPE`/`REPNE`, `LOCK`), string instructions, the full ALU
including BCD adjust (`DAA`, `DAS`, `AAA`, `AAS`, `AAM`, `AAD`), multiply/divide
with divide-error handling, shifts and rotates, `LOOP`/`JCXZ`, far calls and
returns, `PUSHF`/`POPF`, and interrupt instructions. Flags are computed
per instruction. A few 80186 opcodes (`PUSH imm`, `PUSHA`/`POPA`, `IMUL imm`)
are accepted because some DOS toolchains emit them.

### 4.2 Loading the program (DOS loader behavior)

The embedded image is loaded the way `COMMAND.COM` would load it:

- MZ header parsed; load image placed at **PSP segment + 10h**;
- the 4 relocation entries in this file are applied (data segment fixups);
- a **Program Segment Prefix** is built (INT 20h at offset 0, memory top, handle
  table, environment segment, command tail at `80h`);
- initial `SS:SP`, `CS:IP` set from the header (`SS:SP = 1210:0080`, entry point at
  offset 2 of the code segment for this build).

### 4.3 DOS services (INT 21h)

DOS calls are handled directly by Windows code ("high-level emulation"), not by
running a DOS kernel. Implemented services (those PROMlink uses):

| Area | Functions |
|---|---|
| Console | 01h, 02h, 06h, 09h and the other console calls PROMlink uses (see `dos21()` in `plw.c` for the exact list) |
| Version / system | 30h (reports DOS 5.00), 25h/35h (get/set vector), 62h (PSP) |
| Drive / directory | 0Eh (select disk), 19h (current disk), 36h (free space), 3Bh (change directory), 47h (get current directory) |
| Files (handles) | 3Ch create, 3Dh open, 3Eh close, 3Fh read, 40h write, 42h seek, 41h delete, 43h attributes, 56h rename, 44h IOCTL |
| Files (FCB) | 13h delete, 17h rename |
| Directory search | 4Eh find first, 4Fh find next, with DOS attribute filtering (hidden/system/directory) |
| Memory | 4Ah resize block, 48h allocate (simple paragraph allocator) |
| Date and time | 2Ah get date, 2Ch get time (**real hundredths of a second**) |
| Termination | 4Ch, 00h, INT 20h |

DOS file names map straight onto Windows files in the current folder. DOS paths
like `c:\Users\You\Desktop\winplink` are passed to Windows as they are.

Return-value conventions matter to this program. Examples: successful change
directory and delete return `AX = 0`; failures set carry and return the DOS error
code (2 file not found, 3 path not found, 4 too many handles, 5 access denied, 6
invalid handle).

### 4.4 BIOS services

| Interrupt | Used for |
|---|---|
| INT 10h | Video: set mode, cursor shape/position, scroll up/down, read/write character and attribute, teletype output, get video mode |
| INT 11h | Equipment list (returns `0421h`) |
| INT 14h | Serial initialize, send, receive, status (also reconfigures the Windows port) |
| INT 16h | Keyboard read, status, shift flags |
| INT 1Ah | Timer ticks (derived from the Windows clock at 18.2065 Hz) |
| INT 24h | DOS critical-error handler (PROMlink installs its own) |

The BIOS data area (segment `0040h`) is populated: COM port base addresses
(`3F8h`/`2F8h`), equipment word, memory size, video mode/columns/page/cursor,
CRTC base (`3D4h`), keyboard buffer pointers, timer count, rows (24) and character
height (16).

### 4.5 Serial hardware: 8250 UART and 8259 PIC

This is the heart of the port. The emulated **8250** has the full register set:

- `THR`/`RBR` data, `IER`, `IIR`, `LCR` (including the divisor-latch access bit
  `DLAB` and divisor registers `DLL`/`DLM`), `MCR`, `LSR`, `MSR`, scratch.
- **Receive path:** bytes read from the Windows port go into a **4096-byte ring
  buffer**. The emulated chip presents them one at a time through `RBR`, sets the
  data-ready bit in `LSR`, and raises **IRQ4** (COM1) or **IRQ3** (COM2) if the
  program enabled the receive interrupt and set `OUT2` in `MCR`, exactly as a real
  PC gates the interrupt.
- **Transmit path:** a write to `THR` sends the byte to the Windows port;
  the transmitter is reported ready immediately (holding register empty).
- **Modem lines:** `MCR` bits (DTR, RTS) are applied to the real port; `MSR`
  (CTS, DSR, RI, DCD) is read from the real port.
- **Line settings:** when the program writes the divisor and `LCR`, the emulator
  derives baud rate, data bits, parity and stop bits and applies them to the real
  port (DCB). PROMlink's default is 9600 baud, 8 data bits, no parity, 1 stop bit.

The emulated **8259** keeps the interrupt mask register, in-service register and
end-of-interrupt handling, so the program's own IRQ4 interrupt service routine
(at the address it installs in vector 0Ch) runs and acknowledges the interrupt the
real way. Interrupt delivery honors the CPU interrupt flag and vectors through the
interrupt vector table in emulated memory.

### 4.6 Video

The 80x25 text screen lives in emulated memory at `B800h` (with a monochrome
`B000h` page also tracked). Cursor position and shape come from the emulated
CRTC registers (`3D4h`/`3D5h`) and the BIOS data area. Whenever the screen
contents change, the window redraws:

- each cell is drawn with the PC (code page 437) character set, including the
  box-drawing and block characters and the smiley/arrow control glyphs;
- the standard 16 CGA/VGA colors are used for foreground and background;
- text is rendered into a 10x20-pixel-per-cell off-screen bitmap and stretched to
  the window with halftone scaling;
- PROMlink only uses the first **76 columns**, so those 76 columns are what is
  stretched to fill the window (measured across the menus, edit screen and file
  screens);
- an underline cursor blinks at the emulated cursor position.

### 4.7 Keyboard

Windows key events are translated to what DOS programs expect: an ASCII code in
`AL` and a scan code in `AH`. F1-F10 (with Shift/Ctrl/Alt variants), F11/F12,
arrows, Home/End, PgUp/PgDn, Insert/Delete, Shift-Tab, Ctrl combinations, Alt
combinations, Enter, Esc, Tab and Backspace are handled. Keys go into a 64-entry
queue that INT 16h and INT 21h console input read from.

### 4.8 Time

- DOS time of day (`INT 21h/2Ch`) returns real hundredths of a second from the
  Windows clock. PROMlink's splash screen waits on this in a busy loop; with the
  hundredths stuck at zero it hung.
- Busy-wait loops are detected (many repeated time or key-status polls) and
  throttled with short sleeps, so the program does not pin a CPU core at 100%.
- The BIOS tick counter advances at 18.2065 Hz from the Windows millisecond clock.

### 4.9 The environment block

The Lattice C start-up code sizes its stack as `0x200` plus the size of the DOS
environment block, and copies the environment to the top of the stack. A real DOS
environment is large. An almost empty one leaves the stack so small that it
overran the program's own variables. The emulator therefore builds an environment
containing the useful variables from Windows (`PLMSG`, `PATH`, `COMSPEC`,
`PROMPT`, `TEMP`, `TMP`) and pads it with a filler variable (`PLW_PAD=XXXX...`) to
about 2400 bytes. This is invisible to the user.

## 5. Installation and files

Put `PL.EXE` in the same folder as the PROMlink data files:

| File | Purpose |
|---|---|
| `PLMENUS.STR` | All menu and screen text (required) |
| `PLDEVLST.DAT` | Device list (required) |
| `PLERROR.STR` | Error messages |
| `INIT.CFN`, `INITPC.CFN`, `INITTI.CFN`, `S22.CFN`... | Saved configurations |
| `*.BIN`, `*.HEX`, `*.S19`, `*.OCT`, `*.UFN` ... | Your data and user-function files |
| `PL.EXE` | **This program** |

If you start `PL.EXE` from somewhere else (a shortcut with a different "Start in"
folder, or from another command prompt directory) and `PLMENUS.STR` is not in the
current folder, `PL.EXE` automatically switches to **its own folder** before
starting. You can also set the `PLMSG` environment variable to the folder that
holds the message files, as with the DOS version.

PROMlink writes its configuration back to `INIT.CFN` in the current folder, so
install it somewhere you can write (not under `Program Files` unless you have
write permission there).

## 6. Running it

```
PL                  Start PROMlink
PL /H               Show a short help text
PL /LOG             Start with serial-traffic logging to PLW.LOG
PL /X prog.exe args Run another 16-bit DOS program the same way (advanced)
PL <args>           Any other arguments are passed to PROMlink as its command tail
```

`PL.EXE` is a Windows GUI program, so it creates its own window and does not use
the console it was started from.

Typical session: **F1** opens the main menu. Numbered entries start the
corresponding function. **F2** goes to the previous menu, **F3** shows help, and
the bottom line of each screen lists the active function keys. **x** exits
PROMlink.

## 7. Serial port handling

The mapping is fixed in the Windows build:

| PROMlink | Windows |
|---|---|
| COM1 (`3F8h`, IRQ4) | `COM1` |
| COM2 (`2F8h`, IRQ3) | `COM2` |

Notes:

- The programmer should be on COM1 to match PROMlink's default configuration
  (`COM1,9600,n,8,1`). Use Device Manager to change a USB-serial adapter's port
  number to COM1.
- The port is opened **exclusively**. If another program (a terminal, another
  copy of PL) has COM1 open, a message box says that COM1 could not be opened and
  PROMlink runs without a serial connection.
- COM2 failing to open is silent (most PCs have no COM2).
- The serial parameters PROMlink sets (baud, data bits, parity, stop bits) are
  applied to the real port, so changing them in PROMlink's configuration menu
  takes effect on the wire.
- Read timeouts are configured so reads return immediately with whatever has
  arrived; the emulator polls frequently (every 2048 emulated instructions).
- DTR and RTS are driven from the emulated modem control register; CTS, DSR, RI
  and DCD are read back from the real port.

## 8. Window, screen and keyboard

- The window can be **resized or maximized**; the 80x25 PROMlink screen is
  stretched to fill the whole client area (the character proportions change if
  you pick an unusual window shape).
- It opens at 760x500 pixels, which shows the screen at its natural 10x20 cells.
- Closing the window (or Alt+F4) ends the program.
- If the program ends within eight seconds of starting, or ends with an error, a
  message box reports how long it ran and the exit code, instead of the window
  just vanishing.
- The icon is the PROMlink logo.

## 9. Diagnostics: `/LOG`

Start with `PL /LOG` to write **`PLW.LOG`** in the PROMlink folder. Every line is
prefixed with a millisecond timestamp:

```
  2338706863 PLW log started
  2338706932 COM1 config 9600 8N1
  2338706950 COM1 MCR 0B
  2338707100 COM1 TX 1B
  2338707250 COM1 RX 06
  2338709000 COM1 line error flags 00000002 (1=RXOVER 2=OVERRUN 4=PARITY 8=FRAME)
  2338709400 COM1 RING FULL - byte dropped
```

It records: every byte transmitted and received, serial configuration changes,
modem-control writes, line-error flags reported by Windows (receive overrun,
hardware overrun, parity, framing), failed reads, and any received byte dropped
because the 4096-byte emulated receive buffer was full.

**When you report a problem, attach `PLW.LOG`** and say which key you pressed just
before it happened.

The log can grow large during long transfers; delete it when done.

## 10. Troubleshooting

### The window opens and immediately closes

You will now get a message box with the reason. The usual cause: `PLMENUS.STR`
or `PLDEVLST.DAT` could not be found, in which case PROMlink prints
`!!! plmenus.str or pldevlst.dat are lost or damaged` and exits. Make sure
`PL.EXE` sits in the PROMlink folder. Current versions switch to the EXE's own
folder automatically, so an old copy of `PL.EXE` is the likely culprit.

### "Couldn't save PL.EXE" / can't overwrite `PL.EXE`

Windows will not overwrite a running program. Close PROMlink completely
(including from the taskbar) and copy again.

### "Cannot contact programmer"

PROMlink sends probe characters (ESC, 1Bh) at 9600 baud, 8N1 and waits for the
programmer. Check:

- the programmer is on, connected, and set to 9600 baud;
- the cable and adapter work, and the adapter appears as **COM1** in Device
  Manager;
- no other program holds COM1;
- run with `/LOG` and check that `TX 1B` appears and whether any `RX` bytes come
  back.

### "COM1 could not be opened"

COM1 does not exist or is in use by another program. Fix the port number in
Device Manager (Port Settings, Advanced) or close the other program.

### `Error #124 Unable to change directory`

This was a bug in early builds (a leftover non-zero `AX` after a successful
change-directory call, which PROMlink treats as failure). Fixed. If you still see
it, the target folder probably does not exist.

### `Error #FC` (while viewing or editing RAM)

PROMlink's error table (`PLERROR.STR`) has no entry for `FC`, so it prints the raw
code. It can be a corrupted or desynchronized serial exchange, or a code the
programmer sent itself. It has been seen once, while paging through RAM. The cause
is not yet established. Reproduce it with `PL /LOG` and send the log.

### The colors are green

PROMlink picks its own screen colors from its configuration. The emulator just
draws the attribute bytes the program writes (the default configuration produces
green text). It is not a rendering problem.

### Text looks stretched

The window fills at any size, so an unusual window shape changes the character
proportions. Resize to roughly 76:25 character proportions (about 3:1 pixels
for 10x20 cells) for natural-looking text, or open it at the default size.

### Antivirus warnings

The program is an unsigned executable that embeds a 16-bit program image and an
emulator; some scanners may flag it heuristically. The full source is provided so
you can build it yourself.

## 11. Technical reference

### 11.1 Emulated memory map

| Address | Contents |
|---|---|
| `0000:0000-03FF` | Interrupt vector table (each vector points at a small stub in the `F000` segment that traps into the Windows-side handler) |
| `0040:0000` | BIOS data area (COM bases, equipment, video state, keyboard queue, timer ticks) |
| `0800:0000` | DOS environment block |
| `0900:0000` | PSP, then the program image from `0910:0000` |
| `B000:0000` / `B800:0000` | Text video memory (mono and color pages) |
| `F000:xxxx` | Stubs for BIOS/DOS interrupts |
| (1 MB total) | Real-mode address space |

### 11.2 I/O ports handled

| Port(s) | Device |
|---|---|
| `20h`, `21h` | 8259 PIC (command, IMR); EOI, ISR |
| `3F8h-3FFh` | COM1 8250 (also `2F8h-2FFh` for COM2) |
| `3D4h`, `3D5h` (and `3B4h`/`3B5h`) | CRTC index/data (cursor) |
| `3DAh` | CGA status (vertical retrace toggles so wait loops end) |
| `61h` | Keyboard/speaker control (ignored) |
| other | read as `FFh`, writes ignored |

### 11.3 Interrupt vector setup

Each BIOS/DOS interrupt vector in the IVT points at a stub located in segment
`F000`. When the CPU executes such a stub, control passes to a Windows-side
handler that performs the service and returns to the emulated program with the
registers and flags set as DOS or the BIOS would. For calls that must wait (a
blocking keyboard read, for instance) the instruction is retried later instead of
freezing the whole window, so the message loop keeps running.

### 11.4 Source layout (`plw.c`)

| Section | Purpose |
|---|---|
| CPU core | `step()`, ALU, shifts, ModR/M decoding, string ops |
| Memory/IO | Memory accessors, port input/output |
| UART/PIC | 8250 and 8259 emulation, IRQ delivery |
| Video | Text memory refresh, scrolling, tty output |
| Keyboard | Key queue and translation |
| DOS | INT 21h handler, path and directory handling |
| BIOS | INT 10h/11h/14h/16h/1Ah |
| Loader | MZ/COM loader, PSP, environment |
| Win32 platform | Window, GDI drawing, keyboard, COM port, timers |
| POSIX test platform | A scriptable harness for tests (not in the Windows build) |

Companion files: `plimg.h` (the original `PL.EXE` as a C byte array), `pl.rc` and
`pl.ico` (icon resource), `difftest.py` (CPU test), `BUILD.txt`.

## 12. Change log: problems found and fixed

Each item is a real bug found while bringing the program up.

| # | Symptom | Cause | Fix |
|---|---|---|---|
| 1 | Garbled screen, restarts, crashes right after start | `INT 21h/47h` (get current directory) copied up to 64 bytes into a 62-byte buffer; a long working directory overran adjacent globals and corrupted a function pointer | Directory string truncated to what fits; on Windows the short (8.3) path is used |
| 2 | Memory corruption near the stack | Lattice C sizes its stack from the environment size; a tiny environment gave a tiny stack | Environment padded to about 2400 bytes |
| 3 | Splash screen hang | `INT 21h/2Ch` returned hundredths = 0, so the program's time-wait never ended | Real hundredths from the Windows clock; busy-wait throttling |
| 4 | Wrong arithmetic in rare cases | `DAA`/`DAS` carry handling | Corrected; CPU re-verified against Unicorn |
| 5 | Programmer/serial initialization looked dead | Serial setup comes through the UART registers, not just BIOS calls | Full 8250 emulation including divisor latch, LCR and modem control |
| 6 | Received bytes not seen | PROMlink uses interrupt-driven receive | 8259 emulation with IMR/ISR/EOI and `OUT2`-gated IRQ4/IRQ3 |
| 7 | Immediate exit when started from another folder | `PLMENUS.STR`/`PLDEVLST.DAT` not found in the current folder | Automatic switch to the EXE's own folder; message box on early exit |
| 8 | Console window did not fit; text could not scale | Console windows cannot scale text | Replaced the console with a resizable GDI window that stretches the screen |
| 9 | Unused black margin at the right | PROMlink's screens use only columns 0-75 | Stretch columns 0-75 to the full window width |
| 10 | `Error #124 Unable to change directory` | A successful `INT 21h/3Bh` left a stale non-zero `AX`, which PROMlink treats as an error | `AX = 0` on success for change directory and delete |
| 11 | File search returned directories/hidden files incorrectly | Attribute filter was ignored | `INT 21h/4Eh`/`4Fh` honor the search attribute |

## 13. Building from source

Requirements: a MinGW-w64 cross compiler (`i686-w64-mingw32-gcc`,
`i686-w64-mingw32-windres`), or MinGW-w64 on Windows.

```sh
# resource (icon)
i686-w64-mingw32-windres pl.rc -O coff -o pl_res.o

# 32-bit Windows build (the distributed one)
i686-w64-mingw32-gcc -O2 -Wall -mwindows -s -static -o PL.EXE plw.c pl_res.o

# 64-bit Windows build
x86_64-w64-mingw32-windres pl.rc -O coff -o pl_res64.o
x86_64-w64-mingw32-gcc -O2 -Wall -mwindows -s -static -o PL64.EXE plw.c pl_res64.o

# Linux test harness (scripted, no GUI)
gcc -O2 -D_GNU_SOURCE -DPLW_TEST -o plw_test plw.c
```

Optional defines:

| Define | Effect |
|---|---|
| `PLW_TEST` | Linux build with a POSIX platform layer and script engine |
| `PLW_WATCH` | Debug hooks (DOS call trace via `PLW_DOS`, breakpoints, trace ring) |
| `PLW_LIB` | Build as a shared library for the CPU differential test |
| `PLW_NOIMG` | Omit the embedded image (use `/X` to run a program) |

`plimg.h` must be present unless `PLW_NOIMG` is defined.

## 14. How it was tested

- **CPU core:** random single-instruction differential testing against the
  Unicorn x86 emulator in real mode (about 100,000 instructions, random registers,
  flags and memory, with prefixes). Mismatches were investigated; real ones (the
  `DAA`/`DAS` carry) were fixed and the rest were artifacts of the test harness
  (undefined flag bits and such). Script: `difftest.py`.
- **Whole program (Linux harness):** scripted keystrokes and screen dumps drove the
  unmodified `PL.EXE` through boot, the title screen, the "Cannot contact
  programmer" probe sequence, main menu, Program and Verify device menus, File
  functions menu, directory listing, and change directory. Serial transmit was
  checked on a pseudo-terminal, and received bytes were checked through the
  IRQ4 interrupt handler into `RBR`.
- **Windows build:** compiled with MinGW-w64 and smoke-tested under Wine (starts,
  runs, window and keyboard work). Used on real Windows with a real programmer by
  the author, who reached `Contact with programmer established` and used the
  Edit/View RAM and Change directory screens.
- **Not covered:** every programmer model, every device type, long transfers, and
  any flows that depend on programmer replies beyond the above. A full regression
  with real programmers is the next step; `/LOG` exists to support it.

## 15. Known limitations

- **COM1/COM2 are fixed** to Windows COM1/COM2 (no command-line port option in
  the Windows build).
- **Only what PROMlink uses is emulated.** Other DOS programs may need services
  that are not implemented; `/X` is provided for experiments with other tools from
  the same package, not as a general DOS emulator. (`TERM.COM`, for example, did
  not show received text in testing, and was not investigated.)
- **Speed model.** The emulated PC is not clock-accurate. Serial bytes are
  delivered as soon as Windows provides them, and the transmitter is always
  ready. Programs that depend on exact timing between characters may behave
  differently from real hardware.
- **No real DOS drives.** `INT 21h` drive and directory calls map onto the Windows
  filesystem; drive letters other than the current one are not modelled.
- **No sound, no graphics modes.** PROMlink does not use them.
- **Unsigned binary.** Expect SmartScreen or antivirus prompts on some systems.
- **The `Error #FC` report** is open (see [10](#10-troubleshooting)).

## 16. Verifying the embedded original

The embedded image is byte-identical to the `PL.EXE` from the original PROMlink
distribution:

```
Size   : 74,486 bytes
SHA-256: de3bb92fafc75a732bb2e572e48e2025eacb1a3d0d7e8814feae04683778ee14
```

To check: compute the SHA-256 of your original `PL.EXE`; it should match the
value above. The array in `plimg.h` holds exactly those bytes, and the same bytes
appear verbatim inside the built `PL.EXE` (you can search for the first bytes
`4D 5A` of the MZ header followed by the original header contents).

## 17. Legal and credits

- **PROMlink** (the embedded `PL.EXE`, message files and data files):
  © 1984, 1985 Data I/O Corporation. All rights belong to their owner. This
  project does not modify the program. It is an emulation layer to run it on
  modern Windows.
- **Port, emulator, Windows layer, icon artwork integration, documentation:**
  Ported/Patched by **Mickey White Lawless**.
- The PROMlink logo icon was produced from artwork supplied by the author.
- Distributed as-is with no warranty. Programming devices is at your own risk;
  verify results independently before relying on them.
