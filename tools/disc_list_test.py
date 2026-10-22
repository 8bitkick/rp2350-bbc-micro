#!/usr/bin/env python3
"""Check the SD card's disc list is whole: every disc the card scan finds is in the command
layer's JSON reply, over USB and over HTTP (the web page's list), and the board keeps answering.

  tools/disc_list_test.py [repeats]     (default 20; needs the board on USB, a card in it, and
                                         WiFi for the HTTP part: PRESTO_HOST, else the board's
                                         address from "!net")

The card scan is "!discs" (one "DISC <name> <size>" line each, then "DISCS <n>"); the JSON is
"!api discs" and GET /api/discs. A list cut short (the reply buffer full) fails. The HTTP list is
then fetched <repeats> times, with /api/status between, and the free RAM compared before and after.
"""
import json, os, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import presto


def card_scan(s):
    presto.command(s, 'discs')
    discs = {}
    while True:
        line = presto.readline_prefix(s, 'DISC')
        if line.startswith('DISCS '):
            if line == 'DISCS no card':
                sys.exit('FAIL: no card in the board')
            n = int(line.split()[1])
            if n != len(discs):
                sys.exit('FAIL: "!discs" listed %d but counted %d' % (len(discs), n))
            return discs
        name, size = line[5:].rsplit(' ', 1)
        discs[name] = int(size)


def api_usb(s, name):
    presto.command(s, 'api ' + name)
    line = presto.readline_prefix(s, 'API ')
    code, body = line[4:].split(' ', 1)
    return int(code), body


def http_get(host, path):
    with urllib.request.urlopen('http://%s%s' % (host, path), timeout=10) as r:
        return r.status, r.read().decode()


def check(where, code, body, want):
    if code != 200:
        sys.exit('FAIL %s: HTTP-style code %d' % (where, code))
    try:
        j = json.loads(body)
    except ValueError as e:
        sys.exit('FAIL %s: not JSON (%s), %d bytes, ends %r' % (where, e, len(body), body[-60:]))
    got = {d['name']: d['size'] for d in j['discs']}
    if not j.get('card'):
        sys.exit('FAIL %s: "card" false' % where)
    missing = sorted(set(want) - set(got))
    extra = sorted(set(got) - set(want))
    if missing or extra or got != want:
        sys.exit('FAIL %s: %d of %d discs (missing %d: %s%s)%s' % (
            where, len(got), len(want), len(missing), ', '.join(missing[:5]), ', ...' if len(missing) > 5 else '',
            ' extra: %s' % extra if extra else ''))
    print('ok   %s: all %d discs, %d bytes of JSON' % (where, len(got), len(body)))


def main():
    repeats = int(sys.argv[1]) if len(sys.argv) > 1 else 20
    s = presto.open_port()
    want = card_scan(s)
    print('card %d discs' % len(want))
    check('USB !api discs', *api_usb(s, 'discs'), want)

    host = os.environ.get('PRESTO_HOST', '').replace('http://', '')
    if not host:
        presto.command(s, 'net')
        net = presto.readline_prefix(s, 'NET')
        ip = [w[3:] for w in net.split() if w.startswith('ip=')]
        if 'up' not in net.split() or not ip:
            sys.exit('FAIL: no WiFi (%s); set PRESTO_HOST' % net)
        host = ip[0]
    free0 = json.loads(api_usb(s, 'status')[1])['free']
    check('HTTP /api/discs', *http_get(host, '/api/discs'), want)
    t0 = time.time()
    for i in range(repeats):
        code, body = http_get(host, '/api/discs')
        if code != 200 or len(json.loads(body)['discs']) != len(want):
            sys.exit('FAIL HTTP /api/discs, repeat %d' % (i + 1))
        code, body = http_get(host, '/api/status')
        if code != 200:
            sys.exit('FAIL HTTP /api/status, repeat %d: %d' % (i + 1, code))
    time.sleep(1)
    free1 = json.loads(api_usb(s, 'status')[1])['free']
    print('ok   HTTP /api/discs + /api/status x%d in %.1f s; free RAM %d -> %d bytes' % (
        repeats, time.time() - t0, free0, free1))
    if free1 < free0 - 512:
        sys.exit('FAIL: free RAM fell by %d bytes' % (free0 - free1))
    print('PASS')


if __name__ == '__main__':
    main()
