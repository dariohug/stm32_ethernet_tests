#!/usr/bin/env bash
# Exercises the LED command server (Core/Src/cmd_server.c) over TCP.
# Usage: ./test_cmd_server.sh [host] [port]
set -euo pipefail

HOST="${1:-192.168.1.10}"
PORT="${2:-5000}"

send() {
  local cmd="$1" expect_prefix="$2"
  local reply
  reply="$(printf '%s\r\n' "$cmd" | nc -w2 "$HOST" "$PORT")"
  printf '> %-20s -> %s\n' "$cmd" "${reply//$'\r\n'/}"
  if [[ -n "$expect_prefix" && "$reply" != "$expect_prefix"* ]]; then
    echo "FAIL: expected prefix '$expect_prefix'" >&2
    exit 1
  fi
}

echo "== help/status =="
send "help" "commands:"
send "status" "led1="

echo "== led1 on/off/toggle =="
send "led1 on" "ok"
send "led1 off" "ok"
send "led1 toggle" "ok"

echo "== led2, led3 =="
send "led2 on" "ok"
send "led3 on" "ok"
send "status" "led1=off led2=on led3=on"

echo "== error paths =="
send "led1 sideways" "err bad arg"
send "led9 on" "err unknown command"
send "bogus" "err unknown command"

echo "== cleanup =="
send "led2 off" "ok"
send "led3 off" "ok"

echo "ALL PASSED"
