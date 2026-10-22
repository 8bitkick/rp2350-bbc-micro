#!/usr/bin/env python3
"""api_stress.py <presto address> [--usb <serial port>] [--seconds N] [--list-ms N] [--install <app.bin> --cycles N]
                [--squeeze N]:
the web API under load, on the board itself. Nothing here changes the board's state, but for --install.

  1. the disc list: GET /api/discs against the card's own list (USB "!discs", with --usb): every disc, once;
     and its speed, 30 in a row: the median and the 90th percentile (--list-ms N: a median over N ms fails)
  2. one at a time: each read-only endpoint, 20 rounds; every reply valid JSON
  3. at once: 6 clients for --seconds (30), mixed endpoints; failures counted, latency p50/p99
  4. abuse: more idle connections than the server keeps (12), half-sent requests, connections dropped
     mid-reply, junk, an oversized body, unknown routes; the board must still answer afterwards
  5. after each phase: the board answering, the machine at full speed, the heap where it was (a leak)
  6. --install: the app installed over WiFi --cycles times (3), the board back with it running each time
     (the restart an install asks for)
  7. --squeeze N (with --usb, a -DHANG_TRAP=ON build): the heap taken until N bytes are left ("!squeeze"), then
     the load and the abuse again, and an install if --install: requests may fail, the board must go on answering
     (an allocation that failed used to stop it for good); then the heap given back

Exit status 1 if anything failed. Uses only Python's standard library (and pyserial, for --usb)."""
import json, socket, sys, threading, time, random, urllib.request, urllib.error

READ = ['/api/status', '/api/stats', '/api/discs', '/api/app', '/api/net', '/']    # (not /api/program: an app's, if it has one)
fails = []

def fail(why):
    fails.append(why)
    print('  FAIL', why)

def get(host, path, timeout=8, data=None, method=None):
    req = urllib.request.Request('http://%s%s' % (host, path), data=data, method=method or ('POST' if data else 'GET'))
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read()
            return r.status, body, time.time() - t0
    except urllib.error.HTTPError as e:
        return e.code, e.read(), time.time() - t0

def status(host):
    code, body, _ = get(host, '/api/status')
    return json.loads(body)

def health(host, base, what):
    """the board answering, at full speed, the heap where it was"""
    for _ in range(10):
        try:
            s = status(host)
            break
        except (OSError, ValueError):
            time.sleep(1)
    else:
        fail('%s: the board stopped answering' % what)
        return None
    time.sleep(2)       # (a moment for anything still closing)
    s = status(host)
    for _ in range(5):  # (and longer, before calling it a leak: lwIP lets a closed connection go a few seconds on)
        if not base or s['free'] >= base['free'] - 512: break
        time.sleep(2)
        s = status(host)
    if s.get('speed', 0) < 99: fail('%s: speed %s%%' % (what, s.get('speed')))
    if base and s['free'] < base['free'] - 512: fail('%s: the heap %d -> %d bytes free (a leak?)' % (what, base['free'], s['free']))
    print('  ok: speed %s%%, free %d (start %d)' % (s.get('speed'), s['free'], base['free'] if base else s['free']))
    return s

def usb_discs(port):
    import serial
    s = serial.Serial(port, 115200, timeout=1)
    s.reset_input_buffer()
    s.write(b'!discs\n')
    names, until = [], time.time() + 15
    while time.time() < until:
        line = s.readline().decode(errors='replace').strip()
        if line.startswith('DISC '):
            names.append(line[5:].rsplit(' ', 1)[0])
        elif line.startswith('DISCS'):
            s.close()
            return names, line
    s.close()
    raise TimeoutError('no DISCS line')

def phase_list(host, port):
    print('1. the disc list')
    code, body, dt = get(host, '/api/discs', timeout=30)
    try:
        j = json.loads(body)
    except ValueError:
        fail('/api/discs: not JSON (%d bytes: %r...)' % (len(body), body[-60:]))
        return
    web = [d['name'] for d in j['discs']]
    print('  web: %d discs, %d bytes, %.2f s, card %s' % (len(web), len(body), dt, j['card']))
    if len(set(web)) != len(web): fail('/api/discs: a disc listed twice')
    if port:
        usb, last = usb_discs(port)
        print('  USB: %d discs (%s)' % (len(usb), last))
        missing, extra = sorted(set(usb) - set(web)), sorted(set(web) - set(usb))
        if missing: fail('/api/discs is missing %d of the card\'s discs: %s' % (len(missing), ', '.join(missing[:8])))
        if extra: fail('/api/discs lists %d the card hasn\'t: %s' % (len(extra), ', '.join(extra[:8])))

