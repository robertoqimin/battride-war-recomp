import importlib.util
import json
from pathlib import Path
import re
import subprocess
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
            (root/'usr/share/battride/port/port/io').mkdir(parents=True)
            (root/'usr/share/battride/port/port/io/input_bridge.c').write_text('x')
            self.assertIsNone(m.Package(root).built_runner(data))
    def make_package(self,root,version='1'):
        (root/'app').mkdir(parents=True,exist_ok=True)
        (root/'app/package.json').write_text(json.dumps({'version':version,'sdk_commit':'s','elf_sha256':'e','codecs':{}}))
        (root/'app/disc-manifest.json').write_text('{}')
        port=root/'usr/share/battride/port'
        for name in ('CMakeLists.txt','README.md','docs/APPIMAGE.md','port/core/main.cpp','include/ppu_vmx.h',
                     'packaging/launcher.py','packaging/importer.py','tests/test_importer.py','scripts/perf/pgo_train.py',
                     'scripts/build/generate.py'):
            (port/name).parent.mkdir(parents=True,exist_ok=True);(port/name).write_text(name)
        return port
    def test_identity_ignores_docs_tests_launcher_and_version(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);port=self.make_package(root)
            before=m.Package(root).build_identity()
            for name in ('README.md','docs/APPIMAGE.md','packaging/launcher.py','packaging/importer.py',
                         'tests/test_importer.py','scripts/perf/pgo_train.py'):
                (port/name).write_text('changed')
            (port/'docs/NEW.md').write_text('new')
            (root/'app/package.json').write_text(json.dumps({'version':'2','sdk_commit':'s','elf_sha256':'e','codecs':{}}))
            self.assertEqual(m.Package(root).build_identity(),before)
    def test_identity_follows_build_inputs_profiles_and_toolchain(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp);port=self.make_package(root)
            base=m.Package(root).build_identity()
            for name in ('CMakeLists.txt','port/core/main.cpp','include/ppu_vmx.h','scripts/build/generate.py'):
                old=(port/name).read_text();(port/name).write_text('changed')
                self.assertNotEqual(m.Package(root).build_identity()['sources'],base['sources'],name)
                (port/name).write_text(old)
            (port/'port/core/new.c').write_text('x')
            self.assertNotEqual(m.Package(root).build_identity()['sources'],base['sources'])
            (port/'port/core/new.c').unlink()
            self.assertEqual(m.Package(root).build_identity(),base)
            # Profiles: their content, not only whether they exist.
            pgo=root/'usr/share/battride/pgo';pgo.mkdir(parents=True)
            (pgo/'profile.json').write_text(json.dumps({'sdk_commit':'s','eboot_sha256':'e'}))
            (pgo/'a.gcda').write_bytes(b'1');first=m.Package(root).build_identity()['pgo']
            self.assertTrue(first)
            (pgo/'a.gcda').write_bytes(b'2')
            self.assertNotEqual(m.Package(root).build_identity()['pgo'],first)
            # Toolchain: the bundled package versions.
            self.assertIsNone(base['toolchain'])
            (root/'app/build-packages.txt').write_text('gcc-11 11.4.0-1\n');t1=m.Package(root).build_identity()['toolchain']
            (root/'app/build-packages.txt').write_text('gcc-11 11.4.0-2\n')
            self.assertNotEqual(m.Package(root).build_identity()['toolchain'],t1)
    @unittest.skipIf(m.WINDOWS,'Linux AppImage layout')
    def test_build_root_follows_the_mount(self):
        with tempfile.TemporaryDirectory() as tmp:
            def package(name):
                root=Path(tmp)/name;self.make_package(root)
                for d in ('usr/bin','usr/lib','usr/share'):(root/d).mkdir(parents=True,exist_ok=True)
                (root/'usr/toolchain/sysroot/usr/bin').mkdir(parents=True);(root/'usr/toolchain/sysroot/usr/bin/gcc').write_text('gcc')
                (root/'usr/toolchain/sysroot/bin').symlink_to('usr/bin')
                (root/'app/build-packages.txt').write_text('gcc 1\n')
                return root
            data=Path(tmp)/'data';data.mkdir()
            a=package('mountA');appdir=m.Package(a).build_root(data)
            self.assertEqual(appdir,data/'.appdir')
            # The toolchain is a real copy in the data folder (symlinks kept), the rest follows the mount.
            self.assertEqual((appdir/'usr/toolchain').resolve(),(data/'.toolchain').resolve())
            self.assertTrue((data/'.toolchain/sysroot/bin').is_symlink())
            self.assertEqual((appdir/'usr/bin').resolve(),(a/'usr/bin').resolve())
            stamp=data/'.toolchain/.package-toolchain';first=stamp.stat().st_mtime_ns
            b=package('mountB');m.Package(b).build_root(data)
            self.assertEqual((appdir/'usr/bin').resolve(),(b/'usr/bin').resolve())
            self.assertEqual(stamp.stat().st_mtime_ns,first)                   # same toolchain: not copied again
            (b/'app/build-packages.txt').write_text('gcc 2\n');(b/'usr/toolchain/sysroot/usr/bin/gcc').write_text('gcc 2')
            m.Package(b).build_root(data)
            self.assertEqual((data/'.toolchain/sysroot/usr/bin/gcc').read_text(),'gcc 2')
    def test_sync_tree_rewrites_only_changed_files(self):
        import os,time
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'src';dest=Path(tmp)/'dest'
            for name,text in (('a.c','a'),('dir/b.c','b'),('gone.c','x')):
                (src/name).parent.mkdir(parents=True,exist_ok=True);(src/name).write_text(text)
            m.sync_tree(src,dest)
            (dest/'src/recomp').mkdir(parents=True);(dest/'src/recomp/ppu.cpp').write_text('generated')
            old=time.time()-3600
            for name in ('a.c','dir/b.c'):
                os.utime(dest/name,(old,old))
            (src/'dir/b.c').write_text('b2');(src/'gone.c').unlink();(src/'new.c').write_text('n')
            m.sync_tree(src,dest,keep=('src',))
            self.assertAlmostEqual((dest/'a.c').stat().st_mtime,old,delta=1)      # same content: time kept
            self.assertGreater((dest/'dir/b.c').stat().st_mtime,old+60)            # changed: now
            self.assertEqual((dest/'dir/b.c').read_text(),'b2')
            self.assertFalse((dest/'gone.c').exists())
            self.assertEqual((dest/'new.c').read_text(),'n')
            self.assertEqual((dest/'src/recomp/ppu.cpp').read_text(),'generated')  # kept
    def test_build_inputs_cover_what_the_build_reads(self):
        # Every repository path CMakeLists.txt names, every one generate.py and
        # lift_audio_spu.py open below the port root, and every one build_runner()
        # passes from its copy of the tree must count for the identity.
        # Generated folders and the SDK are covered by the EBOOT and SDK entries.
        repo=Path(__file__).resolve().parents[1]
        generated=('src/','spu/','analysis/','game/','firmware/','tests/')
        names=set()
        text=(repo/'CMakeLists.txt').read_text()
        names.update(re.findall(r'\$\{CMAKE_CURRENT_SOURCE_DIR\}/([A-Za-z0-9_][A-Za-z0-9_./-]*)',text))
        for script in ('scripts/build/generate.py','scripts/build/lift_audio_spu.py'):
            names.update(re.findall(r"ROOT\s*/\s*'([^']+)'",(repo/script).read_text()))
        importer=(repo/'packaging/importer.py').read_text()
        build_runner=importer[importer.index('def build_runner'):importer.index('\n    def ',importer.index('def build_runner')+1)]
        names.update(re.findall(r"\bport\s*/\s*'([^']+)'",build_runner))
        checked=0
        for name in sorted(names):
            name=name.rstrip('/')
            if (name+'/').startswith(generated) or name.startswith('build') or not (repo/name).exists():
                continue
            paths=[name] if (repo/name).is_file() else [p.relative_to(repo).as_posix() for p in (repo/name).rglob('*') if p.is_file()]
            for p in paths:
                self.assertTrue(m.is_build_input(p),'%s is read by the build but not in BUILD_INPUTS'%p)
                checked+=1
        self.assertGreater(checked,10)
        # Every public file the build reads must ship, or the player's build fails.
        public=set(l.strip() for l in (repo/'PUBLIC_FILES.txt').read_text().splitlines() if l.strip() and not l.startswith('#'))
        tracked=subprocess.run(['git','-C',str(repo),'ls-files'],capture_output=True,text=True)
        if tracked.returncode:
            self.skipTest('not a git checkout')
        for p in tracked.stdout.split():
            if m.is_build_input(p):
                self.assertIn(p,public,'%s is a build input but does not ship'%p)
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
    def test_gpu_chosen_by_name(self):
        probe=('GPU: AMD Radeon(TM) Graphics (Vulkan 1.3.260), BC textures: yes\r\n'
               'GPU: NVIDIA GeForce GTX 1650 (Vulkan 1.4.357), BC textures: yes\r\n'
               'Vulkan window/device check passed.\r\n')
        names=m.gpu_names(probe)
        self.assertEqual(names,['AMD Radeon(TM) Graphics','NVIDIA GeForce GTX 1650'])
        self.assertEqual(m.gpu_env('NVIDIA GeForce GTX 1650',names),{'PS3RECOMP_VK_DEVICE':'1'})
        self.assertEqual(m.gpu_env('',names),{})
        self.assertEqual(m.gpu_env('Old GPU',names),{})
    def test_software_renderer_left_out_of_the_list(self):
        names=['AMD Radeon(TM) Graphics (RADV RENOIR)','NVIDIA GeForce GTX 1650',
               'llvmpipe (LLVM 19.1.7, 256 bits)']
        self.assertEqual(m.hardware_gpus(names),names[:2])
        self.assertEqual(m.hardware_gpus(['SwiftShader Device (Subzero)']),[])
        # The game's device numbers still count the software renderer.
        self.assertEqual(m.gpu_env('NVIDIA GeForce GTX 1650',['llvmpipe (LLVM 19)','NVIDIA GeForce GTX 1650']),
                         {'PS3RECOMP_VK_DEVICE':'1'})
    def test_display_env_accepts_only_known_values(self):
        self.assertEqual(m.display_env({'mode':'fullscreen','size':'2560x1440','filter':'linear'}),
            {'PORT_DISPLAY_MODE':'fullscreen','PORT_DISPLAY_SIZE':'2560x1440','PORT_DISPLAY_FILTER':'linear'})
        self.assertEqual(m.display_env({'mode':'evil; rm','size':'1920x1080 && x','filter':'?','scale':'8'}),{})
        self.assertEqual(m.display_env({'scale':'2'}),{'PORT_RENDER_SCALE':'2'})
        self.assertEqual(m.display_env({'aniso':'16'}),{'PORT_ANISO':'16'})
        self.assertEqual(m.display_env({'aniso':'3'}),{})
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
