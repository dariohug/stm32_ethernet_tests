#!/usr/bin/env python3
"""Host-side companion for the board's PTP clock and trigger timestamps.

  ptp_tool.py status            one-line PTP servo status from the board
  ptp_tool.py check [-n N]      compare the board's clock with this PC's
  ptp_tool.py listen            stream trigger events as they happen (UTC)

Talks to the command server (tcp/5000). Board times are TAI; this converts
with the UTC offset the board learned from the master's Announce messages
(37 s if the master didn't mark it valid).
"""
import argparse
import socket
import statistics
import sys
import time
from datetime import datetime, timezone

DEFAULT_UTC_OFFSET = 37


class Board:
    def __init__(self, host, port):
        self.sock = socket.create_connection((host, port), timeout=3)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def readline(self, timeout=3):
        self.sock.settimeout(timeout)
        while b"\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("board closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode()

    def cmd(self, line):
        self.sock.sendall(line.encode() + b"\r\n")
        return self.readline()


def parse_kv(line):
    return dict(tok.split("=", 1) for tok in line.split() if "=" in tok)


def utc_offset(board):
    kv = parse_kv(board.cmd("ptp"))
    off = kv.get("utc_offset", "")
    return int(off) if off.lstrip("-").isdigit() else DEFAULT_UTC_OFFSET


def ns_from(s):
    sec, frac = s.split(".")
    return int(sec) * 1_000_000_000 + int(frac.ljust(9, "0")[:9])


def fmt_utc(tai_ns, off):
    utc_ns = tai_ns - off * 1_000_000_000
    dt = datetime.fromtimestamp(utc_ns // 1_000_000_000, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M:%S.") + f"{utc_ns % 1_000_000_000:09d}Z"


def cmd_status(board, args):
    print(board.cmd("ptp"))


def cmd_check(board, args):
    """Board time vs. this PC's CLOCK_REALTIME, over the command connection.

    Each sample brackets the board's reading between two host timestamps and
    assumes it was taken mid-way, so the result is only good to about +-RTT/2
    (typically 100-300 us over TCP) -- a sanity check that the board is on the
    right timescale, not a measurement of PTP precision. For that, watch the
    servo offset (`status`), which is measured with hardware timestamps.
    """
    off = utc_offset(board)
    diffs, rtts = [], []
    for _ in range(args.n):
        t0 = time.clock_gettime_ns(time.CLOCK_REALTIME)
        reply = board.cmd("time")
        t1 = time.clock_gettime_ns(time.CLOCK_REALTIME)
        board_tai = ns_from(reply.split()[1])
        host_tai = (t0 + t1) // 2 + off * 1_000_000_000
        diffs.append(board_tai - host_tai)
        rtts.append(t1 - t0)
        time.sleep(0.02)
    state = reply.split()[-1]
    med = statistics.median(diffs)
    rtt = statistics.median(rtts)
    print(f"board state   : {state}")
    print(f"board time    : {fmt_utc(board_tai, off)} (utc_offset {off} s)")
    print(f"board - host  : median {med / 1e3:+.1f} us, spread {(max(diffs) - min(diffs)) / 1e3:.1f} us"
          f" over {args.n} samples")
    print(f"request rtt   : median {rtt / 1e3:.1f} us (bounds the comparison to about +-{rtt / 2e3:.0f} us)")
    if state != "locked":
        print("note          : PTP is not locked yet; the board is not on master time")
        return 1
    if abs(med) > max(rtt, 1_000_000):
        print("FAIL          : board differs from host by more than the measurement can explain")
        print("                (is phc2sys keeping the master NIC's clock on system time?)")
        return 1
    print("OK")
    return 0


def cmd_listen(board, args):
    off = utc_offset(board)
    reply = board.cmd("subscribe")
    if reply != "ok":
        sys.exit(f"subscribe failed: {reply}")
    print(f"listening for trigger events (TAI-UTC {off} s), ctrl-c to stop", file=sys.stderr)
    last_seq = None
    while True:
        try:
            line = board.readline(timeout=None)
        except KeyboardInterrupt:
            return 0
        parts = line.split()
        if len(parts) != 6 or parts[0] != "ev":
            continue
        _, seq, chan, t, locked, rms = parts
        seq = int(seq)
        if last_seq is not None and seq != last_seq + 1:
            print(f"# {seq - last_seq - 1} event(s) lost", file=sys.stderr)
        last_seq = seq
        flag = "" if locked == "locked" else "  (ptp not locked)"
        print(f"{seq:6d}  {chan:6s}  {t} TAI  {fmt_utc(ns_from(t), off)}  +-{int(rms) / 1e3:.1f} us{flag}",
              flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.10")
    ap.add_argument("--port", type=int, default=5000)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status")
    p = sub.add_parser("check")
    p.add_argument("-n", type=int, default=50)
    sub.add_parser("listen")
    args = ap.parse_args()

    board = Board(args.host, args.port)
    sys.exit({"status": cmd_status, "check": cmd_check, "listen": cmd_listen}[args.cmd](board, args) or 0)


if __name__ == "__main__":
    main()
