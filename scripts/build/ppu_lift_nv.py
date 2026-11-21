#!/usr/bin/env python3
"""The pinned SDK's PPU lifter with the guest registers as C locals.

usage: ppu_lift_nv.py --sdk-tools <sdk>/tools <the arguments of the SDK's tools/ppu_lifter.py>

In the plain lift every guest register is a field of *ctx; the build has no
strict aliasing and every guest access is an atomic, so the compiler stores
each register to the file and loads it back around every guest load and
store. This runs the SDK's lifter unchanged (it is imported, not edited) and
adds one pass, the register-locals pass of soyjxck/ps3recomp as the
infamous2-port uses it (scripts/build/ppu_nv_dirty.py, vendored unmodified):

  * after each function is lifted, its body is rewritten so the registers it
    names are locals, written back where control leaves the function (a call,
    a syscall, a tail jump, a return), and only those that changed;
  * before the functions are written, the registers each transfer has to
    write back are narrowed by a whole-program liveness (ppu_nv_dirty.solve)
    and applied line by line (ppu_nv_dirty.expand).

A body of a shape the pass does not follow keeps the plain form, which is
always correct. The function set, the chunks and everything else of the lift
are the SDK's (docs/PLANO0.6.1.md, Fase 1: the infamous2 lifter itself splits
the EBOOT differently).

PORT_PPU_NV_VERIFY=1 lifts the checking form: a register taken as unchanged
is compared with the file at each transfer (reported once per site on
stderr, "[nv-verify]"), and every write-back the liveness drops stores a
poison value a wrong liveness would read.
"""
import os
import sys
import time

# The lifter's worker processes import this file again (forkserver, spawn):
# they find the SDK through the environment the parent set.
if '--sdk-tools' in sys.argv:
    _i = sys.argv.index('--sdk-tools')
    os.environ['PORT_SDK_TOOLS'] = os.path.abspath(sys.argv[_i + 1])
    del sys.argv[_i:_i + 2]
if not os.environ.get('PORT_SDK_TOOLS'):
    sys.exit(__doc__)
SDK_TOOLS = os.environ['PORT_SDK_TOOLS']
# The pass's other rewrites need the infamous2 runtime's macros: off.
os.environ.setdefault('PPU_FRAME_ENTRY', '0')
os.environ.setdefault('PPU_VM_RUNS', '0')
VERIFY = os.environ.get('PORT_PPU_NV_VERIFY', '0') not in ('', '0')
LIVE = os.environ.get('PPU_NV_LIVE', '1') != '0'

sys.path.insert(0, SDK_TOOLS)
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_lifter as L          # noqa: E402  (the SDK's, from SDK_TOOLS)
import ppu_nv_dirty as NV       # noqa: E402  (vendored beside this file)

NV_STATS = {}

# --- the pass, after every lift_function --------------------------------------
_lift_function = L.PPULifter.lift_function


def _lift_function_nv(self, *args, **kwargs):
    func = _lift_function(self, *args, **kwargs)
    new, func.nv = NV.rewrite_function(func.body_lines, func.start_addr, verify=VERIFY,
                                       stats=NV_STATS, live=LIVE)
    if new is not None:
        func.body_lines = new
    return func


L.PPULifter.lift_function = _lift_function_nv


# --- the workers hand the summary back with the body --------------------------
def _worker_lift(task):
    idx0, bounds = task
    st = L._WORKER_STATE
    lifter = L.PPULifter(prefix=st.get('prefix', ''))
    lifter.name_map = st['names']
    lifter.hle_stub_nids = st.get('hle_stub_nids', {})
    lifter.function_entries = st.get('function_entries', set())
    lifter.code_lo = st.get('code_lo', 0)
    lifter.code_hi = st.get('code_hi', None)
    lifter.jump_tables = st.get('jump_tables', {})
    lifter.toc_base = st.get('toc_base', 0)
    results = []
    for start, end in bounds:
        blob = b''
        for va, d in st['segs']:
            if va <= start < va + len(d):
                blob = d[start - va:min(end - va, len(d))]
                break
        insns = L.disassemble_bytes(blob, start, st['be']) if blob else []
        f = lifter.lift_function(insns, start, end)
        results.append((f.name, f.start_addr, f.end_addr, f.body_lines, f.calls, f.fallthrough_to,
                        getattr(f, 'nv', None)))
    return idx0, results, lifter.call_targets, lifter.branch_targets, dict(NV_STATS)


