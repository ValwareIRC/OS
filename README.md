# Operating System

An operating system written from scratch in C, inspired by OS1 in *Her*. You talk
to it, it gets to know you, and it designs your whole desktop. There is no stock
theme, start menu or file explorer.

In short: our own x86-64 kernel, whose native system call interface is the Linux
ABI (so musl, llama.cpp, whisper.cpp and Flatpak apps such as Prism and Chromium run on it),
an AI daemon with on-device voice and a bundled local model, and **Facet**, the UI
language the AI uses to design every screen.

## What it looks like

Every desktop is written by the AI for the person in front of it. It can design one from nothing, or
start from one of 34 built-in templates and make it theirs (their colours, their words, their
wallpaper). These are the templates as they come, before any of that. Many are live: a 3D fly-over of
your real files, planets that orbit, fish that swim, a sky that follows the time of day.

<!-- gallery -->
| | |
|---|---|
| ![Aqua: a light frosted menu bar, a glass dock that swells under the pointer, a full-screen launcher, pale windows with gel buttons, blue water behind. Polished and calm](docs/screenshots/aqua.jpg) | ![Aquarium: a full-screen animated canvas of a blue-green tank](docs/screenshots/aquarium.jpg) |
| **Aqua** · glass menu bar and dock | **Aquarium** · apps are bubbles among the fish |
| ![Arcade: a full-screen canvas of chunky pixel art](docs/screenshots/arcade.jpg) | ![Bento: the whole screen is one large rounded grid of compartments of different sizes on a soft sand background](docs/screenshots/bento.jpg) |
| **Arcade** · 8-bit title screen | **Bento** · a grid of widget cards |
| ![BeOS: a deep blue desktop with large icons in the top-left, the Deskbar as a compact grey panel pinned in the TOP-RIGHT corner](docs/screenshots/beos.jpg) | ![Blueprint: a full-screen canvas of cyanotype blue paper with a fine and a coarse grid, a title block in the bottom-right corner](docs/screenshots/blueprint.jpg) |
| **BeOS** · Deskbar and yellow tabs | **Blueprint** · apps as drawn parts |
| ![Brutalist: a flat raw grey concrete background, the apps as ENORMOUS black uppercase words stacked down the left](docs/screenshots/brutalist.jpg) | ![Calm: warm stone light, a huge quiet clock and greeting at the left, the apps as a short column of plain words beneath it](docs/screenshots/calm.jpg) |
| **Brutalist** · enormous raw type | **Calm** · a clock and a few words |
| ![Commander: the whole screen is a text-mode interface in blue and cyan](docs/screenshots/commander.jpg) | ![Corkboard: the whole screen is a cork board](docs/screenshots/corkboard.jpg) |
| **Commander** · Norton Commander, two panes | **Corkboard** · notes and polaroids pinned up |
| ![CRT: a full-screen canvas of a curved old monitor](docs/screenshots/crt.jpg) | ![Cube: a full-screen canvas with one large solid cube floating and slowly turning in the middle of a dark studio](docs/screenshots/cube.jpg) |
| **CRT** · green phosphor terminal | **Cube** · a turning 3D cube of apps |
| ![Frost: a dark aurora sky, the companion's orb glowing at the centre of the screen with the apps circling it on a ring, a thin clock and greeting above, frosted glass windows with pale cya](docs/screenshots/frost.jpg) | ![FSN: a full-screen canvas flying over the real home folder in perspective; folders are teal platforms you click to fly inside, files are blocks sized by their size, apps are tall blue t](docs/screenshots/fsn.jpg) |
| **Frost** · apps orbit the orb | **FSN** · 3D file system, after Jurassic Park |
| ![Gears: a full-screen canvas of interlocking brass and copper gears of many sizes turning at matched speeds](docs/screenshots/gears.jpg) | ![HUD: a full-screen canvas with a large animated targeting reticle at the centre](docs/screenshots/hud.jpg) |
| **Gears** · clockwork and brass plaques | **HUD** · a targeting reticle |
| ![Luna: a blue taskbar with a green start button, a two-column start menu, icons on a Bliss-like hill, blue window frames. Familiar and friendly](docs/screenshots/luna.jpg) | ![Matrix: a full-screen canvas of falling green glyph rain](docs/screenshots/matrix.jpg) |
| **Luna** · Windows XP, made modern | **Matrix** · falling code, red pill or blue |
| ![Metro: a solid deep purple backdrop, a huge thin 'Start' heading and the person's name at the top, the apps as flat solid-coloured live tiles of mixed sizes](docs/screenshots/metro.jpg) | ![Neon: a full-screen animated canvas of a striped setting sun, purple mountains and a glowing perspective grid racing towards you; the apps are a row of neon words floating over the horiz](docs/screenshots/neon.jpg) |
| **Metro** · Windows 8 live tiles | **Neon** · synthwave drive |
| ![Notebook: a full-screen canvas of an open spiral-bound notebook lying on a wooden desk](docs/screenshots/notebook.jpg) | ![Orrery: a full-screen animated canvas of a solar system seen from slightly above, each app a planet on its own tilted elliptical orbit](docs/screenshots/orrery.jpg) |
| **Notebook** · a handwritten to-do page | **Orrery** · apps are planets |
| ![Paper: warm paper, a newspaper masthead with the date, a numbered contents column of apps in a serif, a lead story with their greeting, ink-ruled windows with square corners. Literary and](docs/screenshots/paper.jpg) | ![Platinum: a periwinkle desktop, a light grey menu bar across the top with a rainbow-ish apple-like menu, File Edit View Special words and the time at the right, the apps as large desktop ico](docs/screenshots/platinum.jpg) |
| **Paper** · a newspaper front page | **Platinum** · Mac OS 9 |
| ![Program Manager: a teal desktop, one big centred grey Program Manager window with a navy title bar, a menu row](docs/screenshots/progman.jpg) | ![Radar: a full-screen canvas of a round green radar display filling the middle of a dark instrument panel](docs/screenshots/radar.jpg) |
| **Program Manager** · Windows 3.1 | **Radar** · apps are contacts on the scope |
| ![Shelf: a full-screen canvas of a warm wooden bookcase with two shelves, each app a cloth-bound book spine of a different colour, height and width with its title in gold letters running do](docs/screenshots/shelf.jpg) | ![Sky: a full-screen canvas whose sky follows the real clock](docs/screenshots/sky.jpg) |
| **Shelf** · apps are books | **Sky** · the real sky, apps are clouds |
| ![Subway: a full-screen canvas of a clean transit diagram on off-white](docs/screenshots/subway.jpg) | ![Swiss: white paper with a strict 12-column grid, one huge red rectangle taking the left third with the hour as an enormous white numeral inside it, the minutes and date in small black typ](docs/screenshots/swiss.jpg) |
| **Subway** · apps are stations | **Swiss** · a modernist poster |
| ![Tiling: a single thin dark status line across the TOP in small monospace](docs/screenshots/tiling.jpg) | ![Workbench: a flat bright blue screen with a white-and-blue screen title bar across the top showing free memory, the apps as small chunky drawer and disk icons in a loose grid at the top-left,](docs/screenshots/workbench.jpg) |
| **Tiling** · a tiling window manager | **Workbench** · Amiga, with the Boing ball |
| ![Starlit: a dim violet night fading to pink-mauve, warm four-point stars in soft halos, amber lamplight low in the corners, the time on a translucent violet cushion in the middle and the apps in a translucent dock](docs/screenshots/starlit.jpg) | |
| **Starlit** · sleepy, soft and slow | |
<!-- /gallery -->

## Building (inside WSL Ubuntu or Linux)

```
sudo apt install -y build-essential qemu-system-x86 xorriso ovmf mtools git tzdata
make          # kernel + bootable ISO (fetches the Limine bootloader into vendor/)
make run      # boot in QEMU with UEFI, serial log in the terminal
make test     # headless boot tests (UEFI and BIOS), with a scratch disk attached
make test-persist  # two boots on one disk image: data must survive the power cycle
```

On macOS, install Docker and `brew install qemu`: `make` builds in an x86-64 Ubuntu container and
`make run` boots it in the Mac's QEMU.

## Layout

| Path | What |
|---|---|
| `boot/` | bootloader config (Limine loads us; everything after that is ours) |
| `kernel/arch/` | x86_64 (GDT/TSS, IDT, interrupt and syscall entry, APIC, SMP, FPU/XSAVE) and arm64 |
| `kernel/mm/` | physical frames (refcounted), page tables, heap, address spaces, page objects behind ramfs, memfd and MAP_SHARED |
| `kernel/sched/` | the preemptive SMP scheduler, wait queues |
| `kernel/fs/` | VFS and mounts, ramfs, the initramfs unpacker, ext2 (the apps' area on the disk), procfs and sysfs, memfd, pipes, locks |
| `kernel/proc/` | processes and threads, signals, exit/wait, the ELF loader, namespaces (user, mount, pid, net, uts) |
| `kernel/linux/` | the Linux system call table (about 200 calls) and their implementations |
| `kernel/net/` | IPv4, ARP, ICMP, UDP, TCP, DHCP, the socket API |
| `kernel/drivers/` | ACPI, PCI, AHCI and virtio-blk disks, the e1000 network card, Intel HD Audio, evdev input (PS/2, vmmouse, virtio), fbdev and the boot screen, ptys and ttys, time, QEMU fw_cfg |
| `userland/init/` | PID 1: the self-tests, the disk, the person's session and the few things it does as root for the desktop |
| `userland/shell/` | `os-shell`: the desktop, Facet, the companion, the apps, the Wayland compositor (`wl/`) |
| `userland/tools/` | os-audio (os-play, os-rec), os-voicefx (voice shaping), os-speak (Kokoro), os-boot, os-ai |
| `userland/figure/` | os-figure: the companion's 3D body, drawn in software |
| `userland/portal/` | os-portal: the desktop portal Flatpak apps talk to |
| `scripts/` | builds of the base system and ports, the Windows launcher (`launcher.ps1`, `os-win.ps1`) and its helpers |
| `tests/` | the self-tests' runners, VM tests driven through QEMU's monitor, and host tests of single parts |
| `docs/` | Facet 2, the companion API, the GPU plan, arm64, the list of what still feels unfinished (`toy-audit.md`) |

## Status

- [x] Kernel: its own, booting with UEFI and BIOS; SMP, a preemptive scheduler, demand-paged address spaces with
      copy-on-write, about 200 Linux system calls (threads, futexes, signals, epoll, eventfd, timerfd, signalfd,
      memfd, AF_UNIX with SCM_RIGHTS, ptys, /proc and /sys, namespaces and seccomp for sandboxes); musl and glibc
      programs, and Flatpak apps, run unmodified. 205 self-tests (`tests/selftest_win.py`)
- [x] Storage: AHCI and virtio-blk; the person's home and the apps on the disk in a journaled ext3 area (the kernel
      writes ext3's journal, e2fsck replays it: a power cut never leaves it half written), the system's own state
      in a crash-safe two-slot store beside it (`shell/store.c`)
- [x] Networking: e1000/e1000e (with interrupts), IPv4/TCP/UDP/DHCP in the kernel (window scaling, SACK, segments
      kept ahead of a gap), mbed TLS in user space; the clock set from the network
- [x] Apps: the base system is the Freedesktop Platform with Flatpak, the portals and PulseAudio; Flathub apps
      install from the launcher and run in their sandbox (Firefox, Chromium, Prism, MarkText, AbiWord, DOSBox);
      the OS's own Wayland compositor frames their windows in the person's design
- [x] The experience: an onboarding conversation that gets to know them (and keeps its place if the desktop
      restarts), the AI designing the whole desktop in Facet, live redesign and edits by conversation
- [x] Facet 2 (`docs/facet2.md`): the companion writes whole apps (state, logic, live system data, web requests
      and TCP/TLS sockets), checked before they run and kept in `~/Apps`
- [x] A computer, not a demo: an account named after them (`/home/<name>`), an optional password and a lock
      screen, Settings (Appearance, Display, Sound, Keyboard, Mouse, Date & time, AI, Users, Notifications, Apps,
      Power, Storage, Security, Privacy, About), Files, Notes, a Terminal, time zones and network time
- [x] The companion: a character of its own, plain spoken; it sees the screen and pictures, acts on the desktop
      and the system with tags (and is told when they fail), remembers, and can be renamed; its look is theirs to
      choose (a light, a gem, a picture, a 3D model, an anime figure, their own VRM)
- [x] Voice, on this computer: Kokoro's voices, mixed into new ones and shaped (pitch, formants, warmth, effects),
      with tones per reply; listening with whisper.cpp. With the launcher's "Expressive voice" box, a voice with
      real feeling and cloning from the PC's graphics card (Chatterbox, `scripts/voice-gpu-server.py`)
- [x] Minds on this computer: a built-in model (Qwen3 8B, offline) and others to download, run by llama.cpp; or a
      network or cloud one; or Claude through this PC's Claude Code sign-in (the launcher)
- [x] The companion API (`docs/companion-api.md`): apps such as Prism use the companion's AI, name, face, voice
      and memory, with the person's consent and without ever seeing their keys
- [ ] Next: the graphics card driven by the OS itself (`docs/gpu.md`), NVMe, USB, more network cards and Wi-Fi,
      installing to the disk and updates; what still feels unfinished is listed in `docs/toy-audit.md`

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

## Running it

`make` produces `build/os.iso`, which boots on UEFI and BIOS machines, real or virtual. The computer it is made
for has 32 GB of memory and a graphics card with 16 GB of its own; less runs, more slowly (the built-in model alone
takes 5 GB).

- **On Windows, the way it is developed:** `Operating System.cmd` opens a small window first (your disk or a fresh
  one, the AI saved on the disk or Claude on this PC, a design to try, full screen, the expressive voice), remembers
  what you chose, and starts it in Windows QEMU.
- **Another virtual machine** (QEMU, VirtualBox, VMware, Hyper-V...): 64-bit, 20 GB of memory, 8 CPUs, the ISO as
  its CD, an empty disk on SATA/AHCI or virtio so it remembers you (32 GB, sparse), and an Intel PRO/1000 (e1000)
  network adapter. `make run` starts it in QEMU.
- **A real PC:** write the ISO to a USB stick with a raw writer (Rufus in DD mode, balenaEtcher, `dd`), or copy its
  files onto a FAT32 stick for UEFI. Turn off Secure Boot. It only ever writes to a disk that is blank or already
  its own; any other drive is left untouched. Hardware support is still narrow (e1000, AHCI and virtio disks, Intel
  HD Audio, PS/2 keyboard and mouse): no NVMe, USB, other network cards or Wi-Fi yet, and the graphics card isn't
  used.

The first screen asks for a mind: the model that comes with the system (works offline), a bigger one downloaded
onto this computer, a model on your network (Ollama, LM Studio), or a cloud provider with your key. To start
already connected on a real PC, add `cmdline: os.provider=... os.key=...` to `boot/limine/limine.conf` (an example
is in that file).

`tests/` holds how it is checked: `tests/selftest_win.py` (the kernel and userland self-tests), and the
`*_vm_win.py` tests, which boot it in Windows QEMU and use it as a person would (keys and clicks through QEMU's
monitor, never the real mouse), each with a stand-in AI (`tests/fake_ai.py`) or the real one.

## Bundled apps

The OS ships **Prism**, Valerie Liu's agentic IDE, preinstalled as a Flatpak (`com.valware.prism`, built from
Prism's Linux release by `scripts/build-prism-flatpak.sh` and installed into the base by `scripts/build-base.sh
prism-app`). It runs in its Flatpak sandbox like any Flathub app, and uses the companion's AI by default
through the companion API, so it works with no setup.

Other apps, browsers included (Chromium, for one), install from Flathub through the launcher; when
something asks to open a web link and no browser is installed yet, the OS offers some.

## License

MIT (see LICENSE). Files under `kernel/third_party/` keep their own licenses: `limine.h` is BSD Zero Clause, `font8x8_basic.h` is public domain. Libraries fetched at build time (Limine, musl, mbed TLS, stb, Lua, fonts, llama.cpp and others) come under their own licenses.
