# NVIDIA's driver on our kernel

The goal: CUDA (and so the AI models: voice cloning, the language model, speech) running on an NVIDIA card from
the real ISO on a real computer. Valerie's PC has an RTX 5060 Ti (Blackwell, GB206).

## What NVIDIA's open driver is

`open-gpu-kernel-modules`, version 620.70.06 (MIT/GPL dual licence), fetched to `~/osbuild/nvidia-open` in WSL.

- **The core** (`src/nvidia`, `src/common`): about 2 million lines that don't depend on Linux. It compiles as it
  is, to one object, `nv-kernel.o`. It talks to the card, and boots the card's own processor, the GSP, which runs
  most of the driver on the card itself (from firmware NVIDIA ships with its driver).
- **The Linux glue** (`kernel-open/nvidia`, about 53,000 lines; `kernel-open/nvidia-uvm`, about 155,000): what
  joins the core to Linux. The main driver gives programs `/dev/nvidiactl` and `/dev/nvidia0`; UVM (unified
  memory) lets the card and the processor share memory, and CUDA needs it.

## What it needs from the kernel: measured

The glue was compiled against Linux 7.2 (`~/osbuild/linux72`, built once in the Freedesktop SDK), and every
Linux function it calls was listed (`nm -u`): `syms-nvidia.txt`, `syms-nvidia-uvm.txt` here, with
`syms-nvidia-modeset.txt` and `syms-nvidia-drm.txt` for display, which CUDA doesn't need.

| | Linux functions |
|---|---|
| nvidia (main driver) | 370 |
| nvidia-uvm (shared memory, for CUDA) | 295 |
| both, without duplicates | 516, of which about 90 are NVIDIA's own (`nvUvmInterface*`, from nvidia to uvm) |
| **what our layer must provide for CUDA** | **about 420** |
| display (modeset, drm), later if ever | about 300 more |

By area (for CUDA):

| Area | Functions | Notes |
|---|---|---|
| Memory: pages, kmalloc, vmalloc, ioremap, PAT | ~45 | ours exist; a Linux-shaped `struct page` view is the hard part |
| DMA and IOMMU | ~40 | direct mapping (no IOMMU) first |
| PCI | ~40 | most map to ours; MSI-X needed |
| Locks, waiting, kernel threads, work queues, RCU | ~50 | thin wrappers over ours |
| Library: bitmaps, strings, sort, radix trees, rb-trees | ~45 | copied or written small |
| ACPI and power management | ~18 | many can say "not there" at first |
| Files, character devices, mmap | ~20 | `/dev/nvidia*`, ioctl, mmap of card memory |
| proc, sysfs, module parameters | ~14 | parameters from the command line; proc mostly stubs |
| Time and timers | ~10 | |
| Interrupts | ~5 | |
| User memory: pin_user_pages, get_user_pages_remote, handle_mm_fault, follow_pfnmap, mmap_lock | ~15 | **the hardest**: UVM works the memory manager from the inside |
| Tracing, debugging, retpoline thunks | ~25 | stubs |

## The approach

A layer in our kernel that looks, from the inside, like the parts of Linux the glue uses (`kernel/linuxkpi/`: its
own headers with Linux's names and the struct fields the glue touches, and the functions above built on ours).
NVIDIA's glue then compiles nearly unchanged, and a new driver version is a new compile, not a new port. (The same
idea as FreeBSD's LinuxKPI, which runs Linux's graphics drivers.) The core object `nv-kernel.o` is linked in as
NVIDIA builds it.

What the person's computer downloads at runtime, never shipped with the OS: the GSP firmware
(`gsp_ga10x.bin`, `gsp_tu10x.bin`, for Blackwell `gsp_gb*`) and NVIDIA's closed user-space libraries (`libcuda.so`,
`libnvidia-ml.so`, `nvidia-smi`), all from NVIDIA's driver package of exactly the same version as the kernel part.

## Phases

1. **The layer and the main driver.** `kernel/linuxkpi` with what `nvidia` needs; it compiles and links; on a
   computer without an NVIDIA card nothing changes.
2. **The card comes up** (only on real hardware: Valerie's PC). The GSP boots, `nvidia-smi` shows the card.
   The network log (`netlog`, scripts/netlog.ps1) is how its start is seen.
3. **UVM and CUDA.** The memory-manager parts; a CUDA sample runs; then the OS's own AI services use the card.
4. (Later, if wanted.) Display through the card (modeset/drm) instead of the firmware's framebuffer.

## Where it is (2026-11-06)

Phase 1 is written and checked as far as it can be without a build:
- every one of the module's 61 files compiles against the layer (`tests/nvidia/glue_compile.sh`). On x86 the Tegra
  and NVSwitch files are NVIDIA's own "none here" versions, with the conftest answer for platform devices switched
  off;
- the glue, the layer, its host side and `nv-kernel.o` resolve every symbol against each other and our kernel
  (`tests/nvidia/link_check.sh`: what is left are functions this work added to our kernel, missing only from
  kernel objects built before it).

What our kernel gained for it:
- mappings a driver fills in itself, and fills at fault time (`VMA_DEV`), with program pages held for a device;
- a run-on-every-CPU call;
- PCIe extended config space (ECAM);
- per-thread and per-address-space slots for the layer;
- the firmware type and SMBIOS from Limine.

`make kernel LKPI=1` builds it in (`NV_SRC` is NVIDIA's tree). The driver starts on a thread of its own after
boot; `nonvidia` on the command line leaves it out.

Not done yet, in the layer:
- ACPI methods answer "not found" (no AML interpreter yet);
- only MSI interrupts (no INTx routing);
- no resizable BAR;
- a forked program doesn't inherit the driver's mappings;
- `/proc/driver/nvidia` isn't shown;
- video memory isn't saved over suspend.

The GSP firmware has to be at `/lib/firmware/nvidia/620.70.06/` (`request_firmware` reads it from there).

## Risks

- UVM's use of the memory manager (pinning a program's pages, faults the driver handles, mappings it tears down)
  has to match what our kernel's memory manager can do; this is where most of the time goes.
- Only testable on real hardware: every step after phase 1 needs a boot on Valerie's PC.
- The closed user-space libraries must be exactly the kernel part's version, and expect glibc (the Flatpak runtime
  has it).
