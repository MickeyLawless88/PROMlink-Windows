/*
 * PLW.C -- Win32 console host for the 16-bit DOS program PROMlink (PL.EXE)
 *
 * Ported/Patched by Mickey White Lawless
 *
 * PL.EXE (PROMlink 2.00, Copyright 1985 Data I/O Corp., Lattice C 2.1) is a
 * real-mode 8086 program that drives the 8250 UART and 8259 PIC directly,
 * hooks IRQ3/IRQ4, writes straight into video RAM and calls DOS/BIOS.  64-bit
 * Windows has no NTVDM and even 32-bit NTVDM never gave a DOS program real
 * port I/O, so the original image cannot be "patched" into a PE file.
 * Instead this program IS the new PL.EXE: a 32-bit Win32 console executable
 * containing
 *
 *     - an 8086/186 instruction-set core,
 *     - INT 21h / INT 10h / INT 11h / INT 14h / INT 16h / INT 1Ah services
 *       mapped onto Win32 (files, console, clock),
 *     - an 8250 UART at 3F8h/2F8h wired to a real Windows COM port,
 *     - an 8259 PIC so the program's own serial ISRs run unmodified,
 *     - an 80x25 text screen mapped onto the Windows console,
 *
 * and the original, unmodified PL.EXE image (PLIMG.H, 74486 bytes).
 *
 * Build (MinGW, 32-bit, static):
 *     i686-w64-mingw32-gcc -O2 -s -static -o PL.EXE plw.c
 * Linux self-test build (scripted keys, pty serial):
 *     gcc -O2 -DPLW_TEST -o plw_test plw.c
 *
 * Usage:
 *     PL [/COM1=COMx] [/COM2=COMy] [/X prog.exe] [args for PROMlink...]
 *   /COM1=COM5   emulated COM1 (3F8h) uses Windows port COM5   (default COM1)
 *   /COM2=COM6   emulated COM2 (2F8h) uses Windows port COM6   (default COM2)
 *   /COM1=NONE   run without a serial port
 *   /X file      run another DOS program (RW.EXE, LABL.EXE, TERM.COM, ...)
 *   Environment: PLW_COM1, PLW_COM2 give the same mapping; PLMSG as before.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <ctype.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <direct.h>
#define PLW_WIN32 1
#else
#include <unistd.h>
#include <dirent.h>
#include <fnmatch.h>
#include <termios.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <sys/select.h>
#include <errno.h>
#endif

#ifndef O_BINARY
#define O_BINARY 0
#endif

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed char    s8;
typedef short          s16;
typedef int            s32;

#ifndef PLW_NOIMG
#include "plimg.h"        /* const unsigned char plimg[]; const unsigned plimg_len; */
#endif

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
static int ipre(const char *a, const char *pre)
{
    for (; *pre; a++, pre++) if (tolower((unsigned char)*a) != tolower((unsigned char)*pre)) return 0;
    return 1;
}
#ifndef PLW_WIN32
#include <strings.h>
#endif

/* ------------------------------------------------------------------ */
/* Platform interface                                                  */
/* ------------------------------------------------------------------ */
static void plat_init(void);
static void plat_exit(void);
static void plat_draw(const u8 *vram, int row, int col, int curvis);
static int  plat_getkey(u16 *k);          /* 1 = key (ascii | scan<<8) */
static void plat_sleep(int ms);
static u32  plat_ms(void);
static int  plat_hund(void);              /* hundredths of the current second */
static int  ser_open(int n, const char *name);
static int  ser_read(int n, u8 *buf, int max);
static void ser_write(int n, u8 c);
static void ser_config(int n, long baud, int bits, int parity, int stop);
static void ser_mcr(int n, int mcr);
static int  ser_msr(int n);
static FILE *logf = NULL;                 /* /LOG: serial traffic log (PLW.LOG) */
static u32 plat_ms(void);
static void slog(const char *fmt, ...)
{
    va_list ap;
    if (!logf) return;
    fprintf(logf, "%8lu ", (unsigned long)plat_ms());
    va_start(ap, fmt); vfprintf(logf, fmt, ap); va_end(ap);
    fputc('\n', logf); fflush(logf);
}

/* ------------------------------------------------------------------ */
/* CPU state                                                           */
/* ------------------------------------------------------------------ */
#define MEMSZ 0x100000UL
static u8  mem[MEMSZ + 16];
static u16 R[8];                 /* AX CX DX BX SP BP SI DI */
static u16 S[4];                 /* ES CS SS DS            */
static u16 IP, FL;

#define LO(i) (*((u8 *)&R[i]))
#define HI(i) (*((u8 *)&R[i] + 1))
#define AX R[0]
#define CX R[1]
#define DX R[2]
#define BX R[3]
#define SP R[4]
#define BP R[5]
#define SI R[6]
#define DI R[7]
#define AL LO(0)
#define AH HI(0)
#define CL LO(1)
#define CH HI(1)
#define DL LO(2)
#define DH HI(2)
#define BL LO(3)
#define BH HI(3)
#define ES S[0]
#define CS S[1]
#define SS S[2]
#define DS S[3]

#define F_CF 0x0001
#define F_PF 0x0004
#define F_AF 0x0010
#define F_ZF 0x0040
#define F_SF 0x0080
#define F_TF 0x0100
#define F_IF 0x0200
#define F_DF 0x0400
#define F_OF 0x0800

#define STUBSEG 0xF000
#define MEMTOP  0xA000           /* first paragraph above conventional RAM */
#define VID_B8  0xB8000UL
#define VID_B0  0xB0000UL

#define LIN(s, o) ((((u32)(u16)(s) << 4) + (u16)(o)) & 0xFFFFFUL)

static u8 rd8(u16 s, u16 o) { return mem[LIN(s, o)]; }
static u16 rd16(u16 s, u16 o) { return (u16)(mem[LIN(s, o)] | (mem[LIN(s, (u16)(o + 1))] << 8)); }
static unsigned long icount;
#ifdef PLW_WATCH
static int watch_n = 0;
static int vwatch = -1;
static void watch(u16 s, u16 o, u16 v) { u32 l = LIN(s, o); if (vwatch < 0) vwatch = getenv("PLW_VWATCH") ? 1 : 0; if (vwatch && l >= 0xB8000UL && l < 0xB8000UL + 4000 && (l & 1) == 0 && l < 0xB8F00UL && (v & 0xFF) != 0x20 && watch_n < 40) { watch_n++; fprintf(stderr, "VWATCH icount=%lu write %05X=%04X from %04X:%04X ES=%04X DS=%04X\n", icount, l, v, CS, IP, ES, DS); } }
#else
#define watch(s, o, v) ((void)0)
#endif
static void wr8(u16 s, u16 o, u8 v) { watch(s, o, v); mem[LIN(s, o)] = v; }
static void wr16(u16 s, u16 o, u16 v) { watch(s, o, v); mem[LIN(s, o)] = (u8)v; mem[LIN(s, (u16)(o + 1))] = (u8)(v >> 8); }

static u8 par[256];
static int running = 1, exit_code = 0;
static int irq_hint = 0;
static int retry_int = 0;

static void setf(u16 f, int c) { if (c) FL |= f; else FL &= (u16)~f; }

static void setszp(u32 r, int w)
{
    r &= (w == 8) ? 0xFFu : 0xFFFFu;
    FL &= (u16)~(F_ZF | F_SF | F_PF);
    if (!r) FL |= F_ZF;
    if (r & ((w == 8) ? 0x80u : 0x8000u)) FL |= F_SF;
    if (par[r & 0xFF]) FL |= F_PF;
}

