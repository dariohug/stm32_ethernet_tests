# stm32_ethernet_test

Bare-metal Ethernet bring-up on a **NUCLEO-H723ZG** (STM32H723, Cortex-M7 @ 550 MHz):
RMII + LAN8742A PHY, LwIP (raw API, no RTOS) on a static IP, and a tiny
line-oriented TCP server that drives the three onboard LEDs. A serial console
logs incoming traffic.

```
 PC 192.168.1.1  ──── Ethernet cable ────  NUCLEO-H723ZG 192.168.1.10
   nc / test script  ───── tcp/5000 ─────▶  cmd_server  ──▶  LD1 / LD2 / LD3
   /dev/ttyACM0      ◀──── USART3 ───────  eth_log (printf, traffic stats)
```

## Contents

- [Requirements](#requirements)
- [Build & flash](#build--flash)
- [Network setup](#network-setup)
- [Using the command server](#using-the-command-server)
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
- `nc`, `python3` (for the test script)

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
| `help`                              | one-line command summary         |
| anything else                       | `err unknown command`            |

| Name | LED      | Pin  |
|------|----------|------|
| led1 | LD1 green  | PB0  |
| led2 | LD2 yellow | PE1  |
| led3 | LD3 red    | PB14 |

## Serial console

`printf` goes to USART3, which is the ST-LINK virtual COM port
(`/dev/ttyACM0`, **115200 8N1**):

```sh
picocom -b 115200 /dev/ttyACM0     # or: screen /dev/ttyACM0 115200
```

```
[eth] up: 192.168.1.10, command server on tcp/5000
[eth] ping from 192.168.1.1
[eth] 412 pkt/s (icmp 0 tcp 412 udp 0 other 0), 23690 B/s
```

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

## Project layout

| Path | What it is |
|------|------------|
| `stm32_ethernet_test.ioc` | **Source of truth**: pins, peripherals, clocks, MPU, LwIP config |
| `Core/Src/cmd_server.c` | TCP LED command server (hand-written) |
| `Core/Src/eth_log.c` | `printf` → USART3 retarget and inbound traffic logger (hand-written) |
| `Core/Src/main.c` | CubeMX-generated; user code only inside `USER CODE` blocks |
| `LWIP/Target/lwipopts.h` | LwIP options; heap pointer + input hook overrides in `USER CODE BEGIN 1` |
| `cmake/eth_ram_d2.ld` | Linker fragment that puts ETH DMA descriptors/Rx pool in D2 SRAM (hand-written) |
| `CMakeLists.txt` | Top-level build; user sections add the files above |
| `tools/test_cmd_server.sh` | Functional and performance test from the PC |

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
  runs from HSI (`DIVM1=32`, `DIVN1=275`, giving 550 MHz).
