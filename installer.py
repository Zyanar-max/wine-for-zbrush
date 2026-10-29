#!/usr/bin/env python3
"""Wine for ZBrush - fixes for ZBrush running in a Bottles bottle.

  1. Pen pressure that dies after touching the mouse (winex11.so)
  2. Pen pressure that dies after a modal dialog (wintab32.dll)
  3. Tearing at the top of the canvas while rotating (winex11.so)
  4. Native GNOME/KDE file dialogs for Open/Import/Save/Export (zbfiledlg)
  5. Decimation Master: checks the native vcomp140 setup
  6. Optional: GNOME "not responding" warning after 20 s instead of 5 s
  7. ZBrush file icons (.ztl, .zpr, .zbp, .zmt...) and double-click to open in ZBrush

Usage: sh install.sh [--bottle NAME] [--uninstall] [--yes]
"""
import argparse, base64, glob, hashlib, json, os, re, shutil, struct, subprocess, sys, zlib

BASE_RUNNER = 'soda-11.0-10'
FIX_RUNNER = 'soda-11.0-10-zbrush'
HERE = os.path.dirname(os.path.abspath(__file__))
STATE = '.wine-for-zbrush.json'

# (file in runner, offset, original bytes, patched bytes)
PATCHES = [
    ('lib/wine/x86_64-unix/winex11.so', 0x2faca,
     'ff2588730400', 'c39090909090'),                       # 1: no XCloseDevice in update_device_mapping
    ('lib/wine/x86_64-unix/winex11.so', 0x189db,
     'e83035ffff4881c458040000b801000000',
     '31f6e8ce35ffff4881c4580400006a0158'),                 # 3: XSync instead of XFlush after XShmPutImage
    ('lib/wine/x86_64-windows/wintab32.dll', 0x4d24,
     '31ffeb8d0f1f840000000000', '83a3f40000000031ffeb8690'),  # 2: flush overflowed packet queue
]
ORIGINAL_SHA256 = {
    'lib/wine/x86_64-unix/winex11.so': '8e52ca306af5b2a7de6567dd5258b8c9a1890565d5a4f30f80e9ecdf1351c4ff',
    'lib/wine/x86_64-windows/wintab32.dll': 'df6acb6fd0e852743c72ba80eba354b827d4536ed7cf161a277980de898b0da4',
}
CLSIDS = ('DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7',   # FileOpenDialog
          'C0B4E2F3-BA21-4773-8DBA-335EC946EB8B')   # FileSaveDialog
VC_OVERRIDES = ('vcomp140', 'vcomp120', 'vcomp110', 'vcomp100')

ASSUME_YES = False


def say(msg=''):
    print(msg, flush=True)


def ok(msg):
    say('  [ok] ' + msg)


def warn(msg):
    say('  [!]  ' + msg)


def die(msg):
    say('\nERROR: ' + msg)
    sys.exit(1)


def ask(question, default=False):
    if ASSUME_YES:
        return True
    hint = '[Y/n]' if default else '[y/N]'
    ans = input(f'{question} {hint} ').strip().lower()
    return default if not ans else ans in ('y', 'yes', 'i', 'igen')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def find_bottles_root():
    for p in ('~/.var/app/com.usebottles.bottles/data/bottles', '~/.local/share/bottles'):
        p = os.path.expanduser(p)
        if os.path.isdir(os.path.join(p, 'bottles')):
            return p
    die('Bottles was not found (neither Flatpak nor native).')


def find_zbrush_bottles(root):
    found = []
    bdir = os.path.join(root, 'bottles')
    for name in sorted(os.listdir(bdir)):
        pf = os.path.join(bdir, name, 'drive_c', 'Program Files')
        if not os.path.isdir(pf):
            continue
        for d in os.listdir(pf):
            if os.path.isfile(os.path.join(pf, d, 'ZBrush.exe')):
                found.append(name)
                break
    return found


