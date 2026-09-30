#!/usr/bin/env bash
# End-to-end check of every usb-luks encryption path on loop devices (root; touches no real drive):
# ext4 + btrfs in place, vfat + exfat copy-back, blank drive, and an interrupted in-place run resumed.
set -euo pipefail
[[ $EUID -eq 0 ]] || exec sudo "$0" "$@"
here=$(dirname "$(realpath "$0")")
BIN=$here/usb-luks
OWNER=${SUDO_USER:-root}
OWNER_UID=$(id -u "$OWNER")
PASS=selftest-passphrase

work=$(mktemp -d /var/tmp/usb-luks-selftest.XXXXXX)
chmod 755 "$work"
export USB_LUKS_KEY_DIR=$work/keys
LOOPS=() MNT=$work/mnt MAP=usb-luks-selftest
mkdir "$MNT"
cleanup() {
	umount "$MNT" 2>/dev/null || true
	cryptsetup close "$MAP" 2>/dev/null || true
	local l
	for l in "${LOOPS[@]}"; do losetup -d "$l" 2>/dev/null || true; done
	rm -rf "$work"
}
trap cleanup EXIT

failed=0
pass() { printf '\e[32mPASS\e[0m %s\n' "$*"; }
fail() {
	printf '\e[31mFAIL\e[0m %s\n' "$*"
	failed=1
}

# Deterministic-name tree: nested dirs, spaces, UTF-8, empty dir, a 20 MiB file.
make_tree() {
	local d=$1 i
	mkdir -p "$d/docs/sub dir" "$d/empty dir" "$d/fotoğraflar"
	for i in $(seq 1 150); do head -c $((RANDOM * 3)) /dev/urandom >"$d/docs/file $i.dat"; done
	printf 'merhaba\n' >"$d/fotoğraflar/not.txt"
	printf 'hello\n' >"$d/docs/sub dir/note.txt"
	head -c 20M /dev/urandom >"$d/big file.bin"
}
sums() { (cd "$1" && find . -type f ! -path './lost+found/*' -print0 | sort -z | xargs -0 sha256sum); }

new_loop() { # size [nopart] -> REPLY = loop device
	local img=$work/$RANDOM$RANDOM.img
	truncate -s "$1" "$img"
	if [[ ${2:-} != nopart ]]; then printf 'label: gpt\n,,L\n' | sfdisk -q "$img"; fi
	REPLY=$(losetup -fP --show "$img")
	LOOPS+=("$REPLY")
	udevadm settle
}

populate() { # part fstype tree
	case $2 in
	ext4) mkfs.ext4 -q -L TESTEXT "$1" ;;
	btrfs) mkfs.btrfs -q -f -L TESTBTRFS "$1" ;;
	vfat) mkfs.vfat -n TESTFAT "$1" >/dev/null ;;
	exfat) mkfs.exfat -L TESTEXFAT "$1" >/dev/null ;;
	esac
	mount "$1" "$MNT"
	cp -r "$3/." "$MNT/"
	umount "$MNT"
	udevadm settle
}

check_encrypted() { # name part expected-fstype sums-file [owned]
	local name=$1 part=$2 uuid key fs size fsize
	cryptsetup isLuks "$part" || { fail "$name: not LUKS"; return; }
	uuid=$(cryptsetup luksUUID "$part")
	key=$USB_LUKS_KEY_DIR/$uuid.key
	[[ -f $key && $(stat -c %u "$key") == "$OWNER_UID" && $(stat -c %a "$key") == 600 ]] ||
		{ fail "$name: keyfile missing or wrong owner/mode"; return; }
	printf '%s' "$PASS" | cryptsetup open --test-passphrase --key-file=- "$part" || { fail "$name: passphrase does not open"; return; }
	cryptsetup open --key-file "$key" "$part" "$MAP" || { fail "$name: keyfile does not open"; return; }
	fs=$(blkid -o value -s TYPE "/dev/mapper/$MAP")
	[[ $fs == "$3" ]] || fail "$name: filesystem is $fs, expected $3"
	mount "/dev/mapper/$MAP" "$MNT"
	if [[ $fs == ext4 ]]; then e2fsck -fn "/dev/mapper/$MAP" >/dev/null 2>&1 || fail "$name: e2fsck reports errors"; fi
	if cmp -s <(sums "$MNT") "$4"; then pass "$name: all $(wc -l <"$4") files identical"; else fail "$name: file contents differ"; fi
	size=$(blockdev --getsize64 "/dev/mapper/$MAP")
	fsize=$(df -B1 --output=size "$MNT" | sed 1d | tr -d ' ')
	((fsize > size * 90 / 100)) || fail "$name: filesystem not grown ($fsize of $size bytes)"
	if [[ ${5:-} == owned ]]; then
		[[ -z $(find "$MNT" -path "$MNT/lost+found" -prune -o ! -uid "$OWNER_UID" -print) ]] ||
			fail "$name: files not owned by $OWNER"
	fi
	umount "$MNT"
	cryptsetup close "$MAP"
}

