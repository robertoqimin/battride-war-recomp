import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('port_gcm_map', Path(__file__).resolve().parents[1] / 'scripts/analysis/gcm_map.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)

CHUNK = '''void func_00001000(ppu_context* ctx) {
        ctx->gpr[9] = (int64_t)(int32_t)((uint32_t)0x4 << 16);
        ctx->gpr[10] = ctx->gpr[9] | 0x1808;
        ctx->gpr[11] = ctx->gpr[9] | 0x1814;
        ctx->gpr[12] = ctx->gpr[30] | 0x1808;
        ctx->gpr[5] = (int64_t)(int32_t)((uint32_t)0x7FFF << 16);
        ctx->gpr[6] = ctx->gpr[5] | 0x1808;
}
void func_00002000(ppu_context* ctx) {
        ctx->lr = 0x2008; func_00001000(ctx); DRAIN_TRAMPOLINE(ctx);
}
'''


class GcmMapTests(unittest.TestCase):
    def test_headers_built_by_lis_and_ori(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / 'ppu_recomp_000.cpp').write_text(CHUNK)
            funcs = g.scan(Path(tmp), {0x1808: 'SET_BEGIN_END', 0x1814: 'DRAW_ARRAYS'})
        # Only an OR with a register holding a method-count header counts: not a
        # register it never set, not a value too large to be a header.
        self.assertEqual(dict(funcs[0x1000]['methods']), {'SET_BEGIN_END': 1, 'DRAW_ARRAYS': 1})
        self.assertEqual(funcs[0x2000]['calls'], {0x1000})
        self.assertEqual(g.layer('SET_BEGIN_END'), 'draw')
        self.assertEqual(g.layer('SET_SURFACE_FORMAT'), 'pass')
        self.assertEqual(g.layer('SET_SHADER_PROGRAM'), 'material')


if __name__ == '__main__':
    unittest.main()
