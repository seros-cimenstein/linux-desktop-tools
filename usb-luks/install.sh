#!/usr/bin/env bash
# Install usb-luks: prompt to encrypt USB drives on plug-in (keeping their data).
set -euo pipefail
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"
here=$(dirname "$(realpath "$0")")

for bin in cryptsetup udisksctl zenity notify-send rsync sfdisk wipefs resize2fs e2fsck dumpe2fs mkfs.ext4 runuser flock; do
	command -v "$bin" >/dev/null || { echo "missing required program: $bin" >&2; exit 1; }
done

install -Dm755 "$here/usb-luks" /usr/local/sbin/usb-luks
install -Dm644 "$here/usb-luks@.service" /etc/systemd/system/usb-luks@.service
install -Dm644 "$here/99-usb-luks.rules" /etc/udev/rules.d/99-usb-luks.rules
install -d -m 700 /var/lib/usb-luks/staging
install -d -m 755 /etc/usb-luks
[[ -e /etc/usb-luks/ignore ]] ||
	printf '# ID_SERIAL of USB drives never to offer encryption for, one per line\n' >/etc/usb-luks/ignore
systemctl daemon-reload
udevadm control --reload
echo "usb-luks installed. Plug in a USB drive to get the prompt."
echo "Why a drive is (not) offered: usb-luks check sdX1"
