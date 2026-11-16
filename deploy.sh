#!/bin/sh
# wallinfo deploy — run on the dev-machine. Copies binaries, wrapper, plist
# and install script to the target Mac's /tmp; finish with
#   ssh <host> 'echo <sudo-pw> | sudo -S sh /tmp/install.sh'
#
# usage: ./deploy.sh [user@host]   (default: macmini-g4-1)
set -e
HOST=${1:-macmini-g4-1}
DIR=$(cd "$(dirname "$0")" && pwd)

[ -f "$DIR/bin/wallinfo-ppc" ]   || { echo "run make cross32 first"; exit 1; }
[ -f "$DIR/bin/wallinfo-ppc64" ] || { echo "run make cross64 first"; exit 1; }

scp "$DIR"/bin/wallinfo-ppc "$DIR"/bin/wallinfo-ppc64 "$HOST":/tmp/
scp "$DIR"/wallinfo-wrapper "$DIR"/de.consolving.wallinfo.plist "$DIR"/install.sh "$HOST":/tmp/

echo
echo "uploaded to $HOST:/tmp — finish with:"
echo "  ssh $HOST 'echo <sudo-pw> | sudo -S sh /tmp/install.sh'"
