#!/usr/bin/env python3
"""Talk to the Presto firmware over its USB serial channel (the first Raspberry Pi Pico-class USB
serial device, or PRESTO_PORT): the Model B (presto_beebjit) or the Master 128 (presto_master).
None of this is needed to use the Presto; it's for measuring and testing.

Both machines:
  presto.py stats                  speed (100.0% = the real machine) and the two cores' load
  presto.py turbo                  toggle turbo: run as fast as it can (every frame still drawn)
  presto.py shot out.png [hold ms] screenshot of the panel (480x480, as shown); the hold (Model B
                                   only) keeps the machine paused after, for a camera
  presto.py key <sdl_scancode> [ms]   press a key (40 RETURN, 44 SPACE, 4-29 A-Z, 30-39 1-0)
  presto.py bootsel                reboot into the USB bootloader, ready for a new .uf2
  presto.py wifi <secrets.py>      save the WiFi network (Pimoroni's WIFI_SSID / WIFI_PASSWORD) and
                                   restart; the web panel is then at http://beeb.local/
  presto.py net                    the WiFi link and address
  presto.py discs                  the .ssd/.dsd images on the SD card
  presto.py disc [name]            boot a disc from the card (no name: the built-in disc)
  presto.py put <file.ssd> [name]  write a disc image to the card
  presto.py raw '<cmd>'            any other command (the Master: '!...' goes to a built-in program)
The Model B only:
  presto.py crtc | view X W Y H DW DH | touch
  presto.py state <bbc micro bot snapshot .json>
"""
import base64, glob, json, os, struct, sys, time, zlib
import serial

def default_port():
    # by USB identity, so another USB serial device (an ESP32, say) can't take ttyACM0 from it
    ids = sorted(glob.glob('/dev/serial/by-id/usb-Raspberry_Pi_Pico*'))
    return ids[0] if ids else '/dev/ttyACM0'

PORT = os.environ.get('PRESTO_PORT') or default_port()

def open_port():
    s = serial.Serial(PORT, 115200, timeout=3)
    time.sleep(0.1)
    s.reset_input_buffer()
    return s

def command(s, cmd):
    s.write(b'!' + cmd.encode() + b'\n')

def readline_prefix(s, prefix):
    until = time.time() + 10
    while True:
        line = s.readline()
        if not line or time.time() > until:
            # (a board stuck reporting a boot failure talks, but never answers)
            raise TimeoutError('no reply' if not line else 'no %s reply in: %r' % (prefix, line[:80]))
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
    if not a:
        sys.exit(__doc__)
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
        # bbcmicrobot-greatest's runner does (see presto_state_receive in src/machines/beeb/presto_machine.c)
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
        prefix = {'stats': 'STATS', 'turbo': 'TURBO', 'crtc': 'CRTC', 'view': 'VIEW', 'bootsel': 'BOOTSEL', 'touch': 'TOUCH'}.get(a[0], '')
        command(s, cmd)
        print(readline_prefix(s, prefix) if prefix else s.readline().decode().strip())

if __name__ == '__main__':
    main()
