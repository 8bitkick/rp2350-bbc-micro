#!/usr/bin/env python3
"""Measure the Presto on a set of scenes, for the benchmark table:

  tools/scenes.py [scene ...]        (default: all; needs the board on WiFi and USB)

Each scene is set up through the web panel (http://beeb.local/ or PRESTO_HOST)
and measured over USB (presto.py stats, three readings, the middle one):
per-core load and speed at real speed, then turbo speed where it's meaningful.
Discs come from the discs/ directory (chuckie-egg.ssd, Exile.ssd, the
Bitshifters demos) if they're there; a scene whose disc is missing is skipped.
"""
import os, subprocess, sys, time, urllib.request

HOST = os.environ.get('PRESTO_HOST', 'http://beeb.local')
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
PRESTO = os.path.join(HERE, 'presto.py')
M = {'Q': 0x10, 'W': 0x21, 'E': 0x22, 'R': 0x33, 'T': 0x23, 'Y': 0x44, 'U': 0x35, 'I': 0x25, 'O': 0x36, 'P': 0x37,
     'A': 0x41, 'S': 0x51, 'D': 0x32, 'F': 0x43, 'G': 0x53, 'H': 0x54, 'J': 0x45, 'K': 0x46, 'L': 0x56,
     'Z': 0x61, 'X': 0x42, 'C': 0x52, 'V': 0x63, 'B': 0x64, 'N': 0x55, 'M': 0x65, ' ': 0x62, '\n': 0x49,
     '1': 0x30, '2': 0x31, '3': 0x11, '4': 0x12, '5': 0x13, '6': 0x34, '7': 0x24, '8': 0x15, '9': 0x26, '0': 0x27,
     ':': 0x48, ';': 0x57, '-': 0x17, ',': 0x66, '.': 0x67, '/': 0x68}
SHIFTED = {'*': ':', '"': '2', '!': '1', '+': ';', '=': '-', '<': ',', '>': '.', '?': '/', '&': '6', '(': '8', ')': '9', '%': '5', '$': '4'}


def post(path, body=b''):
    req = urllib.request.Request(HOST + path, data=body, method='POST')
    return urllib.request.urlopen(req, timeout=30).read()


def keys(events):
    for i in range(0, len(events), 24):
        post('/api/keys', ' '.join(events[i:i + 24]).encode())
        time.sleep(len(events[i:i + 24]) * 0.06 + 0.2)


def type_text(text):
    ev = []
    for ch in text:
        sh = ch in SHIFTED
        k = M[SHIFTED[ch] if sh else ch.upper()]
        ev += (['d00'] if sh else []) + ['d%02x' % k, 'u%02x' % k] + (['u00'] if sh else [])
    keys(ev)


def disc(name):
    for d in (os.path.join(ROOT, 'discs', name), os.path.join(os.path.expanduser('~/presto/discs'), name)):
        if os.path.exists(d):
            post('/api/disc?name=' + name, open(d, 'rb').read())
            return True
    return False


def stats():
    out = subprocess.run([sys.executable, PRESTO, 'stats'], capture_output=True, text=True).stdout
    f = dict(kv.split('=') for kv in out.split() if '=' in kv)
    return int(f['core0'].rstrip('%')), int(f['core1'].rstrip('%')), int(f['speed'].rstrip('%'))


def measure(turbo):
    subprocess.run([sys.executable, PRESTO, 'raw', 'turbo'], capture_output=True)
    if turbo:
        post('/api/turbo', b'1')
    else:
        post('/api/turbo', b'0')
    time.sleep(4)
    r = sorted(stats() for _ in (time.sleep(2), time.sleep(2), time.sleep(2)))
    post('/api/turbo', b'0')
    return r[1]


SCENES = [
    ('MODE 7 prompt', lambda: (keys(['b']), time.sleep(6))),
    ('Elite title', lambda: (post('/api/eject'), time.sleep(40))),
    ('MODE 0 listing', lambda: (keys(['b']), time.sleep(5), type_text('MODE 0\nFOR I%=1 TO 300:PRINT I%;" ";:NEXT\n'), time.sleep(8))),
    ('Chuckie Egg', lambda: (disc('chuckie-egg.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(30))),
    ('Exile, in game', lambda: (disc('Exile.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(20),
                                keys(['d62', 'u62'] * 10), time.sleep(2), keys(['d20', 'u20']), time.sleep(20))),
    ('Wobble Colours', lambda: (disc('bs-wobble-colours.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(12))),
]


def main():
    want = sys.argv[1:]
    print('| Scene | Core 0 | Core 1 | Speed | Turbo |')
    print('|---|---:|---:|---:|---:|')
    for name, setup in SCENES:
        if want and not any(w.lower() in name.lower() for w in want):
            continue
        try:
            setup()
        except FileNotFoundError:
            print('| %s | (no disc) | | | |' % name)
            continue
        c0, c1, speed = measure(False)
        t0, t1, tspeed = measure(True)
        print('| %s | %d%% | %d%% | %d%% | %.1f× |' % (name, c0, c1, speed, tspeed / 100.0))
        sys.stdout.flush()
    post('/api/eject')


if __name__ == '__main__':
    main()
