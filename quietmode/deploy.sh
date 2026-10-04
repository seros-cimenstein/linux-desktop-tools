#!/usr/bin/env bash
# ./deploy.sh HOST [install|uninstall] — copy this repo to HOST:~/quietmode and run install.sh there.
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
host=${1:?usage: $0 HOST [install|uninstall]}
rsync -a --delete --exclude .git ./ "$host:quietmode/"
ssh -t "$host" "cd ~/quietmode && ./install.sh ${2:-install}"
