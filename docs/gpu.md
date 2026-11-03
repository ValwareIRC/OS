# The graphics card: a plan

Status: proposed (2026-11-02), for review. Nothing here is built yet: the OS itself doesn't use the card. In the meantime, in the VM, the voice can (the launcher's "Expressive voice" box runs Chatterbox on the card under Windows: `scripts/voice-gpu-server.py`), and the VM's network and TCP have to become fast before any of this matters on real hardware (TCP has no window scaling yet).

## Why

The minimum computer for this OS is 32 GB of memory and a graphics card with 16 GB of its own memory (VRAM).
Until now everything has run on the processor: the AI (Qwen3 8B reads its instructions at about 75 tokens a
second, so a first reply takes two minutes), the desktop, its effects and 3D models (drawn in software), and the
apps (Prism and Blender without acceleration). On the card the same model answers in seconds; models two or three
times bigger fit in 16 GB; the desktop can be drawn at full resolution and refresh with real effects.

The goal: the OS uses the card the way Windows or Linux does, on the person's own computer, bundled (nothing to
install), as their main system. The reference machine: Ryzen 7 8700F, 32 GB, GeForce RTX 5060 Ti
16 GB (Blackwell), WD SN3000 NVMe, Realtek 2.5GbE and a Realtek 8811CU USB Wi-Fi adapter, USB keyboard and mouse.

## The way: NVIDIA's open kernel driver, run by our kernel

NVIDIA publishes its kernel driver as open source ([open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules),
MIT/GPLv2). For Blackwell it is the only driver: the older closed one doesn't support RTX 50 cards, and the card's
own processor (GSP) runs the firmware that does most of the work, so GSP is mandatory.

The driver is split in two:
- **The core** ("RM", resource manager, and the display and memory code beside it): written to be independent of
  the operating system. NVIDIA builds the same core for Linux and FreeBSD.
- **A thin layer per system** (kernel-open/ for Linux): memory, mapping, interrupts, PCI, timers, locks, the device
  files (/dev/nvidia0, /dev/nvidiactl, /dev/nvidia-modeset, /dev/nvidia-uvm), firmware loading.

