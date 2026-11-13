#!/usr/bin/env python3
"""Desktop launcher: imports the user's disc and builds the game on this computer."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

from importer import (CONTROLS, DISPLAY_FILTERS, DISPLAY_MODES, EXE, PAD_BUTTONS, PAD_CONTROLS, RENDER_SCALES,
                      STICK_DEADZONE, WINDOWS, ImportError, Package, Runner, binding_ok,
                      clear_interrupted_install)

ROOT = Path(os.environ.get('PORT_APPDIR', Path(__file__).resolve().parents[1]))
# The log files of one launch, in the order they are written.
LOG_FILES = ('import.log', 'build.log', 'graphics.log', 'game.log')
LOG_LIMIT = 2_000_000           # characters kept in the Log page


def cpu_check():
    if WINDOWS:
        import ctypes
        present = ctypes.windll.kernel32.IsProcessorFeaturePresent
        # PF_SSSE3_INSTRUCTIONS_AVAILABLE, PF_SSE4_1_INSTRUCTIONS_AVAILABLE
        missing = [name for name, feature in (('ssse3', 36), ('sse4_1', 37)) if not present(feature)]
        if missing:
            raise ImportError('This game needs an x86_64 CPU with SSE4.1. Missing: ' + ', '.join(missing))
        return
    flags = set()
    try:
        for line in Path('/proc/cpuinfo').read_text().splitlines():
            if line.startswith('flags'):
                flags = set(line.split(':', 1)[1].split())
                break
    except OSError:
        pass
    # The game is compiled for the local CPU; the SPU code needs SSE4.1.
    missing = {'ssse3', 'sse4_1'} - flags
    if missing:
        raise ImportError('This game needs an x86_64 CPU with SSE4.1. Missing: ' + ', '.join(sorted(missing)))


def host_env():
    """The environment for host programs (file manager): without this AppImage's Python and GTK."""
    env = dict(os.environ)
    for k in ('PYTHONHOME', 'PYTHONPATH', 'PYTHONPLATLIBDIR', 'PYTHONDONTWRITEBYTECODE', 'GI_TYPELIB_PATH',
              'GSETTINGS_SCHEMA_DIR', 'GSETTINGS_BACKEND', 'GIO_USE_VFS', 'GIO_MODULE_DIR',
              'GDK_PIXBUF_MODULE_FILE', 'PORT_APPDIR'):
        env.pop(k, None)
    appdir = os.environ.get('PORT_APPDIR')
    if appdir:
        env['PATH'] = os.pathsep.join(p for p in env.get('PATH', '').split(os.pathsep)
                                      if not p.startswith(appdir))
    return env


def gamemode_command():
    """The host's gamemoderun (Feral GameMode), or None when it is not installed."""
    import shutil
    return shutil.which('gamemoderun', path=host_env().get('PATH'))


