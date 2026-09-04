# Project agent memory

This file is the project's committed home for project-intrinsic agent knowledge: build, test, release, architecture, and sharp-edge notes that should travel with the code.

## Regenerating from the .ioc

Everything CubeMX can own (peripherals, pins, LwIP, MPU regions, clocks,
toolchain) lives in `stm32_ethernet_test.ioc`, not in generated C. Edit the
`.ioc`, then regenerate headlessly with:

```sh
~/STM32CubeMX/STM32CubeMX -q script.txt
```

where `script.txt` contains:

```
config load /absolute/path/to/stm32_ethernet_test.ioc
project generate
exit
```

Hand-written code lives only in `USER CODE BEGIN/END` blocks, or in files
CubeMX doesn't generate (`Core/Src/cmd_server.c` + `Core/Inc/cmd_server.h`,
registered in the top-level `CMakeLists.txt`'s user sections, which CubeMX
generates once and never overwrites). Prove any change survives: commit,
regenerate, `git diff` — the only acceptable diff is CubeMX's own churn
(timestamps, ordering).

Two things CubeMX does *not* regenerate and that a `.ioc` edit touching
ETH/LwIP memory sizing could silently invalidate — check these after such a
change, don't just trust the `.ioc`:

- `cmake/eth_ram_d2.ld`, wired in from `CMakeLists.txt` via an extra `-T`.
  CubeMX's own linker script defines the `RAM_D2` memory region but never
  places `.RxDescripSection`/`.TxDescripSection`/`.Rx_PoolSection` into it —
  without this fragment they fall through to GCC's orphan-section handling
  and land outside the region the MPU marks non-cacheable, which is the
  classic H7 "builds fine, never receives a packet" failure.
- `LWIP/Target/lwipopts.h`'s `USER CODE BEGIN 1` overrides
  `LWIP_RAM_HEAP_POINTER` to the linker symbol `_eth_ram_end` (defined at
  the end of `.Rx_PoolSection` in the fragment above) instead of CubeMX's
  fixed `0x30004000`. With `ETH_RX_BUFFER_CNT=12` the descriptors + Rx pool
  take ~19KB, overrunning CubeMX's assumed 16KB split — the fixed address
  would silently alias the LwIP heap into the Rx pool. If you resize the Rx
  buffer count/size via the `.ioc`, re-check `RAM_D2` usage in the OpenOCD/
  linker map output still fits in 32KB total.

## Maintaining this file

Keep this file for knowledge useful to almost every future agent session in this project.
Do not repeat what the codebase already shows; point to the authoritative file or command instead.
Prefer rewriting or pruning existing entries over appending new ones.
When updating this file, preserve this bar for all agents and keep entries concise.