We write that thin layer for our kernel. Above it, NVIDIA's user-side libraries talk to the driver through the
device files with the same ioctls they use on Linux, and our kernel already runs Linux programs unchanged. So the
user side runs as it is:
- **CUDA** (libcuda), which llama.cpp uses: the AI on the card.
- **Vulkan and OpenGL** (NVIDIA's own), for the desktop, Facet's 3D, and apps (Prism/Chromium, Blender, games
  through Flatpak).
- **EGL/GBM** for Wayland: apps hand their frames to the desktop as GPU buffers.

The firmware and the user-side libraries must be from the same driver release as the kernel core; we ship them
together, pinned to one release, and update them together.

### Considered instead: the open stack (Nouveau + Mesa NVK)

Nouveau's kernel driver (with the same GSP firmware, which NVIDIA ships in linux-firmware for it) plus Mesa's NVK
Vulkan driver supports RTX 50 cards (Vulkan 1.4 since Mesa 25.2). Everything in it is open (mostly MIT), and
llama.cpp can use Vulkan. It is the fallback if NVIDIA's licence for the user-side libraries doesn't allow bundling,
but it is slower (bindless-only textures and immature scheduling on Blackwell per its developers), and it has no
CUDA, which most AI tools expect. We'd port Linux's DRM core and the Nouveau driver instead of NVIDIA's core. Not
the first choice for "performant on real hardware".

## What our kernel needs first (real hardware)

A computer boots it from a USB stick; then:
1. **Never touching other disks.** Already the rule (a disk that isn't ours is read-only); kept and tested on NVMe.
2. **NVMe** (the SN3000): a driver for its own partition, to keep state and apps.
3. **USB (xHCI)**: keyboard and mouse (HID), and mass storage, so the kernel reads its own big files from the
   stick, as it now does from the CD in a VM (fast boot on UEFI too).
4. **Network**: Realtek RTL8125 (2.5GbE) first; the USB Wi-Fi adapter later.
5. **PCI**: MSI/MSI-X interrupts, 64-bit BARs and resizable BAR (16 GB of VRAM visible), write-combined mappings
   (exists: PG_WC), IOMMU left off at first.
6. **A way to see what happened**: a log kept on the stick (no serial port on the PC), and the screen console.

## Phases (each ends with something that works on the real PC)

0. **The VM, meanwhile (days).** The AI on the card while we still work in the VM: the launcher starts llama.cpp
   built for CUDA on Windows (the card is Windows' while the VM runs), loaded with the chosen model, reachable
   from the VM at 10.0.2.2 with a key only the VM has; the OS offers it as "On this computer (graphics card)",
   with models sized for 16 GB. Same behaviour as the built-in mind (waking, crash handling, memory sizing).
1. **Boots on the PC (weeks).** USB stick boot with UEFI, xHCI (keyboard, mouse, stick), NVMe, RTL8125, the log.
   The desktop on the firmware's framebuffer (software drawing still). Test: the reference PC boots it, onboarding to
   desktop, state kept on its own partition, Windows disk untouched.
2. **The card wakes (weeks).** The thin layer under NVIDIA's core: PCI, memory and DMA, interrupts, timers and
   locks, firmware loading, /dev/nvidiactl and /dev/nvidia0. Test: nvidia-smi (from the bundled user side) shows
   the card, its 16 GB, its temperature; GSP boots and stays up under load.
3. **The AI on the card (weeks).** nvidia-uvm (unified memory for CUDA: the hardest part of the layer, as it leans
   on the system's memory management) and CUDA. Test: llama.cpp with CUDA runs Qwen3 8B and a 14B model fully on
   the card; first reply in seconds; tokens per second recorded against Linux on the same PC.
4. **The screen through the card (weeks).** nvidia-modeset and DRM/KMS: the display driven by the card (native
   resolution and refresh, several monitors), GBM/EGL buffers. The desktop composites with Vulkan; Facet's
   drawing, text and 3D (m3d) move to the card; Wayland apps hand over GPU buffers (dmabuf). Test: Prism and
   Blender run accelerated; the desktop at full refresh with effects on; a Vulkan game from Flathub runs.
5. **Bundled for daily use.** The driver release pinned and shipped in the image (kernel core built in, firmware
   and user side in the base system), updated as a whole; the software path kept as a fallback for a computer
   without an NVIDIA card (AMD's and Intel's open drivers are later work); power management (idle clocks, fan
   control, suspend), crash recovery (a GSP error resets the card instead of freezing the system).

## Licences

- NVIDIA's kernel core: MIT/GPLv2 dual, used under MIT.
- GSP firmware: NVIDIA allows it to be shipped separately from the driver since its 535 EULA; it's in linux-firmware.
- NVIDIA's user-side libraries (CUDA, Vulkan, GL): under NVIDIA's driver licence. Distributions such as Pop!_OS
  ship them in their images; the exact current terms are read before we bundle them, and if they don't allow it,
  the open stack above is the route.

## Risks

- **The size of the layer**: NVIDIA's Linux layer is large; we implement what the core actually calls, measured by
  building the core against our layer and resolving every symbol. UVM is the riskiest part (CUDA needs it).
- **Real hardware only**: a VM on Windows can't pass the card through, so phases 1-5 are tested on the reference PC,
  booted from a USB stick, with someone at the machine for each test round.
- **Driver releases**: no stable firmware ABI; we stay on one release at a time and move deliberately.
- **Blackwell maturity**: open issues exist on Linux too (GSP hangs under sustained inference); we track the
  release notes and keep a reset path.

## Sources

- [NVIDIA open GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules) and NVIDIA's README chapter
  [Open Linux Kernel Modules](https://download.nvidia.com/XFree86/Linux-x86_64/595.58.03/README/kernel_open.html)
- [NVIDIA: transitioning fully to open-source GPU kernel modules](https://developer.nvidia.com/blog/nvidia-transitions-fully-towards-open-source-gpu-kernel-modules/)
- [GSP is mandatory with the open modules](https://docs.nvidia.com/vgpu/latest/known-issues/bug-6353226-gsp-firmware-cannot-be-disabled-with-open-kernel-modules.html),
  [GSP firmware chapter](https://download.nvidia.com/XFree86/Linux-x86_64/580.65.06/README/gsp.html)
- [NVIDIA cleans up the GSP firmware licence](https://www.phoronix.com/news/NVIDIA-GSP-Firmware-License)
- [Mesa NVK documentation](https://docs.mesa3d.org/drivers/nvk.html),
  [NVK Blackwell support](https://airlied.blogspot.com/2025/07/nvk-blackwell-support.html)
- Blackwell issues on Linux: [#1111](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/1111),
  [#1117](https://github.com/NVIDIA/open-gpu-kernel-modules/issues/1117)