def _parallel_lift(lifter, func_bounds, segs, big_endian, jobs):
    import multiprocessing as mp
    n = len(func_bounds)
    chunk = max(50, n // (jobs * 8))
    tasks = [(i, func_bounds[i:i + chunk]) for i in range(0, n, chunk)]
    print(f'  parallel lift: {jobs} workers, {len(tasks)} chunks of ~{chunk} (register locals)', flush=True)
    results_by_idx, done, t0 = {}, 0, time.time()
    pool = mp.Pool(processes=jobs, initializer=L._worker_init,
                   initargs=(segs, big_endian, lifter.name_map, lifter.prefix, lifter.hle_stub_nids,
                             lifter.function_entries, lifter.code_lo, lifter.code_hi, lifter.jump_tables,
                             getattr(lifter, 'toc_base', 0)))
    try:
        for idx0, results, ct, bt, stats in pool.imap_unordered(_worker_lift, tasks):
            results_by_idx[idx0] = results
            lifter.call_targets |= ct
            lifter.branch_targets |= bt
            for k, v in stats.items():
                if isinstance(v, int):
                    NV_STATS[k] = NV_STATS.get(k, 0) + v
            done += len(results)
            print(f'  ... lifted {done}/{n} functions ({100 * done // n}%) '
                  f'elapsed {(time.time() - t0) / 60:.1f}m', flush=True)
        pool.close()
        pool.join()
    except BaseException:
        pool.terminate()
        pool.join()
        raise
    for idx0 in sorted(results_by_idx):
        for name, s, e, body, calls, ft, nv in results_by_idx[idx0]:
            f = L.LiftedFunction(name=name, start_addr=s, end_addr=e, body_lines=body, calls=calls,
                                 fallthrough_to=ft)
            f.nv = nv
            lifter.functions.append(f)


L._worker_lift = _worker_lift
L._parallel_lift = _parallel_lift


# --- the whole-program liveness, then each write-back line ---------------------
def _nv_needs(self):
    """(need, summaries) for ppu_nv_dirty.expand, solved once when the first
    function is written."""
    cached = getattr(self, '_nv_need_cache', None)
    if cached is not None:
        return cached
    funcs = self._emit_functions()
    t0 = time.time()
    summ = {f.start_addr: (getattr(f, 'nv', None) if getattr(f, 'nv', None) is not None
                           else NV.summarize_plain(f.body_lines)) for f in funcs}
    fall = {f.start_addr: f.fallthrough_to for f in funcs if f.fallthrough_to and f.fallthrough_to in summ}
    ends = {f.start_addr: f.end_addr for f in funcs}
    log = {}
    # The SDK lifter keeps no list of the functions a guest pointer can name:
    # every function may be called indirectly.
    need = NV.solve(summ, set(summ), set(), fall, ends, log)
    self._nv_counts = [0, 0]
    self._nv_summ = summ
    self._nv_nvout = {a: v['nvout'] for a, v in summ.items() if v.get('nvout')}
    called = set(self.call_targets) | set(summ)
    self._nv_vr_reload = any(v.get('vclob') and a in called for a, v in summ.items())
    plain = sum(1 for v in summ.values() if v.get('plain'))
    print(f'  register liveness: {len(summ)} functions ({plain} kept plain), {log.get("rounds")} rounds, '
          f'{time.time() - t0:.1f} s', flush=True)
    self._nv_need_cache = need
    return need


def _function_def_lines(self, func, func_by_addr, sorted_addrs, addr_index):
    """The SDK's _function_def_lines with the write-backs expanded (and without
    its CR fusion: with the registers in locals the compiler sees the compare
    and the branch together)."""
    need = _nv_needs(self)
    saved = func.body_lines
    out = []
    for line in saved:
        if '/*@NV' in line:
            line = NV.expand(line, self._nv_summ[func.start_addr], need[func.start_addr], VERIFY,
                             self._nv_counts, self._nv_vr_reload, self._nv_nvout)
        out.append(line)
    fuse = L._fuse_cr
    try:
        L._fuse_cr = lambda lines: lines
        func.body_lines = out
        return _sdk_function_def_lines(self, func, func_by_addr, sorted_addrs, addr_index)
    finally:
        func.body_lines = saved
        L._fuse_cr = fuse


_sdk_function_def_lines = L.PPULifter._function_def_lines
L.PPULifter._function_def_lines = _function_def_lines

_sdk_preamble_lines = L.PPULifter._preamble_lines


def _preamble_lines(self):
    lines = _sdk_preamble_lines(self)
    if VERIFY:
        lines += [
            '/* PORT_PPU_NV_VERIFY lift (ppu_nv_dirty.py): a local taken as unchanged is compared',
            ' * with the register file at each transfer; a dropped write-back stores poison. */',
            '#ifndef PPU_UNLIKELY',
            '#define PPU_UNLIKELY(x) __builtin_expect(!!(x), 0)',
            '#endif',
            'static void ppu_nv_differs(ppu_context* ctx, unsigned fn)',
            '{',
            '    static int said = 0;',
            '    if (said < 60) { said++; fprintf(stderr, "[nv-verify] func_%08X (lr %08X): a local taken as '
            'unchanged differs from the register file\\n", fn, (unsigned)ctx->lr); fflush(stderr); }',
            '}',
            'static inline uint64_t ppu_nv_xf(const double* a, double b)',
            '{ uint64_t x, y; memcpy(&x, a, 8); memcpy(&y, &b, 8); return x ^ y; }',
            '#define PPU_NV_XF(n) ppu_nv_xf(&ctx->fpr[n], f##n)',
            '#define PPU_NV_XV(n) ((uint64_t)(memcmp(&ctx->vr[n], &v##n, 16) != 0))',
            '#define PPU_NV_POISON_G(n) (ctx->gpr[n] = 0xBAD0BAD0BAD00000ull + n)',
            '#define PPU_NV_POISON_F(n) do { uint64_t _p = 0x7FF4BAD0BAD00000ull + n; '
            'memcpy(&ctx->fpr[n], &_p, 8); } while (0)',
            '#define PPU_NV_POISON_S(n) (ctx->n = 0xBAD0C0DEu)',
            '#define PPU_NV_POISON_V(n) memset(&ctx->vr[n], 0xBD, 16)',
            '',
        ]
    return lines


L.PPULifter._preamble_lines = _preamble_lines


if __name__ == '__main__':
    t0 = time.time()
    L.main()
    keep = {k: v for k, v in NV_STATS.items() if isinstance(v, int)}
    print(f'  register locals: {keep} ({time.time() - t0:.0f} s)', flush=True)
