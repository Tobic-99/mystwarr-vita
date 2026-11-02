#!/usr/bin/env python3
"""m68k_discover.py [--check]: find the 68000 code in Mystic Warriors' ROM image (rom/m68k.bin, 2 MB at 0, made by
tools/m68k_image.py; program 0-0x7ffff, data ROM 0x100000-0x1fffff).

Recursive descent (our decoder, tools/m68k_isa.py) from the vector table, following
bsr/jsr/bra/Bcc/DBcc and absolute jsr/jmp targets. Writes gen/m68k_functions.txt and prints statistics.
--check: compare every instruction's length and mnemonic with capstone's decoder.
"""
import os, sys, struct, collections
import m68k_isa as I

P = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = open(os.path.join(P, 'rom', 'm68k.bin'), 'rb').read()

# extra entry points found by hand (indirect targets), address -> why; plus those the desktop runner recorded
# when the game reached untranslated code (MW_LEARN, tools/m68k_learned.txt)
EXTRA = {}
try:
    for _l in open(os.path.join(P, 'tools', 'm68k_learned.txt')):
        if _l.strip():
            EXTRA[int(_l.split()[0], 16)] = 'learned'
except OSError:
    pass


def region(a):
    if 0 <= a < len(ROM):
        return ROM, 0
    return None, None


def rd16(a):
    data, base = region(a)
    if data is None or a - base + 2 > len(data):
        return None
    return struct.unpack_from('>H', data, a - base)[0]


def rd32(a):
    data, base = region(a)
    return struct.unpack_from('>I', data, a - base)[0]


def decode(a):
    if a & 1 or region(a)[0] is None:
        return None
    return I.decode(rd16, a)


def static_target(e):
    """Static address of a jsr/jmp operand, or None."""
    if e.kind in ('absw', 'absl', 'pcdisp'):
        return e.val & 0xFFFFFF
    return None


class Func:
    def __init__(s, addr, why):
        s.addr, s.why = addr, why
        s.insns = {}
        s.bad = False
        s.ind = 0
        s.switches = {}      # jmp address -> list of case targets (index order)
        s.calltabs = {}      # jsr address -> pointer-table entries (called through the table)


funcs = {}
pending = collections.deque()
indirect = collections.Counter()


def add(a, why):
    if region(a)[0] is None or a & 1:
        return
    if a not in funcs:
        funcs[a] = Func(a, why)
        pending.append(a)


def switch_targets(f, ins):
    """GCC's switch: moveq #N,d1; cmp.l d1,d0; bhi default; add.l d0,d0; move.w T(pc,d0.l),d0; jmp T(pc,d0.w).
    Returns the N+1 case targets (T + table word), or None."""
    e = ins.ops[0]
    if e.kind != 'pcidx':
        return None
    prev = sorted(x for x in f.insns if x < ins.addr)[-6:]
    seq = [f.insns[x] for x in prev]
    if len(seq) < 5 or seq[-1].op != 'move' or seq[-1].ops[0].kind != 'pcidx' or seq[-1].ops[0].val != e.val:
        return None
    n = next((x.ops[0].val for x in reversed(seq) if x.op == 'moveq'), None)
    if n is None or n > 256:
        return None
    return [(e.val + I.sext(rd16(e.val + 2 * i), 16)) & 0xFFFFFF for i in range(n + 1)]


TABLES = set()   # bases of pointer tables found so far (data, never code)


def looks_like_code(v, n=12):
    """Linear decode from v: no undecodable word and no 0x0000 (ori.b #0,d0: zero fill) before the first
    unconditional end (rts/rte/jmp/bra) or n instructions."""
    a = v
    for k in range(n):
        w = rd16(a)
        if w in (None, 0):
            return False
        # 0x00xx with byte size = ori.b #imm: assemblers write the immediate as 0x00ii. A non-zero high byte means
        # this is the high half of a long in a table (0x0002xxxx, 0x0020xxxx), not code.
        if w < 0x100 and (w >> 6) & 3 == 0 and (rd16(a + 2) or 0) >> 8:
            return False
        ins = decode(a)
        if ins is None:
            return False
        if ins.op in ('rts', 'rte', 'rtr', 'jmp', 'bra'):
            return True
        a += ins.size
    return True


def code_ptr(v):
    """A plausible code address: even, in the program or data ROM, and looks like code."""
    return (not v & 1 and (0x400 <= v < 0x80000 or 0x100000 <= v < 0x200000) and v not in TABLES
            and looks_like_code(v))


