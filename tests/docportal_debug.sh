#!/bin/sh
# Runs inside the OS as the person (fw_cfg autorun): the portals restart with
# their debug messages on, so the serial log says why a document is refused.
dbus-update-activation-environment G_MESSAGES_DEBUG=all
pkill -f xdg-document-portal; pkill -f xdg-desktop-portal
echo "DOCDEBUG on"
