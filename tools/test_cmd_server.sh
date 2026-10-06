#!/usr/bin/env bash
# Exercises the LED command server (Core/Src/cmd_server.c) over TCP and
# measures the link's latency/throughput.
# Usage: ./test_cmd_server.sh [host] [port]
set -uo pipefail

HOST="${1:-192.168.1.10}"
PORT="${2:-5000}"
DURATION="${DURATION:-3}"   # seconds spent in the throughput measurement

fail() { echo "FAIL: $*" >&2; exit 1; }

# --- preflight -------------------------------------------------------------
# Done explicitly rather than letting `set -e` abort on a failed nc, which
# exits silently and looks like the script did nothing.
IFACE="$(ip -o route get "$HOST" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')"
[ -n "$IFACE" ] || fail "no route to $HOST — is the board's subnet configured on a local NIC?"

if ! timeout 3 bash -c "cat < /dev/null > /dev/tcp/$HOST/$PORT" 2>/dev/null; then
  echo "no TCP connection to $HOST:$PORT (via $IFACE)" >&2
  echo "  carrier: $(cat "/sys/class/net/$IFACE/carrier" 2>/dev/null || echo '?')  (1 = cable link up)" >&2
  echo "  arp    : $(ip neigh show "$HOST" dev "$IFACE" 2>/dev/null || echo 'no entry')" >&2
  fail "board not reachable"
fi

send() {
  local cmd="$1" expect_prefix="$2" reply
  reply="$(printf '%s\r\n' "$cmd" | nc -w2 "$HOST" "$PORT")" \
    || fail "connection dropped while sending '$cmd'"
  printf '> %-20s -> %s\n' "$cmd" "${reply//$'\r\n'/}"
  if [ -n "$expect_prefix" ] && [[ "$reply" != "$expect_prefix"* ]]; then
    fail "expected prefix '$expect_prefix' for '$cmd'"
  fi
}

echo "== help/status =="
send "help" "commands:"
send "status" "led1="

# Start from a known state; LED state persists across runs.
echo "== reset to known state =="
send "led1 off" "ok"
send "led2 off" "ok"
send "led3 off" "ok"
send "status" "led1=off led2=off led3=off"

echo "== led1 on/off/toggle =="
send "led1 on" "ok"
send "status" "led1=on"
send "led1 off" "ok"
send "status" "led1=off"
send "led1 toggle" "ok"          # off -> on
send "status" "led1=on"

echo "== led2, led3 =="
send "led2 on" "ok"
send "led3 on" "ok"
send "status" "led1=on led2=on led3=on"

echo "== error paths =="
send "led1 sideways" "err bad arg"
send "led9 on" "err unknown command"
send "bogus" "err unknown command"

echo "== cleanup =="
send "led2 off" "ok"
send "led3 off" "ok"

# --- performance -----------------------------------------------------------
echo
echo "== link =="
echo "interface     : $IFACE"
echo "negotiated    : $(cat "/sys/class/net/$IFACE/speed" 2>/dev/null || echo '?') Mb/s \
$(cat "/sys/class/net/$IFACE/duplex" 2>/dev/null || echo '')"

echo "icmp rtt      : $(ping -c 5 -i 0.2 -q "$HOST" 2>/dev/null \
  | sed -n 's#.*= \([^ ]*\) ms#\1 ms (min/avg/max/mdev)#p' || echo 'no reply')"

echo
echo "== throughput (${DURATION}s of back-to-back commands on one connection) =="
python3 - "$HOST" "$PORT" "$DURATION" <<'PY' || fail "throughput measurement failed"
import socket, sys, time

host, port, duration = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
req = b"status\r\n"

s = socket.create_connection((host, port), timeout=5)
s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
buf = b""

def roundtrip():
    """One request -> one CRLF-terminated reply. Returns bytes received."""
    global buf
    s.sendall(req)
    n = 0
    while b"\r\n" not in buf:
        chunk = s.recv(512)
        if not chunk:
            raise RuntimeError("server closed the connection")
        buf += chunk
        n += len(chunk)
    line, buf = buf.split(b"\r\n", 1)
    return len(line) + 2

for _ in range(20):          # warm up: ARP, window opening, first-touch paths
    roundtrip()

lat, rx, count = [], 0, 0
end = time.perf_counter() + duration
while time.perf_counter() < end:
    t0 = time.perf_counter()
    rx += roundtrip()
    lat.append((time.perf_counter() - t0) * 1e3)
    count += 1

elapsed = sum(lat) / 1e3
tx = count * len(req)
lat.sort()
print(f"commands      : {count} in {elapsed:.2f}s -> {count/elapsed:,.0f} req/s")
print(f"rtt           : min {lat[0]:.3f} ms  median {lat[len(lat)//2]:.3f} ms  "
      f"p99 {lat[int(len(lat)*0.99)]:.3f} ms  max {lat[-1]:.3f} ms")
print(f"app payload   : {(tx+rx)/elapsed/1024:,.1f} KiB/s "
      f"(tx {tx/elapsed/1024:,.1f} + rx {rx/elapsed/1024:,.1f})")
print(f"note          : request/response is latency-bound, not a link-rate test;")
print(f"                the 'negotiated' line above is the actual link speed.")
s.close()
PY

echo
echo "ALL PASSED"
