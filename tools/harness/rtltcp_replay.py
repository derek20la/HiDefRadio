#!/usr/bin/env python3
"""rtltcp_replay.py - a stand-in rtl_tcp server that plays a .cu8 recording (1.2, harness only).

    python3 rtltcp_replay.py file.cu8 [port] [--speed X] [--live] [--tuner N] [--gains N] [--trace calls.txt]
                             [--cut-after S]

It speaks the rtl_tcp protocol exactly as the real server does (iqsource_tcp.hpp has the
whole protocol): on connect it sends "RTL0" + tuner type + number of gain steps (32-bit
big-endian), then raw cu8 samples; it reads the client's 5-byte commands and logs them.

Two ways to send:
  default ("lockstep", for comparing runs): nothing is sent until the client's first
      frequency command; 0.2 s after it the recording plays from its first byte. The app throws
      away the first 0.4 s after a tune (TcpSource::TUNE_DROP_S, counted in bytes), so the
      engine then sees exactly the recording minus its first 1,190,700 bytes - the same as the
      USB stand-in (rtl_stub.c) playing a file cut that way (`tail -c +1190701`). Played at
      --speed times real time (default 3): the harness engine keeps up easily, so nothing is
      dropped and a run gives the same audio every time.
  --live: like a real server - samples flow from the moment of connecting, in real time
      (--speed 1), whatever the client does; a retune just carries on in the file.
--cut-after S: close the connection after S seconds of samples (a server that dies / Wi-Fi gone).
--tuner / --gains: what the greeting says (default 5 = R820T, 29 steps).
One client, then it exits. Plain Python 3, no packages.
"""
import socket, struct, sys, threading, time

args = sys.argv[1:]
def opt(name, default, conv=str):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return conv(v)
    return default
speed = opt('--speed', None, float)
tuner = opt('--tuner', 5, int)
gains = opt('--gains', 29, int)
trace = opt('--trace', None)
cut_after = opt('--cut-after', None, float)
live = '--live' in args
if live: args.remove('--live')
path = args[0]
port = int(args[1]) if len(args) > 1 else 1234
if speed is None: speed = 1.0 if live else 3.0
RATE = 1488375
CHUNK = 262144

names = {1: 'freq', 2: 'rate', 3: 'gain_mode', 4: 'gain', 5: 'ppm', 6: 'if_gain', 7: 'test_mode',
         8: 'agc', 9: 'direct_sampling', 10: 'offset_tuning', 13: 'gain_by_index', 14: 'bias_tee'}
tuned = threading.Event()
log = open(trace, 'w') if trace else None

srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', port)); srv.listen(1)
print(f"rtltcp_replay: listening on 127.0.0.1:{port} ({'live' if live else 'lockstep'}, x{speed})", file=sys.stderr, flush=True)
conn, _ = srv.accept()
conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
conn.sendall(b'RTL0' + struct.pack('>II', tuner, gains))

def commands():
    buf = b''
    while True:
        try:
            d = conn.recv(4096)
        except OSError:
            return
        if not d: return
        buf += d
        while len(buf) >= 5:
            cmd, val = struct.unpack('>BI', buf[:5]); buf = buf[5:]
            line = f"{names.get(cmd, hex(cmd))} {val}"
            if log: log.write(line + '\n'); log.flush()
            if cmd == 1 and not tuned.is_set(): tuned.set()
threading.Thread(target=commands, daemon=True).start()

if not live:
    tuned.wait(); time.sleep(0.2)
sent = 0; t0 = time.monotonic()
with open(path, 'rb') as f:
    while True:
        b = f.read(CHUNK)
        if not b: break
        try:
            conn.sendall(b)
        except OSError:
            break
        sent += len(b)
        secs = sent / 2 / RATE
        if cut_after is not None and secs >= cut_after: break
        ahead = secs / speed - (time.monotonic() - t0)
        if ahead > 0: time.sleep(ahead)
print(f"rtltcp_replay: sent {sent} bytes ({sent / 2 / RATE:.1f} s of samples), closing", file=sys.stderr, flush=True)
try:
    conn.shutdown(socket.SHUT_RDWR)
except OSError:
    pass
conn.close()