def read_table(base):
    """Entries of a long-pointer table at base, read while they look like code (at most 256). Null entries are
    holes (unused indices, e.g. 0x1bba[23]) and are skipped; the first other invalid entry ends the table."""
    TABLES.add(base)
    out = []
    for k in range(256):
        v = rd32(base + 4 * k) if rd16(base + 4 * k + 2) is not None else None
        if v == 0:
            continue
        if v is None or not code_ptr(v):
            break
        out.append(v)
    return out or None


def ptr_table(f, ins):
    """Konami's long-pointer table: lea T,aT; ...; movea.l 0(aT,dX.w),aR; jsr/jmp (aR).
    Returns the entries (read while they look like code, at most 256), or None."""
    e = ins.ops[0]
    if e.kind != 'ind':
        return None
    prev = [f.insns[x] for x in sorted(x for x in f.insns if x < ins.addr)[-8:]]
    for k in range(len(prev) - 1, -1, -1):
        m = prev[k]
        if m.op == 'movea' and m.ops[1].kind == 'an' and m.ops[1].reg == e.reg:
            if m.ops[0].kind == 'pcidx':   # movea.l T(pc,dX.w),aR
                return read_table(m.ops[0].val & 0xFFFFFF)
            if m.ops[0].kind != 'idx' or m.ops[0].disp != 0:
                return None
            t = m.ops[0].reg
            for l in prev[k - 1::-1] if k else []:
                if l.op == 'lea' and l.ops[1].kind == 'an' and l.ops[1].reg == t and l.ops[0].kind in ('absw', 'absl', 'pcdisp'):
                    return read_table(l.ops[0].val & 0xFFFFFF)
            return None
    return None


def walk(f):
    work = [f.addr]
    while work:
        a = work.pop()
        while True:
            if a in f.insns:
                break
            ins = decode(a)
            if ins is None:
                f.bad = True
                break
            f.insns[a] = ins
            nxt = a + ins.size
            op = ins.op
            if op in ('jsr', 'bsr'):
                t = ins.target if op == 'bsr' else static_target(ins.ops[0])
                tab = ptr_table(f, ins) if t is None else None
                if tab:
                    f.calltabs[a] = tab
                    for v in tab:
                        add(v, 'calltab')
                elif t is None:
                    f.ind += 1; indirect['call'] += 1
                else:
                    add(t, op)
                a = nxt
                continue
            if op in ('rts', 'rte', 'rtr', 'stop', 'illegal', 'reset'):
                break
            if op == 'jmp':
                t = static_target(ins.ops[0])
                sw = switch_targets(f, ins) or ptr_table(f, ins)
                if sw is not None:
                    f.switches[a] = sw
                    work.extend(sw)
                    break
                if t is None:
                    f.ind += 1; indirect['jmp'] += 1
                else:
                    work.append(t)
                break
            if op == 'bra':
                work.append(ins.target)
                break
            if op in ('bcc', 'dbcc'):
                work.append(ins.target)
            a = nxt


def run():
    for i in range(1, 64):
        v = rd32(4 * i)
        if v:
            add(v, 'vector%d' % i)
    for a, why in EXTRA.items():
        add(a, why)
    for a in scan_tables():
        add(a, 'tablescan')
    while pending:
        walk(funcs[pending.popleft()])
    # code pointers passed as operands (pea fn / pea fn(pc) / lea fn / move.l #fn): callbacks the game registers at run time.
    # Only values inside the code area found so far count; repeat until nothing new turns up.
    while True:
        lo_b, hi_b = code_range(0, 0x100000)
        lo_g, hi_g = code_range(0x100000, 0x200000)
        n = 0
        for f in list(funcs.values()):
            for ins in f.insns.values():
                if ins.op not in ('pea', 'lea', 'move', 'movea'):
                    continue
                for e in ins.ops:
                    if e.kind in ('imm', 'absl') and ins.sz == 4 or e.kind in ('absl', 'pcdisp') and ins.op in ('pea', 'lea'):
                        v = e.val & 0xFFFFFFFF
                        if (lo_b <= v < hi_b or lo_g <= v < hi_g) and v not in funcs and code_ptr(v):
                            add(v, 'ptr-ref'); n += 1
        if not n:
            break
        while pending:
            walk(funcs[pending.popleft()])
    sweep_gaps()


def covered():
    cov = set()
    for f in funcs.values():
        for a, i in f.insns.items():
            cov.update(range(a, a + i.size, 2))
    return cov