def open_folder(path):
    try:
        if WINDOWS:
            os.startfile(str(path))
        else:
            subprocess.Popen(['xdg-open', str(path)], env=host_env(), start_new_session=True,
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass


def default_dirs():
    """(data folder, settings file) for this system."""
    if WINDOWS:
        local = Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData/Local'))
        roaming = Path(os.environ.get('APPDATA', Path.home() / 'AppData/Roaming'))
        return local / 'battride-war-port', roaming / 'battride-war-port/settings.json'
    return (Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local/share')) / 'battride-war-port',
            Path(os.environ.get('XDG_CONFIG_HOME', Path.home() / '.config')) / 'battride-war-port/settings.json')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--import-only', type=Path, metavar='ISO')
    ap.add_argument('--data-dir', type=Path)
    ap.add_argument('--rpcs3', type=Path, metavar='APPIMAGE',
                    help='use this copy of the pinned RPCS3 AppImage instead of downloading it')
    ap.add_argument('--screenshot', type=Path, help=argparse.SUPPRESS)
    ap.add_argument('--page', default='game', help=argparse.SUPPRESS)
    args = ap.parse_args()
    package = Package(ROOT)
    if args.check:
        cpu_check()
        for tool in ('7z', 'ffmpeg', 'ffprobe'):
            if not (ROOT / 'usr/bin' / (tool + EXE)).is_file():
                raise ImportError('Missing bundled tool: ' + tool)
        code, formats = Runner().run(package.tool('7z', 'i'), capture=True)
        if code or 'Codec Load Error' in formats or 'Udf' not in formats:
            raise ImportError('Bundled ISO reader is missing UDF format support.')
        env = package.build_env(Path.home())
        compilers = {}
        if WINDOWS:
            tools = package.windows_toolchain()[0]
            build_tools = [(name, tools / (name + '.exe')) for name in ('clang', 'g++', 'cmake', 'ninja')]
        else:
            build_tools = [(name, ROOT / 'usr/toolchain/sysroot' / tool) for name, tool in (
                ('c', 'usr/lib/llvm-14/bin/clang'), ('cxx', 'usr/bin/x86_64-linux-gnu-g++-11'),
                ('cmake', 'usr/bin/cmake'), ('ninja', 'usr/bin/ninja'))]
        for name, tool in build_tools:
            code, text = Runner().run([tool, '--version'], env=env, capture=True)
            if code:
                raise ImportError('Bundled build tool does not start: ' + name + '\n' + text[-800:])
            compilers[name] = text.splitlines()[0] if text else ''
        print(json.dumps({'status': 'OK', 'title_id': 'BLJS10220', 'files': len(package.manifest),
                          'cpu': 'built locally with -march=native (SSE4.1 minimum)', 'toolchain': compilers,
                          'elf_sha256': package.meta['elf_sha256']}, indent=2))
        return
    default_data, config_path = default_dirs()
    data = (args.data_dir or default_data).resolve()
    if args.import_only:
        data.mkdir(parents=True, exist_ok=True)
        runner = Runner()
        with (data / 'import.log').open('wb') as log:
            package.import_iso(args.import_only, data, runner, print, log, args.rpcs3)
        print('Import verified:', data)
        with (data / 'build.log').open('wb') as log:
            print('Game built:', package.build_runner(data, runner, print, log))
        return
    # The AppImage's SVG icon loader, at the path this AppImage is mounted on.
    loaders = ROOT / 'usr/lib/gdk-pixbuf'
    if (loaders / 'loaders.cache.in').is_file() and 'GDK_PIXBUF_MODULE_FILE' not in os.environ:
        import atexit
        import tempfile
        fd, cache = tempfile.mkstemp(prefix='battride-pixbuf-', suffix='.cache',
                                     dir=os.environ.get('XDG_RUNTIME_DIR') or None)
        with os.fdopen(fd, 'w') as f:
            f.write((loaders / 'loaders.cache.in').read_text().replace('@LOADERS@', str(loaders)))
        os.environ['GDK_PIXBUF_MODULE_FILE'] = cache
        atexit.register(lambda: os.path.exists(cache) and os.unlink(cache))
    # GTK 4.6 and libadwaita 1.1 (Ubuntu 22.04, the AppImage's floor): nothing newer is used.
    import gi
    gi.require_version('Gtk', '4.0')
    gi.require_version('Adw', '1')
    from gi.repository import Adw, Gdk, Gio, GLib, Gtk, Pango

    try:
        saved = json.loads(config_path.read_text())
    except (OSError, ValueError):
        saved = {}
    if not isinstance(saved, dict):
        saved = {}
    if not args.data_dir and saved.get('data'):
        data = Path(saved['data'])

    def display_sizes():
        """Common 16:9 sizes that fit the largest connected monitor (in device pixels)."""
        largest = (1920, 1080)
        display = Gdk.Display.get_default()
        monitors = display.get_monitors() if display else None
        for i in range(monitors.get_n_items() if monitors else 0):
            monitor = monitors.get_item(i)
            g, scale = monitor.get_geometry(), monitor.get_scale_factor()
            if g.width * g.height * scale * scale > largest[0] * largest[1]:
                largest = (g.width * scale, g.height * scale)
        sizes = [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3200, 1800), (3840, 2160)]
        return [('%dx%d' % (w, h), '%d × %d' % (w, h)) for w, h in sizes
                if (w, h) == (1280, 720) or (w <= largest[0] and h <= largest[1])]

    # GDK key names that differ from SDL's (input_bridge.c resolves SDL names).
    SDL_KEYS = {'space': 'Space', 'Return': 'Return', 'KP_Enter': 'Keypad Enter', 'BackSpace': 'Backspace',
                'Escape': 'Escape', 'Tab': 'Tab', 'ISO_Left_Tab': 'Tab', 'Shift_L': 'Left Shift',
                'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl', 'Control_R': 'Right Ctrl',
                'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt', 'Super_L': 'Left GUI', 'Super_R': 'Right GUI',
                'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Insert': 'Insert',
                'Delete': 'Delete', 'Home': 'Home', 'End': 'End', 'Page_Up': 'PageUp', 'Page_Down': 'PageDown',
                'Caps_Lock': 'CapsLock', 'KP_Add': 'Keypad +', 'KP_Subtract': 'Keypad -',
                'KP_Multiply': 'Keypad *', 'KP_Divide': 'Keypad /', 'KP_Decimal': 'Keypad .'}
    SDL_MOUSE = {1: 'Mouse Left', 2: 'Mouse Middle', 3: 'Mouse Right', 8: 'Mouse X1', 9: 'Mouse X2'}

    def sdl_key_name(keyval, keycode, group):
        """The SDL name of the key pressed, ignoring Shift and other modifiers; None if unusable."""
        found, plain, *_ = Gdk.Display.get_default().translate_key(keycode, 0, group)
        if found:
            keyval = plain
        name = Gdk.keyval_name(keyval) or ''
        if name in SDL_KEYS:
            return SDL_KEYS[name]
        if name.startswith('KP_') and name[3:].isdigit():
            return 'Keypad ' + name[3:]
        if len(name) <= 3 and name[:1] == 'F' and name[1:].isdigit():
            return name
        code = Gdk.keyval_to_unicode(keyval)
        char = chr(code).upper() if code else ''
        return char if char.isprintable() and binding_ok(char) and char != ' ' else None

    def flat_button(icon, tooltip, handler):
        button = Gtk.Button(icon_name=icon, tooltip_text=tooltip, valign=Gtk.Align.CENTER)
        button.add_css_class('flat')
        button.connect('clicked', handler)
        return button

    def combo_row(title, subtitle, choices, current, default):
        """An Adw.ComboRow over (id, label) choices; row.value() is the chosen id."""
        row = Adw.ComboRow(title=title, model=Gtk.StringList.new([label for _, label in choices]))
        if subtitle:
            row.set_subtitle(subtitle)
        ids = [key for key, _ in choices]
        row.set_selected(ids.index(current) if current in ids else ids.index(default) if default in ids else 0)
        row.value = lambda: ids[row.get_selected()]
        return row

    def switch_row(title, subtitle, active):
        row = Adw.ActionRow(title=title, subtitle=subtitle or '')
        switch = Gtk.Switch(active=bool(active), valign=Gtk.Align.CENTER)
        row.add_suffix(switch)
        row.set_activatable_widget(switch)
        row.switch = switch
        return row

    class Window:
        def __init__(self, app):
            self.closed = False
            self.runner = None
            self.busy = False
            self.playing = False
            self.log_dir = None
            self.log_offsets = {}
            self.log_current = None
            self.controls = saved.get('controls', {}) if isinstance(saved.get('controls'), dict) else {}
            self.win = Adw.ApplicationWindow(application=app, title='Battride War Recomp')
            self.win.set_default_size(880, 780)
            self.win.connect('close-request', self.close)

            header = Adw.HeaderBar()
            self.stack = Adw.ViewStack()
            title = Adw.ViewSwitcherTitle(stack=self.stack, title='Battride War Recomp')
            header.set_title_widget(title)
            self.launch = Gtk.Button()
            self.launch.connect('clicked', self.on_launch)
            header.pack_end(self.launch)
            header.pack_start(flat_button('help-about-symbolic', 'Credits & licenses', self.show_credits))
            bottom = Adw.ViewSwitcherBar(stack=self.stack)
            title.connect('notify::title-visible', lambda t, _: bottom.set_reveal(t.get_title_visible()))

            self.stack.add_titled(self.build_game_page(), 'game', 'Game').set_icon_name('applications-games-symbolic')
            self.stack.add_titled(self.build_settings_page(), 'settings', 'Settings').set_icon_name(
                'preferences-system-symbolic')
            self.stack.add_titled(self.build_controls_page(), 'controls', 'Keyboard').set_icon_name(
                'input-keyboard-symbolic')
            self.stack.add_titled(self.build_pad_page(), 'controller', 'Controller').set_icon_name(
                'input-gaming-symbolic')
            self.stack.add_titled(self.build_log_page(), 'log', 'Log').set_icon_name('utilities-terminal-symbolic')

            # Progress and the latest message stay visible on every page.
            status = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, margin_start=18, margin_end=18,
                             margin_top=10, margin_bottom=12)
            self.progress = Gtk.ProgressBar(visible=False)
            self.status = Gtk.Label(xalign=0, wrap=True, selectable=True,
                                    label='Ready. Requires a Vulkan GPU, an SSE4.1 CPU, about 8 GB of RAM '
                                          'and 8 GB of free space.')
            self.stats = Gtk.Label(xalign=0, visible=False)
            self.stats.add_css_class('dim-label')
            self.stats.add_css_class('numeric')
            for widget in (self.progress, self.status, self.stats):
                status.append(widget)

            self.toasts = Adw.ToastOverlay(child=self.stack, vexpand=True)
            content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            for widget in (header, self.toasts, Gtk.Separator(), status, bottom):
                content.append(widget)
            self.win.set_content(content)
            self.stack.set_visible_child_name(args.page)
            self.update_launch()
            GLib.timeout_add(1000, self.tick)
            GLib.timeout_add(300, self.follow_logs)
            self.win.present()

        # --- Game page ---------------------------------------------------------------

        def build_game_page(self):
            page = Adw.PreferencesPage()
            about = Adw.PreferencesGroup(
                title='Kamen Rider: Battride War',
                description=('Experimental Windows port' if WINDOWS else 'Experimental Linux port') +
                            '  •  Japan / BLJS10220\n\n'
                            'Select your game ISO and press Play. The first launch downloads the official RPCS3 '
                            'build to read your disc, imports its files and builds the game for this computer '
                            '(15-60 minutes, once). Data, saves and logs stay in the data folder.')
            page.add(about)
            game = Adw.PreferencesGroup(title='Game')
            self.iso_path = saved.get('iso') if saved.get('iso') and Path(saved['iso']).is_file() else None
            self.iso_row = Adw.ActionRow(title='Game ISO')
            self.iso_row.add_suffix(flat_button('document-open-symbolic', 'Select the game ISO', self.choose_iso))
            game.add(self.iso_row)
            self.data_path = data
            self.data_row = Adw.ActionRow(title='Data folder')
            self.data_row.add_suffix(flat_button('folder-open-symbolic', 'Choose the data folder', self.choose_data))
            self.data_row.add_suffix(flat_button('system-file-manager-symbolic', 'Open in the file manager',
                                                 lambda _: open_folder(self.data_path)))
            game.add(self.data_row)
            self.refresh_paths()
            page.add(game)
            keys = Adw.PreferencesGroup(
                title='Playing',
                description='Controller, or keyboard and mouse: click the game window so the mouse turns the '
                            'camera, Alt releases it. Alt+Enter or F11 switches window and fullscreen, Esc pauses. '
                            'Keys and controller buttons are set in the Keyboard and Controller pages.')
            page.add(keys)
            self.lockable = [game]
            return page

        def refresh_paths(self):
            self.iso_row.set_subtitle(GLib.markup_escape_text(self.iso_path or 'Not selected'))
            self.data_row.set_subtitle(GLib.markup_escape_text(str(self.data_path)))

        def choose_iso(self, _):
            dialog = Gtk.FileChooserNative(title='Select the game ISO', transient_for=self.win,
                                           action=Gtk.FileChooserAction.OPEN, modal=True)
            filt = Gtk.FileFilter()
            filt.set_name('Disc image (*.iso)')
            filt.add_pattern('*.iso')
            filt.add_pattern('*.ISO')
            dialog.add_filter(filt)
            if self.iso_path:
                dialog.set_current_folder(Gio.File.new_for_path(str(Path(self.iso_path).parent)))

            def done(d, response):
                if response == Gtk.ResponseType.ACCEPT and d.get_file() and d.get_file().get_path():
                    self.iso_path = d.get_file().get_path()
                    self.save_settings({'iso': self.iso_path})
                    self.refresh_paths()
                d.destroy()
            dialog.connect('response', done)
            self.dialog = dialog            # FileChooserNative needs a live reference
            dialog.show()

        def choose_data(self, _):
            dialog = Gtk.FileChooserNative(title='Choose the data folder', transient_for=self.win,
                                           action=Gtk.FileChooserAction.SELECT_FOLDER, modal=True)
            if self.data_path.exists():
                dialog.set_current_folder(Gio.File.new_for_path(str(self.data_path)))

            def done(d, response):
                if response == Gtk.ResponseType.ACCEPT and d.get_file() and d.get_file().get_path():
                    self.data_path = Path(d.get_file().get_path())
                    self.save_settings({'data': str(self.data_path)})
                    self.refresh_paths()
                    self.update_launch()
                d.destroy()
            dialog.connect('response', done)
            self.dialog = dialog
            dialog.show()

        # --- Settings page -----------------------------------------------------------

        def build_settings_page(self):
            page = Adw.PreferencesPage()
            display = saved.get('display', {}) if isinstance(saved.get('display'), dict) else {}
            screen = Adw.PreferencesGroup(title='Display',
                                          description='The picture keeps 16:9, with black bars on other shapes.')
            self.mode = combo_row('Mode', None, [('windowed', 'Windowed'), ('borderless', 'Borderless fullscreen'),
                                                 ('fullscreen', 'Fullscreen')], display.get('mode'), 'windowed')
            self.size = combo_row('Resolution', 'Window or fullscreen size; borderless uses the desktop size',
                                  display_sizes(), display.get('size'), '1600x900')
            self.filter = combo_row('Upscaling', 'How the picture is enlarged to the window',
                                    [('fsr', 'AMD FSR 1'), ('linear', 'Bilinear')], display.get('filter'), 'fsr')
            for row in (self.mode, self.size, self.filter):
                screen.add(row)
            page.add(screen)
            render = Adw.PreferencesGroup(title='Rendering')
            self.scale = combo_row('Internal resolution', 'Higher renders sharper and needs a faster GPU',
                                   [('1', 'Native (1280 × 720)'), ('2', '2× (2560 × 1440)'),
                                    ('3', '3× (3840 × 2160)'), ('4', '4× (5120 × 2880)')],
                                   str(display.get('scale')), '1')
            render.add(self.scale)
            page.add(render)
            perf = Adw.PreferencesGroup(title='Performance')
            self.hud = switch_row('MangoHud overlay', 'Frame rate and frame times, if MangoHud is installed',
                                  saved.get('mangohud'))
            perf.add(self.hud)
            self.gamemode_path = gamemode_command()
            self.gamemode = switch_row('GameMode', 'Raises CPU clocks and priority while playing (Feral GameMode)'
                                       if self.gamemode_path else 'Install the gamemode package to use it',
                                       saved.get('gamemode', True) and self.gamemode_path)
            self.gamemode.set_sensitive(bool(self.gamemode_path))
            perf.add(self.gamemode)
            if not WINDOWS:                  # MangoHud and GameMode are Linux tools
                page.add(perf)
            for row in (self.mode, self.size, self.filter, self.scale):
                row.connect('notify::selected', lambda *_: self.settings_changed())
            self.hud.switch.connect('notify::active', lambda *_: self.settings_changed())
            self.gamemode.switch.connect('notify::active', lambda *_: self.settings_changed())
            self.size.set_sensitive(self.mode.value() != 'borderless')
            self.lockable += [screen, render, perf]
            return page

        def display_settings(self):
            return {'mode': self.mode.value(), 'size': self.size.value(), 'filter': self.filter.value(),
                    'scale': self.scale.value()}

        def settings_changed(self):
            self.size.set_sensitive(self.mode.value() != 'borderless')
            self.save_settings({'display': self.display_settings(), 'mangohud': self.hud.switch.get_active(),
                                'gamemode': self.gamemode.switch.get_active()})

        # --- Keyboard page -----------------------------------------------------------

        def build_controls_page(self):
            page = Adw.PreferencesPage()
            stored = self.controls.get('bindings', {}) if isinstance(self.controls.get('bindings'), dict) else {}
            self.defaults = {name: [k.strip() for k in default.split(',') if k] for name, _, default in CONTROLS}
            self.bindings = {name: list(stored[name]) if isinstance(stored.get(name), list) else
                             list(self.defaults[name]) for name, _, _ in CONTROLS}
            self.labels = {name: label for name, label, _ in CONTROLS}
            self.binding_rows = {}
            mouse = Adw.PreferencesGroup(title='Mouse camera',
                                         description='Click the game window to let the mouse turn the camera; '
                                                     'Alt releases it. A controller keeps working alongside. '
                                                     'Changes apply the next time the game starts.')
            reset = Gtk.Button(label='Reset to defaults', valign=Gtk.Align.CENTER)
            reset.add_css_class('flat')
            reset.connect('clicked', self.reset_controls)
            mouse.set_header_suffix(reset)
            sens_row = Adw.ActionRow(title='Sensitivity')
            self.sens = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.1, 5.0, 0.1)
            self.sens.set_hexpand(True)
            self.sens.set_size_request(260, -1)
            self.sens.set_draw_value(True)
            self.sens.set_digits(1)
            self.sens.set_value_pos(Gtk.PositionType.LEFT)
            self.sens.add_mark(1.0, Gtk.PositionType.BOTTOM, None)
            try:
                self.sens.set_value(float(self.controls.get('mouse_sensitivity', 1.0)))
            except (TypeError, ValueError):
                self.sens.set_value(1.0)
            sens_row.add_suffix(self.sens)
            mouse.add(sens_row)
            self.invert = switch_row('Invert up and down', None, self.controls.get('invert_y'))
            mouse.add(self.invert)
            page.add(mouse)
            self.sens.connect('value-changed', lambda *_: self.controls_changed())
            self.invert.switch.connect('notify::active', lambda *_: self.controls_changed())
            groups = (('Movement and camera keys', ('move_', 'camera_')), ('Buttons', ('',)))
            placed = set()
            for title, prefixes in groups:
                group = Adw.PreferencesGroup(title=title)
                for name, label, _ in CONTROLS:
                    if name in placed or not name.startswith(prefixes):
                        continue
                    placed.add(name)
                    row = Adw.ActionRow(title=label)
                    row.add_suffix(flat_button('document-edit-symbolic', 'Replace with a new key or mouse button',
                                               lambda _, n=name: self.capture(n, True)))
                    row.add_suffix(flat_button('list-add-symbolic', 'Add another key',
                                               lambda _, n=name: self.capture(n, False)))
                    row.add_suffix(flat_button('edit-clear-symbolic', 'Clear',
                                               lambda _, n=name: self.set_binding(n, [])))
                    self.binding_rows[name] = row
                    self.refresh_binding(name)
                    group.add(row)
                page.add(group)
            self.lockable.append(page)
            return page

        def refresh_binding(self, name):
            self.binding_rows[name].set_subtitle(GLib.markup_escape_text(', '.join(self.bindings[name]) or
                                                                         'Not set'))

        def set_binding(self, name, keys):
            self.bindings[name] = keys
            self.refresh_binding(name)
            self.controls_changed()

        def controls_changed(self):
            if not hasattr(self, 'deadzone'):
                return                       # the Controller page is still being built
            self.controls = {'bindings': self.bindings, 'mouse_sensitivity': round(self.sens.get_value(), 2),
                             'invert_y': self.invert.switch.get_active(),
                             'pad': {name: row.value() for name, row in self.pad_rows.items()},
                             'stick_deadzone': round(self.deadzone.get_value(), 2)}
            self.save_settings({})

        # --- Controller page ---------------------------------------------------------

        def build_pad_page(self):
            page = Adw.PreferencesPage()
            stored = self.controls.get('pad') if isinstance(self.controls.get('pad'), dict) else {}
            sticks = Adw.PreferencesGroup(
                title='Controller',
                description='Xbox, PlayStation, Switch and most other controllers work through SDL; the first '
                            'one connected is player 1. Buttons follow their position, so the bottom face button '
                            'is Cross on every layout. If a controller is not recognised, the Log page names it. '
                            'Changes apply the next time the game starts.')
            reset = Gtk.Button(label='Reset to defaults', valign=Gtk.Align.CENTER)
            reset.add_css_class('flat')
            reset.connect('clicked', self.reset_pad)
            sticks.set_header_suffix(reset)
            row = Adw.ActionRow(title='Stick dead zone',
                                subtitle='Raise it if the camera or character drifts with the sticks released')
            self.deadzone = Gtk.Scale.new_with_range(Gtk.Orientation.HORIZONTAL, 0.0, 0.5, 0.01)
            self.deadzone.set_hexpand(True)
            self.deadzone.set_size_request(220, -1)
            self.deadzone.set_draw_value(True)
            self.deadzone.set_digits(2)
            self.deadzone.set_value_pos(Gtk.PositionType.LEFT)
            self.deadzone.add_mark(STICK_DEADZONE, Gtk.PositionType.BOTTOM, None)
            try:
                self.deadzone.set_value(float(self.controls.get('stick_deadzone', STICK_DEADZONE)))
            except (TypeError, ValueError):
                self.deadzone.set_value(STICK_DEADZONE)
            row.add_suffix(self.deadzone)
            sticks.add(row)
            page.add(sticks)
            buttons = Adw.PreferencesGroup(title='Buttons', description='The controller button each PS3 button '
                                                                        'reads. The sticks stay as they are.')
            self.pad_rows = {}
            for name, label, default in PAD_CONTROLS:
                current = stored.get(name, default)
                row = combo_row(label, None, PAD_BUTTONS, current, default)
                row.connect('notify::selected', lambda *_: self.controls_changed())
                self.pad_rows[name] = row
                buttons.add(row)
            page.add(buttons)
            self.deadzone.connect('value-changed', lambda *_: self.controls_changed())
            self.lockable.append(page)
            return page

        def reset_pad(self, _):
            ids = [key for key, _ in PAD_BUTTONS]
            for name, _, default in PAD_CONTROLS:
                self.pad_rows[name].set_selected(ids.index(default))
            self.deadzone.set_value(STICK_DEADZONE)
            self.controls_changed()
            self.toast('Controller reset to the defaults')

        def reset_controls(self, _):
            for name in self.bindings:
                self.bindings[name] = list(self.defaults[name])
                self.refresh_binding(name)
            self.sens.set_value(1.0)
            self.invert.switch.set_active(False)
            self.controls_changed()
            self.toast('Controls reset to the defaults')

        def capture(self, name, replace):
            cap = Gtk.Window(title='Press a key', transient_for=self.win, modal=True, resizable=False,
                             default_width=460)
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12, margin_start=18, margin_end=18,
                          margin_top=18, margin_bottom=18)
            area = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6, height_request=140)
            area.add_css_class('card')
            heading = Gtk.Label(label=self.labels[name], vexpand=True, valign=Gtk.Align.END)
            heading.add_css_class('title-3')
            hint = Gtk.Label(label='Press a key, or click here with a mouse button', vexpand=True,
                             valign=Gtk.Align.START)
            hint.add_css_class('dim-label')
            area.append(heading)
            area.append(hint)
            cancel = Gtk.Button(label='Cancel', halign=Gtk.Align.END)
            cancel.connect('clicked', lambda _: cap.destroy())
            box.append(area)
            box.append(cancel)
            cap.set_child(box)

            def chosen(key):
                keys = self.bindings[name]
                if replace:
                    keys = [key]
                elif key not in keys and len(keys) < 6:
                    keys = keys + [key]
                self.set_binding(name, keys)
                cap.destroy()

            def on_key(ctrl, keyval, keycode, _state):
                key = sdl_key_name(keyval, keycode, ctrl.get_group())
                if key:
                    chosen(key)
                return True                  # Esc is a key too: it can be bound

            def on_click(gesture, *_):
                button = gesture.get_current_button()
                if button in SDL_MOUSE:
                    chosen(SDL_MOUSE[button])

            keys = Gtk.EventControllerKey()
            keys.set_propagation_phase(Gtk.PropagationPhase.CAPTURE)
            keys.connect('key-pressed', on_key)
            cap.add_controller(keys)
            click = Gtk.GestureClick(button=0)
            click.connect('pressed', on_click)
            area.add_controller(click)
            cap.present()

        # --- Log page ----------------------------------------------------------------

        def build_log_page(self):
            box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
            self.log_view = Gtk.TextView(editable=False, monospace=True, cursor_visible=False,
                                         wrap_mode=Gtk.WrapMode.WORD_CHAR, top_margin=10, bottom_margin=10,
                                         left_margin=12, right_margin=12)
            self.log_scroll = Gtk.ScrolledWindow(vexpand=True, child=self.log_view)
            box.append(self.log_scroll)
            bar = Gtk.ActionBar()
            self.log_place = Gtk.Label(xalign=0, ellipsize=Pango.EllipsizeMode.MIDDLE)
            self.log_place.add_css_class('dim-label')
            bar.pack_start(self.log_place)
            self.log_folder = Gtk.Button(label='Open logs folder')
            self.log_folder.connect('clicked', lambda _: open_folder(self.log_dir or self.data_path / 'logs'))
            self.log_copy = Gtk.Button(label='Copy')
            self.log_copy.connect('clicked', self.copy_log)
            self.log_save = Gtk.Button(label='Save as…')
            self.log_save.connect('clicked', self.save_log)
            for button in (self.log_save, self.log_copy, self.log_folder):
                bar.pack_end(button)
            box.append(bar)
            self.log_view.get_buffer().connect('changed', lambda b: self.log_buttons())
            self.log_buttons()
            return box

        def log_text(self):
            buffer = self.log_view.get_buffer()
            return buffer.get_text(buffer.get_start_iter(), buffer.get_end_iter(), False)

        def log_buttons(self):
            has = self.log_view.get_buffer().get_char_count() > 0
            self.log_copy.set_sensitive(has)
            self.log_save.set_sensitive(has)
            self.log_place.set_text(str(self.log_dir) if self.log_dir else 'Import, build and game output '
                                                                           'appear here while they run')

        def append_log(self, text):
            buffer = self.log_view.get_buffer()
            adj = self.log_scroll.get_vadjustment()
            at_end = adj.get_value() >= adj.get_upper() - adj.get_page_size() - 40
            buffer.insert(buffer.get_end_iter(), text)
            extra = buffer.get_char_count() - LOG_LIMIT
            if extra > 0:
                start = buffer.get_start_iter()
                cut = buffer.get_iter_at_offset(extra)
                cut.forward_line()
                buffer.delete(start, cut)
            if at_end:
                GLib.idle_add(lambda: adj.set_value(adj.get_upper() - adj.get_page_size()) and False)

        def follow_logs(self):
            """Appends what the running launch wrote to its log files since the last look."""
            if self.closed:
                return False
            if not self.log_dir:
                return True
            for name in LOG_FILES:
                path = self.log_dir / name
                offset = self.log_offsets.get(name, 0)
                try:
                    with path.open('rb') as f:
                        f.seek(offset)
                        chunk = f.read(512 * 1024)
                except OSError:
                    continue
                if not chunk:
                    continue
                # Only whole lines; a partial last line waits for the next look.
                end = chunk.rfind(b'\n') + 1 if len(chunk) < 512 * 1024 else len(chunk)
                if not end:
                    continue
                self.log_offsets[name] = offset + end
                if self.log_current != name:
                    self.log_current = name
                    self.append_log('\n── %s ──\n' % name)
                self.append_log(chunk[:end].decode('utf-8', errors='replace'))
            return True

        def copy_log(self, _):
            self.win.get_clipboard().set(self.log_text())
            self.toast('Log copied')

        def save_log(self, _):
            dialog = Gtk.FileChooserNative(title='Save the log', transient_for=self.win,
                                           action=Gtk.FileChooserAction.SAVE, modal=True)
            dialog.set_current_name('battride-%s.log' % time.strftime('%Y%m%d-%H%M%S'))
            text = self.log_text()

            def done(d, response):
                if response == Gtk.ResponseType.ACCEPT and d.get_file() and d.get_file().get_path():
                    try:
                        Path(d.get_file().get_path()).write_text(text)
                        self.toast('Log saved')
                    except OSError as e:
                        self.toast('Could not save the log: ' + str(e))
                d.destroy()
            dialog.connect('response', done)
            self.dialog = dialog
            dialog.show()

        # --- Launch ------------------------------------------------------------------

        def update_launch(self):
            for cls in ('suggested-action', 'destructive-action'):
                self.launch.remove_css_class(cls)
            if self.busy:
                self.launch.set_label('Stop')
                self.launch.add_css_class('destructive-action')
            else:
                built = package.built_runner(self.data_path) is not None
                self.launch.set_label('Play' if built else 'Install & Play')
                self.launch.add_css_class('suggested-action')

        def toast(self, text):
            self.toasts.add_toast(Adw.Toast(title=GLib.markup_escape_text(text), timeout=3))

        def message(self, text):
            def apply():
                if not self.closed:
                    self.status.set_text(text)
                    self.append_log('» ' + text + '\n')
                return False
            GLib.idle_add(apply)

        def set_busy(self, busy):
            def apply():
                if self.closed:
                    return False
                self.busy = busy
                for widget in self.lockable:
                    widget.set_sensitive(not busy)
                self.progress.set_visible(busy)
                self.progress.set_fraction(0)
                self.update_launch()
                return False
            GLib.idle_add(apply)

        def on_launch(self, _):
            if self.busy:
                self.stop_job()
            else:
                self.begin()

        def begin(self):
            iso = self.iso_path
            if not iso:
                self.status.set_text('Choose the game ISO first.')
                self.stack.set_visible_child_name('game')
                return
            try:
                cpu_check()
                dest = self.data_path.expanduser().resolve()
                dest.mkdir(parents=True, exist_ok=True)
            except (OSError, ImportError) as e:
                self.status.set_text(str(e))
                return
            self.runner = Runner()
            hud = self.hud.switch.get_active()
            prefix = [self.gamemode_path] if self.gamemode_path and self.gamemode.switch.get_active() else []
            display = self.display_settings()
            controls = self.controls
            self.log_dir = dest / 'logs' / (time.strftime('%Y%m%d-%H%M%S') + '-' + str(os.getpid()))
            self.log_dir.mkdir(parents=True, exist_ok=False)
            self.log_offsets, self.log_current = {}, None
            self.log_view.get_buffer().set_text('')
            self.save_settings({'iso': iso, 'data': str(dest), 'display': display, 'mangohud': hud})
            self.busy = True
            self.set_busy(True)
            log_dir = self.log_dir

            def worker():
                try:
                    with (log_dir / 'import.log').open('wb') as log:
                        package.import_iso(iso, dest, self.runner, self.message, log, args.rpcs3)
                    with (log_dir / 'build.log').open('wb') as log:
                        game = package.build_runner(dest, self.runner, self.message, log)
                    self.message('Checking Vulkan driver and window support...')
                    code, details = self.runner.run(package.tool('graphics-probe'),
                                                    env=package.game_env(dest, log_dir), capture=True)
                    (log_dir / 'graphics.log').write_text(details)
                    if code:
                        raise ImportError('Vulkan/window check failed. See graphics.log: ' + details[-1800:])
                    if clear_interrupted_install(dest):
                        self.message('The game\'s data installation was interrupted last time; it will install again.')
                    self.playing = True
                    self.message('Starting game. Logs: ' + str(log_dir))
                    with (log_dir / 'game.log').open('wb') as log:
                        code, _ = self.runner.run([*prefix, game, dest / 'game/EBOOT.ELF'], cwd=dest,
                                                  env=package.game_env(dest, log_dir, hud, display, controls),
                                                  log=log)
                    self.message('Game closed.' if code == 0 else 'Game exited (%d). Logs: %s' % (code, log_dir))
                except (OSError, ImportError, ValueError) as e:
                    self.message(str(e) + '\nLogs: ' + str(log_dir))
                finally:
                    self.playing = False
                    self.set_busy(False)
            threading.Thread(target=worker, daemon=False).start()

        def save_settings(self, changes):
            saved.update(changes)
            saved['controls'] = self.controls
            try:
                config_path.parent.mkdir(parents=True, exist_ok=True)
                config_path.write_text(json.dumps(saved, indent=2))
            except OSError as e:
                self.status.set_text('Could not save the settings: ' + str(e))

        def show_credits(self, _):
            dialog = Gtk.Window(title='Credits & licenses', transient_for=self.win, modal=True,
                                default_width=700, default_height=560)
            dialog.set_titlebar(Adw.HeaderBar())
            view = Gtk.TextView(editable=False, cursor_visible=False, wrap_mode=Gtk.WrapMode.WORD,
                                top_margin=12, bottom_margin=12, left_margin=16, right_margin=16)
            try:
                text = (ROOT / 'app/CREDITS.md').read_text()
            except OSError:
                text = 'CREDITS.md is missing from this AppImage.'
            # Plain text view: drop the Markdown emphasis, heading marks and link targets.
            text = re.sub(r'\*\*(.+?)\*\*', r'\1', text)
            text = re.sub(r'\[([^\]]+)\]\([^)]+\)', r'\1', text)
            text = re.sub(r'^#+ ', '', text, flags=re.M)
            text += ('\nLicense texts and THIRD_PARTY_NOTICES.md: ' + str(ROOT / 'usr/share/licenses') +
                     '\nCorresponding source code for every bundled component is published next to this '
                     'AppImage on the release page:\nhttps://github.com/robertoqimin/battride-war-recomp/releases\n')
            view.get_buffer().set_text(text)
            dialog.set_child(Gtk.ScrolledWindow(child=view, vexpand=True))
            dialog.present()

        def stop_job(self):
            if self.runner:
                self.message('Stopping...')
                threading.Thread(target=self.runner.stop, daemon=True).start()

        def close(self, *_):
            self.closed = True
            if self.runner:
                self.runner.stop()
            return False

        def tick(self):
            if self.closed:
                return False
            if self.busy and not self.playing:
                self.progress.pulse()
            self.progress.set_visible(self.busy and not self.playing)
            if self.playing and self.log_dir:
                try:
                    with (self.log_dir / 'game.log').open('rb') as boot:
                        startup = boot.read(16384).decode(errors='replace')
                    if '[rsx] Vulkan backend init OK -- window open' in startup:
                        self.status.set_text('Game window opened. Use your controller or keyboard. '
                                             'Logs: ' + str(self.log_dir))
                    elif '[rsx] Vulkan backend init FAILED' in startup:
                        self.status.set_text('Game Vulkan startup failed. Stop the game and send game.log from: ' +
                                             str(self.log_dir))
                except OSError:
                    pass
                try:
                    with (self.log_dir / 'frames.csv').open('rb') as f:
                        f.seek(0, 2)
                        size = f.tell()
                        f.seek(max(0, size - 32768))
                        lines = f.read().decode().splitlines()[1:]
                    values = [float(x.split(',')[2]) for x in lines[-120:] if len(x.split(',')) == 3]
                    values = [x for x in values if x > 0]
                    if values:
                        ordered = sorted(values)
                        self.stats.set_text('Recent guest frames: %.1f FPS  |  p95 %.1f ms' %
                                            (1000 * len(values) / sum(values),
                                             ordered[min(len(ordered) - 1, int(len(ordered) * .95))]))
                        self.stats.set_visible(True)
                except (OSError, ValueError):
                    pass
            else:
                self.stats.set_visible(False)
            return True

    def shot(ui):
        """Saves the window as a PNG (for checking the layout) and quits."""
        win = ui.win
        paintable = Gtk.WidgetPaintable.new(win)
        snapshot = Gtk.Snapshot()
        paintable.snapshot(snapshot, win.get_width(), win.get_height())
        if gi.version_info >= (3, 44):   # older PyGObject cannot pass render nodes
            node = snapshot.to_node()
            if node:
                win.get_renderer().render_texture(node, None).save_to_png(str(args.screenshot))
        else:
            print('screenshot needs PyGObject 3.44 or newer', file=sys.stderr)
        ui.close()
        win.get_application().quit()
        return False

    app = Adw.Application(application_id='io.github.robertoqimin.BattrideWarRecomp',
                          flags=Gio.ApplicationFlags.NON_UNIQUE)

    def activate(app):
        ui = Window(app)
        if args.screenshot:
            GLib.timeout_add(900, shot, ui)
    app.connect('activate', activate)
    app.run([sys.argv[0]])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, ImportError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
