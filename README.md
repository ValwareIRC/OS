# Operating System (formerly the OS)

An operating system written from scratch in C, inspired by OS1 in *Her*. You talk
to it, it gets to know you, and it designs your whole desktop. There is no stock
theme, start menu or file explorer.

The full plan and the reasoning behind it are in the project plan. The short
version: our own x86-64 kernel, whose native system call interface is the Linux
ABI (so musl, llama.cpp, whisper.cpp, Wayland apps and Firefox can run on it),
an AI daemon with on-device voice and a bundled local model, and **Facet**, the UI
language the AI uses to design every screen.

## Building (inside WSL Ubuntu or Linux)

```
sudo apt install -y build-essential qemu-system-x86 xorriso ovmf mtools git
make          # kernel + bootable ISO (fetches the Limine bootloader into vendor/)
make run      # boot in QEMU with UEFI, serial log in the terminal
make test     # headless boot tests (UEFI and BIOS), with a scratch disk attached
make test-persist  # two boots on one disk image: data must survive the power cycle
```

## Layout

| Path | What |
|---|---|
| `boot/` | bootloader config (Limine loads us; everything after that is ours) |
| `kernel/arch/x86_64/` | GDT/TSS, IDT, interrupt and syscall entry, APIC, ACPI, SMP, FPU/XSAVE |
| `kernel/mm/` | physical frame allocator (with per-page refcounts), kernel page tables, heap, user address spaces, page objects (paged storage behind ramfs/memfd that MAP_SHARED maps directly) |
| `kernel/sched/` | scheduler, wait queues |
| `kernel/fs/` | VFS, ramfs, initramfs unpacker, pipes |
| `kernel/proc/` | processes, signals, exit/wait, ELF loader |
| `kernel/linux/` | the Linux system call table and implementations |
| `kernel/drivers/` | serial, framebuffer console, tty, time (RTC/TSC), QEMU fw_cfg, PCI, e1000, AHCI and virtio-blk disks (block layer: /dev/sdX) |
| `userland/` | PID 1 (musl) and the initramfs root files |
| `kernel/third_party/` | `limine.h` (BSD-0), `font8x8_basic.h` (public domain) |
| `tests/` | QEMU boot tests |

## Status

- [x] Milestone 1 (kernel core): boot, serial and framebuffer log, GDT/TSS/IDT, exceptions, PMM, own page tables (W^X), heap. `make test` passes under UEFI and BIOS
- [x] Milestone 2 (core): ACPI/MADT, x2APIC/xAPIC + I/O APIC, APIC timer, SMP (all cores), preemptive
      scheduler, wait queues; user mode with SYSCALL entry and XSAVE (AVX) state; demand-paged address
      spaces; VFS + ramfs + initramfs (cpio) + /dev (console tty, null, zero, urandom) + pipes; ELF loader
      (static, static-pie, dynamic via PT_INTERP) and `#!` scripts; fork/vfork/execve/wait4, signals with
      handlers, ~130 Linux system calls. A musl-built PID 1 and Ubuntu's glibc busybox run unmodified;
      `make test` runs the userland suite under UEFI and BIOS.
- [x] Networking: PCI, Intel e1000 driver, IPv4/ARP/ICMP/UDP/TCP/DHCP in the kernel, Linux socket API;
      mbed TLS in user space with certificate verification; AI providers (Anthropic, OpenAI-compatible,
      Gemini, Azure) reached over HTTPS from inside the OS
- [x] Graphics and input: /dev/fb0 (fbdev, write-combining mmap), PS/2 keyboard and the vmmouse absolute
      pointer as evdev devices, SMP reschedule IPIs
- [x] The experience: first boot, onboarding conversation with AI-invented picks, the AI designing the
      entire desktop (Facet), living wallpapers, glass, vector icons, live redesign by conversation
- [x] Threads: clone/clone3 with CLONE_THREAD (musl and glibc pthreads run unmodified), futex
      (wait/wake, bitsets, timeouts, requeue, wake-op), per-thread signals (tkill/tgkill), exit vs
      exit_group, TLB shootdown IPIs, user copies with an exception fixup table
- [x] Storage: AHCI (SATA) and legacy virtio-blk drivers; disks are Linux block devices /dev/sda, /dev/sdb...
      (byte-granular read/write, lseek/fstat size, BLKGETSIZE64/BLKSSZGET, fsync flushes the drive cache).
      `make run` and the launchers attach `build/os-disk.img` (1 GiB, sparse; `make clean` keeps it)
- [x] Linux apps: ptys and a terminal app; AF_UNIX sockets with SCM_RIGHTS, epoll, eventfd, timerfd,
      signalfd, memfd, real MAP_SHARED (page objects); /proc and /sys. Ubuntu's glibc programs run
      unmodified from the sysroot (`scripts/build-sysroot.sh`): Python 3, Weston's Wayland clients