def wine_running():
    # Leftover services (explorer.exe, winedevice.exe...) without a wineserver are
    # harmless orphans; only a live wineserver or ZBrush itself would rewrite the registry.
    out = subprocess.run(['ps', '-eo', 'comm'], capture_output=True, text=True).stdout.split()
    return any(c == 'wineserver' or c.lower() == 'zbrush.exe' for c in out)


def read_yaml_value(path, key):
    with open(path, encoding='utf-8') as f:
        for line in f:
            if line.startswith(key + ':'):
                return line.split(':', 1)[1].strip().strip("'\"")
    return None


def set_yaml_value(path, key, value):
    with open(path, encoding='utf-8') as f:
        text = f.read()
    text, n = re.subn(r'^%s:.*$' % re.escape(key), '%s: %s' % (key, value), text, count=1, flags=re.M)
    if not n:
        die(f'"{key}" not found in {path}')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


# ---- runner ---------------------------------------------------------------

def build_runner(root):
    src = os.path.join(root, 'runners', BASE_RUNNER)
    dst = os.path.join(root, 'runners', FIX_RUNNER)
    if not os.path.isdir(src):
        die(f'Runner {BASE_RUNNER} is not installed. In Bottles: Preferences -> Runners -> install {BASE_RUNNER}.')
    for rel, digest in ORIGINAL_SHA256.items():
        if sha256(os.path.join(src, rel)) != digest:
            die(f'{BASE_RUNNER}/{rel} is not the version these patches were made for. Nothing was changed.')
    if os.path.isdir(dst):
        shutil.rmtree(dst)
    # hard links: the copy costs no disk space; patched files become real copies below
    shutil.copytree(src, dst, symlinks=True, copy_function=os.link)
    for rel in ORIGINAL_SHA256:
        p = os.path.join(dst, rel)
        data = open(p, 'rb').read()
        os.unlink(p)
        with open(p, 'wb') as f:
            f.write(data)
        shutil.copymode(os.path.join(src, rel), p)
    for rel, off, old, new in PATCHES:
        p = os.path.join(dst, rel)
        with open(p, 'r+b') as f:
            f.seek(off)
            if f.read(len(old) // 2) != bytes.fromhex(old):
                die(f'unexpected bytes in {rel} at {off:#x}')
            f.seek(off)
            f.write(bytes.fromhex(new))
    ok(f'runner {FIX_RUNNER} created (patched copy of {BASE_RUNNER})')
    return dst


# ---- registry -------------------------------------------------------------

def set_inproc_server(system_reg, dll):
    with open(system_reg, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    for clsid in CLSIDS:
        pat = re.compile(r'(\[Software\\\\Classes\\\\CLSID\\\\\{%s\}\\\\InprocServer32\][^\[]*?@=")[^"]*(")' % clsid,
                         re.I)
        text, n = pat.subn(lambda m: m.group(1) + 'C:\\\\windows\\\\system32\\\\' + dll + m.group(2), text)
        if not n:
            die(f'file dialog class {clsid} not found in system.reg')
    with open(system_reg, 'w', encoding='utf-8', errors='surrogateescape') as f:
        f.write(text)


def ensure_dll_overrides(user_reg):
    with open(user_reg, encoding='utf-8', errors='surrogateescape') as f:
        text = f.read()
    m = re.search(r'^\[Software\\\\Wine\\\\DllOverrides\][^\n]*\n(?:#[^\n]*\n)?', text, re.M)
    if not m:
        text += '\n[Software\\\\Wine\\\\DllOverrides]\n'
        m = re.search(r'^\[Software\\\\Wine\\\\DllOverrides\][^\n]*\n', text, re.M)
    end = text.find('\n\n', m.end())
    end = len(text) if end < 0 else end
    section = text[m.end():end]
    changed = []
    for dll in VC_OVERRIDES:
        line = '"%s"="native,builtin"' % dll
        if re.search(r'^"%s"=' % dll, section, re.M | re.I):
            if not re.search(r'^"%s"="native,builtin"' % dll, section, re.M | re.I):
                section = re.sub(r'^"%s"=.*$' % dll, line, section, flags=re.M | re.I)
                changed.append(dll)
        else:
            section += line + '\n' if section.endswith('\n') or not section else '\n' + line
            changed.append(dll)
    if changed:
        text = text[:m.end()] + section + text[end:]
        with open(user_reg, 'w', encoding='utf-8', errors='surrogateescape') as f:
            f.write(text)
    return changed


def is_wine_builtin(path):
    with open(path, 'rb') as f:
        return b'Wine builtin DLL' in f.read(0x100)


# ---- desktop integration (icons, double-click) ----------------------------

# (mime type, description, extensions, icon in ZData, opens on double-click) - the same files ZBrush
# registers on Windows.
MIME_TYPES = (('application/x-zbrush-tool', 'ZBrush Tool', ('ztl',), 'ztl_extension_icons.ico', True),
              ('application/x-zbrush-project', 'ZBrush Project', ('zpr',), 'zpr_extension_icons.ico', True),
              ('application/x-zbrush-file', 'ZBrush File',
               ('zbp', 'zmt', 'zsc', 'zam', 'zbr', 'zcv', 'zfp', 'zgr', 'zld', 'zli', 'zmo', 'zmv', 'znm', 'zsl', 'zst'),
               'z_generic_extension_icons.ico', True))
OPENABLE = [m[0] for m in MIME_TYPES if m[4]]
DATA = os.path.expanduser(os.environ.get('XDG_DATA_HOME', '~/.local/share'))


def ico_to_pngs(path):
    """Returns {size: png bytes} for the 32-bit BMP or PNG images in an .ico file."""
    d = open(path, 'rb').read()
    out = {}
    for i in range(struct.unpack_from('<H', d, 4)[0]):
        w, h, _, _, _, bpp, size, off = struct.unpack_from('<BBBBHHII', d, 6 + i * 16)
        w, h = w or 256, h or 256
        if d[off:off + 4] == b'\x89PNG':
            out[w] = d[off:off + size]
            continue
        if bpp != 32:
            continue
        px = d[off + struct.unpack_from('<I', d, off)[0]:]
        rows = []
        for y in range(h):
            src = px[(h - 1 - y) * w * 4:(h - y) * w * 4]
            row = bytearray(src)
            row[0::4], row[2::4] = src[2::4], src[0::4]
            rows.append(b'\0' + bytes(row))
        chunk = lambda t, b: struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)
        out[w] = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                  chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))
    return out