static u32 alu(int op, u32 a, u32 b, int w)
{
    u32 m = (w == 8) ? 0xFFu : 0xFFFFu, sg = (w == 8) ? 0x80u : 0x8000u, r, c;
    switch (op) {
    case 0: case 2:
        c = (op == 2 && (FL & F_CF)) ? 1 : 0;
        r = a + b + c;
        setf(F_CF, r > m);
        setf(F_OF, ((a ^ r) & (b ^ r) & sg) != 0);
        setf(F_AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    case 3: case 5: case 7:
        c = (op == 3 && (FL & F_CF)) ? 1 : 0;
        r = a - b - c;
        setf(F_CF, a < b + c);
        setf(F_OF, ((a ^ b) & (a ^ r) & sg) != 0);
        setf(F_AF, ((a ^ b ^ r) & 0x10) != 0);
        break;
    case 1: r = a | b; setf(F_CF, 0); setf(F_OF, 0); setf(F_AF, 0); break;
    case 4: r = a & b; setf(F_CF, 0); setf(F_OF, 0); setf(F_AF, 0); break;
    default: r = a ^ b; setf(F_CF, 0); setf(F_OF, 0); setf(F_AF, 0); break;
    }
    r &= m;
    setszp(r, w);
    return r;
}

static u32 shiftop(int op, u32 v, int cnt, int w)
{
    u32 m = (w == 8) ? 0xFFu : 0xFFFFu, sg = (w == 8) ? 0x80u : 0x8000u, hs = sg >> 1;
    int i, cf, oc;
    for (i = 0; i < cnt; i++) {
        switch (op) {
        case 0: cf = (v & sg) ? 1 : 0; v = ((v << 1) | (u32)cf) & m;
                setf(F_CF, cf); setf(F_OF, ((v & sg) ? 1 : 0) ^ cf); break;
        case 1: cf = (int)(v & 1); v = (v >> 1) | (cf ? sg : 0);
                setf(F_CF, cf); setf(F_OF, ((v & sg) ? 1 : 0) ^ ((v & hs) ? 1 : 0)); break;
        case 2: cf = (v & sg) ? 1 : 0; v = ((v << 1) | ((FL & F_CF) ? 1u : 0u)) & m;
                setf(F_CF, cf); setf(F_OF, ((v & sg) ? 1 : 0) ^ cf); break;
        case 3: oc = (FL & F_CF) ? 1 : 0; cf = (int)(v & 1);
                setf(F_OF, ((v & sg) ? 1 : 0) ^ oc);
                v = (v >> 1) | (oc ? sg : 0); setf(F_CF, cf); break;
        case 5: cf = (int)(v & 1); setf(F_OF, (v & sg) ? 1 : 0); v >>= 1;
                setf(F_CF, cf); setszp(v, w); break;
        case 7: cf = (int)(v & 1); v = (v >> 1) | (v & sg);
                setf(F_CF, cf); setf(F_OF, 0); setszp(v, w); break;
        default: cf = (v & sg) ? 1 : 0; v = (v << 1) & m;
                setf(F_CF, cf); setf(F_OF, ((v & sg) ? 1 : 0) ^ cf); setszp(v, w); break;
        }
    }
    return v & m;
}

/* ModR/M decoding */
static int m_mod, m_reg, m_rm, ovr;
static u16 m_seg, m_off;

static u8 fetch8(void) { u8 v = mem[LIN(CS, IP)]; IP++; return v; }
static u16 fetch16(void) { u16 v = fetch8(); v |= (u16)(fetch8() << 8); return v; }

static void modrm(void)
{
    u8 b = fetch8();
    u16 off = 0;
    int dseg = 3;
    m_mod = b >> 6; m_reg = (b >> 3) & 7; m_rm = b & 7;
    if (m_mod == 3) return;
    switch (m_rm) {
    case 0: off = (u16)(BX + SI); break;
    case 1: off = (u16)(BX + DI); break;
    case 2: off = (u16)(BP + SI); dseg = 2; break;
    case 3: off = (u16)(BP + DI); dseg = 2; break;
    case 4: off = SI; break;
    case 5: off = DI; break;
    case 6: if (m_mod == 0) off = fetch16(); else { off = BP; dseg = 2; } break;
    default: off = BX; break;
    }
    if (m_mod == 1) off = (u16)(off + (u16)(s16)(s8)fetch8());
    else if (m_mod == 2) off = (u16)(off + fetch16());
    m_off = off;
    m_seg = S[ovr >= 0 ? ovr : dseg];
}

static u8 reg8g(int i) { return (i < 4) ? LO(i) : HI(i - 4); }
static void reg8s(int i, u8 v) { if (i < 4) LO(i) = v; else HI(i - 4) = v; }
static u32 rmg(int w)
{
    if (m_mod == 3) return (w == 8) ? reg8g(m_rm) : R[m_rm];
    return (w == 8) ? rd8(m_seg, m_off) : rd16(m_seg, m_off);
}
static void rms(int w, u32 v)
{
    if (m_mod == 3) { if (w == 8) reg8s(m_rm, (u8)v); else R[m_rm] = (u16)v; }
    else if (w == 8) wr8(m_seg, m_off, (u8)v); else wr16(m_seg, m_off, (u16)v);
}
static u32 rg(int i, int w) { return (w == 8) ? reg8g(i) : R[i]; }
static void rs(int i, int w, u32 v) { if (w == 8) reg8s(i, (u8)v); else R[i] = (u16)v; }

static void push16(u16 v) { SP = (u16)(SP - 2); wr16(SS, SP, v); }
static u16 pop16(void) { u16 v = rd16(SS, SP); SP = (u16)(SP + 2); return v; }

static int cond(int n)
{
    int r = 0, sf = (FL & F_SF) != 0, of = (FL & F_OF) != 0;
    switch (n >> 1) {
    case 0: r = of; break;
    case 1: r = (FL & F_CF) != 0; break;
    case 2: r = (FL & F_ZF) != 0; break;
    case 3: r = (FL & (F_CF | F_ZF)) != 0; break;
    case 4: r = sf; break;
    case 5: r = (FL & F_PF) != 0; break;
    case 6: r = sf != of; break;
    default: r = (FL & F_ZF) || (sf != of); break;
    }
    return (n & 1) ? !r : r;
}

/* forward decls */
static void hle(int n);
static void poll(void);
static u8  port_in(u16 p);
static void port_out(u16 p, u8 v);
static void terminate(int code);

static void do_int(int n, int soft)
{
    u16 vo = rd16(0, (u16)(n * 4)), vs = rd16(0, (u16)(n * 4 + 2));
    if (vs == STUBSEG) {
        retry_int = 0;
        hle(n);
        if (retry_int && soft) { IP = (u16)(IP - 2); poll(); }
        return;
    }
    push16(FL); push16(CS); push16(IP);
    FL &= (u16)~(F_IF | F_TF);
    CS = vs; IP = vo;
}

static void div_error(void) { do_int(0, 0); }

static void strop(u8 op, int rep)
{
    int w = (op & 1) ? 16 : 8, step = w / 8, done;
    u16 sseg = S[ovr >= 0 ? ovr : 3];
    u32 a, b;
    if (FL & F_DF) step = -step;
    for (;;) {
        if (rep && CX == 0) break;
        switch (op & 0xFE) {
        case 0xA4:
            if (w == 8) wr8(ES, DI, rd8(sseg, SI)); else wr16(ES, DI, rd16(sseg, SI));
            SI = (u16)(SI + step); DI = (u16)(DI + step); break;
        case 0xA6:
            a = (w == 8) ? rd8(sseg, SI) : rd16(sseg, SI);
            b = (w == 8) ? rd8(ES, DI) : rd16(ES, DI);
            alu(7, a, b, w); SI = (u16)(SI + step); DI = (u16)(DI + step); break;
        case 0xAA:
            if (w == 8) wr8(ES, DI, AL); else wr16(ES, DI, AX);
            DI = (u16)(DI + step); break;
        case 0xAC:
            if (w == 8) AL = rd8(sseg, SI); else AX = rd16(sseg, SI);
            SI = (u16)(SI + step); break;
        default: /* AE */
            b = (w == 8) ? rd8(ES, DI) : rd16(ES, DI);
            alu(7, (w == 8) ? AL : AX, b, w); DI = (u16)(DI + step); break;
        }
        if (!rep) break;
        CX = (u16)(CX - 1);
        done = 0;
        if ((op & 0xFE) == 0xA6 || (op & 0xFE) == 0xAE) {
            if (rep == 1 && !(FL & F_ZF)) done = 1;     /* REPE  */
            if (rep == 2 && (FL & F_ZF)) done = 1;      /* REPNE */
        }
        if (done) break;
    }
}

static void step(void)
{
    u8 op;
    int rep = 0, w, d;
    u32 a, b, r;
    u16 t;
    s32 sa, sb;

    ovr = -1;
    for (;;) {
        op = fetch8();
        if (op == 0x26) ovr = 0; else if (op == 0x2E) ovr = 1;
        else if (op == 0x36) ovr = 2; else if (op == 0x3E) ovr = 3;
        else if (op == 0xF2) rep = 2; else if (op == 0xF3) rep = 1;
        else if (op == 0xF0) { /* lock */ }
        else break;
    }

    if (op < 0x40 && (op & 7) < 6) {                   /* ALU group */
        int aop = op >> 3;
        w = (op & 1) ? 16 : 8;
        switch (op & 7) {
        case 0: case 1:
            modrm(); a = rmg(w); b = rg(m_reg, w); r = alu(aop, a, b, w);
            if (aop != 7) rms(w, r); break;
        case 2: case 3:
            modrm(); a = rg(m_reg, w); b = rmg(w); r = alu(aop, a, b, w);
            if (aop != 7) rs(m_reg, w, r); break;
        case 4: b = fetch8(); r = alu(aop, AL, b, 8); if (aop != 7) AL = (u8)r; break;
        default: b = fetch16(); r = alu(aop, AX, b, 16); if (aop != 7) AX = (u16)r; break;
        }
        return;
    }

    switch (op) {
    case 0x06: case 0x0E: case 0x16: case 0x1E: push16(S[(op >> 3) & 3]); break;
    case 0x07: case 0x17: case 0x1F: S[(op >> 3) & 3] = pop16(); break;
    case 0x27: /* daa */
        { u8 al = AL; int oc = (FL & F_CF) != 0, c2 = oc;
          if ((al & 0x0F) > 9 || (FL & F_AF)) { c2 = oc || ((u32)al + 6 > 0xFF); al = (u8)(al + 6); setf(F_AF, 1); } else setf(F_AF, 0);
          if (AL > 0x99 || oc) { al = (u8)(al + 0x60); c2 = 1; }
          AL = al; setf(F_CF, c2); setszp(al, 8); }
        break;
    case 0x2F: /* das */
        { u8 al = AL; int oc = (FL & F_CF) != 0, c2 = oc;
          if ((al & 0x0F) > 9 || (FL & F_AF)) { c2 = oc || (al < 6); al = (u8)(al - 6); setf(F_AF, 1); } else setf(F_AF, 0);
          if (AL > 0x99 || oc) { al = (u8)(al - 0x60); c2 = 1; }
          AL = al; setf(F_CF, c2); setszp(al, 8); }
        break;
    case 0x37: /* aaa */
        if ((AL & 0x0F) > 9 || (FL & F_AF)) { AX = (u16)(AX + 0x106); setf(F_AF, 1); setf(F_CF, 1); }
        else { setf(F_AF, 0); setf(F_CF, 0); }
        AL &= 0x0F; break;
    case 0x3F: /* aas */
        if ((AL & 0x0F) > 9 || (FL & F_AF)) { AX = (u16)(AX - 6); AH = (u8)(AH - 1); setf(F_AF, 1); setf(F_CF, 1); }
        else { setf(F_AF, 0); setf(F_CF, 0); }
        AL &= 0x0F; break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        { u16 cf = (u16)(FL & F_CF); R[op & 7] = (u16)alu(0, R[op & 7], 1, 16); setf(F_CF, cf); } break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        { u16 cf = (u16)(FL & F_CF); R[op & 7] = (u16)alu(5, R[op & 7], 1, 16); setf(F_CF, cf); } break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        if ((op & 7) == 4) push16((u16)(SP - 2)); else push16(R[op & 7]); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        R[op & 7] = pop16(); break;

    case 0x60: t = SP; push16(AX); push16(CX); push16(DX); push16(BX); push16(t); push16(BP); push16(SI); push16(DI); break;
    case 0x61: DI = pop16(); SI = pop16(); BP = pop16(); (void)pop16(); BX = pop16(); DX = pop16(); CX = pop16(); AX = pop16(); break;
    case 0x68: push16(fetch16()); break;
    case 0x6A: push16((u16)(s16)(s8)fetch8()); break;
    case 0x69: case 0x6B:
        modrm(); sa = (s16)rmg(16);
        sb = (op == 0x69) ? (s16)fetch16() : (s16)(s8)fetch8();
        r = (u32)(sa * sb); R[m_reg] = (u16)r;
        setf(F_CF, (s32)r != (s16)r); setf(F_OF, (s32)r != (s16)r); break;
    case 0x6C: case 0x6D: case 0x6E: case 0x6F:
        /* ins/outs: not needed */ break;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        d = (s8)fetch8(); if (cond(op & 15)) IP = (u16)(IP + d); break;

    case 0x80: case 0x81: case 0x82: case 0x83:
        w = (op & 1) ? 16 : 8; modrm(); a = rmg(w);
        if (op == 0x81) b = fetch16(); else if (op == 0x83) b = (u16)(s16)(s8)fetch8(); else b = fetch8();
        if (w == 8) b &= 0xFF;
        r = alu(m_reg, a, b, w); if (m_reg != 7) rms(w, r); break;
    case 0x84: case 0x85:
        w = (op & 1) ? 16 : 8; modrm(); alu(4, rmg(w), rg(m_reg, w), w); break;
    case 0x86: case 0x87:
        w = (op & 1) ? 16 : 8; modrm(); a = rmg(w); b = rg(m_reg, w); rms(w, b); rs(m_reg, w, a); break;
    case 0x88: case 0x89:
        w = (op & 1) ? 16 : 8; modrm(); rms(w, rg(m_reg, w)); break;
    case 0x8A: case 0x8B:
        w = (op & 1) ? 16 : 8; modrm(); rs(m_reg, w, rmg(w)); break;
    case 0x8C: modrm(); rms(16, S[m_reg & 3]); break;
    case 0x8D: modrm(); R[m_reg] = m_off; break;
    case 0x8E: modrm(); S[m_reg & 3] = (u16)rmg(16); break;
    case 0x8F: modrm(); t = pop16(); rms(16, t); break;

    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        t = AX; AX = R[op & 7]; R[op & 7] = t; break;
    case 0x98: AX = (u16)(s16)(s8)AL; break;
    case 0x99: DX = (AX & 0x8000) ? 0xFFFF : 0; break;
    case 0x9A: { u16 o = fetch16(), s = fetch16(); push16(CS); push16(IP); CS = s; IP = o; } break;
    case 0x9B: break;
    case 0x9C: push16((u16)(FL | 0x0002)); break;
    case 0x9D: FL = (u16)((pop16() & 0x0FD5) | 0x0002); break;
    case 0x9E: FL = (u16)((FL & 0xFF00) | (AH & 0xD5) | 0x02); break;
    case 0x9F: AH = (u8)((FL & 0xD5) | 0x02); break;

    case 0xA0: AL = rd8(S[ovr >= 0 ? ovr : 3], fetch16()); break;
    case 0xA1: AX = rd16(S[ovr >= 0 ? ovr : 3], fetch16()); break;
    case 0xA2: wr8(S[ovr >= 0 ? ovr : 3], fetch16(), AL); break;
    case 0xA3: wr16(S[ovr >= 0 ? ovr : 3], fetch16(), AX); break;
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        strop(op, rep); break;
    case 0xA8: alu(4, AL, fetch8(), 8); break;
    case 0xA9: alu(4, AX, fetch16(), 16); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        reg8s(op & 7, fetch8()); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        R[op & 7] = fetch16(); break;

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        w = (op & 1) ? 16 : 8; modrm();
        if (op == 0xC0 || op == 0xC1) d = fetch8() & 0x1F;
        else if (op & 2) d = CL; else d = 1;
        a = rmg(w); rms(w, shiftop(m_reg, a, d, w)); break;
    case 0xC2: t = fetch16(); IP = pop16(); SP = (u16)(SP + t); break;
    case 0xC3: IP = pop16(); break;
    case 0xC4: case 0xC5:
        modrm(); R[m_reg] = rd16(m_seg, m_off);
        if (op == 0xC4) ES = rd16(m_seg, (u16)(m_off + 2)); else DS = rd16(m_seg, (u16)(m_off + 2));
        break;
    case 0xC6: case 0xC7:
        w = (op & 1) ? 16 : 8; modrm(); rms(w, (w == 8) ? fetch8() : fetch16()); break;
    case 0xC8: { u16 sz = fetch16(); u8 lv = fetch8(); push16(BP); BP = SP; (void)lv; SP = (u16)(SP - sz); } break;
    case 0xC9: SP = BP; BP = pop16(); break;
    case 0xCA: t = fetch16(); IP = pop16(); CS = pop16(); SP = (u16)(SP + t); break;
    case 0xCB: IP = pop16(); CS = pop16(); break;
    case 0xCC: do_int(3, 0); break;
    case 0xCD: d = fetch8(); do_int(d, 1); break;
    case 0xCE: if (FL & F_OF) do_int(4, 0); break;
    case 0xCF: IP = pop16(); CS = pop16(); FL = (u16)((pop16() & 0x0FD5) | 0x0002); break;
    case 0xD4: d = fetch8(); if (!d) { div_error(); break; } AH = (u8)(AL / d); AL = (u8)(AL % d); setszp(AL, 8); break;
    case 0xD5: d = fetch8(); AL = (u8)(AH * d + AL); AH = 0; setszp(AL, 8); break;
    case 0xD6: AL = (FL & F_CF) ? 0xFF : 0; break;
    case 0xD7: AL = rd8(S[ovr >= 0 ? ovr : 3], (u16)(BX + AL)); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm(); break;                                   /* no FPU */
    case 0xE0: d = (s8)fetch8(); CX = (u16)(CX - 1); if (CX && !(FL & F_ZF)) IP = (u16)(IP + d); break;
    case 0xE1: d = (s8)fetch8(); CX = (u16)(CX - 1); if (CX && (FL & F_ZF)) IP = (u16)(IP + d); break;
    case 0xE2: d = (s8)fetch8(); CX = (u16)(CX - 1); if (CX) IP = (u16)(IP + d); break;
    case 0xE3: d = (s8)fetch8(); if (!CX) IP = (u16)(IP + d); break;
    case 0xE4: AL = port_in(fetch8()); break;
    case 0xE5: { u16 p = fetch8(); AL = port_in(p); AH = port_in((u16)(p + 1)); } break;
    case 0xE6: port_out(fetch8(), AL); break;
    case 0xE7: { u16 p = fetch8(); port_out(p, AL); port_out((u16)(p + 1), AH); } break;
    case 0xE8: t = fetch16(); push16(IP); IP = (u16)(IP + t); break;
    case 0xE9: t = fetch16(); IP = (u16)(IP + t); break;
    case 0xEA: { u16 o = fetch16(), s = fetch16(); CS = s; IP = o; } break;
    case 0xEB: d = (s8)fetch8(); IP = (u16)(IP + d); break;
    case 0xEC: AL = port_in(DX); break;
    case 0xED: AL = port_in(DX); AH = port_in((u16)(DX + 1)); break;
    case 0xEE: port_out(DX, AL); break;
    case 0xEF: port_out(DX, AL); port_out((u16)(DX + 1), AH); break;
    case 0xF4: IP = (u16)(IP - 1); retry_int = 0; plat_sleep(1); icount |= 0xFFF; break;   /* HLT: wait for IRQ */
    case 0xF5: FL ^= F_CF; break;
    case 0xF6: case 0xF7:
        w = (op & 1) ? 16 : 8; modrm();
        switch (m_reg) {
        case 0: case 1: a = rmg(w); b = (w == 8) ? fetch8() : fetch16(); alu(4, a, b, w); break;
        case 2: rms(w, ~rmg(w)); break;
        case 3: a = rmg(w); r = alu(5, 0, a, w); setf(F_CF, a != 0); rms(w, r); break;
        case 4:
            a = rmg(w);
            if (w == 8) { r = AL * a; AX = (u16)r; setf(F_CF, AH != 0); setf(F_OF, AH != 0); }
            else { r = (u32)AX * a; AX = (u16)r; DX = (u16)(r >> 16); setf(F_CF, DX != 0); setf(F_OF, DX != 0); }
            break;
        case 5:
            if (w == 8) { sa = (s8)AL; sb = (s8)rmg(8); r = (u32)(sa * sb); AX = (u16)r; setf(F_CF, (s16)r != (s8)AL); setf(F_OF, (s16)r != (s8)AL); }
            else { sa = (s16)AX; sb = (s16)rmg(16); r = (u32)(sa * sb); AX = (u16)r; DX = (u16)(r >> 16); setf(F_CF, (s32)r != (s16)AX); setf(F_OF, (s32)r != (s16)AX); }
            break;
        case 6:
            a = rmg(w);
            if (a == 0) { div_error(); break; }
            if (w == 8) { u32 q = AX / a; if (q > 0xFF) { div_error(); break; } t = (u16)(AX % a); AL = (u8)q; AH = (u8)t; }
            else { u32 n = ((u32)DX << 16) | AX, q = n / a; if (q > 0xFFFF) { div_error(); break; } AX = (u16)q; DX = (u16)(n % a); }
            break;
        default:
            if (w == 8) { s32 dv = (s8)rmg(8), n = (s16)AX; if (!dv) { div_error(); break; } sa = n / dv; if (sa > 127 || sa < -128) { div_error(); break; } AL = (u8)sa; AH = (u8)(n % dv); }
            else { s32 dv = (s16)rmg(16); s32 n = (s32)(((u32)DX << 16) | AX); if (!dv) { div_error(); break; } if (n == (s32)0x80000000L && dv == -1) { div_error(); break; } sa = n / dv; if (sa > 32767 || sa < -32768) { div_error(); break; } AX = (u16)sa; DX = (u16)(n % dv); }
            break;
        }
        break;
    case 0xF8: FL &= (u16)~F_CF; break;
    case 0xF9: FL |= F_CF; break;
    case 0xFA: FL &= (u16)~F_IF; break;
    case 0xFB: FL |= F_IF; break;
    case 0xFC: FL &= (u16)~F_DF; break;
    case 0xFD: FL |= F_DF; break;
    case 0xFE: case 0xFF:
        w = (op & 1) ? 16 : 8; modrm();
        switch (m_reg) {
        case 0: case 1:
            { u16 cf = (u16)(FL & F_CF); a = rmg(w); r = alu(m_reg ? 5 : 0, a, 1, w); setf(F_CF, cf); rms(w, r); }
            break;
        case 2: t = (u16)rmg(16); push16(IP); IP = t; break;
        case 3: { u16 o = rd16(m_seg, m_off), s = rd16(m_seg, (u16)(m_off + 2)); push16(CS); push16(IP); CS = s; IP = o; } break;
        case 4: IP = (u16)rmg(16); break;
        case 5: { u16 o = rd16(m_seg, m_off), s = rd16(m_seg, (u16)(m_off + 2)); CS = s; IP = o; } break;
        default: t = (u16)rmg(16); push16(t); break;
        }
        break;
    default:
        fprintf(stderr, "PLW: illegal opcode %02X at %04X:%04X\n", op, CS, (u16)(IP - 1));
        terminate(99);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Video                                                               */
/* ------------------------------------------------------------------ */
static u8  crtc_reg[32], crtc_idx;
static u8  vshadow8[4000], vshadow0[4000];
static int vforce = 1, vactive_b0 = 0;
static int cur_start = 6, cur_end = 7;

static int  cur_get(void) { return (crtc_reg[14] << 8) | crtc_reg[15]; }
static void cur_set(int pos) { crtc_reg[14] = (u8)(pos >> 8); crtc_reg[15] = (u8)pos; }
static u8  *vram(void) { return mem + VID_B8; }

static void video_clear(u8 attr)
{
    int i;
    for (i = 0; i < 2000; i++) { vram()[i * 2] = ' '; vram()[i * 2 + 1] = attr; }
}

static void video_scroll(int up, int n, int t, int l, int b, int r, u8 attr)
{
    int row, col, rows = b - t + 1, i;
    if (b > 24) b = 24;
    if (r > 79) r = 79;
    rows = b - t + 1;
    if (n == 0 || n >= rows) {
        for (row = t; row <= b; row++)
            for (col = l; col <= r; col++) { vram()[(row * 80 + col) * 2] = ' '; vram()[(row * 80 + col) * 2 + 1] = attr; }
        return;
    }
    for (i = 0; i < rows - n; i++) {
        int dst = up ? t + i : b - i, src = up ? t + i + n : b - i - n;
        for (col = l; col <= r; col++) {
            vram()[(dst * 80 + col) * 2] = vram()[(src * 80 + col) * 2];
            vram()[(dst * 80 + col) * 2 + 1] = vram()[(src * 80 + col) * 2 + 1];
        }
    }
    for (i = 0; i < n; i++) {
        row = up ? b - i : t + i;
        for (col = l; col <= r; col++) { vram()[(row * 80 + col) * 2] = ' '; vram()[(row * 80 + col) * 2 + 1] = attr; }
    }
}

static void tty_putc(u8 c)
{
    int pos = cur_get(), row = pos / 80, col = pos % 80;
    switch (c) {
    case 7: break;
    case 8: if (col) col--; break;
    case 9: col = (col + 8) & ~7; if (col > 79) col = 79; break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
        vram()[(row * 80 + col) * 2] = c;
        col++;
        if (col > 79) { col = 0; row++; }
        break;
    }
    if (row > 24) { video_scroll(1, 1, 0, 0, 24, 79, 0x07); row = 24; }
    cur_set(row * 80 + col);
}

static void video_refresh(void)
{
    const u8 *src;
    if (memcmp(mem + VID_B0, vshadow0, 4000) != 0) { memcpy(vshadow0, mem + VID_B0, 4000); vactive_b0 = 1; vforce = 1; }
    if (memcmp(mem + VID_B8, vshadow8, 4000) != 0) { memcpy(vshadow8, mem + VID_B8, 4000); vactive_b0 = 0; vforce = 1; }
    if (!vforce) return;
    vforce = 0;
    src = vactive_b0 ? mem + VID_B0 : mem + VID_B8;
    plat_draw(src, cur_get() / 80, cur_get() % 80, !(crtc_reg[10] & 0x20));
}

/* ------------------------------------------------------------------ */
/* Keyboard queue                                                      */
/* ------------------------------------------------------------------ */
static u16 kq[64];
static int kq_h = 0, kq_t = 0;
static int pending_scan = -1;     /* DOS: second half of an extended key */

static void key_pump(void)
{
    u16 k;
    while (plat_getkey(&k)) {
        int nt = (kq_t + 1) & 63;
        if (nt != kq_h) { kq[kq_t] = k; kq_t = nt; }
    }
}
static int idle_cnt = 0;
static int key_avail(void)
{
    key_pump();
    if (kq_h != kq_t || pending_scan >= 0) { idle_cnt = 0; return 1; }
    if ((++idle_cnt & 15) == 0) { plat_sleep(1); poll(); }
    return 0;
}
static u16 key_take(void) { u16 k = kq[kq_h]; kq_h = (kq_h + 1) & 63; return k; }

/* ------------------------------------------------------------------ */
/* 8259 PIC + 8250 UART                                                */
/* ------------------------------------------------------------------ */
static u8 pic_imr = 0xFF, pic_isr = 0;

typedef struct {
    int open;
    u8 ring[4096];
    int head, tail;
    u8 ier, lcr, mcr, scr, fcr, dll, dlm;
} UART;
static UART uart[2];                 /* 0 = COM1 (3F8, IRQ4)  1 = COM2 (2F8, IRQ3) */
static const char *com_name[2] = { "COM1", "COM2" };

static int ring_n(UART *u) { return (u->tail - u->head) & 4095; }

static void uart_apply(int n)
{
    UART *u = &uart[n];
    long div = (long)(u->dlm << 8) | u->dll, baud;
    int bits = (u->lcr & 3) + 5, parity = 0, stop = (u->lcr & 4) ? 2 : 1;
    if (div <= 0) return;
    baud = 115200L / div;
    if (u->lcr & 8) parity = (u->lcr & 0x10) ? 2 : 1;     /* 1 odd, 2 even */
    slog("COM%d config %ld baud %d%c%d", n + 1, baud, bits, "NOE"[parity], stop);
    ser_config(n, baud, bits, parity, stop);
}

static int uart_irq(int n)
{
    UART *u = &uart[n];
    return (u->ier & 1) && (u->mcr & 8) && ring_n(u) > 0;
}

static void pic_update(void)
{
    int irq, lines = 0;
    if (uart_irq(0)) lines |= 0x10;
    if (uart_irq(1)) lines |= 0x08;
    lines &= ~pic_imr;
    irq_hint = 0;
    for (irq = 0; irq < 8; irq++) {
        if (pic_isr & (1 << irq)) break;           /* in-service blocks lower priority */
        if (lines & (1 << irq)) { irq_hint = irq + 1; break; }
    }
}

static void deliver_irq(void)
{
    int irq = irq_hint - 1;
    irq_hint = 0;
    if (getenv("PLW_TRACE")) fprintf(stderr, "[irq%d delivered, ring=%d]\n", irq, ring_n(&uart[irq == 4 ? 0 : 1]));
    pic_isr |= (u8)(1 << irq);
    do_int(8 + irq, 0);
    pic_update();
}

static int uart_sel(u16 p, int *reg)
{
    if (p >= 0x3F8 && p <= 0x3FF) { *reg = p - 0x3F8; return 0; }
    if (p >= 0x2F8 && p <= 0x2FF) { *reg = p - 0x2F8; return 1; }
    return -1;
}

static u8 port_in(u16 p)
{
    int n, reg;
    UART *u;
    static u8 vsync = 0;
    if ((n = uart_sel(p, &reg)) >= 0) {
        u = &uart[n];
        switch (reg) {
        case 0:
            if (u->lcr & 0x80) return u->dll;
            if (u->head != u->tail) { u8 c = u->ring[u->head]; u->head = (u->head + 1) & 4095; pic_update(); if (getenv("PLW_TRACE")) fprintf(stderr, "[rbr read %02X]\n", c); return c; }
            return 0;
        case 1: return (u->lcr & 0x80) ? u->dlm : u->ier;
        case 2: return uart_irq(n) ? 0x04 : 0x01;
        case 3: return u->lcr;
        case 4: return u->mcr;
        case 5: return (u8)(0x60 | (ring_n(u) ? 1 : 0));
        case 6: return u->open ? (u8)ser_msr(n) : 0;
        default: return u->scr;
        }
    }
    switch (p) {
    case 0x20: return pic_isr;
    case 0x21: return pic_imr;
    case 0x3DA: case 0x3BA: vsync ^= 9; return vsync;
    case 0x3D4: case 0x3B4: return crtc_idx;
    case 0x3D5: case 0x3B5: return crtc_reg[crtc_idx & 31];
    case 0x61: return 0;
    default: return 0xFF;
    }
}

static void port_out(u16 p, u8 v)
{
    int n, reg, i;
    UART *u;
    if ((n = uart_sel(p, &reg)) >= 0) {
        u = &uart[n];
        switch (reg) {
        case 0:
            if (u->lcr & 0x80) { u->dll = v; uart_apply(n); }
            else if (u->open) { slog("COM%d TX %02X", n + 1, v); ser_write(n, v); }
            break;
        case 1:
            if (u->lcr & 0x80) { u->dlm = v; uart_apply(n); } else { u->ier = v; pic_update(); }
            break;
        case 2: u->fcr = v; break;
        case 3: u->lcr = v; if (!(v & 0x80)) uart_apply(n); break;
        case 4: u->mcr = v; slog("COM%d MCR %02X", n + 1, v); if (u->open) ser_mcr(n, v); pic_update(); break;
        case 7: u->scr = v; break;
        default: break;
        }
        return;
    }
    switch (p) {
    case 0x20:
        if (v & 0x20) {                                /* EOI */
            if ((v & 0xE0) == 0x60) pic_isr &= (u8)~(1 << (v & 7));
            else for (i = 0; i < 8; i++) if (pic_isr & (1 << i)) { pic_isr &= (u8)~(1 << i); break; }
            pic_update();
        }
        break;
    case 0x21: pic_imr = v; pic_update(); break;
    case 0x3D4: case 0x3B4: crtc_idx = v; break;
    case 0x3D5: case 0x3B5: crtc_reg[crtc_idx & 31] = v; vforce = 1; break;
    default: break;
    }
}

static void serial_poll(void)
{
    int n, got, i;
    u8 buf[256];
    for (n = 0; n < 2; n++) {
        UART *u = &uart[n];
        if (!u->open) continue;
        got = ser_read(n, buf, (int)sizeof buf);
        for (i = 0; i < got; i++) {
            int nt = (u->tail + 1) & 4095;
            slog("COM%d RX %02X", n + 1, buf[i]);
            if (nt != u->head) { u->ring[u->tail] = buf[i]; u->tail = nt; } else slog("COM%d RING FULL - byte dropped", n + 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/* DOS emulation                                                       */
/* ------------------------------------------------------------------ */
#define MAXH 40
static int hmap[MAXH];                   /* DOS handle -> host fd (-1 free, -2 console) */
static u16 dta_seg, dta_off;
static u16 psp_seg, prog_end;            /* end of the program's memory block */
static u16 arena_ptr;                    /* simple paragraph allocator (INT 21h/48h) */
static char host_cwd[512];
static u32 start_ms;

static void set_err(int e) { setf(F_CF, 1); AX = (u16)e; }

static void get_dosstr(u16 seg, u16 off, char *dst, int max)
{
    int i = 0;
    while (i < max - 1) { char c = (char)rd8(seg, (u16)(off + i)); if (!c) break; dst[i++] = c; }
    dst[i] = 0;
}

static void host_path(const char *dos, char *out, int max)
{
#ifdef PLW_WIN32
    strncpy(out, dos, (size_t)(max - 1)); out[max - 1] = 0;
#else
    /* case-insensitive resolution of a DOS path on a case-sensitive host */
    char tmp[512], cur[512];
    char *p, *tok;
    int i;
    if (dos[1] == ':') dos += 2;
    for (i = 0; dos[i] && i < 510; i++) tmp[i] = (dos[i] == '\\') ? '/' : dos[i];
    tmp[i] = 0;
    cur[0] = 0;
    p = tmp;
    if (*p == '/') { strcpy(cur, "/"); p++; }
    else strcpy(cur, ".");
    for (tok = strtok(p, "/"); tok; tok = strtok(NULL, "/")) {
        DIR *d;
        struct dirent *e;
        char cand[512];
        int found = 0;
        if (!strcmp(tok, ".") || !strcmp(tok, "..")) { strcat(cur, "/"); strcat(cur, tok); continue; }
        d = opendir(cur);
        if (d) {
            while ((e = readdir(d)) != NULL) if (!strcasecmp(e->d_name, tok)) { snprintf(cand, sizeof cand, "%s/%s", cur, e->d_name); found = 1; break; }
            closedir(d);
        }
        if (!found) snprintf(cand, sizeof cand, "%s/%s", cur, tok);
        strcpy(cur, cand);
    }
    strncpy(out, cur, (size_t)(max - 1)); out[max - 1] = 0;
#endif
}

static int hnew(int fd)
{
    int i;
    for (i = 5; i < MAXH; i++) if (hmap[i] == -1) { hmap[i] = fd; return i; }
    return -1;
}

static void dos_find_fill(const char *name, u8 attr, u32 size, time_t mt)
{
    struct tm *tm = localtime(&mt);
    u16 dt = 0, dd = 0, i;
    char n83[16];
    if (tm) {
        dt = (u16)((tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
        dd = (u16)(((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
    }
    wr8(dta_seg, (u16)(dta_off + 0x15), attr);
    wr16(dta_seg, (u16)(dta_off + 0x16), dt);
    wr16(dta_seg, (u16)(dta_off + 0x18), dd);
    wr16(dta_seg, (u16)(dta_off + 0x1A), (u16)size);
    wr16(dta_seg, (u16)(dta_off + 0x1C), (u16)(size >> 16));
    strncpy(n83, name, 12); n83[12] = 0;
    for (i = 0; n83[i]; i++) n83[i] = (char)toupper((unsigned char)n83[i]);
    for (i = 0; i < 13; i++) wr8(dta_seg, (u16)(dta_off + 0x1E + i), (u8)(i <= strlen(n83) ? n83[i] : 0));
}

#ifdef PLW_WIN32
static HANDLE find_h = INVALID_HANDLE_VALUE;
static u16 find_attr = 0;
static int find_ok(const WIN32_FIND_DATAA *fd)
{
    DWORD a = fd->dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY);
    return (a & ~(DWORD)find_attr) == 0;
}
static int find_next_fill(WIN32_FIND_DATAA *fd)
{
    FILETIME lft;
    WORD dd, dt;
    const char *nm = fd->cAlternateFileName[0] ? fd->cAlternateFileName : fd->cFileName;
    char n83[16];
    int i;
    u8 attr = (u8)(fd->dwFileAttributes & 0x3F);
    FileTimeToLocalFileTime(&fd->ftLastWriteTime, &lft);
    FileTimeToDosDateTime(&lft, &dd, &dt);
    wr8(dta_seg, (u16)(dta_off + 0x15), attr);
    wr16(dta_seg, (u16)(dta_off + 0x16), dt);
    wr16(dta_seg, (u16)(dta_off + 0x18), dd);
    wr16(dta_seg, (u16)(dta_off + 0x1A), (u16)fd->nFileSizeLow);
    wr16(dta_seg, (u16)(dta_off + 0x1C), (u16)(fd->nFileSizeLow >> 16));
    strncpy(n83, nm, 12); n83[12] = 0;
    for (i = 0; n83[i]; i++) n83[i] = (char)toupper((unsigned char)n83[i]);
    for (i = 0; i < 13; i++) wr8(dta_seg, (u16)(dta_off + 0x1E + i), (u8)(i <= (int)strlen(n83) ? n83[i] : 0));
    return 1;
}
static int dos_findfirst(const char *pat)
{
    WIN32_FIND_DATAA fd;
    if (find_h != INVALID_HANDLE_VALUE) { FindClose(find_h); find_h = INVALID_HANDLE_VALUE; }
    find_attr = CX;
    find_h = FindFirstFileA(pat, &fd);
    if (find_h == INVALID_HANDLE_VALUE) return 0;
    while (!find_ok(&fd))
        if (!FindNextFileA(find_h, &fd)) return 0;
    return find_next_fill(&fd);
}
static int dos_findnext(void)
{
    WIN32_FIND_DATAA fd;
    if (find_h == INVALID_HANDLE_VALUE) return 0;
    for (;;) {
        if (!FindNextFileA(find_h, &fd)) return 0;
        if (!find_ok(&fd)) continue;
        return find_next_fill(&fd);
    }
}
#else
static DIR *find_d = NULL;
static char find_pat[256], find_dir[512];
static u16 find_attr = 0;
static int find_match_next(void)
{
    struct dirent *e;
    while (find_d && (e = readdir(find_d)) != NULL) {
        struct stat st;
        char full[1024];
        if (fnmatch(find_pat, e->d_name, FNM_CASEFOLD) != 0) continue;
        snprintf(full, sizeof full, "%s/%s", find_dir, e->d_name);
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode) && !(find_attr & 0x10)) continue;
        dos_find_fill(e->d_name, (u8)(S_ISDIR(st.st_mode) ? 0x10 : 0x20), (u32)st.st_size, st.st_mtime);
        return 1;
    }
    return 0;
}
static int dos_findfirst(const char *pat)
{
    char hp[512], *sl;
    if (find_d) { closedir(find_d); find_d = NULL; }
    find_attr = CX;
    host_path(pat, hp, sizeof hp);
    sl = strrchr(hp, '/');
    if (sl) { *sl = 0; strncpy(find_pat, sl + 1, 255); strncpy(find_dir, hp[0] ? hp : "/", 511); }
    else { strncpy(find_pat, hp, 255); strcpy(find_dir, "."); }
    find_pat[255] = 0; find_dir[511] = 0;
    find_d = opendir(find_dir);
    if (!find_d) return 0;
    return find_match_next();
}
static int dos_findnext(void) { return find_match_next(); }
#endif

static int fcb_name(u16 seg, u16 off, char *out)
{
    int i, n = 0;
    for (i = 1; i <= 8; i++) { char c = (char)rd8(seg, (u16)(off + i)); if (c == ' ') break; out[n++] = c; }
    if (rd8(seg, (u16)(off + 9)) != ' ') {
        out[n++] = '.';
        for (i = 9; i <= 11; i++) { char c = (char)rd8(seg, (u16)(off + i)); if (c == ' ') break; out[n++] = c; }
    }
    out[n] = 0;
    return n;
}

static void dos_getch_wait(int echo, int ctl)
{
    u16 k;
    (void)ctl;
    if (pending_scan >= 0) { AL = (u8)pending_scan; pending_scan = -1; return; }
    if (!key_avail()) { retry_int = 1; plat_sleep(1); return; }
    k = key_take();
    if ((k & 0xFF) == 0) { AL = 0; pending_scan = k >> 8; }
    else { AL = (u8)k; if (echo) tty_putc(AL); }
}

static u8 line_buf[256];
static int line_len = 0;

static void dos21(void)
{
    u8 ah = AH;
    u16 t;
    char path[300], hp[600];
    int h, n;
    setf(F_CF, 0);
#ifdef PLW_WATCH
    if (getenv("PLW_DOS")) {
        fprintf(stderr, "DOS %02X AL=%02X BX=%04X CX=%04X DX=%04X", ah, AL, BX, CX, DX);
        if (ah == 0x3D || ah == 0x3C || ah == 0x4E || ah == 0x41 || ah == 0x43) { char pth[100]; get_dosstr(DS, DX, pth, sizeof pth); fprintf(stderr, " '%s'", pth); }
        if (ah == 0x09) { char pth[100]; int q; for (q = 0; q < 99 && rd8(DS, (u16)(DX + q)) != '$'; q++) pth[q] = (char)rd8(DS, (u16)(DX + q)); pth[q] = 0; fprintf(stderr, " '%s'", pth); }
        fprintf(stderr, "\n");
    }
#endif
#ifdef PLW_WATCH
    if (ah == 0x4C && getenv("PLW_DOS")) { int q; fprintf(stderr, "EXIT stack:"); for (q = 0; q < 24; q++) fprintf(stderr, " %04X", rd16(SS, (u16)(SP + q * 2))); fprintf(stderr, "  BP=%04X SS=%04X CS=%04X\n", BP, SS, CS); }
#endif
    switch (ah) {
    case 0x00: terminate(0); break;
    case 0x01: dos_getch_wait(1, 1); break;
    case 0x02: tty_putc(DL); break;
    case 0x06:
        if (DL == 0xFF) {
            if (pending_scan >= 0) { AL = (u8)pending_scan; pending_scan = -1; setf(F_ZF, 0); }
            else if (key_avail()) {
                u16 k = key_take();
                if ((k & 0xFF) == 0) { AL = 0; pending_scan = k >> 8; } else AL = (u8)k;
                setf(F_ZF, 0);
            } else { AL = 0; setf(F_ZF, 1); }
        } else tty_putc(DL);
        break;
    case 0x07: case 0x08: dos_getch_wait(0, ah == 0x08); break;
    case 0x09:
        for (n = 0, t = DX; n < 65535 && rd8(DS, t) != '$'; n++, t++) tty_putc(rd8(DS, t));
        break;
    case 0x0A:
        {
            int maxl = rd8(DS, DX);
            while (key_avail()) {
                u16 k = key_take();
                u8 c = (u8)k;
                if (c == 13) {
                    wr8(DS, (u16)(DX + 1), (u8)line_len);
                    for (n = 0; n < line_len; n++) wr8(DS, (u16)(DX + 2 + n), line_buf[n]);
                    wr8(DS, (u16)(DX + 2 + line_len), 13);
                    tty_putc(13); tty_putc(10); line_len = 0; return;
                } else if (c == 8) { if (line_len) { line_len--; tty_putc(8); tty_putc(' '); tty_putc(8); } }
                else if (c >= 32 && line_len < maxl - 1) { line_buf[line_len++] = c; tty_putc(c); }
            }
            retry_int = 1; plat_sleep(1);
        }
        break;
    case 0x0B: AL = key_avail() ? 0xFF : 0x00; break;
    case 0x0C:
        kq_h = kq_t = 0; pending_scan = -1;
        if (AL == 1 || AL == 6 || AL == 7 || AL == 8 || AL == 0x0A) { AH = AL; dos21(); }
        break;
    case 0x0E: AL = 26; break;
    case 0x13:
        fcb_name(DS, DX, path); host_path(path, hp, sizeof hp);
        AL = (u8)(remove(hp) == 0 ? 0 : 0xFF);
        break;
    case 0x17:
        { char o[40], nn[40], ho[600], hn[600];
          fcb_name(DS, DX, o); fcb_name(DS, (u16)(DX + 0x10), nn);
          host_path(o, ho, sizeof ho); host_path(nn, hn, sizeof hn);
          AL = (u8)(rename(ho, hn) == 0 ? 0 : 0xFF); }
        break;
    case 0x19:
#ifdef PLW_WIN32
        { char cd[260]; GetCurrentDirectoryA(260, cd); AL = (u8)(toupper((unsigned char)cd[0]) - 'A'); }
#else
        AL = 2;
#endif
        break;
    case 0x1A: dta_seg = DS; dta_off = DX; break;
    case 0x25: wr16(0, (u16)(AL * 4), DX); wr16(0, (u16)(AL * 4 + 2), DS); break;
    case 0x2A:
        { time_t tt = time(NULL); struct tm *tm = localtime(&tt);
          CX = (u16)(tm->tm_year + 1900); DH = (u8)(tm->tm_mon + 1); DL = (u8)tm->tm_mday; AL = (u8)tm->tm_wday; }
        break;
    case 0x2C:
        { static unsigned spin = 0;
          time_t tt = time(NULL); struct tm *tm = localtime(&tt);
          int hs = plat_hund();
          CH = (u8)tm->tm_hour; CL = (u8)tm->tm_min; DH = (u8)tm->tm_sec; DL = (u8)hs;
          if ((++spin & 63) == 0) { plat_sleep(1); poll(); } }   /* PL busy-waits on the clock */
        break;
    case 0x2F: ES = dta_seg; BX = dta_off; break;
    case 0x30: AX = 0x0005; BX = 0; CX = 0; break;
    case 0x35: BX = rd16(0, (u16)(AL * 4)); ES = rd16(0, (u16)(AL * 4 + 2)); break;
    case 0x36:
#ifdef PLW_WIN32
        { DWORD spc, bps, fc, tc;
          if (GetDiskFreeSpaceA(NULL, &spc, &bps, &fc, &tc)) {
              while (tc > 0xFFFF) { spc <<= 1; tc >>= 1; fc >>= 1; }
              AX = (u16)spc; BX = (u16)fc; CX = (u16)bps; DX = (u16)tc;
          } else AX = 0xFFFF; }
#else
        { struct statvfs sv; unsigned long long tc, fc; unsigned spc = 1;
          if (statvfs(".", &sv) == 0) {
              tc = sv.f_blocks * sv.f_frsize / 512; fc = sv.f_bavail * sv.f_frsize / 512;
              while (tc > 0xFFFF) { spc <<= 1; tc >>= 1; fc >>= 1; }
              AX = (u16)spc; BX = (u16)fc; CX = 512; DX = (u16)tc;
          } else AX = 0xFFFF; }
#endif
        break;
    case 0x3B:
        get_dosstr(DS, DX, path, sizeof path); host_path(path, hp, sizeof hp);
        if (chdir(hp) != 0) set_err(3); else AX = 0;
        break;
    case 0x3C: case 0x3D:
        get_dosstr(DS, DX, path, sizeof path); host_path(path, hp, sizeof hp);
        { int fd;
          if (ah == 0x3C) fd = open(hp, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0666);
          else { int m = AL & 3; fd = open(hp, (m == 0 ? O_RDONLY : m == 1 ? O_WRONLY : O_RDWR) | O_BINARY); }
          if (fd < 0) { set_err(ah == 0x3C ? 3 : 2); break; }
          h = hnew(fd);
          if (h < 0) { close(fd); set_err(4); break; }
          AX = (u16)h; }
        break;
    case 0x3E:
        h = BX;
        if (h >= 5 && h < MAXH && hmap[h] >= 0) { close(hmap[h]); hmap[h] = -1; }
        else if (h >= MAXH || hmap[h] == -1) set_err(6);
        break;
    case 0x3F:
        h = BX;
        if (h >= MAXH || hmap[h] == -1) { set_err(6); break; }
        if (hmap[h] == -2) {                             /* console input */
            if (CX == 0) { AX = 0; break; }
            if (pending_scan >= 0) { wr8(DS, DX, (u8)pending_scan); pending_scan = -1; AX = 1; break; }
            if (!key_avail()) { retry_int = 1; plat_sleep(1); break; }
            { u16 k = key_take(); wr8(DS, DX, (u8)k); if (!(k & 0xFF)) pending_scan = k >> 8; else tty_putc((u8)k); AX = 1; }
            break;
        }
        { static u8 iob[65536]; int got, want = CX;
          got = (int)read(hmap[h], iob, (unsigned)want);
          if (got < 0) { set_err(5); break; }
          for (n = 0; n < got; n++) wr8(DS, (u16)(DX + n), iob[n]);
          AX = (u16)got; }
        break;
    case 0x40:
        h = BX;
        if (h >= MAXH || hmap[h] == -1) { set_err(6); break; }
        if (hmap[h] == -2) { for (n = 0; n < CX; n++) tty_putc(rd8(DS, (u16)(DX + n))); AX = CX; break; }
        { static u8 iob2[65536]; int wr;
          for (n = 0; n < CX; n++) iob2[n] = rd8(DS, (u16)(DX + n));
          wr = (CX == 0) ? 0 : (int)write(hmap[h], iob2, CX);
          if (wr < 0) { set_err(5); break; }
          AX = (u16)wr; }
        break;
    case 0x41:
        get_dosstr(DS, DX, path, sizeof path); host_path(path, hp, sizeof hp);
        if (remove(hp) != 0) set_err(2); else AX = 0;
        break;
    case 0x42:
        h = BX;
        if (h >= MAXH || hmap[h] < 0) { set_err(6); break; }
        { long off = (long)(((u32)CX << 16) | DX), r;
          r = (long)lseek(hmap[h], off, AL);
          if (r < 0) { set_err(1); break; }
          DX = (u16)((unsigned long)r >> 16); AX = (u16)r; }
        break;
    case 0x43:
        get_dosstr(DS, DX, path, sizeof path); host_path(path, hp, sizeof hp);
        if (AL == 0) {
#ifdef PLW_WIN32
            DWORD a = GetFileAttributesA(hp);
            if (a == INVALID_FILE_ATTRIBUTES) { set_err(2); break; }
            CX = (u16)(a & 0x3F);
#else
            struct stat st;
            if (stat(hp, &st) != 0) { set_err(2); break; }
            CX = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
#endif
        } else {
#ifdef PLW_WIN32
            if (!SetFileAttributesA(hp, CX & 0x27)) set_err(5);
#endif
        }
        break;
    case 0x44:
        if (AL == 0) { h = BX; if (h >= MAXH || hmap[h] == -1) { set_err(6); break; } DX = (hmap[h] == -2) ? 0x80D3 : 0x0000; }
        else set_err(1);
        break;
    case 0x47:
        { char cd[512];
          char *s;
          int i, len;
          if (!getcwd(cd, sizeof cd)) { set_err(15); break; }
#ifdef PLW_WIN32
          { char tmp[512]; if (GetShortPathNameA(cd, tmp, sizeof tmp) > 0) strcpy(cd, tmp); }
#endif
          s = cd;
          if (s[1] == ':') s += 2;
          while (*s == '\\' || *s == '/') s++;
          len = (int)strlen(s);
          /* DOS promises <= 64 bytes; PL.EXE only reserves 62.  Keep the tail that fits. */
          if (len > 60) {
              s += len - 60;
              while (*s && *s != '\\' && *s != '/') s++;
              while (*s == '\\' || *s == '/') s++;
          }
          for (i = 0; s[i] && i < 60; i++) wr8(DS, (u16)(SI + i), (u8)(s[i] == '/' ? '\\' : s[i]));
          wr8(DS, (u16)(SI + i), 0); }
        break;
    case 0x48:
        { u16 want = BX;
          if ((u32)arena_ptr + want > MEMTOP || arena_ptr < prog_end) { BX = (u16)(MEMTOP > arena_ptr ? MEMTOP - arena_ptr : 0); set_err(8); break; }
          AX = arena_ptr; arena_ptr = (u16)(arena_ptr + want); }
        break;
    case 0x49: break;
    case 0x4A:
        if (ES == psp_seg) {
            u32 top = (u32)ES + BX;
            if (top > MEMTOP) { BX = (u16)(MEMTOP - ES); set_err(8); break; }
            prog_end = (u16)top; if (arena_ptr < prog_end) arena_ptr = prog_end;
        }
        break;
    case 0x4C: terminate(AL); break;
    case 0x4E:
        get_dosstr(DS, DX, path, sizeof path);
        if (!dos_findfirst(path)) set_err(18);
        break;
    case 0x4F:
        if (!dos_findnext()) set_err(18);
        break;
    case 0x56:
        { char p2[300], h2[600];
          get_dosstr(DS, DX, path, sizeof path); get_dosstr(ES, DI, p2, sizeof p2);
          host_path(path, hp, sizeof hp); host_path(p2, h2, sizeof h2);
          if (rename(hp, h2) != 0) set_err(5); }
        break;
    case 0x62: BX = psp_seg; break;
    case 0x51: BX = psp_seg; break;
    default:
        set_err(1);
        break;
    }
}

/* ------------------------------------------------------------------ */
/* BIOS                                                                */
/* ------------------------------------------------------------------ */
static void bios_video(void)
{
    int pos, row, col;
    switch (AH) {
    case 0x00: if (!(AL & 0x80)) { video_clear(0x07); cur_set(0); } break;
    case 0x01: cur_start = CH; cur_end = CL; crtc_reg[10] = CH; crtc_reg[11] = CL; vforce = 1; break;
    case 0x02: cur_set((DH > 24 ? 24 : DH) * 80 + (DL > 79 ? 79 : DL)); vforce = 1; break;
    case 0x03: pos = cur_get(); DH = (u8)(pos / 80); DL = (u8)(pos % 80); CH = crtc_reg[10]; CL = crtc_reg[11]; break;
    case 0x05: break;
    case 0x06: video_scroll(1, AL, CH, CL, DH, DL, BH); break;
    case 0x07: video_scroll(0, AL, CH, CL, DH, DL, BH); break;
    case 0x08: pos = cur_get(); AL = vram()[pos * 2]; AH = vram()[pos * 2 + 1]; break;
    case 0x09: case 0x0A:
        { int i; pos = cur_get();
          for (i = 0; i < CX && pos + i < 2000; i++) {
              vram()[(pos + i) * 2] = AL;
              if (AH == 0x09) vram()[(pos + i) * 2 + 1] = BL;
          } }
        break;
    case 0x0E: tty_putc(AL); break;
    case 0x0F: AL = 3; AH = 80; BH = 0; break;
    case 0x13:
        { int i; u8 sub = AL; pos = (DH * 80 + DL);
          for (i = 0; i < CX; i++) {
              u8 c = rd8(ES, (u16)(BP + i * ((sub & 2) ? 2 : 1)));
              if (pos < 2000) { vram()[pos * 2] = c; vram()[pos * 2 + 1] = (sub & 2) ? rd8(ES, (u16)(BP + i * 2 + 1)) : BL; pos++; }
          }
          if (sub & 1) cur_set(pos);
          (void)row; (void)col; }
        break;
    default: break;
    }
    vforce = 1;
}

static void bios_serial(void)
{
    int n = DX;
    UART *u;
    if (n < 0 || n > 1) { AH = 0x80; return; }
    u = &uart[n];
    switch (AH) {
    case 0x00:
        {
            static const long bauds[8] = { 110, 150, 300, 600, 1200, 2400, 4800, 9600 };
            long b = bauds[(AL >> 5) & 7];
            int par_ = ((AL >> 3) & 3), bits = (AL & 3) + 5, stop = (AL & 4) ? 2 : 1;
            long div = 115200L / b;
            u->dll = (u8)div; u->dlm = (u8)(div >> 8);
            u->lcr = (u8)((bits - 5) | ((AL & 4) ? 4 : 0) | (par_ & 1 ? 8 : 0) | (par_ == 3 ? 0x10 : 0));
            if (u->open) ser_config(n, b, bits, par_ == 0 || par_ == 2 ? 0 : (par_ == 1 ? 1 : 2), stop);
            AH = 0x60; AL = u->open ? (u8)ser_msr(n) : 0;
        }
        break;
    case 0x01: if (u->open) { slog("COM%d TX(bios) %02X", n + 1, AL); ser_write(n, AL); } AH = 0x00; break;
    case 0x02:
        if (u->head != u->tail) { AL = u->ring[u->head]; u->head = (u->head + 1) & 4095; AH = 0; pic_update(); }
        else AH = 0x80;
        break;
    default: AH = (u8)(0x60 | (ring_n(u) ? 1 : 0)); AL = u->open ? (u8)ser_msr(n) : 0; break;
    }
}

static void bios_kbd(void)
{
    switch (AH) {
    case 0x00: case 0x10:
        if (!key_avail()) { retry_int = 1; plat_sleep(1); break; }
        { u16 k = key_take(); AX = k; }
        break;
    case 0x01: case 0x11:
        if (!key_avail() || pending_scan >= 0) { if (kq_h == kq_t) { setf(F_ZF, 1); break; } }
        AX = kq[kq_h]; setf(F_ZF, 0);
        break;
    case 0x02: AL = 0; break;
    default: break;
    }
}

static void hle(int n)
{
#ifdef PLW_WATCH
    if (getenv("PLW_INT") && n != 0x21) fprintf(stderr, "INT %02X AX=%04X BX=%04X CX=%04X DX=%04X\n", n, AX, BX, CX, DX);
#endif
    switch (n) {
    case 0x00: fprintf(stderr, "PLW: divide error at %04X:%04X\n", CS, IP); terminate(98); break;
    case 0x10: bios_video(); break;
    case 0x11: AX = 0x0421; break;
    case 0x12: AX = 640; break;
    case 0x14: bios_serial(); break;
    case 0x16: bios_kbd(); break;
    case 0x1A:
        { u32 ms = plat_ms() - start_ms; u32 ticks = (u32)((double)ms * 18.2065 / 1000.0);
          if (AH == 0) { CX = (u16)(ticks >> 16); DX = (u16)ticks; AL = 0; } }
        break;
    case 0x20: terminate(0); break;
    case 0x21: dos21(); break;
    default: break;
    }
}

static void terminate(int code)
{
    running = 0;
    exit_code = code;
}

/* ------------------------------------------------------------------ */
/* Housekeeping poll                                                   */
/* ------------------------------------------------------------------ */
static u32 last_poll = 0;
static void test_script_tick(void);

static void poll(void)
{
    u32 now = plat_ms();
    key_pump();
    serial_poll();
    pic_update();
    if (now - last_poll >= 15) {
        u32 ms = now - start_ms;
        last_poll = now;
        { u32 ticks = (u32)((double)ms * 18.2065 / 1000.0);
          wr16(0x40, 0x6C, (u16)ticks); wr16(0x40, 0x6E, (u16)(ticks >> 16)); }
        test_script_tick();
        video_refresh();
    }
}

/* ------------------------------------------------------------------ */
/* Program loader                                                      */
/* ------------------------------------------------------------------ */
static int build_env(u16 seg, const char *progname)
{
    /* PL.EXE (Lattice C 2.1 startup) sizes its stack as 0x200 + the size of the
       DOS environment block.  A real DOS environment is large; a stripped one
       leaves the stack so small that it overruns the program's globals.  So
       pass the useful variables and pad to a realistic size. */
    char buf[4096];
    int n = 0, i;
    static const char *keep[] = { "PLMSG", "PATH", "COMSPEC", "PROMPT", "TEMP", "TMP", NULL };
    const char *v;
    for (i = 0; keep[i]; i++) {
        v = getenv(keep[i]);
        if (v && n + (int)strlen(v) + (int)strlen(keep[i]) + 4 < 1500) n += sprintf(buf + n, "%s=%s", keep[i], v) + 1;
    }
    n += sprintf(buf + n, "PLW_PAD=");
    while (n < 2400) buf[n++] = 'X';
    buf[n++] = 0;
    buf[n++] = 0;
    buf[n++] = 1; buf[n++] = 0;
    n += sprintf(buf + n, "%s", progname) + 1;
    memcpy(mem + ((u32)seg << 4), buf, (size_t)n);
    return n;
}

static int load_program(const u8 *img, u32 len, const char *name, const char *tail)
{
    u16 envseg = 0x0800, pspseg = 0x0900, loadseg;
    u32 hdr, modlen, i, pb;
    int tl = (int)strlen(tail);

    psp_seg = pspseg;
    build_env(envseg, name);
    memset(mem + ((u32)pspseg << 4), 0, 256);
    pb = (u32)pspseg << 4;
    mem[pb] = 0xCD; mem[pb + 1] = 0x20;
    mem[pb + 2] = (u8)(MEMTOP & 0xFF); mem[pb + 3] = (u8)(MEMTOP >> 8);
    mem[pb + 0x2C] = (u8)envseg; mem[pb + 0x2D] = (u8)(envseg >> 8);
    memset(mem + pb + 0x18, 0xFF, 20);
    for (i = 0; i < 5; i++) mem[pb + 0x18 + i] = (u8)i;
    if (tl > 126) tl = 126;
    mem[pb + 0x80] = (u8)tl;
    memcpy(mem + pb + 0x81, tail, (size_t)tl);
    mem[pb + 0x81 + tl] = 13;
    memset(mem + pb + 0x5D, ' ', 11); memset(mem + pb + 0x6D, ' ', 11);

    loadseg = (u16)(pspseg + 0x10);
    dta_seg = pspseg; dta_off = 0x80;
    if (len > 2 && img[0] == 'M' && img[1] == 'Z') {
        u16 lastp = (u16)(img[2] | (img[3] << 8)), pages = (u16)(img[4] | (img[5] << 8));
        u16 nrel = (u16)(img[6] | (img[7] << 8)), hpar = (u16)(img[8] | (img[9] << 8));
        u16 ss = (u16)(img[14] | (img[15] << 8)), sp = (u16)(img[16] | (img[17] << 8));
        u16 ip = (u16)(img[20] | (img[21] << 8)), cs = (u16)(img[22] | (img[23] << 8));
        u16 relo = (u16)(img[24] | (img[25] << 8));
        hdr = (u32)hpar * 16;
        modlen = (u32)pages * 512 - (lastp ? 512 - lastp : 0) - hdr;
        if (hdr + modlen > len) modlen = len - hdr;
        memcpy(mem + ((u32)loadseg << 4), img + hdr, modlen);
        for (i = 0; i < nrel; i++) {
            u16 ro = (u16)(img[relo + i * 4] | (img[relo + i * 4 + 1] << 8));
            u16 rs_ = (u16)(img[relo + i * 4 + 2] | (img[relo + i * 4 + 3] << 8));
            u32 a = (((u32)(loadseg + rs_)) << 4) + ro;
            u16 v = (u16)(mem[a] | (mem[a + 1] << 8));
            v = (u16)(v + loadseg);
            mem[a] = (u8)v; mem[a + 1] = (u8)(v >> 8);
        }
        CS = (u16)(loadseg + cs); IP = ip; SS = (u16)(loadseg + ss); SP = sp;
        prog_end = MEMTOP;
    } else {                                                  /* .COM */
        memcpy(mem + ((u32)pspseg << 4) + 0x100, img, len);
        CS = SS = pspseg; IP = 0x100; SP = 0xFFFE;
        mem[((u32)pspseg << 4) + 0xFFFE] = 0; mem[((u32)pspseg << 4) + 0xFFFF] = 0;
        prog_end = MEMTOP;
    }
    DS = ES = pspseg;
    AX = BX = CX = DX = SI = DI = BP = 0;
    FL = 0x0202;
    arena_ptr = MEMTOP;
    return 0;
}

static void mach_init(void)
{
    int i, n;
    for (i = 0; i < 256; i++) { int b = i, c = 0; while (b) { c += b & 1; b >>= 1; } par[i] = (u8)((c & 1) ? 0 : 1); }
    memset(mem, 0, sizeof mem);
    for (n = 0; n < 256; n++) { wr16(0, (u16)(n * 4), (u16)n); wr16(0, (u16)(n * 4 + 2), STUBSEG); }
    for (i = 0; i < MAXH; i++) hmap[i] = -1;
    hmap[0] = hmap[1] = hmap[2] = -2;
    wr16(0x40, 0x00, 0x3F8); wr16(0x40, 0x02, 0x2F8);
    wr16(0x40, 0x10, 0x0421); wr16(0x40, 0x13, 640);
    wr8(0x40, 0x49, 3); wr16(0x40, 0x4A, 80);
    wr16(0x40, 0x4C, 0x1000); wr16(0x40, 0x4E, 0); wr8(0x40, 0x62, 0);
    wr16(0x40, 0x60, 0x0607); wr16(0x40, 0x63, 0x3D4); wr8(0x40, 0x65, 0x29); wr8(0x40, 0x66, 0x30);
    wr16(0x40, 0x1A, 0x1E); wr16(0x40, 0x1C, 0x1E); wr16(0x40, 0x80, 0x1E); wr16(0x40, 0x82, 0x3E);
    wr8(0x40, 0x84, 24); wr16(0x40, 0x85, 16);
    video_clear(0x07);
    crtc_reg[10] = 6; crtc_reg[11] = 7;
    cur_set(0);
    if (!getcwd(host_cwd, sizeof host_cwd)) host_cwd[0] = 0;
}

/* ------------------------------------------------------------------ */
/* Platform: Win32                                                     */
/* ------------------------------------------------------------------ */
#ifdef PLW_WIN32
static const unsigned short cp437hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0 };
static const unsigned short cp437lo[32] = {
    0x0020,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
    0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC };
static HANDLE hCom[2] = { INVALID_HANDLE_VALUE, INVALID_HANDLE_VALUE };
static int plat_up = 0;

/* ---- resizable GDI window: the 80x25 text screen is rendered at 10x20 per cell
   into an off-screen bitmap and stretched (aspect preserved) to the client area ---- */
#define CELLW 10
#define CELLH 20
#define USEDCOLS 76
static HWND hwnd;
static HDC mdc;
static HBITMAP mbmp, mold;
static HFONT hfont;
static int cur_row, cur_col, cur_vis, cur_on = 1;
static int win_w = 760, win_h = 500;
static u16 keyq[64]; static int kq_head, kq_tail;
static const COLORREF vga[16] = {
    RGB(0,0,0), RGB(0,0,170), RGB(0,170,0), RGB(0,170,170), RGB(170,0,0), RGB(170,0,170), RGB(170,85,0), RGB(170,170,170),
    RGB(85,85,85), RGB(85,85,255), RGB(85,255,85), RGB(85,255,255), RGB(255,85,85), RGB(255,85,255), RGB(255,255,85), RGB(255,255,255) };

static void gui_error(const char *msg) { MessageBoxA(hwnd, msg, "PROMlink", MB_OK | MB_ICONERROR); }
static void pause_msg(const char *why) { gui_error(why); }

static void paint_to(HDC dc)
{
    RECT rc;
    int cw, ch, w, h, x, y;
    GetClientRect(hwnd, &rc);
    cw = rc.right; ch = rc.bottom;
    if (cw < 1 || ch < 1) return;
    w = cw; h = ch; x = 0; y = 0;                         /* fill the whole client area */
    if (x > 0 || y > 0) {
        HBRUSH b = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT r;
        r.left = 0; r.top = 0; r.right = cw; r.bottom = y; FillRect(dc, &r, b);
        r.top = y + h; r.bottom = ch; FillRect(dc, &r, b);
        r.top = y; r.bottom = y + h; r.right = x; FillRect(dc, &r, b);
        r.left = x + w; r.right = cw; FillRect(dc, &r, b);
    }
    SetStretchBltMode(dc, (w == USEDCOLS * CELLW && h == 25 * CELLH) ? COLORONCOLOR : HALFTONE);
    SetBrushOrgEx(dc, 0, 0, NULL);
    StretchBlt(dc, x, y, w, h, mdc, 0, 0, USEDCOLS * CELLW, 25 * CELLH, SRCCOPY);   /* PL's UI uses columns 0-75 only */
    if (cur_vis && cur_on && cur_row >= 0 && cur_row < 25 && cur_col >= 0 && cur_col < USEDCOLS) {   /* underline cursor */
        RECT r;
        HBRUSH b = CreateSolidBrush(RGB(170, 170, 170));
        r.left = x + cur_col * w / USEDCOLS; r.right = x + (cur_col + 1) * w / USEDCOLS;
        r.bottom = y + (cur_row + 1) * h / 25; r.top = r.bottom - (h / 25) / 6 - 1;
        FillRect(dc, &r, b);
        DeleteObject(b);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); paint_to(dc); EndPaint(h, &ps); return 0; }
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_TIMER: cur_on = !cur_on; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 240; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 160; return 0;
    case WM_CLOSE: running = 0; return 0;
    default: break;
    }
    return DefWindowProcA(h, m, wp, lp);
}

static void plat_init(void)
{
    WNDCLASSA wc;
    RECT r;
    HDC sdc;
    LOGFONTA lf;
    static const char *faces[] = { "Consolas", "Lucida Console", "Courier New", NULL };
    int i;
    HMODULE u = LoadLibraryA("user32.dll");
    if (u) { BOOL (WINAPI *f)(void) = (BOOL (WINAPI *)(void))GetProcAddress(u, "SetProcessDPIAware"); if (f) f(); }
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "PLWWnd";
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(1));
    RegisterClassA(&wc);
    r.left = 0; r.top = 0; r.right = win_w; r.bottom = win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("PLWWnd", "PROMlink", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right - r.left, r.bottom - r.top, NULL, NULL, wc.hInstance, NULL);
    SendMessageA(hwnd, WM_SETICON, ICON_BIG, (LPARAM)LoadImageA(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0));
    SendMessageA(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageA(wc.hInstance, MAKEINTRESOURCE(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    sdc = GetDC(hwnd);
    mdc = CreateCompatibleDC(sdc);
    mbmp = CreateCompatibleBitmap(sdc, 80 * CELLW, 25 * CELLH);
    mold = (HBITMAP)SelectObject(mdc, mbmp);
    ReleaseDC(hwnd, sdc);
    memset(&lf, 0, sizeof lf);
    lf.lfHeight = -(CELLH - 3); lf.lfWidth = 0; lf.lfCharSet = DEFAULT_CHARSET; lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    for (i = 0; faces[i]; i++) {
        strcpy(lf.lfFaceName, faces[i]);
        hfont = CreateFontIndirectA(&lf);
        if (hfont) break;
    }
    SelectObject(mdc, hfont);
    SetTextAlign(mdc, TA_LEFT | TA_TOP);
    SetTimer(hwnd, 1, 530, NULL);
    plat_up = 1;
    SetForegroundWindow(hwnd);
}

static void plat_exit(void)
{
    int i;
    if (!plat_up) return;
    plat_up = 0;
    if (hwnd) { KillTimer(hwnd, 1); DestroyWindow(hwnd); hwnd = NULL; }
    for (i = 0; i < 2; i++) if (hCom[i] != INVALID_HANDLE_VALUE) { CloseHandle(hCom[i]); hCom[i] = INVALID_HANDLE_VALUE; }
}

static void plat_draw(const u8 *v, int row, int col, int curvis)
{
    int i, ox;
    if (!hwnd) return;
    /* glyph width follows the font's real advance, centred in a CELLW cell */
    for (i = 0; i < 2000; i++) {
        u8 ch = v[i * 2], at = v[i * 2 + 1];
        WCHAR wc = (WCHAR)(ch >= 0x80 ? cp437hi[ch - 0x80] : ch < 0x20 ? cp437lo[ch] : ch == 0x7F ? 0x2302 : ch);
        RECT r;
        SIZE sz;
        r.left = (i % 80) * CELLW; r.top = (i / 80) * CELLH; r.right = r.left + CELLW; r.bottom = r.top + CELLH;
        SetBkColor(mdc, vga[(at >> 4) & 15]);
        SetTextColor(mdc, vga[at & 15]);
        if (wc == 0x20 || wc == 0x00A0) { ExtTextOutW(mdc, r.left, r.top, ETO_OPAQUE, &r, L" ", 1, NULL); continue; }
        GetTextExtentPoint32W(mdc, &wc, 1, &sz);
        ox = (CELLW - sz.cx) / 2;
        if (wc >= 0x2500 && wc <= 0x259F) ox = 0;           /* line/block graphics must tile edge to edge */
        ExtTextOutW(mdc, r.left + ox, r.top + 1, ETO_OPAQUE | ETO_CLIPPED, &r, &wc, 1, NULL);
    }
    cur_row = row; cur_col = col; cur_vis = curvis;
    InvalidateRect(hwnd, NULL, FALSE);
}

static void kpush(u16 k) { int nt = (kq_tail + 1) & 63; if (nt != kq_head) { keyq[kq_tail] = k; kq_tail = nt; } }

static int plat_getkey(u16 *k)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        int skip = 0;
        if (msg.message == WM_QUIT) { running = 0; continue; }
        if ((msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN) && msg.hwnd == hwnd) {
            int vk = (int)msg.wParam, scan = (int)((msg.lParam >> 16) & 0xFF);
            int shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0, ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            int alt = msg.message == WM_SYSKEYDOWN;
            int sc = -1;
            if (vk >= VK_F1 && vk <= VK_F10) sc = 0x3B + (vk - VK_F1) + (alt ? 0x2D : ctrl ? 0x23 : shift ? 0x19 : 0);
            else if (vk == VK_F11 || vk == VK_F12) sc = (vk == VK_F11 ? 0x85 : 0x86) + (alt ? 6 : ctrl ? 4 : shift ? 2 : 0);
            else switch (vk) {
                case VK_HOME: sc = ctrl ? 0x77 : 0x47; break;
                case VK_UP: sc = 0x48; break;
                case VK_PRIOR: sc = ctrl ? 0x84 : 0x49; break;
                case VK_LEFT: sc = ctrl ? 0x73 : 0x4B; break;
                case VK_RIGHT: sc = ctrl ? 0x74 : 0x4D; break;
                case VK_END: sc = ctrl ? 0x75 : 0x4F; break;
                case VK_DOWN: sc = 0x50; break;
                case VK_NEXT: sc = ctrl ? 0x76 : 0x51; break;
                case VK_INSERT: sc = 0x52; break;
                case VK_DELETE: sc = 0x53; break;
                case VK_TAB: if (shift) sc = 0x0F; break;
                default: break;
            }
            if (alt && vk == VK_F4) { running = 0; skip = 1; }
            else if (sc >= 0 && !(alt && vk == VK_F10 && 0)) { kpush((u16)(sc << 8)); skip = 1; }
            else if (alt && vk != VK_SPACE && vk != VK_RETURN && vk != VK_ESCAPE && vk != VK_BACK && vk != VK_TAB && scan) {
                if (vk != VK_MENU && vk != VK_SHIFT && vk != VK_CONTROL) kpush((u16)(scan << 8));
                skip = 1;
            }
            if (!skip && (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)) skip = 1;
        }
        if (msg.message == WM_CHAR && msg.hwnd == hwnd) {
            int ascv = (int)(msg.wParam & 0xFF), scan = (int)((msg.lParam >> 16) & 0xFF);
            if (msg.wParam < 256) kpush((u16)(ascv | (scan << 8)));
            continue;
        }
        if (!skip) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        else if (msg.message != WM_SYSKEYDOWN) { /* swallowed */ }
    }
    if (kq_head != kq_tail) { *k = keyq[kq_head]; kq_head = (kq_head + 1) & 63; return 1; }
    return 0;
}

static void plat_sleep(int ms) { Sleep((DWORD)ms); }
static int plat_hund(void) { SYSTEMTIME st; GetLocalTime(&st); return st.wMilliseconds / 10; }
static u32 plat_ms(void) { return (u32)GetTickCount(); }

static int ser_open(int n, const char *name)
{
    char full[64];
    COMMTIMEOUTS to;
    DCB dcb;
    if (!_stricmp(name, "NONE")) return 0;
    if (strncmp(name, "\\\\.\\", 4)) sprintf(full, "\\\\.\\%s", name); else strcpy(full, name);
    hCom[n] = CreateFileA(full, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (hCom[n] == INVALID_HANDLE_VALUE) return (int)GetLastError();
    memset(&to, 0, sizeof to);
    to.ReadIntervalTimeout = MAXDWORD;                    /* return at once with whatever is there */
    to.WriteTotalTimeoutConstant = 2000;
    SetCommTimeouts(hCom[n], &to);
    SetupComm(hCom[n], 8192, 8192);
    memset(&dcb, 0, sizeof dcb); dcb.DCBlength = sizeof dcb;
    GetCommState(hCom[n], &dcb);
    dcb.BaudRate = 9600; dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = 1; dcb.fParity = 0; dcb.fOutxCtsFlow = 0; dcb.fOutxDsrFlow = 0; dcb.fDsrSensitivity = 0;
    dcb.fDtrControl = DTR_CONTROL_ENABLE; dcb.fRtsControl = RTS_CONTROL_ENABLE;
    dcb.fOutX = 0; dcb.fInX = 0; dcb.fNull = 0; dcb.fAbortOnError = 0;
    SetCommState(hCom[n], &dcb);
    return 0;
}
static int ser_read(int n, u8 *buf, int max)
{
    DWORD got = 0;
    if (hCom[n] == INVALID_HANDLE_VALUE) return 0;
    if (!ReadFile(hCom[n], buf, (DWORD)max, &got, NULL)) { DWORD er; COMSTAT cs; ClearCommError(hCom[n], &er, &cs); slog("COM%d ReadFile failed, err %08lX", n + 1, (unsigned long)er); return 0; }
    if (logf) { static unsigned cnt; if ((++cnt & 255) == 0 || got) { DWORD er; COMSTAT cs; if (ClearCommError(hCom[n], &er, &cs) && er) slog("COM%d line error flags %08lX (1=RXOVER 2=OVERRUN 4=PARITY 8=FRAME)", n + 1, (unsigned long)er); } }
    return (int)got;
}
static void ser_write(int n, u8 c)
{
    DWORD w;
    if (hCom[n] != INVALID_HANDLE_VALUE) WriteFile(hCom[n], &c, 1, &w, NULL);
}
static void ser_config(int n, long baud, int bits, int parity, int stop)
{
    DCB dcb;
    if (hCom[n] == INVALID_HANDLE_VALUE) return;
    memset(&dcb, 0, sizeof dcb); dcb.DCBlength = sizeof dcb;
    if (!GetCommState(hCom[n], &dcb)) return;
    dcb.BaudRate = (DWORD)baud; dcb.ByteSize = (BYTE)bits;
    dcb.Parity = (BYTE)(parity == 0 ? NOPARITY : parity == 1 ? ODDPARITY : EVENPARITY);
    dcb.fParity = parity ? 1 : 0;
    dcb.StopBits = (BYTE)(stop == 2 ? TWOSTOPBITS : ONESTOPBIT);
    SetCommState(hCom[n], &dcb);
}
static void ser_mcr(int n, int mcr)
{
    if (hCom[n] == INVALID_HANDLE_VALUE) return;
    EscapeCommFunction(hCom[n], (mcr & 1) ? SETDTR : CLRDTR);
    EscapeCommFunction(hCom[n], (mcr & 2) ? SETRTS : CLRRTS);
}
static int ser_msr(int n)
{
    DWORD m = 0;
    int r = 0;
    if (hCom[n] == INVALID_HANDLE_VALUE) return 0;
    GetCommModemStatus(hCom[n], &m);
    if (m & MS_CTS_ON) r |= 0x10;
    if (m & MS_DSR_ON) r |= 0x20;
    if (m & MS_RING_ON) r |= 0x40;
    if (m & MS_RLSD_ON) r |= 0x80;
    return r;
}
static void test_script_tick(void) { }

#else
/* ------------------------------------------------------------------ */
/* Platform: POSIX self-test (scripted keys, pty serial, screen dumps)  */
/* ------------------------------------------------------------------ */
static int ser_fd[2] = { -1, -1 };
static u8 scr_copy[4000];
static int scr_row, scr_col;
static u16 tkq[512];
static int tkq_h = 0, tkq_t = 0;

static void plat_init(void) { }
static void plat_exit(void) { }
static void plat_draw(const u8 *v, int row, int col, int curvis) { (void)curvis; memcpy(scr_copy, v, 4000); scr_row = row; scr_col = col; }
static int plat_getkey(u16 *k)
{
    if (tkq_h == tkq_t) return 0;
    *k = tkq[tkq_h]; tkq_h = (tkq_h + 1) & 511; return 1;
}
static void plat_sleep(int ms) { struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = ms * 1000000L; nanosleep(&ts, NULL); }
static int plat_hund(void) { struct timeval tv; gettimeofday(&tv, NULL); return (int)(tv.tv_usec / 10000); }
static u32 plat_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return (u32)(tv.tv_sec * 1000UL + tv.tv_usec / 1000); }

static int ser_open(int n, const char *name)
{
    struct termios t;
    if (!strcmp(name, "NONE")) return 0;
    ser_fd[n] = open(name, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (ser_fd[n] < 0) return errno;
    if (tcgetattr(ser_fd[n], &t) == 0) { cfmakeraw(&t); tcsetattr(ser_fd[n], TCSANOW, &t); }
    return 0;
}
static int ser_read(int n, u8 *buf, int max)
{
    int r;
    if (ser_fd[n] < 0) return 0;
    r = (int)read(ser_fd[n], buf, (size_t)max);
    return r > 0 ? r : 0;
}
static void ser_write(int n, u8 c) { if (ser_fd[n] >= 0) { ssize_t r = write(ser_fd[n], &c, 1); (void)r; } }
static void ser_config(int n, long baud, int bits, int parity, int stop)
{
    if (getenv("PLW_TRACE")) fprintf(stderr, "[ser%d config %ld %d%c%d]\n", n, baud, bits, "NOE"[parity], stop);
}
static void ser_mcr(int n, int mcr) { if (getenv("PLW_TRACE")) fprintf(stderr, "[ser%d mcr %02X]\n", n, mcr); }
static int ser_msr(int n) { (void)n; return 0xB0; }

/* script: lines "wait MS", "text STR", "scan HEX", "key HEX", "dump", "waitfor STR MS", "quit" */
static FILE *scr_fp = NULL;
static u32 scr_next = 0;
static char scr_wf[128];
static u32 scr_wf_deadline = 0;
static int scr_failed = 0;

static void dump_screen(void)
{
    int r, c;
    printf("+--------------------------------------------------------------------------------+\n");
    for (r = 0; r < 25; r++) {
        char line[81];
        for (c = 0; c < 80; c++) { u8 ch = scr_copy[(r * 80 + c) * 2]; line[c] = (ch >= 32 && ch < 127) ? (char)ch : (ch ? '#' : ' '); }
        line[80] = 0;
        printf("|%s|\n", line);
    }
    { int mn=99,mx=-1,r2,c2; for (r2=0;r2<25;r2++) for (c2=0;c2<80;c2++) { u8 ch=scr_copy[(r2*80+c2)*2], at=scr_copy[(r2*80+c2)*2+1]; if (ch!=0x20 && ch!=0 || (at>>4)) { if(c2<mn)mn=c2; if(c2>mx)mx=c2; } } printf("EXTENT %d %d\n", mn, mx); }
    printf("+ cursor row %d col %d --------------------------------------------------------\n", scr_row, scr_col);
    if (getenv("PLW_RAW")) { printf("active_b0=%d b0[0..8]=%02X %02X %02X %02X b8[0..8]=%02X %02X %02X %02X\n", vactive_b0, mem[VID_B0], mem[VID_B0+1], mem[VID_B0+2], mem[VID_B0+3], mem[VID_B8], mem[VID_B8+1], mem[VID_B8+2], mem[VID_B8+3]); for (r = 0; r < 3; r++) { for (c = 0; c < 40; c++) printf("%02X/%02X ", scr_copy[(r * 80 + c) * 2], scr_copy[(r * 80 + c) * 2 + 1]); printf("\n"); } }
    fflush(stdout);
}
static int screen_has(const char *s)
{
    int r, c;
    for (r = 0; r < 25; r++) {
        char line[81];
        for (c = 0; c < 80; c++) { u8 ch = scr_copy[(r * 80 + c) * 2]; line[c] = (ch >= 32 && ch < 127) ? (char)ch : ' '; }
        line[80] = 0;
        if (strstr(line, s)) return 1;
    }
    return 0;
}
static void tq(u16 k) { int nt = (tkq_t + 1) & 511; if (nt != tkq_h) { tkq[tkq_t] = k; tkq_t = nt; } }

static void test_script_tick(void)
{
    char line[256];
    u32 now = plat_ms();
    if (!scr_fp) return;
    if (scr_wf[0]) {
        if (screen_has(scr_wf)) scr_wf[0] = 0;
        else if (now > scr_wf_deadline) { printf("SCRIPT FAIL: timeout waiting for '%s'\n", scr_wf); dump_screen(); scr_failed = 1; terminate(3); return; }
        else return;
    }
    if (now < scr_next) return;
    while (fgets(line, sizeof line, scr_fp)) {
        int l = (int)strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (!strncmp(line, "wait ", 5)) { scr_next = now + (u32)atoi(line + 5); return; }
        if (!strncmp(line, "text ", 5)) { char *p; for (p = line + 5; *p; p++) { if (*p == '\\' && p[1] == 'r') { tq(13); p++; } else if (*p == '\\' && p[1] == 'e') { tq(27); p++; } else tq((u16)(u8)*p); } }
        else if (!strncmp(line, "scan ", 5)) tq((u16)(strtol(line + 5, NULL, 16) << 8));
        else if (!strncmp(line, "key ", 4)) tq((u16)strtol(line + 4, NULL, 16));
        else if (!strncmp(line, "dump", 4)) { scr_next = now + 150; vforce = 1; video_refresh(); dump_screen(); return; }
        else if (!strncmp(line, "waitfor ", 8)) {
            char *sp = strrchr(line, ' ');
            *sp = 0; strncpy(scr_wf, line + 8, 127); scr_wf_deadline = now + (u32)atoi(sp + 1); return;
        }
        else if (!strncmp(line, "expect ", 7)) { if (!screen_has(line + 7)) { printf("SCRIPT FAIL: expected '%s'\n", line + 7); dump_screen(); scr_failed = 1; terminate(3); return; } else printf("ok: found '%s'\n", line + 7); }
        else if (!strncmp(line, "quit", 4)) { terminate(0); return; }
    }
    terminate(0);
}
#endif

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */
static u8 *file_load(const char *fn, u32 *len)
{
    FILE *f = fopen(fn, "rb");
    u8 *b;
    long sz;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    b = (u8 *)malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f);
    *len = (u32)sz;
    return b;
}

#ifdef PLW_LIB
u8 *lib_mem(void) { return mem; }
void lib_init(void) { mach_init(); }
void lib_get(u16 *o) { int i; for (i = 0; i < 8; i++) o[i] = R[i]; for (i = 0; i < 4; i++) o[8 + i] = S[i]; o[12] = IP; o[13] = FL; }
void lib_set(const u16 *o) { int i; for (i = 0; i < 8; i++) R[i] = o[i]; for (i = 0; i < 4; i++) S[i] = o[8 + i]; IP = o[12]; FL = o[13]; running = 1; }
int lib_step(void) { step(); return running; }
#else
#ifdef PLW_WATCH
static unsigned long stop_at = 0, bp_at = 0;
static u16 trace_ring[4096]; static unsigned trace_i = 0;
#endif
#ifdef PLW_WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    char b[120];
    sprintf(b, "crash: exception %08lX at %p (emulated %04X:%04X)", (unsigned long)ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress, CS, IP);
    plat_exit();
    pause_msg(b);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
int main(int argc, char **argv)
{
    char tail[200];
    const char *xprog = NULL, *cn[2];
    const u8 *img = NULL;
    u8 *xbuf = NULL;
    u32 len = 0;
    int i, er, fail = 0, serial_warn = 0, want_log = 0;
    const char *e1 = getenv("PLW_COM1"), *e2 = getenv("PLW_COM2");
    char dosname[128] = "C:\\PL.EXE";

#ifdef PLW_WATCH
    if (getenv("PLW_BP")) bp_at = strtoul(getenv("PLW_BP"), NULL, 16);
    if (getenv("PLW_STOP")) stop_at = strtoul(getenv("PLW_STOP"), NULL, 10);
#endif
    cn[0] = e1 ? e1 : "COM1"; cn[1] = e2 ? e2 : "COM2";
#ifdef PLW_WIN32
    cn[0] = "COM1"; cn[1] = "COM2";                      /* hardwired: PROMlink COM1/COM2 = Windows COM1/COM2 */
#endif
    tail[0] = 0;
    for (i = 1; i < argc; i++) {
#ifndef PLW_WIN32
        if (ipre(argv[i], "/COM1=")) cn[0] = argv[i] + 6;
        else if (ipre(argv[i], "/COM2=")) cn[1] = argv[i] + 6;
        else
#endif
        if (ieq(argv[i], "/LOG")) want_log = 1;
        else if (ieq(argv[i], "/X") && i + 1 < argc) xprog = argv[++i];
#ifndef PLW_WIN32
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) { scr_fp = fopen(argv[++i], "r"); if (!scr_fp) { perror("script"); return 2; } }
#endif
        else if (ieq(argv[i], "/H") || !strcmp(argv[i], "/?")) {
            printf("PROMlink for Windows (PLW) - 8086 host for PL.EXE / Data I/O PROMlink 2.00\n"
                   "Ported/Patched by Mickey White Lawless\n\n"
                   "Usage: PL [/X prog.exe] [PROMlink args]   (serial hardwired: COM1->COM1, COM2->COM2)\n"
                   "  /COM1=COM5  use Windows port COM5 as PROMlink's COM1 (default COM1)\n"
                   "  /COM2=COM6  use Windows port COM6 as PROMlink's COM2 (default COM2)\n"
                   "  /COMn=NONE  run without that serial port\n"
                   "  /X file     run another DOS program (RW.EXE, LABL.EXE, TERM.COM ...)\n"
                   "Env: PLW_COM1, PLW_COM2, PLMSG\n");
            return 0;
        } else {
            if (tail[0]) strcat(tail, " ");
            if (strlen(tail) + strlen(argv[i]) < 120) strcat(tail, argv[i]);
        }
    }

    if (xprog) {
        xbuf = file_load(xprog, &len);
        if (!xbuf) { fprintf(stderr, "PLW: cannot read %s\n", xprog); return 2; }
        img = xbuf;
        sprintf(dosname, "C:\\%.100s", xprog);
    } else {
#ifdef PLW_NOIMG
        fprintf(stderr, "PLW: built without embedded image; use /X\n"); return 2;
#else
        img = plimg; len = plimg_len;
#endif
    }

#ifdef PLW_WIN32
    SetUnhandledExceptionFilter(crash_filter);
#endif
#ifdef PLW_WIN32
    if (GetFileAttributesA("PLMENUS.STR") == INVALID_FILE_ATTRIBUTES && !getenv("PLMSG")) {
        char ep[MAX_PATH], *sl;      /* not started in the PROMlink folder: use the exe's own folder */
        if (GetModuleFileNameA(NULL, ep, sizeof ep) && (sl = strrchr(ep, '\\')) != NULL) { *sl = 0; SetCurrentDirectoryA(ep); }
    }
#endif
    if (want_log) { logf = fopen("PLW.LOG", "w"); slog("PLW log started"); }
    mach_init();
    for (i = 0; i < 2; i++) {
        com_name[i] = cn[i];
        er = ser_open(i, cn[i]);
        if (er) { if (i == 0 || strcmp(cn[i], i ? "COM2" : "COM1")) { fprintf(stderr, "PLW: cannot open %s for COM%d (error %d) - serial disabled\n", cn[i], i + 1, er); fail = 1; } }
        else uart[i].open = strcmp(cn[i], "NONE") != 0;
    }
    #ifdef PLW_WIN32
    if (fail) serial_warn = 1;
#else
    if (fail) { fprintf(stderr, "PLW: serial is hardwired to Windows COM1 - is it present/free?\n"); plat_sleep(2500); }
#endif

    load_program(img, len, dosname, tail);
    start_ms = plat_ms();
    plat_init();
#ifdef PLW_WIN32
    if (serial_warn) gui_error("Windows COM1 could not be opened (missing or in use by another program).\nPROMlink will run without a serial connection.");
#endif
    video_refresh();

    while (running) {
        if (irq_hint && (FL & F_IF)) deliver_irq();
#ifdef PLW_WATCH
        trace_ring[trace_i++ & 4095] = IP;
        if (bp_at && IP == bp_at) { fprintf(stderr, "REGS icount=%lu AX=%04X BX=%04X CX=%04X DX=%04X SP=%04X BP=%04X SI=%04X DI=%04X DS=%04X SS=%04X\n", icount, AX, BX, CX, DX, SP, BP, SI, DI, DS, SS); char q1[40], q2[40]; get_dosstr(DS, rd16(SS, (u16)(SP + 2)), q1, 40); fprintf(stderr, "BP %04X: arg1='%s' [sp+2]=%04X [sp+4]=%04X AX=%04X\n", IP, q1, rd16(SS, (u16)(SP + 2)), rd16(SS, (u16)(SP + 4)), AX); { char a2[40]; { int q3; fprintf(stderr, "   HEX@arg2:"); for (q3 = 0; q3 < 96; q3++) fprintf(stderr, " %02X", rd8(DS, (u16)(rd16(SS, (u16)(SP + 4)) + q3))); fprintf(stderr, "\n"); } get_dosstr(DS, rd16(SS, (u16)(SP + 4)), a2, 40); fprintf(stderr, "   arg2='%s'\n", a2); } get_dosstr(DS, 0x1218, q2, 40); fprintf(stderr, "   DS:1218='%s' 121C='", q2); get_dosstr(DS, 0x121C, q2, 40); fprintf(stderr, "%s' 1220='", q2); get_dosstr(DS, 0x1220, q2, 40); fprintf(stderr, "%s'\n", q2); }
#endif
        step();
#ifdef PLW_WATCH
        if (stop_at && icount == stop_at) { fprintf(stderr, "STOP at %04X:%04X\n", CS, IP); { int q, bad = 0; for (q = 0; q < 4000; q += 2) if (mem[VID_B8 + q] < 0x20 && mem[VID_B8+q] != 0 ) bad++; fprintf(stderr, "NONPRINT=%d\n", bad); } break; }
#endif
        if ((++icount & 0x7FF) == 0) poll();
    }
    video_refresh();
#ifdef PLW_WATCH
    fprintf(stderr, "TOTAL icount=%lu\n", icount);
    { unsigned q; fprintf(stderr, "LAST:"); for (q = trace_i - 150; q != trace_i; q++) fprintf(stderr, " %04X", trace_ring[q & 4095]); fprintf(stderr, "\n"); }
#endif
#ifdef PLW_WIN32
    if (plat_ms() - start_ms < 8000 || exit_code) { char b[100]; video_refresh(); sprintf(b, "Program ended after %lu ms (exit code %d).", (unsigned long)(plat_ms() - start_ms), exit_code); pause_msg(b); }
#endif
    plat_exit();
#ifndef PLW_WIN32
    if (scr_fp && !scr_failed) { /* final dump for the log */ dump_screen(); }
#endif
    free(xbuf);
    return exit_code;
}
#endif