def phase_list_speed(host, limit_ms):
    t, size = [], 0
    for _ in range(30):
        code, body, dt = get(host, '/api/discs', timeout=30)
        t.append(dt * 1000)
        size = len(body)
    t.sort()
    med, p90 = t[len(t) // 2], t[int(len(t) * .9)]
    print('  speed: median %.1f ms, 90th percentile %.1f ms (%d bytes, %.0f KB/s)' % (med, p90, size, size / med))
    if limit_ms and med > limit_ms: fail('/api/discs: median %.1f ms, over %.1f' % (med, limit_ms))

def phase_serial(host):
    print('2. one at a time')
    for rnd in range(20):
        for p in READ:
            try:
                code, body, _ = get(host, p)
            except OSError as e:
                fail('%s: %s' % (p, e)); continue
            if code != 200: fail('%s: HTTP %d' % (p, code)); continue
            if p.startswith('/api/'):
                try: json.loads(body)
                except ValueError: fail('%s: not JSON (%d bytes)' % (p, len(body)))

def phase_load(host, seconds):
    print('3. 6 clients at once for %d s' % seconds)
    lat, errs, n = [], [], [0]
    lock = threading.Lock()
    def client():
        end = time.time() + seconds
        while time.time() < end:
            p = random.choice(READ)
            try:
                code, body, dt = get(host, p, timeout=10)
                with lock:
                    n[0] += 1
                    lat.append(dt)
                    if code != 200: errs.append('%s HTTP %d' % (p, code))
            except OSError as e:
                with lock: n[0] += 1; errs.append('%s %s' % (p, type(e).__name__))
    ts = [threading.Thread(target=client) for _ in range(6)]
    for t in ts: t.start()
    for t in ts: t.join()
    lat.sort()
    print('  %d requests, %d failed; latency p50 %.3f s, p99 %.3f s' % (n[0], len(errs), lat[len(lat) // 2] if lat else 0, lat[int(len(lat) * .99)] if lat else 0))
    # (with six at once the server turns some away as busy: a failure rate over 5% is one)
    if n[0] and len(errs) > n[0] * 0.05: fail('under load: %d of %d failed (%s)' % (len(errs), n[0], ', '.join(sorted(set(errs))[:4])))

def raw(host, data, read=True, linger0=False, hold=0):
    s = socket.create_connection((host, 80), timeout=10)
    if linger0: s.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b'\1\0\0\0\0\0\0\0')
    if data: s.sendall(data)
    if hold: time.sleep(hold)
    out = b''
    if read:
        try:
            while True:
                d = s.recv(4096)
                if not d: break
                out += d
        except OSError: pass
    s.close()
    return out

def phase_abuse(host):
    print('4. abuse')
    # idle connections, more than the server keeps (MAX_CONNS 6, lwIP's 12 PCBs): a request still gets through after
    idle = [socket.create_connection((host, 80), timeout=10) for _ in range(14)]
    time.sleep(3)
    try:
        code, _, _ = get(host, '/api/status', timeout=15)
        print('  with 14 idle connections open: /api/status HTTP %d' % code)
    except OSError as e:
        print('  with 14 idle connections open: refused (%s), as busy' % type(e).__name__)
    for s in idle: s.close()
    time.sleep(2)
    try: get(host, '/api/status', timeout=15)
    except OSError as e: fail('after the idle connections closed: %s' % e)
    # half a request, held, then dropped
    for _ in range(6): raw(host, b'GET /api/status HTTP/1.1\r\nHost: x\r\n', read=False, hold=0.5)
    # dropped mid-reply (an RST), the big replies
    for _ in range(20): raw(host, b'GET /api/discs HTTP/1.1\r\n\r\n', read=False, linger0=True)
    for _ in range(10): raw(host, b'GET / HTTP/1.1\r\n\r\n', read=False, linger0=True)
    # junk, a long path, a bad method, an oversized body, unknown routes
    raw(host, bytes(random.getrandbits(8) for _ in range(3000)))
    raw(host, b'GET /' + b'a' * 3000 + b' HTTP/1.1\r\n\r\n')
    raw(host, b'BREW /api/status HTTP/1.1\r\n\r\n')
    r = raw(host, b'POST /api/command HTTP/1.1\r\nContent-Length: 65536\r\n\r\n' + b'x' * 2000)
    if b' 413 ' not in r[:20] and r: print('  (an oversized body: %r)' % r[:40])
    for p in ['/api/nope', '/nope', '/api/../etc']:
        try:
            code, _, _ = get(host, p)
            if code not in (404, 400): fail('%s: HTTP %d, not 404' % (p, code))
        except OSError as e: fail('%s: %s' % (p, e))

def usb_cmd(port, cmd, want):
    import serial
    s = serial.Serial(port, 115200, timeout=1)
    s.reset_input_buffer()
    s.write(cmd.encode() + b'\n')
    until = time.time() + 10
    while time.time() < until:
        line = s.readline().decode(errors='replace').strip()
        if line.startswith(want):
            s.close()
            return line
    s.close()
    raise TimeoutError('no %s reply to %s' % (want, cmd))

def phase_squeeze(host, port, left, seconds, app):
    print('7. the heap squeezed to %d bytes free' % left)
    print('  ' + usb_cmd(port, '!squeeze %d' % left, 'SQUEEZE'))
    try:
        print('  free now %d' % status(host)['free'])
    except (OSError, ValueError) as e:
        print('  /api/status with the heap squeezed: %s (allowed)' % type(e).__name__)
    errs = len(fails)
    phase_load(host, seconds)
    phase_abuse(host)
    del fails[errs:]        # (requests failing for want of memory is allowed here: answering at all is the test)
    if app:
        data = open(app, 'rb').read()
        try:
            code, body, _ = get(host, '/api/app', data=data, timeout=120)
            print('  an install with the heap squeezed: HTTP %d %s' % (code, body[:80]))
        except OSError as e:
            print('  an install with the heap squeezed: %s' % type(e).__name__)
        time.sleep(15)
    for _ in range(30):
        try:
            status(host)
            break
        except (OSError, ValueError):
            time.sleep(1)
    else:
        fail('with the heap squeezed the board stopped answering')
        return
    try: print('  ' + usb_cmd(port, '!squeeze off', 'SQUEEZE'))
    except (OSError, TimeoutError) as e: print('  (squeeze off: %s; a restart gives it back anyway)' % e)

def phase_install(host, app, cycles):
    data = open(app, 'rb').read()
    print('6. %d installs of %s (%d KB) over WiFi' % (cycles, app, len(data) // 1024))
    for k in range(cycles):
        t0 = time.time()
        code, body, _ = get(host, '/api/app', data=data, timeout=120)
        j = json.loads(body or b'{}')
        if code != 200 or not j.get('installed'):
            fail('install %d: HTTP %d %s' % (k + 1, code, j)); return
        want = j['app']['version']
        time.sleep(3)
        back = None
        while time.time() - t0 < 90:
            try:
                s = status(host)
                code, body, _ = get(host, '/api/app', timeout=3)
                a = json.loads(body)
                if a.get('state') == 'running' and a.get('version') == want: back = time.time() - t0; break
            except (OSError, ValueError):
                pass
            time.sleep(1)
        if back is None: fail('install %d: the board didn\'t come back running %s within 90 s' % (k + 1, want)); return
        print('  install %d: back in %.1f s, running %s' % (k + 1, back, want))
        time.sleep(20)      # (let it play a while)

def main():
    a = sys.argv[1:]
    if not a or a[0].startswith('-'): sys.exit(__doc__)
    host = a.pop(0)
    opt = lambda k, d=None: a[a.index(k) + 1] if k in a else d
    port, seconds, app, cycles = opt('--usb'), int(opt('--seconds', 30)), opt('--install'), int(opt('--cycles', 3))
    list_ms = float(opt('--list-ms', 0))
    squeeze = opt('--squeeze')
    base = health(host, None, 'start')
    if not base: sys.exit(1)
    phase_list(host, port); phase_list_speed(host, list_ms); health(host, base, 'the disc list')
    phase_serial(host); health(host, base, 'one at a time')
    phase_load(host, seconds); health(host, base, 'at once')
    phase_abuse(host); health(host, base, 'abuse')
    if app: phase_install(host, app, cycles); health(host, None, 'installs')
    if squeeze and port: phase_squeeze(host, port, int(squeeze), seconds, app); health(host, None, 'squeezed')
    print('api_stress: %s' % ('%d failed' % len(fails) if fails else 'all passed'))
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
