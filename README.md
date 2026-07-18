# FreeRTOS Guest for kvmm

AArch64 FreeRTOS guest that runs on the x-kernel kvmm hypervisor.
Single-core, EL1, no MMU, no modifications to the upstream FreeRTOS-Kernel.

## Hardware Model (kvmm virt)

| Device | Address | Notes |
|--------|---------|-------|
| Guest RAM | `0x7000_0000` | 192 MiB, VMM patches DTB `/memory` node |
| Kernel load | `0x7080_0000` | `mem_base + 0x80_0000` |
| DTB load | `0x7400_0000` | `mem_base + 0x400_0000` |
| GICv2 GICD | `0x0800_0000` | VMM traps to vGICD emulation |
| GICv2 GICC | `0x0801_0000` | VMM remaps to physical GICV |
| PL011 UART | `0x0900_0000` | TX-only polled output |
| Virtual Timer | CNTV PPI 27 | 100 Hz tick via `CNTV_CVAL_EL0` / `CNTV_CTL_EL0` |

Entry conditions: EL1h, MMU off, DAIF masked, `x0` = DTB address.

## Source Layout

```
src/
  FreeRTOSConfig.h   FreeRTOS configuration (GIC addresses, tick rate, heap size)
  startup.S          EL1 entry: set stack, clear BSS, call main()
  vectors.S          EL1 exception vector table → FreeRTOS IRQ/SWI handlers
  main.c             IRQ dispatch (PPI 27 → tick), 4 demo tasks, hooks
  gic.c / gic.h      GICv2 distributor + CPU interface init/enable/ack/eoi
  timer.c / timer.h  CNTV generic timer: 100 Hz tick setup and re-arm
  uart.c / uart.h    PL011 polled TX with mini printf
  string.c           Freestanding memset/memcpy/memcmp
freertos.lds         Linker script: .text at 0x70800000, 128K heap, 16K stack
Makefile             Cross-build with aarch64-linux-musl-gcc
```

## Prerequisites

- `aarch64-linux-musl-gcc` cross toolchain
- FreeRTOS-Kernel source:
  ```
  git clone https://github.com/FreeRTOS/FreeRTOS-Kernel.git --depth=1 /tmp/FreeRTOS-Kernel
  ```
  The Makefile expects it at `/tmp/FreeRTOS-Kernel`. Override with `make KERNEL=/path/to/FreeRTOS-Kernel`.

## Build

```
make            # → freertos.bin (~35 KB)
make clean
```

The FreeRTOS-Kernel is compiled with `-DGUEST` which activates the EL1 path
in `portable/GCC/ARM_AARCH64` (uses `SPSR_EL1`/`ELR_EL1`/`VBAR_EL1` instead
of EL3 registers).

## Deploy & Boot

```sh
# Copy binary and DTB into the root filesystem image
debugfs -w /path/to/disk.img -R "write freertos.bin freertos.bin"
debugfs -w /path/to/disk.img -R "write /path/to/freertos.dtb freertos.dtb"

# Boot on kvmm (from guest shell)
echo "boot /freertos.bin /freertos.dtb" > /dev/kvmm
echo "attach" > /dev/kvmm
```

The DTB needs one CPU node with `enable-method = "psci"`, a GICv2 node,
a timer node (PPI 27), and a memory node (VMM patches the base/size).
See `x-kernel/virt/bin/freertos.dts` for the reference.

## What It Does

Creates 4 equal-priority tasks that each print a counter and tick count
every 1 second (100 ticks at 100 Hz). The scheduler uses preemptive
round-robin with time slicing.

```
[FreeRTOS] Booting on kvmm (AArch64 EL1)
[FreeRTOS] Creating tasks...
[FreeRTOS] Starting scheduler...
[FreeRTOS] Task 0: count=0 tick=0
[FreeRTOS] Task 1: count=0 tick=0
[FreeRTOS] Task 2: count=0 tick=0
[FreeRTOS] Task 3: count=0 tick=1
[FreeRTOS] Task 0: count=1 tick=100
...
```

## Key Design Decisions

**No FreeRTOS-Kernel patches.** The upstream `ARM_AARCH64` port's `#if defined(GUEST)`
path handles EL1 register access. All platform adaptation is in the BSP files.

**Timer via CNTV.** The guest uses the AArch64 virtual timer (`CNTV_CTL_EL0` /
`CNTV_CVAL_EL0`). The VMM saves/restores timer state on world-switch and applies
`CNTVOFF_EL2`. PPI 27 is injected as a virtual interrupt via GICH list registers.

**GIC split.** GICD accesses trap to the VMM's software emulation (Stage-2 fault).
GICC accesses go directly to the hardware GICV (Stage-2 remap), providing
near-native interrupt acknowledge/EOI performance.

## Future: SMP

Extending to multi-core requires a custom FreeRTOS SMP port:

- `portGET_CORE_ID` via `MPIDR_EL1`
- `portYIELD_CORE` via GICD SGI (IPI)
- ISR/task spinlocks
- Per-core idle tasks and interrupt stacks
- Secondary CPU startup via PSCI `CPU_ON` (SMC `0xC4000003`)

The VMM already supports multi-vCPU (proven with Linux SMP at 4 cores).



## qemu run:
cd /home/ajax/Desktop/Project/Kernel/freertos-guest
make clean && make MEM_BASE=0x3f880000 && cp freertos.bin freertos.qemu.bin
运行 QEMU：
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -nographic \
    -smp 1 \
    -m 256M \
    -kernel freertos.qemu.bin \
    -dtb freertos.dtb

## bench:
./bench_qemu.py --rounds 10