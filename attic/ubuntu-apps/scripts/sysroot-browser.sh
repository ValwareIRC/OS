# The Web app: NetSurf's GTK 3 front end, a small real web browser (HTML,
# CSS, images, HTTPS through libcurl), shown as a Wayland window in the
# shell's compositor. Sourced by scripts/build-sysroot.sh, which provides
# ROOT, OUT, copy_file and copy_libs.
#
# NetSurf is not installed on the build machine, so its two Ubuntu packages
# are downloaded once (apt-get download needs no root) and kept in
# build/browser/debs; every library it loads is an ordinary Ubuntu library
# that is installed (GTK 3, curl, OpenSSL...), copied the usual way.
#
# Inside, /apps/web/app.conf starts /usr/bin/web, which sets up GTK for a
# system without D-Bus or a settings daemon and opens the start page.

BROWSER_CACHE=$ROOT/build/browser
mkdir -p "$BROWSER_CACHE/debs"
if ! ls "$BROWSER_CACHE"/debs/netsurf-gtk_*.deb >/dev/null 2>&1; then
    (cd "$BROWSER_CACHE/debs" && apt-get download netsurf-gtk netsurf-common >/dev/null 2>&1) || true
fi
if ls "$BROWSER_CACHE"/debs/netsurf-gtk_*.deb >/dev/null 2>&1; then
    rm -rf "$BROWSER_CACHE/root"
    for deb in "$BROWSER_CACHE"/debs/*.deb; do dpkg-deb -x "$deb" "$BROWSER_CACHE/root"; done
    install -m 755 "$BROWSER_CACHE/root/usr/bin/netsurf-gtk" "$OUT/usr/bin/netsurf-gtk"
    copy_libs "$OUT/usr/bin/netsurf-gtk"
    mkdir -p "$OUT/usr/share/netsurf"
    cp -a "$BROWSER_CACHE/root/usr/share/netsurf/." "$OUT/usr/share/netsurf/"
    # (ca-bundle.txt links to /etc/ssl/certs/ca-certificates.crt, in the initrd)

    # GTK's own needs at run time: settings schemas (the file chooser reads
    # them), the image loaders for SVG icons, and the icon theme's symbolic
    # icons for the toolbar (cursors are copied above already).
    copy_file /usr/share/glib-2.0/schemas/gschemas.compiled
    PIXBUF=/usr/lib/x86_64-linux-gnu/gdk-pixbuf-2.0/2.10.0
    if [ -d "$PIXBUF" ]; then
        mkdir -p "$OUT$PIXBUF"
        cp -rL "$PIXBUF/." "$OUT$PIXBUF/"
        for so in "$PIXBUF"/loaders/*.so; do [ -e "$so" ] && copy_libs "$so"; done
    fi
    for d in index.theme icon-theme.cache 16x16 scalable symbolic; do
        if [ -e "/usr/share/icons/Adwaita/$d" ]; then
            mkdir -p "$OUT/usr/share/icons/Adwaita"
            cp -rL "/usr/share/icons/Adwaita/$d" "$OUT/usr/share/icons/Adwaita/"
        fi
    done
    [ -f /usr/share/icons/hicolor/index.theme ] && copy_file /usr/share/icons/hicolor/index.theme
    # This gdk-pixbuf hands images to glycin, whose decoders are programs of
    # their own (it runs them without a sandbox when bwrap can't be used, as
    # here); without them GTK can load no icon at all.
    for d in /usr/libexec/glycin-loaders /usr/share/glycin-loaders; do
        if [ -d "$d" ]; then
            mkdir -p "$OUT$d"
            cp -rL "$d/." "$OUT$d/"
        fi
    done
    for exe in /usr/libexec/glycin-loaders/*/*; do [ -x "$exe" ] && [ -f "$exe" ] && copy_libs "$exe"; done
    # glycin starts each decoder through bwrap; there are no namespaces to
    # sandbox with here yet, so a stand-in runs the decoder as it is.
    : # the real bubblewrap comes with the base system now
    # The MIME database's indexes: gdk-pixbuf tells an SVG from a PNG by
    # them, and without them GTK can't load its own icons and gives up.
    for f in mime.cache globs globs2 magic aliases subclasses types icons generic-icons XMLnamespaces treemagic version; do
        [ -e "/usr/share/mime/$f" ] && copy_file "/usr/share/mime/$f"
    done
    # GTK builds a keymap of its own at start (before the compositor sends
    # one) from the xkb rules and layouts; without them it stops at once.
    mkdir -p "$OUT/usr/share/X11"
    cp -rL /usr/share/X11/xkb "$OUT/usr/share/X11/"

    install -m 755 "$ROOT/userland/apps/web/web" "$OUT/usr/bin/web"
    mkdir -p "$OUT/usr/share/os/web" "$OUT/apps/web"
    cp "$ROOT/userland/apps/web/start.html" "$OUT/usr/share/os/web/"
    cp "$ROOT/userland/apps/web/app.conf" "$OUT/apps/web/app.conf"
else
    echo "sysroot: netsurf-gtk could not be downloaded, the Web app is left out" >&2
fi
