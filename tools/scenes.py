#!/usr/bin/env python3
"""Measure the Presto on a set of scenes, for the benchmark table:

  tools/scenes.py [scene ...]        (default: all; needs the board on WiFi and USB)

Each scene is set up through the web panel (PRESTO_HOST: http://presto-xxxx.local, the board's own name)
and measured over USB (presto.py stats, three readings, the middle one):
per-core load and speed at real speed, then turbo speed where it's meaningful.
Discs come from the discs/ directory (Exile.ssd, teletext-elite.ssd, the
Bitshifters demos) if they're there; a scene whose disc is missing is skipped.
SHOTS=<directory> saves each scene's picture there (presto.py shot).
LOCK=1 adds the frame lock's figures over the real-speed reading (presto.py
raw lock): rows drawn late / rows, and the nearest any row came to the scan
(lines; negative is ahead).
"""
import os, subprocess, sys, time, urllib.request

HOST = os.environ.get('PRESTO_HOST') or sys.exit('set PRESTO_HOST (http://presto-xxxx.local: the name the Presto shows)')
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
    # a character's events go in one request: split across two, the key would be held down
    # through the pause between them, and auto-repeat
    post_ev = []
    for ch in text:
        sh = ch in SHIFTED
        k = M[SHIFTED[ch] if sh else ch.upper()]
        ev = (['d00'] if sh else []) + ['d%02x' % k, 'u%02x' % k] + (['u00'] if sh else [])
        if len(post_ev) + len(ev) > 24:
            keys(post_ev)
            post_ev = []
        post_ev += ev
    keys(post_ev)


def disc(name):
    for d in (os.path.join(ROOT, 'discs', name),):
        if os.path.exists(d):
            post('/api/disc?name=' + name, open(d, 'rb').read())
            return True
    return False


def stats():
    out = subprocess.run([sys.executable, PRESTO, 'stats'], capture_output=True, text=True).stdout
    f = dict(kv.split('=') for kv in out.split() if '=' in kv)
    return int(f['core0'].rstrip('%')), int(f['core1'].rstrip('%')), int(f['speed'].rstrip('%'))


def lock():
    out = subprocess.run([sys.executable, PRESTO, 'raw', 'lock'], capture_output=True, text=True).stdout
    # "... rows N: late L, near M, lines from the scan min X avg Y max Z; ..."
    f = out.split()
    try:
        i = f.index('scan')
        return (int(f[f.index('late') + 1].rstrip(',')), int(f[f.index('rows') + 1].rstrip(':')),
                int(f[i + 2]), int(f[i + 6].rstrip(';')))
    except (ValueError, IndexError):
        return (0, 0, 0, 0)


def measure(turbo, do_lock=False):
    subprocess.run([sys.executable, PRESTO, 'raw', 'turbo'], capture_output=True)
    if turbo:
        post('/api/turbo', b'1')
    else:
        post('/api/turbo', b'0')
    time.sleep(4)
    if do_lock:
        lock()   # (reading resets the counters: the lock has settled by now)
    r = sorted(stats() for _ in (time.sleep(2), time.sleep(2), time.sleep(2)))
    late = lock() if do_lock else None
    post('/api/turbo', b'0')
    return r[1] + (late,)


SCENES = [
    # (from Elite's title, which leaves BREAK alone; some games catch it)
    ('MODE 7 prompt', lambda: (post('/api/eject'), time.sleep(40), keys(['b']), time.sleep(6))),
    ('Elite title', lambda: (post('/api/eject'), time.sleep(40))),
    ('MODE 0 listing', lambda: (keys(['b']), time.sleep(5), type_text('MODE 0\nFOR I%=1 TO 300:PRINT I%;" ";:NEXT\n'), time.sleep(8))),
    # (teletext under load: the screen scrolls continuously)
    ('MODE 7 scroll', lambda: (keys(['b']), time.sleep(5), type_text('10 MODE 7\n20 P."abc";\n30 GOTO 20\nRUN\n'), time.sleep(8))),
    # (moving teletext the plain scroll doesn't cover: double height, a color change each line,
    # graphics characters, the whole screen scrolling)
    ('MODE 7 color scroller', lambda: (keys(['b']), time.sleep(5), type_text(
        '10 MODE 7\n20 P.CHR$(141);CHR$(129+R%);"DOUBLE";CHR$(151);"5?5?5?":R%=(R%+1)MOD 7\n30 GOTO 20\nRUN\n'),
        time.sleep(8))),
    # (Mark Moxon's Elite in MODE 7: elite.bbcelite.com/hacks/teletext_elite; the title's ship rotates every frame)
    ('Teletext Elite', lambda: (disc('teletext-elite.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(25))),
    # (its instruction pages load in turn: one SPACE every 2 s gets to the menu; f0 runs the game)
    ('Exile, in game', lambda: (disc('Exile.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(20),
                                [(keys(['d62', 'u62']), time.sleep(2)) for _ in range(16)],
                                keys(['d20', 'u20']), time.sleep(15))),
    ('Wobble Colours', lambda: (disc('bs-wobble-colours.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(12))),
    ('Bar Beeb', lambda: (disc('bs-bar-beeb.ssd') or (_ for _ in ()).throw(FileNotFoundError), time.sleep(5),
                          type_text('*RUN BARBEEB\n'), time.sleep(10))),
]


def main():
    want = sys.argv[1:]
    do_lock = bool(os.environ.get('LOCK'))
    print('| Scene | Core 0 | Core 1 | Speed | Turbo |' + (' Late rows | Nearest (lines) |' if do_lock else ''))
    print('|---|---:|---:|---:|---:|' + ('---:|---:|' if do_lock else ''))
    for name, setup in SCENES:
        if want and not any(w.lower() in name.lower() for w in want):
            continue
        try:
            setup()
        except FileNotFoundError:
            print('| %s | (no disc) | | | |' % name)
            continue
        c0, c1, speed, late = measure(False, do_lock)
        t0, t1, tspeed, _ = measure(True)
        if os.environ.get('SHOTS'):
            # the picture, to check the scene ran (SHOTS=<directory>)
            subprocess.run([sys.executable, PRESTO, 'shot', os.path.join(os.environ['SHOTS'], name.split(',')[0].replace(' ', '-') + '.png')],
                           capture_output=True)
        print('| %s | %d%% | %d%% | %d%% | %.1f× |' % (name, c0, c1, speed, tspeed / 100.0)
              + (' %d / %d | %d |' % (late[0], late[1], late[3]) if do_lock else ''))
        sys.stdout.flush()
    post('/api/eject')


if __name__ == '__main__':
    import signal
    # a SIGTERM (pkill) unwinds through the finally below too
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    try:
        main()
    finally:
        # never leave the board in turbo (an interrupted measurement would)
        try:
            post('/api/turbo', b'0')
        except Exception:
            pass