- [x] the OS's own Wayland compositor in the shell (`userland/shell/wl`): apps' windows are framed by
      the person's design; `tests/wl_os_test.py` checks a live app inside the OS
- [x] Memory: `/etc/os` (design, portrait, conversation) and `/root` are kept on the disk
      (`shell/store.c`, crash-safe two-slot archive), restored at boot; the companion welcomes them back
- [x] Sound and voice: Intel HDA driver (`/dev/dsp`, `/dev/mixer`); the companion speaks (Piper) and
      listens (whisper.cpp, Ctrl+Space), on this computer
- [ ] Next: the offline built-in model, Wi-Fi, GTK/Chromium apps (Prism), copy-on-write fork

## The experience (userland/shell)

`os-shell` is everything a person sees. The first-boot screens (choose a mind, connect) are the only
ones in the OS's own plain style. From the onboarding conversation on, the AI shapes everything:

- **Onboarding** (`onboard.c`): a conversation with visual picks the AI invents (palettes, moods), plus
  the questions the plan leaves to onboarding: the AI's name, the background, how autonomous it may be,
  and whether it may redesign later.
- **Facet markup** (`shell/fm/`): the AI writes the whole desktop as one document, XML-like markup for
  structure and a CSS-like stylesheet for looks. It is not web technology: a small OS-specific vocabulary
  (surfaces docked to screen edges or placed freely, rows, columns, grids, stacks, orbits, popovers,
  sheets, live app lists, window lists, clocks, search, the companion's orb, embedded app views), a subset
  of CSS (gradients, frosted glass, vibrancy, glow, shadows, transitions, transforms), a closed set of
  actions, and a window-frame template. A taskbar with a start menu, a menu bar with a dock, or apps
  orbiting a light are just different documents (see `tests/designs/`). A `<canvas>` element runs a small
  sandboxed Lua script for anything the markup can't express. The validator reports problems back to the
  AI (every app must stay reachable) and text colour is checked against what is actually behind it.
- **The companion drives the UI**: it sees a compact view of the element tree and acts with tags such as
  `[[ui:press #start]]`, `[[ui:style dock { ... }]]`, `[[ui:add #bar <clock/>]]` or `[[ui:remove #x]]`.
  Touched elements glow briefly so the person sees what changed. For a whole new look it redesigns.
- **Desktop** (`desktop.c`, `wallpaper.c`): hosts the document, the windows and the built-in apps.
  Living wallpapers are procedural (AI-chosen kind, colours, seed and intensity).

`tests/fm-dump.sh DOC.xml` runs the engine natively and prints the laid-out tree; `tests/preview.py
DOC.xml OUT.png` boots the OS straight into a document. `tests/demo.py` drives a complete first boot in
QEMU against a local model and saves screenshots.

## Try the demo image

`build/os.iso` boots on UEFI and BIOS machines and virtual machines (it is also a USB image: write it to
a stick with a raw writer such as Rufus in DD mode, or balenaEtcher).

- **On this PC:** double-click `Operating System.cmd`. It starts the image in a window (QEMU in WSL, with KVM). A model
  served by Ollama on this PC is reachable from the OS at the default "On your network" address.
- **VirtualBox / VMware:** create a 64-bit "Other" VM with 3 GB of memory and 4 CPUs, NAT networking with
  the Intel PRO/1000 (e1000) adapter, attach the ISO and boot. For Ollama on the same PC, the default
  address (`http://10.0.2.2:11434/v1`) works as is.
- **A cloud provider:** pick it on the first screen and paste your key with Ctrl+V. `Operating System.cmd` shares this
  PC's clipboard with the OS (text only, from the PC into the OS).
- **Start already connected:** copy `os.conf.example` to `os.conf`, fill in the provider and key, and
  `Operating System.cmd` hands them to the OS at boot. On a real PC, add `cmdline: os.provider=... os.key=...` to
  `boot/limine/limine.conf` on the USB stick (an example is in that file).

What works in this build: first boot, the onboarding conversation, the AI designing the whole desktop,
files, notes, the companion (talk to it; it can open apps and redesign the desktop). Not yet: voice,
Wi-Fi, saving anything across reboots (the disk driver is in; saving to it comes next), the offline built-in model, real-hardware
drivers beyond Intel e1000 and PS/2.

## Trying it

`make run` boots into a busybox shell on the serial console (your terminal). `tests/shell_demo.sh` types
a few commands into it automatically; `tests/debug_boot.sh` boots once with system-call tracing.

## Bundled apps

The OS ships **Prism**, Valerie Liu's agentic IDE, as a bundled app. `make` copies the newest
`Prism-*.AppImage` from the Prism project's `dist` folder into a separate boot module (`apps.cpio`),
which appears inside the OS as `/apps/prism/`. Running it needs the graphical Linux apps milestone:
glibc-style dynamic linking, the system calls Chromium uses (threads, futex, epoll, eventfd, memfd) and a
Wayland display server. Until then the OS lists it as installed but cannot open it yet.
