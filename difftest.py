#!/usr/bin/env python3
# Differential test: plw 8086 core vs Unicorn (x86 real mode) on random single instructions.
import ctypes, random, sys, collections
from unicorn import *
from unicorn.x86_const import *

lib = ctypes.CDLL('/tmp/plwlib.so')
lib.lib_mem.restype = ctypes.POINTER(ctypes.c_ubyte)
lib.lib_step.restype = ctypes.c_int
lib.lib_init()
mem = lib.lib_mem()
U16 = ctypes.c_uint16 * 14

SEG = {'ES': 0x1000, 'DS': 0x2000, 'SS': 0x3000, 'CS': 0x4000}
BASE, SIZE = 0x10000, 0x40000
uc = Uc(UC_ARCH_X86, UC_MODE_16)
uc.mem_map(0, 0x100000)

REGS = [UC_X86_REG_AX, UC_X86_REG_CX, UC_X86_REG_DX, UC_X86_REG_BX, UC_X86_REG_SP, UC_X86_REG_BP, UC_X86_REG_SI, UC_X86_REG_DI]
SREGS = [UC_X86_REG_ES, UC_X86_REG_CS, UC_X86_REG_SS, UC_X86_REG_DS]

rnd = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
N = int(sys.argv[1]) if len(sys.argv) > 1 else 20000

def fill():
    data = bytes(rnd.getrandbits(8) for _ in range(SIZE))
    uc.mem_write(BASE, data)
    ctypes.memmove(ctypes.addressof(mem.contents) + BASE, data, SIZE)

fill()

BAD = {0x0F, 0x54, 0x6C, 0x6D, 0x6E, 0x6F, 0xCC, 0xCD, 0xCE, 0xCF, 0xE4, 0xE5, 0xE6, 0xE7, 0xEC, 0xED, 0xEE, 0xEF,
       0xF4, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0x9B, 0xC8, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0xD6, 0xF1,
       0x9A, 0xEA}
# (FF, C4/5, etc. tested separately below via specific generators)

def mask_for(code, regs_before):
    op = code[0]
    i = 0
    while op in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3, 0xF0):
        i += 1; op = code[i]
    m = 0xED5
    mr = (code[i + 1] >> 3) & 7 if i + 1 < len(code) else 0
    if op in (0xD0, 0xD1, 0xD2, 0xD3, 0xC0, 0xC1):
        m &= ~0x10
        cnt = 1 if op in (0xD0, 0xD1) else (regs_before[1] & 0xFF if op in (0xD2, 0xD3) else code[i + 2 + 0] & 0x1F)
        # displacement unknown for C0/C1; be conservative
        if op in (0xC0, 0xC1) or cnt != 1: m &= ~0x800
    if op in (0xF6, 0xF7):
        if mr in (4, 5): m &= 0x801
        elif mr in (6, 7): m &= 0x600
        else:
            if mr in (0, 1): m &= ~0x10
    if op in (0xD4, 0xD5): m &= 0x0C4
    if op in (0x27, 0x2F): m &= ~0x800
    if op in (0x37, 0x3F): m &= 0x011
    if op in (0x69, 0x6B): m &= 0x801
    if op in (0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x84, 0x85, 0xA8, 0xA9):
        m &= ~0x10
    if op in (0x80, 0x81, 0x82, 0x83) and mr in (1, 4, 6): m &= ~0x10
    return m

def npre0(code):
    n = 0
    while code[n] in (0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF2, 0xF3): n += 1
    return n