def icon_theme_dirs():
    """mimetypes folders of the active icon theme if it lives in the user's home.
    Themes with a generic file icon (e.g. Papirus) win over hicolor otherwise."""
    try:
        theme = subprocess.run(['gsettings', 'get', 'org.gnome.desktop.interface', 'icon-theme'],
                               capture_output=True, text=True).stdout.strip().strip("'")
    except OSError:
        return None, []
    root = os.path.join(DATA, 'icons', theme)
    if not theme or theme == 'hicolor' or not os.path.isdir(root):
        return None, []
    return root, glob.glob(os.path.join(root, '*', 'mimetypes'))


def find_program(yml):
    """Name of the ZBrush program entry in bottle.yml."""
    text = open(yml, encoding='utf-8').read()
    for block in re.split(r'\n    [0-9a-f-]{36}:\n', text):
        if re.search(r'executable: .*zbrush\.exe', block, re.I):
            m = re.search(r'^        name: (.+)$', block, re.M)
            if m:
                return m.group(1).strip().strip("'\"")
    return None


def install_desktop(root, bottle, bdir):
    zdata = glob.glob(os.path.join(bdir, 'drive_c', 'Program Files', '*', 'ZData'))
    zdata = [z for z in zdata if os.path.isfile(os.path.join(os.path.dirname(z), 'ZBrush.exe'))]
    program = find_program(os.path.join(bdir, 'bottle.yml'))
    if not zdata or not program:
        warn('ZBrush program entry or ZData folder not found, skipped')
        return
    zdata = zdata[0]

    # file types
    mime_dir = os.path.join(DATA, 'mime', 'packages')
    os.makedirs(mime_dir, exist_ok=True)
    with open(os.path.join(mime_dir, 'wine-for-zbrush.xml'), 'w') as f:
        f.write('<?xml version="1.0" encoding="UTF-8"?>\n'
                '<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">\n')
        for mime, comment, exts, _, _ in MIME_TYPES:
            f.write(f'  <mime-type type="{mime}">\n    <comment>{comment}</comment>\n')
            for ext in exts:
                f.write(f'    <glob pattern="*.{ext}" case-sensitive="false"/>\n')
            f.write('  </mime-type>\n')
        f.write('</mime-info>\n')
    subprocess.run(['update-mime-database', os.path.join(DATA, 'mime')], capture_output=True)

    # icons: hicolor PNGs, plus SVG wrappers in a user-installed icon theme
    theme_root, theme_dirs = icon_theme_dirs()
    for mime, _, _, ico, _ in MIME_TYPES:
        name = mime.replace('/', '-')
        pngs = ico_to_pngs(os.path.join(zdata, ico))
        for size, png in pngs.items():
            d = os.path.join(DATA, 'icons', 'hicolor', f'{size}x{size}', 'mimetypes')
            os.makedirs(d, exist_ok=True)
            open(os.path.join(d, name + '.png'), 'wb').write(png)
        for d in theme_dirs:
            m = re.match(r'(\d+)x\d+(@2x)?', os.path.basename(os.path.dirname(d)))
            dim = int(m.group(1)) if m else 256
            want = dim * (2 if m and m.group(2) else 1)
            src = min((s for s in pngs if s >= want), default=max(pngs))
            b64 = base64.b64encode(pngs[src]).decode()
            open(os.path.join(d, name + '.svg'), 'w').write(
                f'<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
                f'width="{dim}" height="{dim}" viewBox="0 0 {src} {src}"><image width="{src}" height="{src}" '
                f'xlink:href="data:image/png;base64,{b64}"/></svg>\n')
    app_icon = ico_to_pngs(os.path.join(zdata, 'ZLogo.ico')) if os.path.exists(os.path.join(zdata, 'ZLogo.ico')) else {}
    if app_icon:
        size = max(app_icon)
        d = os.path.join(DATA, 'icons', 'hicolor', f'{size}x{size}', 'apps')
        os.makedirs(d, exist_ok=True)
        open(os.path.join(d, 'wine-for-zbrush.png'), 'wb').write(app_icon[size])
    for t in filter(None, (os.path.join(DATA, 'icons', 'hicolor'), theme_root)):
        subprocess.run(['gtk-update-icon-cache', '-f', '-t', t], capture_output=True)

    # double-click: files/wine-for-zbrush-open.sh with this bottle's values filled in
    flatpak = '/.var/app/com.usebottles.bottles/' in root
    cli = 'flatpak run --command=bottles-cli com.usebottles.bottles' if flatpak else 'bottles-cli'
    launcher = os.path.join(os.path.expanduser('~/.local/bin'), 'wine-for-zbrush-open')
    os.makedirs(os.path.dirname(launcher), exist_ok=True)
    script = open(os.path.join(HERE, 'files', 'wine-for-zbrush-open.sh')).read()
    for key, value in (('CLI', cli), ('BOTTLE', bottle), ('PROGRAM', program), ('PREFIX', bdir)):
        script = script.replace('@%s@' % key, value.replace("'", "'\\''"))
    with open(launcher, 'w') as f:
        f.write(script)
    os.chmod(launcher, 0o755)
    apps = os.path.join(DATA, 'applications')
    os.makedirs(apps, exist_ok=True)
    with open(os.path.join(apps, 'wine-for-zbrush-open.desktop'), 'w') as f:
        f.write('[Desktop Entry]\nType=Application\nName=ZBrush\n'
                f'Comment=Open in ZBrush ({bottle} bottle)\nExec={launcher} %f\n'
                'Icon=wine-for-zbrush\nNoDisplay=true\nStartupWMClass=steam_proton\n'
                'MimeType=' + ''.join(m + ';' for m in OPENABLE) + '\n')
    subprocess.run(['update-desktop-database', apps], capture_output=True)
    subprocess.run(['xdg-mime', 'default', 'wine-for-zbrush-open.desktop'] + OPENABLE,
                   capture_output=True)
    ok('ZBrush file icons installed (.ztl, .zpr, .zbp, .zmt, ...), double-click opens them in ZBrush')
    if not theme_root:
        say('       (if you only see a blank page icon, your icon theme hides them; restart the file manager first)')
    say('       Files must be on a drive the bottle can see (e.g. E:/F:/G: or Z:).')


