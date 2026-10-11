#!/usr/bin/env python3
"""Talk to the Presto build over its USB serial debug channel (set PRESTO_PORT if not /dev/ttyACM0).

  presto.py stats | crtc | bootsel | view X W Y H DW DH | shot out.png [hold ms] | key <sdl_scancode> [ms] | raw '<cmd>'
  presto.py discs | disc [<name>] | put <file.ssd> [name on card]
  presto.py state <bbc micro bot snapshot .json>
  presto.py wifi <secrets.py>      save the WiFi network (Pimoroni's WIFI_SSID / WIFI_PASSWORD file) and restart
  presto.py net                    the WiFi link and address (the control panel is at http://beeb.local/)
"""
import base64, json, os, struct, sys, time, zlib
import serial

PORT = os.environ.get('PRESTO_PORT', '/dev/ttyACM0')

def open_port():
    s = serial.Serial(PORT, 115200, timeout=3)
    time.sleep(0.1)
    s.reset_input_buffer()
    return s

def command(s, cmd):
    s.write(b'!' + cmd.encode() + b'\n')

def readline_prefix(s, prefix):
    while True:
        line = s.readline()
        if not line:
            raise TimeoutError('no reply')
        if line.startswith(prefix.encode()):
            return line.decode().strip()

def write_png(path, w, h, rgb565):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            p = rgb565[y * w + x]
            r, g, b = (p >> 11) & 31, (p >> 5) & 63, p & 31
            row += bytes([r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2])
        rows.append(bytes(row))
    def chunk(t, d):
        c = t + d
        return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(b''.join(rows), 6)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def main():
    a = sys.argv[1:]
    s = open_port()
    if a[0] == 'shot':
        # an optional hold (ms): the machine stays paused that long after, so a camera can
        # photograph the same frame on the panel
        command(s, 'shot' + (' ' + a[2] if len(a) > 2 else ''))
        hdr = readline_prefix(s, 'SHOT').split()
        w, h = int(hdr[1]), int(hdr[2])
        data = b''
        while len(data) < w * h * 2:
            chunk = s.read(w * h * 2 - len(data))
            if not chunk:
                raise TimeoutError('short screenshot %d' % len(data))
            data += chunk
        write_png(a[1], w, h, struct.unpack('<%dH' % (w * h), data))
        print('saved', a[1])
    elif a[0] == 'wifi':
        # read Pimoroni's secrets.py without running it; never echo the password
        import ast
        vals = {}
        for node in ast.parse(open(a[1]).read()).body:
            if isinstance(node, ast.Assign) and isinstance(node.value, ast.Constant):
                for t in node.targets:
                    if isinstance(t, ast.Name):
                        vals[t.id] = node.value.value
        ssid, pw = vals.get('WIFI_SSID', ''), vals.get('WIFI_PASSWORD', '')
        if not ssid:
            sys.exit('no WIFI_SSID in ' + a[1])
        command(s, 'wifi "%s" %s' % (ssid, pw))
        print(readline_prefix(s, 'WIFI'))
    elif a[0] == 'net':
        command(s, 'net')
        print(readline_prefix(s, 'NET'))
    elif a[0] == 'discs':
        command(s, 'discs')
        while True:
            line = s.readline().decode().strip()
            if not line:
                raise TimeoutError('no reply')
            print(line)
            if line.startswith('DISCS'):
                break
    elif a[0] == 'disc':
        s.timeout = 15
        command(s, ('disc ' + ' '.join(a[1:])) if len(a) > 1 else 'disc')   # no name: the built-in disc
        print(readline_prefix(s, 'DISC'))
    elif a[0] == 'state':
        # pack a BBC Micro Bot snapshot for !state, deciding what to restore the way
        # bbcmicrobot-greatest's runner does (see src/pico/presto/presto_state.c)
        st = json.load(open(a[1]))
        st = st.get('state', st)
        ram = base64.b64decode(st['RAM'])[:0x8000].ljust(0x8000, b'\0')
        # which emulator's ROM sockets: beebjit's if BASIC's type byte (&60) is in socket 12
        beebjit = ram[0x2a1 + 12] == 0x60 and ram[0x2a1 + 15] != 0x60
        sockets = {'basic': 12, 'dfs': 13, 'gxr': 7} if beebjit else {'basic': 15, 'dfs': 14, 'gxr': 13}
        paged = sockets[st['paged']] if st.get('v', 1) >= 2 and st.get('paged') in sockets else ram[0xf4]
        crtc = (list(st['CRTC']) + [0] * 18)[:18]
        r = st['CPU6502']
        f = r.get('F', '')
        p = sum(bit for ch, bit in (('N', 0x80), ('V', 0x40), ('O', 0x40), ('D', 8), ('I', 4), ('Z', 2), ('C', 1)) if ch in f)
        sysvia, uservia = st.get('sysvia') or {}, st.get('uservia') or {}
        flags = (1 if 'IC32' in sysvia else 0) | (2 if isinstance(st.get('crtcReg'), int) else 0) | \
            (4 if 't1l' in sysvia else 0) | (8 if 't1l' in uservia else 0)
        def via(v):
            return struct.pack('<iIiIBBBBBB', int(v.get('t1c', 0)), int(v.get('t1l', 0)), int(v.get('t2c', 0)),
                               int(v.get('t2l', 0)), int(bool(v.get('t1hit'))), int(bool(v.get('t2hit'))),
                               v.get('acr', 0), v.get('pcr', 0), v.get('ifr', 0), v.get('ier', 0))
        data = ram + bytes(crtc) + bytes([st['ULAcontrol']]) + bytes(st['ULApalette']) + \
            bytes([r['A'], r['X'], r['Y'], r['S'], p, r['PC'] & 0xff, r['PC'] >> 8]) + \
            bytes([1 if beebjit else 0, paged & 15, st.get('crtcReg', 0) & 31 if flags & 2 else 0,
                   sysvia.get('IC32', 0) & 0xff, flags]) + via(sysvia) + via(uservia)
        command(s, 'state %d' % len(data))
        reply = readline_prefix(s, 'STATE')
        if reply != 'STATE ready':
            sys.exit(reply)
        s.write(data)
        print(readline_prefix(s, 'STATE'), 'beebjit' if beebjit else 'jsbeeb', 'paged', paged, 'flags', flags)
    elif a[0] == 'put':
        data = open(a[1], 'rb').read()
        name = a[2] if len(a) > 2 else os.path.basename(a[1])
        s.timeout = 10
        command(s, 'put %s %d' % (name, len(data)))
        reply = readline_prefix(s, 'PUT')
        if reply != 'PUT ready':
            sys.exit(reply)
        for i in range(0, len(data), 4096):
            s.write(data[i:i + 4096])
        s.timeout = 30
        reply = readline_prefix(s, 'PUT')
        print(reply)
        want = '%08x' % (zlib.crc32(data) & 0xffffffff)
        if not reply.startswith('PUT ok') or reply.split()[-1] != want:
            sys.exit('upload failed or corrupted (expected crc %s)' % want)
    elif a[0] == 'key':
        sc, ms = int(a[1]), int(a[2]) if len(a) > 2 else 100
        s.write(bytes([0x1a, 0, sc])); time.sleep(ms / 1000); s.write(bytes([0x1a, 1, sc]))
    else:
        cmd = ' '.join(a) if a[0] != 'raw' else a[1]
        prefix = {'stats': 'STATS', 'crtc': 'CRTC', 'view': 'VIEW', 'bootsel': 'BOOTSEL', 'touch': 'TOUCH'}.get(a[0], '')
        command(s, cmd)
        print(readline_prefix(s, prefix) if prefix else s.readline().decode().strip())

if __name__ == '__main__':
    main()
