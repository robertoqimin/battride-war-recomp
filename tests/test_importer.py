import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import threading

spec=importlib.util.spec_from_file_location('port_importer',Path(__file__).resolve().parents[1]/'packaging/importer.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

class ImporterTests(unittest.TestCase):
    def test_unsafe_paths(self):
        for name in ('../x','/x','a/../../b','a\\b','a\nb',''):
            with self.subTest(name=name),self.assertRaises(m.ImportError):m.safe_name(name)
    def test_archive_links_and_duplicates(self):
        for text in ('Path = x\nSymbolic Link = /tmp/x','Path = x\n\nPath = x'):
            with self.assertRaises(m.ImportError):m.disc_entries(text)
    def test_listing(self):
        self.assertEqual(m.disc_entries('Path = PS3_GAME/EBOOT.BIN\nSize = 123\n\n')['PS3_GAME/EBOOT.BIN']['Size'],'123')
    def test_bad_iso_leaves_no_partial_import(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            (root/'app/package.json').write_text(json.dumps({'elf_sha256':'x','codecs':{}}))
            (root/'app/disc-manifest.json').write_text(json.dumps({'PS3_GAME/test':{'size':1,'sha256':'x'}}))
            iso=root/'game.iso';iso.write_bytes(b'bad')
            data=root/'data'
            class FakeRunner:
                cancelled=threading.Event()
                def run(self,*a,**kw):return 2,'ERROR: invalid archive'
            with self.assertRaisesRegex(m.ImportError, 'Could not read the ISO filesystem'):m.Package(root).import_iso(iso,data,FakeRunner())
            self.assertEqual(list(data.iterdir()),[])
    def test_missing_codec_is_not_reported_as_wrong_game(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            (root/'app/package.json').write_text('{}');(root/'app/disc-manifest.json').write_text('{}')
            iso=root/'game.iso';iso.write_bytes(b'iso');data=root/'data'
            class FakeRunner:
                cancelled=threading.Event()
                def run(self,*a,**kw):return 2,'Codec Load Error: /usr/libexec/7zip/7z.so'
            with self.assertRaisesRegex(m.ImportError, 'bundled ISO reader'):
                m.Package(root).import_iso(iso,data,FakeRunner())
            self.assertEqual(list(data.iterdir()),[])

    def test_existing_import_preserved(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            (root/'app/package.json').write_text('{}');(root/'app/disc-manifest.json').write_text('{}')
            iso=root/'game.iso';iso.write_bytes(b'iso');data=root/'data';(data/'game').mkdir(parents=True)
            sentinel=data/'game/keep';sentinel.write_text('saved')
            with self.assertRaises(m.ImportError):m.Package(root).import_iso(iso,data,m.Runner())
            self.assertEqual(sentinel.read_text(),'saved')
    def test_cancelled_runner_does_not_start_process(self):
        r=m.Runner();r.stop()
        with self.assertRaises(m.ImportError):r.run(['/does/not/exist'])

    def test_built_runner_reused_only_for_same_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            (root/'app/package.json').write_text(json.dumps({'version':'1','sdk_commit':'s','elf_sha256':'e','codecs':{}}))
            (root/'app/disc-manifest.json').write_text('{}')
            p=m.Package(root);data=root/'data';(data/'runner').mkdir(parents=True)
            self.assertIsNone(p.built_runner(data))
            (data/'runner'/('game-runner'+m.EXE)).write_bytes(b'x')
            (data/'runner/build.json').write_text(json.dumps(p.build_identity()))
            self.assertEqual(p.built_runner(data),data/'runner'/('game-runner'+m.EXE))
            other=dict(p.build_identity(),cpu={'model':'other','flags_sha256':'x'})
            (data/'runner/build.json').write_text(json.dumps(other))
            self.assertIsNone(p.built_runner(data))
            # Same version, changed port sources: rebuild.
            (data/'runner/build.json').write_text(json.dumps(p.build_identity()))
            (root/'usr/share/battride/port').mkdir(parents=True);(root/'usr/share/battride/port/input_bridge.c').write_text('x')
            self.assertIsNone(m.Package(root).built_runner(data))
    def test_wrong_local_rpcs3_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            info={'version':'v','file':'r.AppImage','url':'https://invalid.example/r','size':3,'sha256':'0'*64}
            (root/'app/package.json').write_text(json.dumps({'rpcs3':info}));(root/'app/disc-manifest.json').write_text('{}')
            fake=root/'r.AppImage';fake.write_bytes(b'bad')
            with self.assertRaisesRegex(m.ImportError,'not the expected build'):
                m.Package(root).rpcs3_helper(root/'data',m.Runner(),local=fake)
    def test_profiles_used_only_for_same_sdk_and_eboot(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);(root/'app').mkdir()
            (root/'app/package.json').write_text(json.dumps({'version':'1','sdk_commit':'s','elf_sha256':'e','codecs':{}}))
            (root/'app/disc-manifest.json').write_text('{}')
            p=m.Package(root)
            self.assertIsNone(p.profiles())
            self.assertFalse(p.build_identity()['pgo'])
            pgo=root/'usr/share/battride/pgo';pgo.mkdir(parents=True)
            (pgo/'profile.json').write_text(json.dumps({'sdk_commit':'other','eboot_sha256':'e'}))
            self.assertIsNone(p.profiles())
            (pgo/'profile.json').write_text(json.dumps({'sdk_commit':'s','eboot_sha256':'e'}))
            self.assertEqual(p.profiles(),pgo)
            self.assertTrue(p.build_identity()['pgo'])
    def test_profiles_renamed_for_this_sdk_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'pgo';src.mkdir()
            (src/'profile.json').write_text(json.dumps({'sdk_dir':'/home/a/ps3recomp'}))
            (src/'CMakeFiles#x.dir#home#a#ps3recomp#runtime#ppu#ppu_hle.cpp.gcda').write_bytes(b'1')
            (src/'CMakeFiles#x.dir#src#recomp#ppu_recomp_000.cpp.gcda').write_bytes(b'2')
            out=m.place_profiles(src,Path(tmp)/'work/pgo',Path('/tmp/.mount_X/usr/share/battride/ps3recomp'))
            self.assertEqual(sorted(f.name for f in out.glob('*.gcda')),
                sorted(['CMakeFiles#x.dir#src#recomp#ppu_recomp_000.cpp.gcda',
                        'CMakeFiles#x.dir#tmp#.mount_X#usr#share#battride#ps3recomp#runtime#ppu#ppu_hle.cpp.gcda']))
    @unittest.skipUnless(m.WINDOWS,'Windows profile names')
    def test_profiles_renamed_for_windows_sdk_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'pgo';src.mkdir()
            (src/'profile.json').write_text(json.dumps({'sdk_dir':'/ps3recomp'}))
            (src/'CMakeFiles#x.dir##ps3recomp#runtime#ppu#ppu_hle.cpp.gcda').write_bytes(b'1')
            out=m.place_profiles(src,Path(tmp)/'work/pgo',Path('C:\\pkg\\usr\\share\\battride\\ps3recomp'))
            self.assertEqual([f.name for f in out.glob('*.gcda')],
                ['CMakeFiles#x.dir#C_#pkg#usr#share#battride#ps3recomp#runtime#ppu#ppu_hle.cpp.gcda'])
    def test_display_env_accepts_only_known_values(self):
        self.assertEqual(m.display_env({'mode':'fullscreen','size':'2560x1440','filter':'linear'}),
            {'PORT_DISPLAY_MODE':'fullscreen','PORT_DISPLAY_SIZE':'2560x1440','PORT_DISPLAY_FILTER':'linear'})
        self.assertEqual(m.display_env({'mode':'evil; rm','size':'1920x1080 && x','filter':'?','scale':'8'}),{})
        self.assertEqual(m.display_env({'scale':'2'}),{'PORT_RENDER_SCALE':'2'})
        self.assertEqual(m.display_env({}),{})
    def test_controls_text(self):
        text=m.controls_text({})
        self.assertIn('square = Mouse Left, J\n',text)
        self.assertIn('camera_up = \n',text)
        self.assertIn('mouse_sensitivity = 1.00\n',text)
        text=m.controls_text({'bindings':{'square':['F','bad,key','x\ny'],'cross':[]},'mouse_sensitivity':99,'invert_y':True})
        self.assertIn('square = F\n',text)
        self.assertIn('cross = \n',text)
        self.assertIn('mouse_sensitivity = 10.00\n',text)
        self.assertIn('invert_y = 1\n',text)
        self.assertIn('pad_cross = A\n',text)
        self.assertIn('stick_deadzone = 0.15\n',text)
        self.assertEqual(len([l for l in text.splitlines() if ' = ' in l]),len(m.CONTROLS)+len(m.PAD_CONTROLS)+3)
        text=m.controls_text({'pad':{'pad_cross':'B','pad_circle':'evil\nx','pad_l2':''},'stick_deadzone':3})
        self.assertIn('pad_cross = B\n',text)
        self.assertIn('pad_circle = B\n',text)
        self.assertIn('pad_l2 = \n',text)
        self.assertIn('stick_deadzone = 0.50\n',text)
    def test_interrupted_install_cleared(self):
        with tempfile.TemporaryDirectory() as tmp:
            data=Path(tmp);system=data/'game/game/BLJS10220DATA/USRDIR/nativePS/system'
            self.assertFalse(m.clear_interrupted_install(data))
            system.mkdir(parents=True);(system/'BLJS10220DATA.start').write_text('OK')
            self.assertTrue(m.clear_interrupted_install(data))
            self.assertFalse((data/'game/game/BLJS10220DATA').exists())
            system.mkdir(parents=True);(system/'BLJS10220DATA.installed').write_text('OK')
            self.assertFalse(m.clear_interrupted_install(data))
            self.assertTrue(system.is_dir())
    def test_build_jobs_positive(self):
        self.assertGreaterEqual(m.build_jobs(),1)

if __name__=='__main__':unittest.main()
