#!/usr/bin/env bash
# Runs this PC as PTP grandmaster for the board, as two transient systemd
# units so they keep running in the background:
#   nucleo-ptp4l    ptp4l on the board-facing NIC, hardware timestamping
#   nucleo-phc2sys  keeps that NIC's PTP hardware clock on the system clock,
#                   so the board ends up on this PC's wall-clock time (as TAI)
# If ufw is active, also allows PTP (udp/319-320) in on that NIC only; the
# board's Delay_Req messages are otherwise dropped by the default policy.
#
# Usage: sudo tools/ptp_master.sh start|stop|status [iface]
#        (iface defaults to the one routing to the board, 192.168.1.10)
set -euo pipefail

CMD="${1:-status}"
BOARD="${BOARD:-192.168.1.10}"
IFACE="${2:-$(ip -o route get "$BOARD" 2>/dev/null | sed -n 's/.* dev \([^ ]*\).*/\1/p')}"
CONF="$(cd "$(dirname "$0")" && pwd)/ptp4l-master.conf"
UNITS=(nucleo-phc2sys nucleo-ptp4l)

need_root() { [ "$(id -u)" = 0 ] || { echo "needs root: sudo $0 $CMD" >&2; exit 1; }; }
ufw_active() { command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q '^Status: active'; }

case "$CMD" in
start)
  need_root
  command -v ptp4l >/dev/null || { echo "ptp4l not found: sudo apt install linuxptp" >&2; exit 1; }
  [ -n "$IFACE" ] || { echo "no route to $BOARD; configure the board-facing NIC first (see README)" >&2; exit 1; }
  ethtool -T "$IFACE" | grep -q 'hardware-transmit' \
    || { echo "$IFACE has no hardware timestamping; edit time_stamping in $CONF" >&2; exit 1; }

  if ufw_active; then
    ufw allow in on "$IFACE" proto udp to any port 319:320 comment 'PTP for NUCLEO' >/dev/null
    echo "ufw: allowed udp/319-320 in on $IFACE"
  fi

  systemctl stop "${UNITS[@]}" 2>/dev/null || true
  systemd-run --quiet --collect --unit=nucleo-ptp4l \
    ptp4l -f "$CONF" -i "$IFACE" -m
  sleep 1 # phc2sys -a discovers ports through ptp4l's management socket
  systemd-run --quiet --collect --unit=nucleo-phc2sys \
    phc2sys -a -rr -m
  echo "started ptp4l + phc2sys on $IFACE"
  echo "  logs: journalctl -fu nucleo-ptp4l -u nucleo-phc2sys"
  echo "  stop: sudo $0 stop"
  ;;
stop)
  need_root
  systemctl stop "${UNITS[@]}" 2>/dev/null || true
  if ufw_active && [ -n "$IFACE" ]; then
    ufw delete allow in on "$IFACE" proto udp to any port 319:320 >/dev/null 2>&1 || true
  fi
  echo "stopped"
  ;;
status)
  for u in "${UNITS[@]}"; do
    printf '%-16s %s\n' "$u" "$(systemctl is-active "$u" 2>/dev/null || true)"
  done
  journalctl -u nucleo-ptp4l -u nucleo-phc2sys -n 10 --no-pager 2>/dev/null || true
  ;;
*)
  echo "usage: sudo $0 start|stop|status [iface]" >&2
  exit 2
  ;;
esac
