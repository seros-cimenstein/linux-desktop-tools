#!/usr/bin/env bash
# Remove usb-luks. Encrypted drives stay encrypted; keys in ~/.local/share/usb-luks/keys are kept.
set -euo pipefail
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"
rm -f /etc/udev/rules.d/99-usb-luks.rules /etc/systemd/system/usb-luks@.service /usr/local/sbin/usb-luks
udevadm control --reload
systemctl daemon-reload
if [[ -n $(ls -A /var/lib/usb-luks/staging 2>/dev/null) ]]; then
	echo "kept /var/lib/usb-luks/staging: it holds files of an unfinished copy-back"
else
	rm -rf /var/lib/usb-luks
fi
echo "usb-luks removed (kept /etc/usb-luks/ignore and per-user keys)"
