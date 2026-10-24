#!/usr/bin/python3
# Native (xdg-desktop-portal) file dialog helper for the zbfiledlg Wine shim.
# Result file: "OK\n<filter index>\n<path>\n..." or "CANCEL\n", written atomically.
import argparse, os, sys, urllib.parse

OUT = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv[:-1] else None

def finish(lines):
    if OUT:
        tmp = OUT + '.tmp'
        with open(tmp, 'w', encoding='utf-8') as f:
            f.write('\n'.join(lines) + '\n')
        os.replace(tmp, OUT)
    sys.exit(0)

try:
    from gi.repository import Gio, GLib
except Exception as e:
    finish(['ERROR', repr(e)])

ap = argparse.ArgumentParser()
ap.add_argument('--mode', default='open')          # open | open-multi | save | folder
ap.add_argument('--title', default='')
ap.add_argument('--parent', default='')            # X11 window id (hex)
ap.add_argument('--folder', default='')
ap.add_argument('--name', default='')
ap.add_argument('--filter', action='append', default=[])   # "Name|*.a;*.b"
ap.add_argument('--filter-index', type=int, default=0)
ap.add_argument('--out', required=True)
try:
    a = ap.parse_args()
except SystemExit:
    finish(['ERROR', 'bad arguments'])

def patterns(spec):
    out = []
    for p in spec.split(';'):
        p = p.strip()
        if not p:
            continue
        for v in dict.fromkeys((p, p.lower(), p.upper())):
            out.append((0, v))
    return out

try:
    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    token = 'zbfiledlg%d' % os.getpid()
    sender = bus.get_unique_name()[1:].replace('.', '_')
    req_path = '/org/freedesktop/portal/desktop/request/%s/%s' % (sender, token)

    filters = []
    for f in a.filter:
        name, _, spec = f.partition('|')
        pats = patterns(spec) or [(0, '*')]
        filters.append((name, pats))

    opts = {'handle_token': GLib.Variant('s', token), 'modal': GLib.Variant('b', True)}
    if filters and a.mode != 'folder':
        opts['filters'] = GLib.Variant('a(sa(us))', filters)
        idx = min(max(a.filter_index, 0), len(filters) - 1)
        opts['current_filter'] = GLib.Variant('(sa(us))', filters[idx])
    if a.folder and os.path.isdir(a.folder):
        opts['current_folder'] = GLib.Variant('ay', a.folder.encode() + b'\0')
    if a.mode == 'open-multi':
        opts['multiple'] = GLib.Variant('b', True)
    if a.mode == 'folder':
        opts['directory'] = GLib.Variant('b', True)
    if a.mode == 'save' and a.name:
        opts['current_name'] = GLib.Variant('s', a.name)

    loop = GLib.MainLoop()
    result = {}
    def on_response(conn, sender_name, path, iface, signal, params):
        result['code'], result['data'] = params.unpack()
        loop.quit()
    bus.signal_subscribe('org.freedesktop.portal.Desktop', 'org.freedesktop.portal.Request',
                         'Response', req_path, None, Gio.DBusSignalFlags.NO_MATCH_RULE, on_response)

    parent = 'x11:%s' % a.parent if a.parent else ''
    method = 'SaveFile' if a.mode == 'save' else 'OpenFile'
    title = a.title or ('Save' if a.mode == 'save' else 'Open')
    bus.call_sync('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop',
                  'org.freedesktop.portal.FileChooser', method,
                  GLib.Variant('(ssa{sv})', (parent, title, opts)), None,
                  Gio.DBusCallFlags.NONE, -1, None)
    loop.run()

    if result.get('code') != 0:
        finish(['CANCEL'])
    data = result['data']
    paths = [urllib.parse.unquote(urllib.parse.urlparse(u).path) for u in data.get('uris', [])]
    if not paths:
        finish(['CANCEL'])
    idx = a.filter_index
    cur = data.get('current_filter')
    if cur:
        for i, (n, _) in enumerate(filters):
            if n == cur[0]:
                idx = i
                break
    finish(['OK', str(idx)] + paths)
except SystemExit:
    raise
except Exception as e:
    finish(['ERROR', repr(e)])
