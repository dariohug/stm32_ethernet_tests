# stm32_ethernet_test

NUCLEO-H723ZG firmware that brings up Ethernet (RMII, LAN8742A PHY) with a
static IP and a tiny plain-text TCP server for toggling the three onboard
LEDs. Built with STM32CubeMX (CMake/GCC toolchain) + Ninja + OpenOCD.

## Build

```sh
cmake --preset Debug
cmake --build build/Debug
```

Produces `build/Debug/stm32_ethernet_test.elf`.

## Flash

Board is a NUCLEO-H723ZG with an onboard ST-LINK; just plug it in via USB.

```sh
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
  -c "program build/Debug/stm32_ethernet_test.elf verify reset exit"
```

## PC-side network setup

The board uses a fixed IP with DHCP off, for a direct cable between the
board and this machine (no router):

| | IP | Netmask |
|---|---|---|
| Board | 192.168.1.10 | 255.255.255.0 |
| PC (you) | 192.168.1.1 | 255.255.255.0 |

Find the interface the cable is plugged into (`ip link` — look for the one
that goes `LOWER_UP` once the cable's connected), then:

```sh
sudo ip addr add 192.168.1.1/24 dev <iface>
sudo ip link set <iface> up
```

This needs root and changes machine-wide network config, so it isn't done
for you — run it yourself when you're ready to talk to the board.

## Talking to the board

Once the PC side is up, plain `netcat` works:

```
$ nc 192.168.1.10 5000
status
led1=off led2=off led3=off
led1 on
ok
status
led1=on led2=off led3=off
led2 toggle
ok
nonsense
err unknown command
```

Commands are line-oriented, one per line, `\n` or `\r\n` terminated.

| Command | Reply |
|---|---|
| `led1\|led2\|led3 on\|off\|toggle` | `ok` / `err bad arg` |
| `status` | `led1=on\|off led2=... led3=...` |
| `help` | one-line command summary |
| anything else | `err unknown command` |

led1/led2/led3 map to LD1 (green, PB0), LD2 (yellow, PE1), LD3 (red, PB14).

## Regenerating with CubeMX

Everything CubeMX can own lives in `stm32_ethernet_test.ioc` — peripherals,
pins, LwIP settings, MPU regions, clocks, the CMake toolchain choice.
Hand-written code lives only in `USER CODE BEGIN/END` blocks or in files
CubeMX doesn't generate (`Core/Src/cmd_server.c`). Open the `.ioc` in
CubeMX, change what you need, hit Generate Code — your edits and the
command server survive.

The regeneration command and the two exceptions that need re-checking
after a regen (the D2 SRAM linker sections and the LwIP heap pointer
override) are recorded in `AGENTS.md`.

## Verification status

- Builds clean (0 warnings) from a fresh CubeMX generation with CMake +
  Ninja + `arm-none-eabi-gcc`.
- Flashed via OpenOCD, verified OK, confirmed running (PC advancing across
  two halts).
- Link layer confirmed: with the board's Ethernet port cabled to this
  machine, `ethtool` on the host NIC already showed a live 100 Mb/s
  full-duplex carrier from the board's PHY — checked read-only, no host
  config changed.
- IP-level connectivity (the `nc` session above) is **not yet verified**:
  it needs the PC-side IP config above, which touches machine-wide network
  settings outside this repo and wasn't done. Run the PC setup and the
  `nc` command yourself to confirm.
