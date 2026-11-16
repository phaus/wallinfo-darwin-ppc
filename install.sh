#!/bin/sh
# wallinfo install — run as root ON the target Mac:
#   sudo sh install.sh
#
# Installs both binaries to /usr/local/libexec, the arch-picking wrapper to
# /usr/local/bin/wallinfo and the LaunchAgent to /Library/LaunchAgents.
set -e
cd "$(dirname "$0")"

LIBEXEC=/usr/local/libexec
BIN=/usr/local/bin
PLIST=/Library/LaunchAgents/de.consolving.wallinfo.plist

[ -f wallinfo-ppc ]   || { echo "wallinfo-ppc missing"; exit 1; }
[ -f wallinfo-ppc64 ] || { echo "wallinfo-ppc64 missing"; exit 1; }
[ -f wallinfo-wrapper ] || { echo "wallinfo-wrapper missing"; exit 1; }
[ -f de.consolving.wallinfo.plist ] || { echo "plist missing"; exit 1; }

mkdir -p "$LIBEXEC"
install -m 755 wallinfo-ppc   "$LIBEXEC/wallinfo-ppc"
install -m 755 wallinfo-ppc64 "$LIBEXEC/wallinfo-ppc64"
install -m 755 wallinfo-wrapper "$BIN/wallinfo"
install -m 644 de.consolving.wallinfo.plist "$PLIST"

echo "installed."
echo "test run:      $BIN/wallinfo -o"
echo "restore:       $BIN/wallinfo -r"
echo "load agent:    launchctl load $PLIST"