def uninstall_desktop():
    paths = [os.path.join(DATA, 'mime', 'packages', 'wine-for-zbrush.xml'),
             os.path.join(DATA, 'applications', 'wine-for-zbrush-open.desktop'),
             os.path.expanduser('~/.local/bin/wine-for-zbrush-open')]
    for mime, *_ in MIME_TYPES:
        name = mime.replace('/', '-')
        paths += glob.glob(os.path.join(DATA, 'icons', '*', '*', 'mimetypes', name + '.*'))
    paths += glob.glob(os.path.join(DATA, 'icons', 'hicolor', '*', 'apps', 'wine-for-zbrush.png'))
    for p in paths:
        if os.path.exists(p):
            os.unlink(p)
    subprocess.run(['update-mime-database', os.path.join(DATA, 'mime')], capture_output=True)
    subprocess.run(['update-desktop-database', os.path.join(DATA, 'applications')], capture_output=True)
    ok('file icons and double-click handler removed')


# ---- install / uninstall --------------------------------------------------

def install(root, bottle):
    bdir = os.path.join(root, 'bottles', bottle)
    yml = os.path.join(bdir, 'bottle.yml')
    sys32 = os.path.join(bdir, 'drive_c', 'windows', 'system32')
    state_path = os.path.join(bdir, STATE)
    state = json.load(open(state_path)) if os.path.exists(state_path) else {}

    say('\n1-3. Pen pressure + tearing (patched Wine runner)')
    runner = build_runner(root)
    old_runner = read_yaml_value(yml, 'Runner')
    if old_runner != FIX_RUNNER:
        state.setdefault('runner', old_runner)
    shutil.copy2(yml, yml + '.bak-wine-for-zbrush')
    set_yaml_value(yml, 'Runner', FIX_RUNNER)
    ok(f'bottle "{bottle}" now uses {FIX_RUNNER} (was {old_runner})')
    wt = os.path.join(sys32, 'wintab32.dll')
    if os.path.exists(wt) and not os.path.exists(wt + '.orig'):
        shutil.copy2(wt, wt + '.orig')
    shutil.copy2(os.path.join(runner, 'lib/wine/x86_64-windows/wintab32.dll'), wt)
    ok('patched wintab32.dll copied into the bottle')

    say('\n4. Native file dialogs')
    for f in ('zbfiledlg.dll', 'zbfiledlg.py'):
        shutil.copy2(os.path.join(HERE, 'files', f), os.path.join(sys32, f))
    system_reg = os.path.join(bdir, 'system.reg')
    if not os.path.exists(system_reg + '.bak-wine-for-zbrush'):
        shutil.copy2(system_reg, system_reg + '.bak-wine-for-zbrush')
    set_inproc_server(system_reg, 'zbfiledlg.dll')
    ok('Open/Import/Save/Export now use the desktop file chooser')

    say('\n5. Decimation Master (native vcomp140)')
    vcomp = os.path.join(sys32, 'vcomp140.dll')
    if os.path.exists(vcomp) and not is_wine_builtin(vcomp):
        ok('Microsoft vcomp140.dll is present')
    else:
        warn('Microsoft vcomp140.dll is missing. In Bottles open the bottle -> Dependencies ->')
        warn('install "vcredist2019" (Visual C++ 2015-2022), then run this script again.')
    user_reg = os.path.join(bdir, 'user.reg')
    changed = ensure_dll_overrides(user_reg)
    ok('DLL overrides set to native,builtin: ' + (', '.join(changed) if changed else 'already correct'))

    say('\n6. Optional: "not responding" warning')
    if shutil.which('gsettings') and subprocess.run(
            ['gsettings', 'get', 'org.gnome.mutter', 'check-alive-timeout'],
            capture_output=True).returncode == 0:
        say('  GNOME warns after 5 s that ZBrush is "not responding" while it loads big files.')
        say('  This changes it to 20 s for ALL programs (undo: gsettings reset org.gnome.mutter check-alive-timeout).')
        if ask('  Set it to 20 seconds?'):
            subprocess.run(['gsettings', 'set', 'org.gnome.mutter', 'check-alive-timeout', '20000'])
            ok('set to 20 s')
        else:
            ok('left unchanged')
    else:
        ok('not GNOME, skipped')

    say('\n7. File icons and double-click')
    install_desktop(root, bottle, bdir)

    json.dump(state, open(state_path, 'w'), indent=1)
    say('\nDone. Start ZBrush from Bottles as usual.')
    say('Do not change the bottle\'s runner in Bottles, or fixes 1-3 are lost (run this script again).')


