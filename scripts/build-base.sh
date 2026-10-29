#!/usr/bin/env bash
# The OS's base system: the Freedesktop Platform runtime (the same
# distro-neutral platform Flathub apps run on) as /usr, plus what it lacks
# for running apps, built here from source against the Freedesktop SDK:
# bubblewrap, D-Bus, xdg-dbus-proxy, OSTree, AppStream, Flatpak and the
# portal front end.
#
#   scripts/build-base.sh [COMPONENT...]     (default: all, in order)
#
# Sources go to build/base-src, builds to build/base-build, and everything
# is installed into ~/osbuild/base (a root file system tree: its usr is
# the Platform with our additions). Needs scripts/get-build-tools.sh and the
# org.freedesktop.Sdk//26.08 runtime installed for this user.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=$ROOT/build/base-src
OUT=${BASE_OUT:-$HOME/osbuild/base}   # on the Linux file system: symlinks, case, speed
BLD=${BASE_BUILD:-$HOME/osbuild/base-build}
RT=26.08
mkdir -p "$SRC" "$OUT" "$BLD"
source "$ROOT/scripts/ostools-env.sh"

PLATFORM=$(flatpak info --user --show-location org.freedesktop.Platform//$RT)/files

# name  version  url
SOURCES=(
  "bubblewrap 0.11.0 https://github.com/containers/bubblewrap/releases/download/v0.11.0/bubblewrap-0.11.0.tar.xz"
  "dbus 1.16.2 https://dbus.freedesktop.org/releases/dbus/dbus-1.16.2.tar.xz"
  "xdg-dbus-proxy 0.1.6 https://github.com/flatpak/xdg-dbus-proxy/releases/download/0.1.6/xdg-dbus-proxy-0.1.6.tar.xz"
  "libxmlb 0.3.24 https://github.com/hughsie/libxmlb/releases/download/0.3.24/libxmlb-0.3.24.tar.xz"
  "libfyaml 0.9 https://github.com/pantoniou/libfyaml/releases/download/v0.9/libfyaml-0.9.tar.gz"
  "appstream 1.1.1 https://www.freedesktop.org/software/appstream/releases/AppStream-1.1.1.tar.xz"
  "ostree 2025.7 https://github.com/ostreedev/ostree/releases/download/v2025.7/libostree-2025.7.tar.xz"
  "flatpak 1.16.6 https://github.com/flatpak/flatpak/releases/download/1.16.6/flatpak-1.16.6.tar.xz"
  "fuse 3.17.4 https://github.com/libfuse/libfuse/releases/download/fuse-3.17.4/fuse-3.17.4.tar.gz"
  "xdg-desktop-portal 1.20.3 https://github.com/flatpak/xdg-desktop-portal/releases/download/1.20.3/xdg-desktop-portal-1.20.3.tar.xz"
  "pulseaudio 17.0 https://www.freedesktop.org/software/pulseaudio/releases/pulseaudio-17.0.tar.xz"
  "e2fsprogs 1.47.3 https://mirrors.edge.kernel.org/pub/linux/kernel/people/tytso/e2fsprogs/v1.47.3/e2fsprogs-1.47.3.tar.xz"
)

fetch() { # name -> unpacked source directory
    local name=$1 line ver url file dir
    if [ "$name" = os-portal ]; then echo "$ROOT/userland/portal"; return; fi
    if [ "$name" = os-record ]; then echo "$ROOT/userland/recorder"; return; fi
    for line in "${SOURCES[@]}"; do
        read -r n ver url <<<"$line"
        [ "$n" = "$name" ] || continue
        file=$SRC/$(basename "$url")
        [ -s "$file" ] || curl -fsSL "$url" -o "$file"
        dir=$SRC/$name-$ver
        if [ ! -e "$dir/meson.build" ] && [ ! -e "$dir/configure" ]; then
            rm -rf "$dir" "$dir.x"
            mkdir -p "$dir.x"
            tar -xf "$file" -C "$dir.x"
            # the one directory the tarball holds (some put "./" in front)
            local top
            top=$(find "$dir.x" -mindepth 1 -maxdepth 3 \( -name meson.build -o -name configure \) -printf '%h\n' | sort | head -1)
            mv "$top" "$dir"
            rm -rf "$dir.x"
            # our changes to it (scripts/patches/NAME/*.patch), each saying why
            for p in "$ROOT/scripts/patches/$name"/*.patch; do
                [ -e "$p" ] && patch -d "$dir" -p1 -s < "$p"
            done
        fi
        echo "$dir"
        return
    done
    echo "build-base: no source for $name" >&2
    exit 1
}

# Runs a command in the Freedesktop SDK, with this tree and the staged
# base visible, and the base's libraries found before the SDK's own.
in_sdk() {
    flatpak run --user --devel --share=network --filesystem="$ROOT" --filesystem="$OUT" --filesystem="$BLD" --env=PKG_CONFIG_PATH="$BLD/pc" \
        --env=LD_LIBRARY_PATH="$OUT/usr/lib/x86_64-linux-gnu:$OUT/usr/lib" --env=PATH="$OUT/usr/bin:/usr/bin" \
        --env=CPATH="$OUT/usr/include" --env=PYTHONPATH="$BLD/pylib" --env=LIBRARY_PATH="$OUT/usr/lib/x86_64-linux-gnu:$OUT/usr/lib" \
        --command=bash org.freedesktop.Sdk//$RT -c "$*"
}

# The staged libraries' pkg-config files, pointing at the staging tree (the
# originals say /usr, which inside the SDK is the SDK's own).
fix_pc() {
    mkdir -p "$BLD/pc"
    local f
    for f in "$OUT"/usr/lib/x86_64-linux-gnu/pkgconfig/*.pc "$OUT"/usr/share/pkgconfig/*.pc "$OUT"/usr/lib/pkgconfig/*.pc; do
        [ -e "$f" ] || continue
        sed -e "s|^prefix=/usr|prefix=$OUT/usr|" -e "s|=/usr/|=$OUT/usr/|g" "$f" > "$BLD/pc/$(basename "$f")"
    done
}

# Pure-Python build helpers the SDK lacks (Flatpak's build wants pyparsing),
# unpacked from their wheels on PyPI.
pylib() {
    local dir=$BLD/pylib
    [ -d "$dir/pyparsing" ] && return
    mkdir -p "$dir"
    local url
    url=$(curl -fsSL https://pypi.org/pypi/pyparsing/json | python3 -c \
        'import json,sys; d=json.load(sys.stdin); print([u["url"] for u in d["urls"] if u["filename"].endswith(".whl")][0])')
    curl -fsSL "$url" -o "$dir/pyparsing.whl"
    (cd "$dir" && python3 -c 'import zipfile; zipfile.ZipFile("pyparsing.whl").extractall(".")')
}

meson_build() { # name [meson options...]
    local name=$1; shift
    local src; src=$(fetch "$name")
    local b=$BLD/$name
    rm -rf "$b"
    in_sdk "meson setup '$b' '$src' --prefix=/usr --libdir=lib/x86_64-linux-gnu --buildtype=release $* && ninja -C '$b' && DESTDIR='$OUT' ninja -C '$b' install"
    fix_pc
}

auto_build() { # name [configure options...]
    local name=$1; shift
    local src; src=$(fetch "$name")
    local b=$BLD/$name
    rm -rf "$b"
    mkdir -p "$b"
    in_sdk "cd '$b' && '$src/configure' --prefix=/usr --libdir=/usr/lib/x86_64-linux-gnu --sysconfdir=/etc --localstatedir=/var $* && make -j\$(nproc) && make DESTDIR='$OUT' install"
    fix_pc
}

platform() { # the runtime's files as /usr
    mkdir -p "$OUT/usr"
    rsync -a --delete-excluded "$PLATFORM/" "$OUT/usr/"
    # extension points (docs, locales, GL) that are not installed: real directories
    find "$OUT/usr" -maxdepth 3 -xtype l -lname '*runtime/*' -exec sh -c 'rm -f "$1" && mkdir -p "$1"' _ {} \;
    # the dynamic loader where binaries look for it, and the merged-/usr links
    mkdir -p "$OUT/lib64"
    ln -sfn ../usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 "$OUT/lib64/ld-linux-x86-64.so.2"
    for d in lib; do [ -e "$OUT/$d" ] || ln -s usr/$d "$OUT/$d"; done
}


flatpak_system() { # the base /usr is the installed Platform runtime; Flathub configured
    local dep; dep=$(readlink -f "$HOME/.local/share/flatpak/runtime/org.freedesktop.Platform/x86_64/$RT/active")
    local commit; commit=$(basename "$dep")
    local inst=$OUT/var/lib/flatpak
    local d=$inst/runtime/org.freedesktop.Platform/x86_64/$RT
    rm -rf "$inst"
    mkdir -p "$d/$commit" "$inst/repo"/{objects,tmp,extensions,state,refs/heads,refs/mirrors,refs/remotes/flathub}
    cp "$dep/deploy" "$dep/metadata" "$d/$commit/"
    ln -s /usr "$d/$commit/files" # the very same files: no second copy
    ln -s "$commit" "$d/active"
    cp "$HOME/.local/share/flatpak/repo/flathub.trustedkeys.gpg" "$inst/repo/"
    cat > "$inst/repo/config" <<CFG
[core]
repo_version=1
mode=bare-user-only
min-free-space-size=500MB
xa.masked=runtime/org.freedesktop.Platform/x86_64/$RT
xa.languages=en

[remote "flathub"]
url=https://dl.flathub.org/repo/
xa.title=Flathub
gpg-verify=true
gpg-verify-summary=true
xa.comment=Central repository of Flatpak applications
xa.homepage=https://flathub.org/
CFG
    mkdir -p "$inst/exports/share/applications" "$inst/exports/share/icons" "$inst/app"
}

llama_server() { # llama.cpp's server (MIT): runs the built-in model and any the person downloads
    local src=$SRC/llama.cpp b=$BLD/llama.cpp
    [ -d "$src/.git" ] || git clone --depth 1 https://github.com/ggml-org/llama.cpp "$src"
    # AVX2 and FMA (any x86-64 PC of the last decade, and the VM's CPU model); no network fetching in it
    in_sdk "cmake -S '$src' -B '$b' -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF \
        -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON >/dev/null && \
        cmake --build '$b' -j\$(nproc) --target llama-server >/dev/null && \
        install -Dm755 '$b/bin/llama-server' '$OUT/usr/bin/llama-server'"
    echo "build-base: llama.cpp $(git -C "$src" log -1 --format=%h)"
}

prism_app() { # Prism, preinstalled as a Flatpak (scripts/build-prism-flatpak.sh makes the bundle)
    local bundle=$HOME/osbuild/com.valware.prism.flatpak
    [ -s "$bundle" ] || { echo "build-base: no $bundle (run scripts/build-prism-flatpak.sh)"; return; }
    # the same layout as a system installation; as a "user" one no root helper is needed
    FLATPAK_USER_DIR=$OUT/var/lib/flatpak flatpak install --user --noninteractive -y --no-related --reinstall --bundle "$bundle"
}

host_etc() { # the runtime's /etc (kept as /usr/etc) is the OS's /etc too, except what the OS owns
    local own=" group passwd nsswitch.conf machine-id mtab resolv.conf localtime os-release issue issue.net "
    mkdir -p "$OUT/etc"
    for e in "$OUT"/usr/etc/*; do
        local n; n=$(basename "$e")
        case "$own" in *" $n "*) continue ;; esac
        [ -e "$OUT/etc/$n" ] && [ ! -L "$OUT/etc/$n" ] && continue # a component installed its own
        ln -sfn "../usr/etc/$n" "$OUT/etc/$n"
    done
    # the usual name for the CA bundle, which programs built elsewhere look for
    local hash=$OUT/usr/etc/pki/ca-trust/extracted/pem/directory-hash
    [ -e "$hash/ca-certificates.crt" ] || ln -s ../tls-ca-bundle.pem "$hash/ca-certificates.crt"
}
build_one() {
    case $1 in
    platform) platform ;;
    bubblewrap) meson_build bubblewrap -Dman=disabled -Dtests=false -Dselinux=disabled -Dbash_completion=disabled -Dzsh_completion=disabled ;;
    dbus) meson_build dbus -Dsystemd=disabled -Dx11_autolaunch=disabled -Dmodular_tests=disabled -Ddoxygen_docs=disabled \
            -Dxml_docs=disabled -Dducktype_docs=disabled -Dqt_help=disabled -Dselinux=disabled -Dapparmor=disabled \
            -Dlibaudit=disabled -Dinotify=disabled -Dsystem_socket=/run/dbus/system_bus_socket --localstatedir=/var --sysconfdir=/etc ;;
    xdg-dbus-proxy) meson_build xdg-dbus-proxy -Dman=disabled -Dtests=false ;;
    libxmlb) meson_build libxmlb -Dgtkdoc=false -Dintrospection=false -Dtests=false -Dcli=false -Dstemmer=false ;;
    libfyaml) auto_build libfyaml --disable-static ;;
    appstream) meson_build appstream -Dstemming=false -Dsystemd=false -Dvapi=false -Dqt=false -Dcompose=false -Dapt-support=false \
            -Dgir=false -Dsvg-support=false -Ddocs=false -Dapidocs=false -Dinstall-docs=false -Dzstd-support=true ;;
    ostree) auto_build ostree --with-curl --without-soup --without-soup3 --without-avahi --without-selinux --without-libsystemd \
            --without-dracut --without-mkinitcpio --without-grub2 --disable-man --disable-gtk-doc --disable-introspection \
            --with-gpgme --with-libarchive --without-composefs --disable-rust --disable-rofiles-fuse ;;
    fuse) meson_build fuse -Dexamples=false -Dutils=true -Dtests=false -Duseroot=false -Dinitscriptdir= -Dudevrulesdir=/usr/lib/udev/rules.d ;;
    flatpak) pylib
        # its configure runs /usr/bin/bwrap --version: that path is the OS image's, not the SDK's
        sed -i 's/^    if can_run_host_binaries$/    if false # the OS image has it, not the SDK/' "$(fetch flatpak)/meson.build"
        meson_build flatpak -Dsystem_helper=disabled -Dmalcontent=disabled -Dwayland_security_context=disabled -Dgir=disabled \
            -Ddocbook_docs=disabled -Dman=disabled -Dtests=false -Dsystemd=disabled -Dselinux_module=disabled -Ddconf=disabled \
            -Dsystem_bubblewrap=/usr/bin/bwrap -Dsystem_dbus_proxy=/usr/bin/xdg-dbus-proxy -Dsandboxed_triggers=true \
            -Dgdm_env_file=false -Dhttp_backend=curl -Dauto_sideloading=false -Dlibzstd=enabled -Dinternal_checks=false ;;
    xdg-desktop-portal) meson_build xdg-desktop-portal -Ddocumentation=disabled -Dtests=disabled -Dsystemd=disabled \
            -Dgeoclue=disabled -Dgudev=disabled -Dman-pages=disabled -Dsandboxed-image-validation=disabled \
            -Dsandboxed-sound-validation=disabled -Dinstalled-tests=false ;;
    os-portal) meson_build os-portal ;;
    os-record) meson_build os-record ;;
    llama-server) llama_server ;;
    pulseaudio) # the sound server Flatpak apps talk to; it plays through the kernel's /dev/dsp (OSS)
        meson_build pulseaudio -Ddaemon=true -Dclient=true -Ddoxygen=false -Dman=false -Dtests=false \
            -Ddatabase=simple -Doss-output=enabled -Dalsa=disabled -Dbluez5=disabled -Dgsettings=disabled -Dgtk=disabled \
            -Djack=disabled -Dlirc=disabled -Dopenssl=disabled -Dorc=disabled -Dsystemd=disabled -Dudev=disabled \
            -Dx11=disabled -Dwebrtc-aec=disabled -Dfftw=disabled -Dsoxr=disabled -Dtcpwrap=disabled -Dvalgrind=disabled \
            -Davahi=disabled -Dgstreamer=disabled -Delogind=disabled -Dconsolekit=disabled -Dasyncns=disabled \
            -Dbashcompletiondir=no -Dzshcompletiondir=no --sysconfdir=/etc --localstatedir=/var ;;
    host-etc) host_etc ;;
    e2fsprogs) auto_build e2fsprogs --disable-nls --disable-fuse2fs --disable-defrag --disable-imager --disable-debugfs --disable-uuidd --disable-libuuid --disable-libblkid --without-systemd-unit-dir --with-udev-rules-dir=no --with-crond-dir=no ;;
    flatpak-system) flatpak_system ;;
    prism-app) prism_app ;;
    *) echo "build-base: unknown component $1" >&2; exit 1 ;;
    esac
    echo "build-base: $1 done"
}

ALL="platform host-etc bubblewrap dbus xdg-dbus-proxy libxmlb libfyaml appstream ostree fuse flatpak xdg-desktop-portal os-portal os-record llama-server pulseaudio e2fsprogs flatpak-system prism-app"
for c in ${*:-$ALL}; do
    build_one "$c"
done
touch "$OUT/.stamp" # the image packs the base again
