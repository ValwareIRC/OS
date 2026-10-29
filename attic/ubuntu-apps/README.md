# Retired: apps carried over from Ubuntu

Until 2026-10-12 the image had a second boot module, apps.cpio: an Ubuntu
sysroot (python3, bash, weston demos and their libraries), the NetSurf "Web"
app, and Prism unpacked from its AppImage with Ubuntu's libraries copied in.

That approach only worked app by app. Apps now come from Flatpak on the
Freedesktop runtime the base system is made of (Prism: packaging/com.valware.prism).
Kept here for reference only; nothing builds from it.
