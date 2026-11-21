import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('port_generate', Path(__file__).resolve().parents[1] / 'scripts/build/generate.py')
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)


class GenerateCacheTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.sdk = self.dir / 'sdk'
        (self.sdk / 'tools').mkdir(parents=True)
        (self.sdk / 'tools/ppu_lifter.py').write_text('lifter')
        self.flash = self.dir / 'dev_flash'
        (self.flash / 'sys/external').mkdir(parents=True)
        (self.flash / 'sys/external/flashMP3.pic').write_bytes(b'mp3')

    def tearDown(self):
        self.tmp.cleanup()

    def test_key_follows_what_the_sources_depend_on(self):
        base = g.cache_key(self.sdk, 'eboot', self.flash)
        self.assertEqual(g.cache_key(self.sdk, 'eboot', self.flash), base)
        self.assertNotEqual(g.cache_key(self.sdk, 'other', self.flash), base)
        (self.sdk / 'tools/ppu_lifter.py').write_text('lifter 2')
        self.assertNotEqual(g.cache_key(self.sdk, 'eboot', self.flash), base)
        (self.sdk / 'tools/ppu_lifter.py').write_text('lifter')
        (self.flash / 'sys/external/flashMP3.pic').write_bytes(b'other mp3')
        self.assertNotEqual(g.cache_key(self.sdk, 'eboot', self.flash), base)
        # Without the firmware the audio lifter leaves the codec interpreted: another key.
        self.assertNotEqual(g.cache_key(self.sdk, 'eboot', self.dir / 'none'), base)
        # Bytecode caches of the SDK tools do not count.
        (self.flash / 'sys/external/flashMP3.pic').write_bytes(b'mp3')
        (self.sdk / 'tools/__pycache__').mkdir()
        (self.sdk / 'tools/__pycache__/ppu_lifter.cpython.pyc').write_bytes(b'x')
        self.assertEqual(g.cache_key(self.sdk, 'eboot', self.flash), base)

    def test_store_and_restore_rewrite_the_spu_list(self):
        made = self.dir / 'made'
        sources = [made / name for name in g.GENERATED]
        for path in sources:
            path.mkdir(parents=True)
            (path / 'file.txt').write_text(path.name)
        old_root = g.ROOT
        try:
            g.ROOT = made
            (made / 'spu/sources.cmake').write_text('set(X "%s/a.c")\n' % (made / 'spu'))
            cache = self.dir / 'cache'
            (cache / 'gen-stale').mkdir(parents=True)
            g.store_cached(cache, 'k' * 64, sources)
            self.assertEqual(sorted(p.name for p in cache.iterdir()), ['gen-' + 'k' * 32])
            entry = cache / ('gen-' + 'k' * 32)
            self.assertEqual(json.loads((entry / 'complete.json').read_text())['key'], 'k' * 64)
            elsewhere = self.dir / 'elsewhere'
            g.ROOT = elsewhere
            g.restore_cached(entry, [elsewhere / name for name in g.GENERATED])
            self.assertEqual((elsewhere / 'spu/sources.cmake').read_text(),
                             'set(X "%s/a.c")\n' % (elsewhere / 'spu'))
            for name in g.GENERATED:
                self.assertTrue((elsewhere / name / 'file.txt').is_file())
        finally:
            g.ROOT = old_root


    def test_chunk_map_and_hook_follow_the_functions(self):
        ppu = self.dir / 'recomp'
        ppu.mkdir()
        (ppu / 'ppu_recomp_000.cpp').write_text('void func_00000100(ppu_context* ctx) {\n}\n')
        (ppu / 'ppu_recomp_001.cpp').write_text('void func_00000200(ppu_context* ctx) {\n    ctx->lr = 0x00000208;\n'
                                                '    func_00000300(ctx);\n}\nvoid func_00000400(ppu_context* ctx) {\n}\n')
        where = g.chunk_map(ppu, {'native_lookup': 'func_00000400', 'fast_memory': ['func_00000100', 'func_00000200']})
        self.assertEqual(where, {'func_00000100': '000', 'func_00000200': '001', 'func_00000400': '001'})
        text = (ppu / 'port_chunks.cmake').read_text()
        self.assertIn('set(PORT_CHUNK_NATIVE_LOOKUP "001")', text)
        self.assertIn('set(PORT_CHUNKS_FAST_MEMORY "000;001")', text)
        with self.assertRaises(RuntimeError):
            g.chunk_map(ppu, {'native_lookup': 'func_DEADBEEF'})
        # The graphics hook is found in whichever chunk holds the call.
        g.hook_graphics_buffers(ppu, '0x00000208')
        lines = (ppu / 'ppu_recomp_001.cpp').read_text().split('\n')
        self.assertTrue(lines[0].startswith('extern "C" void port_expand_graphics_buffers'))
        self.assertEqual(lines[2].strip(), 'port_expand_graphics_buffers(ctx, (uint32_t)ctx->gpr[31]);')
        self.assertEqual(lines[3].strip(), 'ctx->lr = 0x00000208;')
        with self.assertRaises(RuntimeError):
            g.hook_graphics_buffers(ppu, '0x00000999')

    def test_the_edition_names_the_native_code(self):
        ppu = self.dir / 'recomp'
        ppu.mkdir()
        (ppu / 'ppu_recomp_000.cpp').write_text('void func_00000100(ppu_context* ctx) {\n}\n'
                                                'void func_00000200(ppu_context* ctx) {\n}\n')
        edition = {'title_id': 'BLJS10219', 'memmove': '0x3712F4'}
        g.chunk_map(ppu, {'native_lookup': 'func_00000100', 'frame_limiter': 'func_00000200'}, edition)
        text = (ppu / 'port_chunks.cmake').read_text()
        self.assertIn('set(PORT_TITLE_ID "BLJS10219")', text)
        self.assertIn('set(PORT_FN_NATIVE_LOOKUP "func_00000100")', text)
        self.assertIn('set(PORT_FN_FRAME_LIMITER "func_00000200")', text)
        self.assertIn('set(PORT_GUEST_MEMMOVE "0x003712F4")', text)


class EditionTests(unittest.TestCase):
    spec = json.loads((Path(__file__).resolve().parents[1] / 'recompile.json').read_text())

    def test_each_edition_is_found_by_its_eboot(self):
        hashes = [e['eboot_sha256'] for e in self.spec['editions']]
        self.assertEqual(len(set(hashes)), len(hashes))
        self.assertEqual([e['title_id'] for e in self.spec['editions']], ['BLJS10220', 'BLJS10219'])
        for e in self.spec['editions']:
            self.assertIs(g.edition_for(self.spec, e['eboot_sha256']), e)
        self.assertIsNone(g.edition_for(self.spec, '0' * 64))

    def test_each_edition_names_what_the_port_tunes(self):
        for e in self.spec['editions']:
            a = e['chunk_anchors']
            self.assertEqual(set(a), set(g.CHUNK_VARS), e['title_id'])
            names = [a['native_lookup'], a['frame_limiter'], a['hot_o3'], a['flags_unit'], *a['fast_memory']]
            for name in names:
                self.assertRegex(name, r'^func_[0-9A-F]{8}$')
            self.assertEqual(a['hot_o3'], a['native_lookup'])
            self.assertIn(a['native_lookup'], a['fast_memory'])
            self.assertIn(a['flags_unit'], a['fast_memory'])
            for key in ('arena_hook_return', 'memmove'):
                self.assertRegex(e[key], r'^0x[0-9A-F]{8}$')

if __name__ == '__main__':
    unittest.main()