def run_one(code, flags_init=None):
    # random state
    regs = [rnd.getrandbits(16) for _ in range(8)]
    for k in (1, 3, 5, 6, 7):
        if rnd.random() < 0.6: regs[k] = rnd.getrandbits(11)
    regs[4] = rnd.choice([rnd.getrandbits(16) & 0xFFFE, rnd.getrandbits(12) + 0x100])
    if rnd.random() < 0.5: regs[1] = rnd.getrandbits(3)
    segs = [SEG['ES'], SEG['CS'], SEG['SS'], SEG['DS']]
    ip = 0x100 + rnd.getrandbits(8)
    fl = (rnd.getrandbits(16) & 0x0ED5) | 0x0002
    # code
    la = (SEG['CS'] << 4) + ip
    uc.mem_write(la, code); ctypes.memmove(ctypes.addressof(mem.contents) + la, code, len(code))
    if code[npre0(code)] in (0xD2, 0xD3): regs[1] = (regs[1] & 0xFF00) | (regs[1] & 0x1F)
    st = U16(*(regs + segs + [ip, fl]))
    lib.lib_set(st)
    for r, v in zip(REGS, regs): uc.reg_write(r, v)
    for r, v in zip(SREGS, segs): uc.reg_write(r, v)
    uc.reg_write(UC_X86_REG_EFLAGS, fl)
    uc.reg_write(UC_X86_REG_IP, ip)
    npre = 0
    while code[npre] in (0x26, 0x2E, 0x36, 0x3E, 0xF0, 0xF2, 0xF3): npre += 1
    allowed = set(range(la, la + npre + 1))
    def hk(u, addr, size, ud):
        if addr not in allowed: u.emu_stop()
    h = uc.hook_add(UC_HOOK_CODE, hk)
    try:
        uc.emu_start(la, la + 0x1000, count=200000)
    except UcError as e:
        uc.hook_del(h)
        return 'exc'
    uc.hook_del(h)
    if not lib.lib_step():
        return 'exc'
    o = U16(); lib.lib_get(o)
    ur = [uc.reg_read(r) for r in REGS] + [uc.reg_read(r) for r in SREGS] + [uc.reg_read(UC_X86_REG_IP), uc.reg_read(UC_X86_REG_EFLAGS) & 0xFFFF]
    return regs, segs, ip, fl, list(o), ur

def resync():
    data = ctypes.string_at(ctypes.addressof(mem.contents) + BASE, SIZE)
    uc.mem_write(BASE, data)

fails = collections.Counter()
shown = 0
skipped = 0
for n in range(N):
    while True:
        op = rnd.getrandbits(8)
        if op in BAD: continue
        break
    body = bytes(rnd.getrandbits(8) for _ in range(7))
    pre = b''
    if rnd.random() < 0.15: pre = bytes([rnd.choice([0x26, 0x2E, 0x36, 0x3E])])
    if rnd.random() < 0.08 and op in (0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF):
        pre += bytes([rnd.choice([0xF2, 0xF3])])
    code = pre + bytes([op]) + body
    mr_ = (body[0] >> 3) & 7; mo_ = body[0] >> 6
    if op == 0x8C and mr_ > 3: continue
    if op == 0x8D and mo_ == 3: continue
    if op in (0xC6, 0xC7, 0x8F) and mr_ != 0: continue
    if op == 0xFE and mr_ > 1: continue
    if op == 0xFF and (mr_ == 7 or (mr_ in (3, 5) and mo_ == 3)): continue
    if op == 0x8E and (mr_ == 1 or mr_ > 3): continue
    if op in (0xC4, 0xC5) and mo_ == 3: continue
    if op in (0xF6, 0xF7) and mr_ == 1: continue
    if op == 0xF0: continue
    if op in (0x26, 0x2E, 0x36, 0x3E, 0xF2, 0xF3): continue
    res = run_one(code)
    if res == 'exc':
        resync(); skipped += 1; continue
    regs, segs, ip, fl, o, ur = res
    mask = mask_for(code, regs)
    bad = []
    names = ['AX', 'CX', 'DX', 'BX', 'SP', 'BP', 'SI', 'DI', 'ES', 'CS', 'SS', 'DS', 'IP']
    for k in range(13):
        if o[k] != ur[k]: bad.append((names[k], o[k], ur[k]))
    if (o[13] ^ ur[13]) & mask: bad.append(('FL', o[13] & mask, ur[13] & mask))
    # memory
    mine = ctypes.string_at(ctypes.addressof(mem.contents) + BASE, SIZE)
    theirs = bytes(uc.mem_read(BASE, SIZE))
    if mine != theirs:
        d = [i for i in range(SIZE) if mine[i] != theirs[i]][:4]
        bad.append(('MEM', [(hex(BASE + i), mine[i], theirs[i]) for i in d], ''))
        resync()
    if bad:
        key = code[len(pre)]
        fails[key] += 1
        if fails[key] <= 2 and shown < 40:
            shown += 1
            print('MISMATCH code=%s regs_in=%s fl=%04x ->' % (code.hex(), [hex(x) for x in regs], fl))
            for b in bad: print('   ', b)
print('done', N, 'skipped(exc)', skipped, 'failing opcodes:', {hex(k): v for k, v in sorted(fails.items())})
