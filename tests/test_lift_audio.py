import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('port_lift_audio', Path(__file__).resolve().parents[1] / 'scripts/build/lift_audio_spu.py')
a = importlib.util.module_from_spec(spec)
spec.loader.exec_module(a)

SOURCE = '''/* Auto-generated */
#include "spu_recomp.h"

void spu_t_spu_func_00000100(spu_context* ctx) {
    { uint32_t _tg = ctx->gpr[5]._u32[0]; ctx->gpr[0] = spu_link(0x108); ctx->pc = _tg; ctx->host_depth++; spu_indirect_branch(ctx); spu_drain_call(ctx, 0x108); ctx->host_depth--; }
    spu_t_spu_func_00000200(ctx); spu_drain_call(ctx, 0x10C);
}
void spu_t_spu_func_00000200(spu_context* ctx) {
    g_spu_trampoline_fn = spu_indirect_branch;
}
void spu_t_spu_func_00000300(spu_context* ctx) {
    spu_unsupported(ctx, 0x300);
}
void spu_t_spu_func_00000400(spu_context* ctx) {
    spu_t_spu_func_00000300(ctx); spu_drain_call(ctx, 0x404);
}
'''


class LiftAudioTests(unittest.TestCase):
    def test_calls_return_through_the_port_drain(self):
        out = a.own_drains(SOURCE)
        self.assertNotIn('spu_drain_call(', out)
        self.assertNotIn('spu_indirect_branch(ctx)', out)
        self.assertIn('port_audio_drain(ctx, 0x108);', out)
        self.assertIn('spu_t_spu_func_00000200(ctx); port_audio_drain(ctx, 0x10C);', out)
        self.assertIn('#include "spu_recomp.h"\nvoid port_audio_drain(spu_context* ctx, uint32_t return_pc);', out)

    def test_only_untranslatable_words_stay_interpreted(self):
        run, keep = a.compiled_entries(a.own_drains(SOURCE), 'spu_t_')
        # An indirect call and a queued indirect transfer are safe now; a word the
        # translator rejects is not, nor a direct call into it.
        self.assertEqual(run, [0x100, 0x200])
        self.assertEqual(keep, [0x300, 0x400])
        # Without the port drain an indirect call still keeps its function interpreted.
        self.assertIn(0x100, a.compiled_entries(SOURCE, 'spu_t_')[1])


if __name__ == '__main__':
    unittest.main()