def uninstall(root, bottle):
    bdir = os.path.join(root, 'bottles', bottle)
    yml = os.path.join(bdir, 'bottle.yml')
    sys32 = os.path.join(bdir, 'drive_c', 'windows', 'system32')
    state_path = os.path.join(bdir, STATE)
    state = json.load(open(state_path)) if os.path.exists(state_path) else {}

    if read_yaml_value(yml, 'Runner') == FIX_RUNNER:
        set_yaml_value(yml, 'Runner', state.get('runner') or BASE_RUNNER)
        ok('runner restored to ' + (state.get('runner') or BASE_RUNNER))
    wt = os.path.join(sys32, 'wintab32.dll')
    if os.path.exists(wt + '.orig'):
        shutil.move(wt + '.orig', wt)
        ok('original wintab32.dll restored')
    set_inproc_server(os.path.join(bdir, 'system.reg'), 'comdlg32.dll')
    for f in ('zbfiledlg.dll', 'zbfiledlg.py'):
        p = os.path.join(sys32, f)
        if os.path.exists(p):
            os.unlink(p)
    ok('Wine file dialogs restored')
    uninstall_desktop()
    if os.path.exists(state_path):
        os.unlink(state_path)
    say(f'\nDone. The runner folder {FIX_RUNNER} can be deleted in Bottles if no other bottle uses it.')
    say('DLL overrides (vcomp140) and the GNOME timeout were left as they are.')


