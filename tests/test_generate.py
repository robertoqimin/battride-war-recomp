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


if __name__ == '__main__':
    unittest.main()
