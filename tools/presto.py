#!/usr/bin/env python3
"""Talk to the Presto firmware over its USB serial channel (the first Raspberry Pi Pico-class USB
serial device, or PRESTO_PORT). None of this is needed to use the Presto; it's for measuring.

  presto.py stats                  speed (100.0% = a real BBC Master) and the two cores' load
  presto.py turbo                  toggle turbo: run as fast as it can (every frame still drawn)
  presto.py shot out.png           screenshot of the panel (480x480, as shown)
  presto.py key <sdl_scancode> [ms]   press a key (40 RETURN, 44 SPACE, 4-29 A-Z, 30-39 1-0)
  presto.py bootsel                reboot into the USB bootloader, ready for a new .uf2
  presto.py wifi <secrets.py>      save the WiFi network (Pimoroni's WIFI_SSID / WIFI_PASSWORD) and
                                   restart; the web panel is then at http://beeb.local/
  presto.py net                    the WiFi link and address
  presto.py discs                  the .ssd/.dsd images on the SD card
  presto.py disc [name]            boot a disc from the card (no name: the built-in disc)
  presto.py put <file.ssd> [name]  write a disc image to the card
"""
import glob, os, struct, sys, time, zlib
import serial

def default_port():
    # by USB identity, so another USB serial device can't take ttyACM0 from it
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
        command(s, 'shot')
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
    elif a[0] in ('stats', 'turbo', 'bootsel'):
        command(s, a[0])
        print(readline_prefix(s, a[0].upper()))
    else:
        sys.exit(__doc__)

if __name__ == '__main__':
    main()