def sweep_gaps():
    """Routines nothing static points at (reached through RAM copies or computed addresses): in the program ROM's
    uncovered stretches, every place that starts a stretch or follows a routine's end and decodes as code is an entry.
    An extra entry into data costs code size only; a missing entry ends the game."""
    while True:
        cov = covered()
        n = 0
        a = 0x400
        while a < 0x80000:
            if a in cov:
                a += 2
                continue
            start = a
            while a < 0x80000 and a not in cov:
                if (a == start or after_end(a)) and a not in funcs and code_ptr(a):
                    add(a, 'gap')
                    n += 1
                a += 2
        if not n:
            return
        while pending:
            walk(funcs[pending.popleft()])


def after_end(v):
    """The word(s) before v end a routine: rts, rte, jmp (An), jmp abs.l, or bra."""
    w = rd16(v - 2) or 0
    return (w in (0x4E75, 0x4E73) or 0x4ED0 <= w <= 0x4ED7 or (w >> 8 == 0x60 and w & 0xFF not in (0, 0xFF))
            or rd16(v - 6) == 0x4EF9 or rd16(v - 4) == 0x6000)


def scan_tables(min_run=3):
    """Pointer tables anywhere in the ROM: runs of >= min_run records (stride 4..16 bytes) whose first long is a
    plausible code pointer, e.g. the 10-byte object records at 0x4dd18 (code pointer + 6 bytes of parameters).
    Konami reaches many of these through RAM copies of the table address, which the per-call idioms cannot follow."""
    from functools import lru_cache
    ok = lru_cache(maxsize=None)(lambda v: code_ptr(v))
    def ptr_at(a):
        if a + 4 > len(ROM):
            return None
        v = rd32(a)
        if v & 1 or not (0x400 <= v < 0x80000 or 0x100000 <= v < 0x200000):
            return None
        return v
    out = set()
    for stride in range(4, 18, 2):
        a = 0
        while a < len(ROM) - 4:
            run = []
            b = a
            while True:
                v = ptr_at(b)
                if v is None:
                    break
                run.append(v)
                b += stride
            if len(run) >= min_run and all(ok(v) for v in run):
                out.update(run)
                a = b
            else:
                # A table can mix code and data pointers (0x1215a, per-object records), so one data pointer must
                # not lose the rest: keep stretches of plausible code pointers where most start like routines (some
                # routines follow data, not another routine's end).
                good = []
                for v in run + [None]:
                    if v is not None and ok(v):
                        good.append(v)
                    else:
                        if len(good) >= min_run and 3 * sum(map(after_end, good)) >= 2 * len(good):
                            out.update(good)
                        good = []
                a += 2
    # A lone pointer counts when it lands right after a return.
    for a in range(0, len(ROM) - 4, 2):
        v = ptr_at(a)
        if v is not None and v not in out and rd16(v - 2) in (0x4E75, 0x4E73) and ok(v):
            out.add(v)
    return sorted(out)


def code_range(lo, hi):
    xs = [a for f in funcs.values() for a in f.insns if lo <= a < hi]
    return (min(xs), max(xs)) if xs else (0, 0)


def check():
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
    md.detail = True
    bad = 0
    for f in funcs.values():
        for a, ins in f.insns.items():
            data, base = region(a)
            c = next(md.disasm(data[a - base:a - base + 10], a, 1), None)
            cm = c.mnemonic.split('.')[0] if c else None
            om = repr(ins).split()[0].split('.')[0]
            if c is None or c.size != ins.size or cm != om:
                bad += 1
                if bad <= 40:
                    print('%06x ours %-30r (%d)  capstone %s %s (%s)' % (a, ins, ins.size, cm, c.op_str if c else '', c.size if c else '-'))
    print('check: %d differences' % bad)


def main():
    run()
    n = sum(len(f.insns) for f in funcs.values())
    nb = sum(len(f.insns) for a, f in funcs.items() if a < 0x100000)
    print('functions: %d, instructions %d (program ROM %d, data ROM %d), bad %d' % (len(funcs), n, nb, n - nb, sum(f.bad for f in funcs.values())))
    print('indirect:', dict(indirect), 'switches %d' % sum(len(f.switches) for f in funcs.values()))
    mn = collections.Counter(i.op for f in funcs.values() for i in f.insns.values())
    print('operations (%d):' % len(mn), ', '.join('%s %d' % kv for kv in mn.most_common()))
    with open(os.path.join(P, 'gen', 'm68k_functions.txt'), 'w') as o:
        for a in sorted(funcs):
            f = funcs[a]
            o.write('%06x %5d insns ind %d %s%s\n' % (a, len(f.insns), f.ind, f.why, ' BAD' if f.bad else ''))
    if '--check' in sys.argv:
        check()


if __name__ == '__main__':
    main()
