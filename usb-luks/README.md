# usb-luks

> **Warning: usb-luks re-encrypts drives.** It rewrites the whole partition (or recreates it),
> and a power cut, a bug or an unplugged cable at the wrong moment can lose data. **Back up a
> drive before you let it encrypt it.** It has not been security-audited.

When a USB drive is plugged in, usb-luks asks (in a zenity dialog) whether to encrypt it with
LUKS2, keeping the data on it. Drives it has encrypted are unlocked and mounted automatically
on this PC; on any other Linux computer they open with the passphrase.

The prompt offers **Encrypt**, **Not now**, or **Never for this drive** (adds the drive's
`ID_SERIAL` to `/etc/usb-luks/ignore`).

## How a drive is encrypted

| Filesystem | Method |
|---|---|
| ext2/3/4, btrfs | shrink the filesystem by 32 MiB, then `cryptsetup reencrypt --encrypt` in place; resumable (replug and enter the passphrase) |
| vfat, exfat, ntfs | copy the files to `/var/lib/usb-luks/staging` on this PC, verify by checksum, recreate the partition as LUKS2 + ext4, copy back, verify, delete the copy; resumable (replug). Needs free space on this PC for the used data plus 1 GB |
| blank drive (no filesystem, no partition table) | GPT + one LUKS2/ext4 partition; anything unrecognised on it is erased |

Every encrypted drive gets the chosen passphrase plus a random keyfile at
`~/.local/share/usb-luks/keys/<LUKS-UUID>.key`, which is used to unlock it through udisks.
Copied-back drives come back as ext4, so they no longer open on Windows or macOS.

Never touched: devices on USB ports the firmware marks "fixed" (built into the PC), anything
that is not pure USB mass storage (phones, cameras, media players), drives on the ignore list,
read-only media, EFI system partitions, partitions in use by RAID/LVM/device-mapper, and
partitions mounted anywhere other than `/run/media`, `/media` or `/mnt`.

## Requirements

Linux with systemd, udev, udisks2 and a graphical session (the prompt goes to the active
session's user), plus: `cryptsetup`, `udisksctl`, `zenity`, `notify-send`, `rsync`, `sfdisk`,
`wipefs`, `resize2fs`, `e2fsck`, `dumpe2fs`, `mkfs.ext4`, `runuser`, `flock` (`install.sh`
checks for them). btrfs drives also need `btrfs-progs`.

## Install

```
sudo ./install.sh       # /usr/local/sbin/usb-luks, udev rule, usb-luks@.service
sudo ./uninstall.sh     # encrypted drives stay encrypted; keys and the ignore list are kept
```

## Usage

Plug in a drive. Other commands:

```
usb-luks check sdX1                           # explain whether sdX1 would be offered, and why not
sudo usb-luks encrypt /dev/sdX1 USER <<<"$pw"  # encrypt (or resume) without the GUI
sudo ./selftest.sh                            # end-to-end test of every method on loop devices; touches no real drive
```

## Status

Personal tool. `selftest.sh` covers ext4 and btrfs in place, vfat and exfat copy-back, a blank
drive and an interrupted in-place run resumed, on loop devices only; that does not cover every
real-world drive, filesystem state or failure (ntfs is not in the self-test). Not
security-audited.