def main():
    global ASSUME_YES
    ap = argparse.ArgumentParser(description='Wine for ZBrush fixes for Bottles')
    ap.add_argument('--bottle', help='bottle name (default: the one with ZBrush.exe)')
    ap.add_argument('--uninstall', action='store_true')
    ap.add_argument('--yes', action='store_true', help='answer yes to questions')
    a = ap.parse_args()
    ASSUME_YES = a.yes

    say('Wine for ZBrush')
    root = find_bottles_root()
    bottle = a.bottle
    if not bottle:
        found = find_zbrush_bottles(root)
        if not found:
            die('no bottle with ZBrush.exe found. Install ZBrush in a Bottles bottle first, or use --bottle NAME.')
        if len(found) > 1:
            die('several ZBrush bottles found: %s. Choose one with --bottle NAME.' % ', '.join(found))
        bottle = found[0]
    if not os.path.isfile(os.path.join(root, 'bottles', bottle, 'bottle.yml')):
        die(f'bottle "{bottle}" not found in {root}/bottles')
    say(f'Bottle: {bottle}  ({root})')

    if wine_running():
        die('a Windows program / Wine is still running. Close ZBrush and Bottles, then run this again.')

    if a.uninstall:
        uninstall(root, bottle)
    else:
        install(root, bottle)


if __name__ == '__main__':
    main()
