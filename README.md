# stm32_ethernet_test

Bare-metal Ethernet bring-up on a **NUCLEO-H723ZG** (STM32H723, Cortex-M7 @ 550 MHz):
RMII + LAN8742A PHY, LwIP (raw API, no RTOS) on a static IP, and a tiny
line-oriented TCP server that drives the three onboard LEDs. The board's
Ethernet hardware clock is synchronised to the PC over **PTP (IEEE 1588)**, and
edges on trigger inputs (camera shutter, external interrupts, the user button)
are timestamped with it. A serial console logs what's going on.

```
 PC 192.168.1.1  ──── Ethernet cable ────  NUCLEO-H723ZG 192.168.1.10
   ptp4l (master)    ── udp/319,320 ────▶  ptp.c slave ──▶ ETH MAC hardware clock
   nc / ptp_tool.py  ───── tcp/5000 ─────▶  cmd_server  ──▶  LD1 / LD2 / LD3
                                                       ◀──  trigger events
                                            PC13 / PE9 / PE11 edges ─┘ (timestamped)
   /dev/ttyACM0      ◀──── USART3 ───────  eth_log (printf, traffic stats)
```

## Contents

- [Requirements](#requirements)
- [Build & flash](#build--flash)
- [Network setup](#network-setup)
- [Using the command server](#using-the-command-server)
- [Precise time (PTP)](#precise-time-ptp)
- [Trigger timestamps](#trigger-timestamps)
- [Serial console](#serial-console)
- [Testing](#testing)
- [Project layout](#project-layout)
- [Regenerating with CubeMX](#regenerating-with-cubemx)
- [Pitfalls](#pitfalls)

## Requirements

- NUCLEO-H723ZG (onboard ST-LINK, USB cable) and an Ethernet cable to your PC
- `arm-none-eabi-gcc`, CMake ≥ 3.22, Ninja
- OpenOCD (flashing)
- STM32CubeMX (only if you regenerate code from the `.ioc`)
- `nc`, `python3` (for the test scripts)
- `linuxptp` (`sudo apt install linuxptp`) and a PC NIC with hardware
  timestamping (`ethtool -T <iface>` lists `hardware-transmit`) for PTP

> `Drivers/` and `Middlewares/` (HAL, CMSIS, LwIP sources) are **not committed**,
> because CubeMX generates them. On a fresh clone, run a
> [CubeMX regeneration](#regenerating-with-cubemx) once before building.

## Build & flash

```sh
cmake --preset Debug            # or: Release
cmake --build build/Debug       # → build/Debug/stm32_ethernet_test.elf

openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "program build/Debug/stm32_ethernet_test.elf verify reset exit"
```

`compile_commands.json` is exported to `build/Debug/`, and `.clangd` already
points there.

## Network setup

The board has a fixed IP and DHCP is off. It's meant for a direct cable to your
PC, with no router in between:

|          | IP             | Netmask         |
|----------|----------------|-----------------|
| Board    | `192.168.1.10` | `255.255.255.0` |
| PC (you) | `192.168.1.1`  | `255.255.255.0` |

Find the NIC the cable is plugged into (`ip link`; it shows `LOWER_UP` once
the link is up), then:

```sh
sudo ip addr add 192.168.1.1/24 dev <iface>
sudo ip link set <iface> up
ping 192.168.1.10
```

If NetworkManager manages the NIC and its profile is set to DHCP, it drops the
address above when its DHCP attempt times out (about 45 s). Tests then fail
partway through with `connection dropped`. Instead, switch the device to a
static address for the current session. This needs no root and doesn't change
the saved profile:

```sh
nmcli device modify <iface> ipv4.method manual ipv4.addresses 192.168.1.1/24
```

To change the board's address, edit `LWIP.IP_ADDRESS` / `NETMASK_ADDRESS` /
`GATEWAY_ADDRESS` in the `.ioc` and regenerate.

## Using the command server

TCP port **5000**. The protocol is plain text, one command per line, ending in
`\n` or `\r\n`:

```
$ nc 192.168.1.10 5000
status
led1=off led2=off led3=off
led1 on
ok
led2 toggle
ok
nonsense
err unknown command
```

| Command                             | Reply                            |
|-------------------------------------|----------------------------------|
| `led1\|led2\|led3 on\|off\|toggle`  | `ok` / `err bad arg`             |
| `status`                            | `led1=on\|off led2=… led3=…`     |
| `time`                              | `time <sec>.<nsec> tai utc_offset=<n\|?> <state>` |
| `ptp`                               | `ptp state=… offset=… rms=… delay=… freq=… …` (servo status, ns / ppb) |
| `events`                            | queued trigger events as `ev …` lines, then `end <n>` |
| `subscribe` / `unsubscribe`         | `ok`; this connection then gets `ev …` lines pushed as edges happen |
| `uptime`                            | `uptime <ms>` since boot, from the raw oscillator (diagnostics) |
| `help`                              | one-line command summary         |
| anything else                       | `err unknown command`            |

| Name | LED      | Pin  |
|------|----------|------|
| led1 | LD1 green  | PB0  |
| led2 | LD2 yellow | PE1  |
| led3 | LD3 red    | PB14 |

## Precise time (PTP)

The PC runs linuxptp as PTP grandmaster; the board is a slave-only ordinary
clock (`Core/Src/ptp.c`) that disciplines the STM32's Ethernet MAC hardware
clock to it. Both ends timestamp PTP frames in hardware, so network and
software latency don't enter the result. The board keeps correcting its clock
on every Sync for as long as the master runs, which is what tracks oscillator
drift.

### Quick start

With the [network](#network-setup) up:

```sh
sudo apt install linuxptp           # once
sudo tools/ptp_master.sh start      # ptp4l + phc2sys in the background
tools/ptp_tool.py status            # wait for state=locked (a few seconds)
tools/ptp_tool.py check             # board time vs. this PC's clock
sudo tools/ptp_master.sh stop
```

`ptp_master.sh start` runs two transient systemd units, using
`tools/ptp4l-master.conf`:

- `nucleo-ptp4l`: `ptp4l` on the board-facing NIC as master-only, with
  hardware timestamping and 64 Syncs/s.
- `nucleo-phc2sys`: `phc2sys -a -rr`, which keeps that NIC's PTP hardware clock
  on the PC's system clock. The board therefore ends up on the PC's wall-clock
  time.

If ufw is active, the script also allows udp/319–320 in on that NIC only.
Without that rule the board's Delay_Req messages are dropped. `stop` removes the
rule again. Logs: `journalctl -fu nucleo-ptp4l -u nucleo-phc2sys`.

### How it works

- **Protocol:** PTPv2 over UDP/IPv4 multicast (224.0.1.129), end-to-end delay
  mechanism, domain 0, one- or two-step. These are ptp4l's defaults. The first
  master heard is used; there is no BMCA, because this is built for one master
  on a direct link.
- **Clock:** the MAC's IEEE 1588 system time, clocked from HCLK (275 MHz) in
  fine-update mode with 5 ns resolution. At boot the firmware checks that rate
  against the CPU cycle counter and prints it (`rate vs CPU … ppm`, expect ~0).
- **Servo:** a PI controller adjusts the clock's frequency (addend register)
  on every Sync. On first lock it measures the frequency error from two Syncs,
  then steps the clock onto master time in one jump. If the offset ever
  exceeds 1 ms it steps again. If the master goes quiet for 5 s, the clock
  keeps running at the last corrected frequency.
- **Timescale:** TAI, PTP's native timescale. `utc_offset` comes from the
  master's Announce messages. ptp4l sends 37 but doesn't mark it valid, so it
  shows as `?`/`(unset)`, and `ptp_tool.py` falls back to 37 s.
- **Locked:** `state=locked` once the RMS offset over the last 8 Syncs is
  below 10 µs (`PTP_LOCK_THRESHOLD_NS`).

### Measured performance (stock NUCLEO-H723ZG, direct cable)

| | |
|---|---|
| Offset from master, RMS | **~1.6 µs** (64 Syncs/s, over 2 min) |
| Offset from master, worst case | ~6 µs |
| Timestamp noise (path delay jitter) | ±10 ns |
| Board raw oscillator error | about +2300 ppm, wandering ±80–120 ppm second-to-second |

The limit is the board's oscillator, not PTP. The NUCLEO-H723ZG ships without
an HSE crystal (X3 is unpopulated), so the MCU runs from its internal HSI RC
oscillator. The HSI wanders by tens of ppm within a second, and the servo can
only follow that so closely between Syncs. The offset shrinks roughly in
proportion to the Sync interval: it was 12 µs at 8 Syncs/s and 1.6 µs at 64.

**To get to sub-µs / ~100 ns:** give the MCU a real crystal or TCXO on HSE.
Fit X3 plus its load capacitors and solder bridges (see the board manual,
UM2407), or feed an external reference into PH0. Then switch the `.ioc` to HSE
(PLL source HSE, adjust `DIVM1` to keep 2 MHz at the PLL input). The board's
default HSE, the 8 MHz MCO from the ST-LINK, doesn't help. Measured here, it
ran 0.43% fast and wandered ±235 ppm, worse than the HSI.

### Absolute accuracy

PTP measures the *round trip* and assumes both directions take equally long.
Any asymmetry becomes a constant offset that PTP cannot see. The measured path
delay here is ~10.7 µs, far more than a cable, so most of it is NIC/PHY
timestamping latency. If those latencies differ between directions, the board
can sit a few µs off the PC's clock, steadily. Calibrate against an external
reference if that matters. For example, put a scope on a trigger input and a
known PPS, then set `PTP_RX_LATENCY_NS` / `PTP_TX_LATENCY_NS` in `ptp.c`.
`ptp_tool.py check` only compares over TCP, to about ±150 µs, so it catches a
wrong timescale but not µs-level bias.

The PC's own system clock is only as good as its NTP sync. phc2sys keeps the
NIC's clock within ~0.3 µs of it.

## Trigger timestamps

Rising edges on these inputs are timestamped with the PTP clock and queued:

| Channel | Name    | Pin  | Where              | Pull |
|---------|---------|------|--------------------|------|
| 0       | `btn`   | PC13 | blue user button B1 | (board) |
| 1       | `trig1` | PE9  | Zio CN10 **D6**    | down |
| 2       | `trig2` | PE11 | Zio CN10 **D5**    | down |

Drive the trigger pins with 3.3 V logic, sharing GND with the board. Pins,
pulls and edge selection live in the `.ioc` (labels `TRIG_*`). To add a
channel, add an EXTI pin there and a row in `channels[]` in
`Core/Src/trig_events.c`.

Read events by polling (`events`), or keep a connection open and stream them:

```sh
tools/ptp_tool.py listen
     0  trig1   1791296684.360744986 TAI  2026-10-06 14:24:07.360744986Z  +-0.7 us
     1  trig2   1791296684.360744986 TAI  2026-10-06 14:24:07.360744986Z  +-0.7 us
```

On the wire, an event line is `ev <seq> <name> <tai_sec>.<nsec> <locked|unlocked> <rms_ns>`.

- **`seq`** counts every captured edge, so a gap means events were dropped.
  This happens if 64 are queued and nobody reads them; `ptp` reports
  `dropped_events`.
- **`rms_ns`** is the PTP servo's recent RMS offset at the time of the edge.
  Treat it as that timestamp's uncertainty.
- **`locked`** says whether PTP was locked when the edge happened.

How an edge is timestamped:

- **Capture:** the clock is read first thing in the EXTI interrupt, which has
  the highest priority. A measured 250 ns of interrupt latency is subtracted
  (`TRIG_IRQ_LATENCY_NS`); the jitter is about ±25 ns. A higher-priority or
  interrupts-off section can delay that read. The firmware has only very short
  ones, but this is software timestamping of the edge, not a hardware capture.
- **Simultaneous edges:** edges on several pins at once get identical
  timestamps, even across the two EXTI vectors.
- **Button bounce:** the button bounces, so one press can produce several
  events. External trigger signals don't have this problem.
- **Serial echo:** the serial console echoes edges, at most 5 per second. The
  queue still gets every edge.

## Serial console

`printf` goes to USART3, which is the ST-LINK virtual COM port
(`/dev/ttyACM0`, **115200 8N1**):

```sh
picocom -b 115200 /dev/ttyACM0     # or: screen /dev/ttyACM0 115200
```

```
[eth] up: 192.168.1.10, command server on tcp/5000
[ptp] hw clock 5 ns resolution, rate vs CPU -3 ppm
[ptp] slave, clock id 0080e1.fffe.000000 port 1
[ptp] master a82bdd.fffe.57389a port 1
[ptp] clock stepped by +1791295483.227347576 s, frequency -2678006 ppb
[ptp] locked (offset rms 1450 ns)
[ptp] offset -72 ns, rms 1021 ns, path delay 10719 ns, freq -2540821 ppb
[trig] btn #0 at 1791295773.228132336
[eth] ping from 192.168.1.1
[eth] 412 pkt/s (icmp 0 tcp 412 udp 0 ptp 135 other 0), 23690 B/s
```

PTP prints its state changes, and a status line every 10 s while it is a slave.
PTP traffic is counted in the `[eth]` summary, but it doesn't trigger the
summary by itself.

Pings are logged per packet, with rate limiting. Other traffic is only counted
and summarised once a second. At 115200 baud a log line blocks for about 5 ms,
so per-packet logging would dominate the main loop. The hook is LwIP's
`LWIP_HOOK_IP4_INPUT`, wired up in `lwipopts.h`.

## Testing

```sh
tools/test_cmd_server.sh [host] [port]      # defaults: 192.168.1.10 5000
DURATION=10 tools/test_cmd_server.sh        # longer throughput run
```

The script runs these steps in order:

1. Preflight: checks the route to the board and TCP reachability. If it fails,
   it prints carrier and ARP diagnostics.
2. Functional tests: every LED command plus the error paths. It resets the LEDs
   to a known state first.
3. Performance: reports negotiated link speed, ICMP RTT, and request/response
   rate and latency (min, median, p99, max) on one persistent connection.

It ends with `ALL PASSED` or exits non-zero with `FAIL: …`.

For PTP and triggers, use `tools/ptp_tool.py`:

- `status`: servo state, offset, RMS, path delay, frequency.
- `check`: the board must be locked and agree with the PC's clock to within
  the TCP measurement bound. Exits non-zero otherwise.
- `listen`: streams trigger events as they happen.

Without wiring anything, you can fire all three trigger lines from the
debugger by writing the EXTI software-interrupt register:

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg -c "init; mww 0x58000008 0x2A00; exit"
```

## Project layout

| Path | What it is |
|------|------------|
| `stm32_ethernet_test.ioc` | **Source of truth**: pins, peripherals, clocks, MPU, LwIP config |
| `Core/Src/cmd_server.c` | TCP LED command server (hand-written) |
| `Core/Src/eth_log.c` | `printf` → USART3 retarget and inbound traffic logger (hand-written) |
| `Core/Src/ptp_clock.c` | ETH MAC hardware clock: init, read, step, frequency trim, Rx/Tx timestamps (hand-written) |
| `Core/Src/ptp.c` | PTPv2 slave: UDP 319/320, Sync/Follow_Up/Delay_Req/Delay_Resp/Announce, PI servo (hand-written) |
| `Core/Src/trig_events.c` | EXTI trigger capture, event queue, serial echo (hand-written) |
| `Core/Src/main.c` | CubeMX-generated; user code only inside `USER CODE` blocks |
| `LWIP/Target/lwipopts.h` | LwIP options; heap pointer + input hook overrides in `USER CODE BEGIN 1` |
| `cmake/eth_ram_d2.ld` | Linker fragment that puts ETH DMA descriptors/Rx pool in D2 SRAM (hand-written) |
| `CMakeLists.txt` | Top-level build; user sections add the files above |
| `tools/test_cmd_server.sh` | Functional and performance test from the PC |
| `tools/ptp4l-master.conf` | linuxptp config for the PC as master |
| `tools/ptp_master.sh` | Starts/stops ptp4l + phc2sys (+ ufw rule) on the PC |
| `tools/ptp_tool.py` | PTP status, board-vs-PC time check, trigger event stream |

## Regenerating with CubeMX

Change anything CubeMX owns in the `.ioc`, not in generated C. Then
regenerate, either from the GUI ("Generate Code") or headlessly:

```sh
cat > /tmp/cubemx.txt <<EOF
config load $PWD/stm32_ethernet_test.ioc
project generate
exit
EOF
~/STM32CubeMX/STM32CubeMX -q /tmp/cubemx.txt
```

Hand-written code lives only in `USER CODE BEGIN/END` blocks or in files that
CubeMX doesn't generate, so it survives regeneration. To check, commit first,
regenerate, and run `git diff`. The only acceptable diff is CubeMX's own churn
(timestamps, ordering).

CubeMX does **not** manage these, so re-check them after any `.ioc` change to
ETH or LwIP memory sizing:

- **`cmake/eth_ram_d2.ld`**: CubeMX's linker script defines `RAM_D2` but never
  places `.RxDescripSection` / `.TxDescripSection` / `.Rx_PoolSection` in it.
  Without this fragment they land outside the non-cacheable MPU region. The
  build succeeds, but the board never receives a packet.
- **`LWIP_RAM_HEAP_POINTER`** (in `lwipopts.h`) points at the linker symbol
  `_eth_ram_end` instead of CubeMX's fixed `0x30004000`. With 12 Rx buffers the
  descriptors + Rx pool take about 19 KB, which overruns CubeMX's assumed 16 KB
  split and would alias the heap into the Rx pool. Keep total `RAM_D2` usage
  within 32 KB.
- **`DATA_IN_D2_SRAM`** (defined in `CMakeLists.txt`): D2 SRAM is unclocked out
  of reset, and `SystemInit()` only enables it when this symbol is defined.
  Without it the link comes up, but the first received packet HardFaults.

## Pitfalls

These each cost a day once:

- **The D2 SRAM MPU region must be `TEX_LEVEL1`, not `LEVEL0`.** TEX=000 with
  C=B=0 is *Strongly-Ordered* memory, where unaligned accesses fault. LwIP
  parses IP headers in place at byte offset 14, so the link comes up and then
  the first ping HardFaults. `TEX_LEVEL1` + non-cacheable gives Normal
  non-cacheable memory, which is still DMA-coherent and allows unaligned access.
- **CubeMX can silently emit a broken `SystemClock_Config()`.** If it can't
  solve the clock tree, it skips `HAL_RCC_OscConfig()` but still switches
  SYSCLK to the unconfigured PLL, and the board hangs in `Error_Handler()`.
  The only warning is `IP not ready for code generation: Clock` in
  `~/.stm32cubemx/STM32CubeMX.log`. After any clock edit, grep the generated
  `main.c` for `HAL_RCC_OscConfig` before flashing. The clock tree currently
  runs from HSI (`DIVM1=32`, `DIVN1=275`, giving 550 MHz). See
  [Precise time](#measured-performance-stock-nucleo-h723zg-direct-cable)
  for why it isn't on HSE.
- **CubeMX renumbers `Mcu.PinN` entries when it saves the `.ioc`.** It saves
  on every generation, even a headless one. Never script `.ioc` edits by pin
  *index*; match on the pin *name*. Deleting what used to be the last two
  entries once removed the LwIP and SysTick virtual pins (`VP_*`). CubeMX then
  silently dropped LwIP and deleted `LWIP/`, `lwipopts.h` user code included.
- **The HAL's PTP API (`HAL_ETH_USE_PTP`, FW_H7 1.13.0) is not used, on
  purpose.** `HAL_ETH_PTP_SetTime` adds to the clock (TSUPDT) instead of
  setting it, `HAL_ETH_PTP_AddTimeOffset` also corrupts the addend, and the
  define changes `ETH_HandleTypeDef`'s layout everywhere. `ptp_clock.c`
  programs the registers directly. It also relies on two HAL details. First,
  Rx timestamps arrive via `heth.RxDescList.TimeStamp`, filtered to Sync
  frames only, so a stale value can't be mistaken for a fresh one. Second,
  nothing calls `HAL_ETH_ReleaseTxPacket`, so `ptp_clock.c` clears TTSE
  itself.
