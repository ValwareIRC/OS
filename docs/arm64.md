# Operating System on arm64 (Apple Silicon, in QEMU)

The OS runs on arm64 in QEMU's `virt` machine. On an M1/M2/M3 Mac, QEMU uses the Mac's own
hypervisor (`-accel hvf -cpu host`), so it runs at close to native speed. Bare metal on Apple Silicon is
not a goal for now.

## What is what

| Part | arm64 status |
|---|---|
| Kernel (`kernel/`, its own, not Linux) | ported (`kernel/arch/arm64`, `kernel/platform/virt`), young |
| Desktop, init, the OS's own tools (`userland/`) | the same C code, built for arm64 |
| Base system: Flatpak, portals, llama.cpp, whisper.cpp (`scripts/build-base.sh`) | builds for the machine it runs on (x86_64 or aarch64) |
| Piper, its voice (`ports/build-voice.sh`) | the matching prebuilt release (`piper_linux_aarch64`) |
| Prism | needs an arm64 Linux build of Prism; without one the image has no Prism |
| Chromium and other apps | from Flathub, which has aarch64 builds |
| AI models (`.gguf`) | the same files on every architecture |

## Building on a Mac

You need Docker (Docker Desktop, OrbStack or colima) and QEMU (`brew install qemu`). The Makefile
builds inside an arm64 Ubuntu container when it runs on macOS.

```sh
make ARCH=arm64 base-system   # once: the base system (about an hour; Flatpak needs --privileged, given)
make ARCH=arm64               # the kernel, userland and image: build/arm64/os.iso
make ARCH=arm64 run           # boots it in QEMU with hvf, on a disk image of its own
```

Without `base-system` the image is the OS alone: the desktop and the companion (thinking through an
AI service you connect), but no Flatpak apps, local model or voice.

## What to report back

The boot log is on the terminal (`-serial stdio`). Useful to send:

- the first lines that go wrong, or the last ones before it stops;
- `make ARCH=arm64 test` (boots the image and checks the kernel's self-tests);
- anything in `build/arm64/` named `*.log` if a build step fails.

## Not done yet

- These steps haven't been run end to end on an arm64 machine: the x86_64 build is tested daily, arm64
  isn't. Expect fixes.
- The built-in model's prebuilt llama-server on the image (`ports/build-mind.sh`) and the Prism Flatpak
  are built on x86_64 today; on arm64 they come from the base system and a Prism arm64 build.