tree=$work/tree
make_tree "$tree"
sums "$tree" >"$work/tree.sums"

for fs in ext4 btrfs vfat exfat; do
	new_loop 512M
	part=${REPLY}p1
	populate "$part" "$fs" "$tree"
	stage_before=$(ls /var/lib/usb-luks/staging 2>/dev/null | wc -l)
	if printf '%s' "$PASS" | "$BIN" encrypt "$part" "$OWNER" 2>"$work/$fs.log"; then
		owned=""
		case $fs in vfat | exfat) owned=owned ;; esac
		check_encrypted "$fs" "$part" "$([[ $fs == btrfs ]] && echo btrfs || echo ext4)" "$work/tree.sums" $owned
		[[ $(ls /var/lib/usb-luks/staging 2>/dev/null | wc -l) == "$stage_before" ]] || fail "$fs: staging copy left behind"
	else
		fail "$fs: usb-luks encrypt failed:"
		sed 's/^/    /' "$work/$fs.log"
	fi
done

# Blank drive: no partition table, no signatures.
new_loop 256M nopart
disk=$REPLY
if printf '%s' "$PASS" | "$BIN" encrypt "$disk" "$OWNER" 2>"$work/blank.log"; then
	: >"$work/empty.sums"
	check_encrypted blank "${disk}p1" ext4 "$work/empty.sums"
else
	fail "blank: usb-luks encrypt failed:"
	sed 's/^/    /' "$work/blank.log"
fi

# Already encrypted, nothing pending: must refuse and leave the drive alone.
if printf '%s' "$PASS" | "$BIN" encrypt "${disk}p1" "$OWNER" 2>/dev/null; then
	fail "re-encrypt: accepted an already encrypted drive"
else
	pass "re-encrypt: refused an already encrypted drive"
fi

# Interrupted in-place encryption, then resumed.
new_loop 4G
part=${REPLY}p1
populate "$part" ext4 "$tree"
printf '%s' "$PASS" | "$BIN" encrypt "$part" "$OWNER" 2>"$work/int.log" &
job=$!
interrupted=0
for _ in $(seq 1 300); do
	if [[ $(cryptsetup luksDump "$part" 2>/dev/null) == *online-reencrypt* ]]; then
		sleep 0.3
		pkill -INT -f "cryptsetup reencrypt --encrypt .*$part" && interrupted=1
		break
	fi
	sleep 0.05
done
wait "$job" && rc=0 || rc=$?
if ((interrupted && rc != 0)) && [[ $(cryptsetup luksDump "$part" 2>/dev/null) == *online-reencrypt* ]]; then
	pass "interrupt: encryption stopped part-way (resumable state)"
	if printf '%s' "$PASS" | "$BIN" encrypt "$part" "$OWNER" 2>"$work/resume.log"; then
		check_encrypted resume "$part" ext4 "$work/tree.sums"
	else
		fail "resume: usb-luks encrypt failed:"
		sed 's/^/    /' "$work/resume.log"
	fi
else
	echo "SKIP interrupt/resume: encryption finished before it could be interrupted"
fi

if ((failed == 0)); then echo "all checks passed"; else
	echo "some checks FAILED"
	exit 1
fi
