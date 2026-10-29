# Sourced: puts the tools from scripts/get-build-tools.sh on the PATH.
TOOLS=${OSTOOLS:-$HOME/ostools}
export PATH="$TOOLS/root/usr/bin:$TOOLS/root/usr/sbin:$PATH"
export LD_LIBRARY_PATH="$TOOLS/root/usr/lib/x86_64-linux-gnu:$TOOLS/root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PYTHONPATH="$TOOLS/root/usr/lib/python3/dist-packages${PYTHONPATH:+:$PYTHONPATH}"
# flatpak looks for its helpers and data under its own prefix
export FLATPAK_SYSTEM_DIR="$TOOLS/flatpak-system"
export XDG_DATA_DIRS="$TOOLS/root/usr/share:${XDG_DATA_DIRS:-/usr/share}"
export FLATPAK_VALIDATE_ICON="$HOME/ostools/root/usr/libexec/flatpak-validate-icon" # flatpak looks in /usr/libexec otherwise
