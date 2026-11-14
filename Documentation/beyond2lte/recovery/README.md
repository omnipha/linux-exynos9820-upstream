# beyond2lte crash recovery and USB boot

This is the model-level recovery recipe for a Samsung Galaxy S10+
(`beyond2lte`). It contains no device identifiers, keys, firmware or
filesystem UUIDs. Replace the UUID placeholders from the target installation.

## Persistent crash log

`exynos9820-beyond2lte.dts` reserves `ramoops@fd910000`, 2 MiB, in the stock
`log_kernel` region. The kernel configuration already enables
`CONFIG_PSTORE`, `CONFIG_PSTORE_CONSOLE`, `CONFIG_PSTORE_PMSG` and
`CONFIG_PSTORE_RAM`. After a panic and reboot, read
`/var/lib/systemd/pstore/dmesg-ramoops-0`.

Verified on one SM-G975F on 2026-11-13: a deliberate sysrq crash survived a
warm reboot, including the panic line and call trace. The same DTB node was
also loaded by the safe-mode kernel. A crash while already in safe mode and
survival across a forced button reset are not yet verified.

## USB without replugging

Current `main` contains `drivers/extcon/extcon-max77705.c`, which issues the
same MAX77705 `CTRL1_W(COM_USB)` command from the kernel. Prefer that driver.
The userspace helper here is the measured fallback for a kernel that does not
bind the MUIC driver.

If `mb007-muic-route.service` is absent or disabled, the gadget can be ready
on the phone while the USB host does not enumerate it until the cable is
replugged. This was measured on 2026-11-12 and again on 2026-11-13. Build and
enable the fallback with:

```sh
aarch64-linux-gnu-gcc -static -O2 -Wall -o mb007-max77705-usb-route max77705-usb-route.c
install -m 0755 mb007-max77705-usb-route /usr/local/libexec/
install -m 0644 mb007-muic-route.service /etc/systemd/system/
systemctl enable mb007-muic-route.service
```

The boot entries must contain both `mb007.usb=1` and `mb007.muic=1`. Reboot
verification after re-enabling the service on 2026-11-13 is still pending.
A software gadget disconnect/reconnect restored host enumeration in three
seconds, but that does not replace the boot test.

## Boot menu

The current phone has exactly three loader entries. The normal entry,
`/boot/loader/entries/pmos-mb007.conf`, uses:

- `/boot/vmlinuz-unified-mb007-camera`
- `/boot/initramfs-unified-mb007-camera`
- `/boot/dtbs/exynos/exynos9820-beyond2lte-camera.dtb`

`safemode.conf` and `fsck.conf` use the separate payloads `/boot/vmlinuz`,
`/boot/initramfs-muic-guard`, and their separate DTBs. Do not replace those
payloads or edit any loader entry during normal-entry rollback. Do not make a
test kernel the default.

The published templates are not this layout: both share `vmlinuz`, `initramfs`,
and one DTB. They therefore do not guarantee an independent recovery entry.
They use `<BOOT_UUID>` and `<ROOT_UUID>`; do not publish values from one phone.

## Host side (USB SSH and internet sharing)

`host/` holds the scripts that make the phone reachable without manual steps
after each boot: a udev rule matching the phone's NCM gadget on one physical
USB port, `mb007-usb-autoconnect` (sets the host IP, checks the pinned SSH
host key and the `samsung,beyond2lte` compatible string), and
`mb007-usb-share` (NAT for `172.16.42.1/32` only). Copy `mb007-usb.default` to
`/etc/default/mb007-usb` and fill in your USB path, user and key paths. The
interface name changes on every boot; the scripts match by `ID_PATH`, not by
name. Warm-reboot operation was measured on one host; cold start without a
host and later plug-in were not tested. The diagnostic recovery guard (sudo
grant, recovery timer) from the private repo is deliberately not published.

`phone/` holds `mb007-font.c` (GPL-2.0) and its unit: doubles the Linux
console font to 32x64 for the 6.4" panel. Needs `ter-u32n` installed.

## Initramfs compression (measured 2026-11-13)

The in-kernel XZ decoder rejected an initramfs packed with the default CRC64
check: `rootfs image is not initramfs (Input was encoded with settings that
are not supported by this XZ decoder)`, followed by
`Kernel panic - VFS: Unable to mount root fs on unknown-block(0,0)` at 1.09 s.
A working initramfs had xz stream flags `00 01` (CRC32); the failing one had
`00 04` (CRC64). Pack with `xz --check=crc32` (see
`Documentation/staging/xz.rst`). A CRC32 initramfs containing one XZ stream
with a 1 MiB dictionary booted once with the QCH kernel. CRC32 was not tested
in isolation, so this does not establish that the checksum change alone fixed
the boot.

## Untested rollback of the normal boot entry

This restores only the normal kernel, initramfs and camera DTB from a backup
taken before a test kernel was installed. The restore has not been executed, so
neither this procedure nor the resulting boot is verified. It does not touch
the safe-mode and fsck payloads, the loader entries or `/usr/lib/modules`.

Backup layout (one directory, here `/root/issue17-pre-qch-20261114` on the
phone, with a verified second copy off the device):

- `vmlinuz-unified-mb007-camera`, `initramfs-unified-mb007-camera` and
  `dtbs/exynos/exynos9820-beyond2lte-camera.dtb`: the previous normal payloads.
  The file name says "camera"; the kernel inside is the previous audio build, so
  take the old release from its embedded version string, not from the name.
- `loader/`: copies of `loader.conf` and the three entries.
- `modules/<old release>/`: a copy of the old module tree.
- `live-module-hashes.txt` and `backup-module-hashes.txt`: hashes of the module
  files, relative to `modules/`.
- `safe-fsck-payload-hashes.txt`: hashes of the live safe-mode and fsck
  payloads, relative to `/boot`. They are not copied, so a changed file can be
  detected but not restored from this backup.
- `SHA256SUMS`: hashes of the normal payloads, the loader copies and both
  module manifests, relative to the backup directory.

Prerequisites: stable power, a way to pick a boot-menu entry, root access over
a console or USB SSH, `/boot` mounted read-write with about 64 MiB free (the
staged copies sit next to the old files), and an independent recovery method.
Boot the safe-mode entry, not the normal one, and keep loader contents and
manifests out of public logs.

Put the blocks below in a script and run it with `sh -eu`; they use `exit`, so
pasting them into a login shell would end it. `OLDREL` is the release string
found inside the backed-up kernel.

```sh
BACKUP=/root/issue17-pre-qch-20261114
OLDREL='<release string from the backed-up kernel>'
ENTRIES='<normal entry>.conf <safe entry>.conf <fsck entry>.conf'
KERNEL=vmlinuz-unified-mb007-camera
INITRAMFS=initramfs-unified-mb007-camera
DTB=dtbs/exynos/exynos9820-beyond2lte-camera.dtb

grep -q 'systemd.unit=multi-user.target' /proc/cmdline || exit 1
grep -Eq ' /boot [^ ]+ rw[, ]' /proc/mounts || exit 1
df -Pk /boot | awk 'NR==2 { exit !($4 >= 65536) }' || exit 1
grep -aqF "$OLDREL" "$BACKUP/$KERNEL" || exit 1
(cd "$BACKUP" && sha256sum -c SHA256SUMS) || exit 1
(cd "$BACKUP/modules" && sha256sum -c ../backup-module-hashes.txt) || exit 1
(cd /boot && sha256sum -c "$BACKUP/safe-fsck-payload-hashes.txt") || exit 1
[ "$(find /boot/loader/entries -name '*.conf' | wc -l)" -eq 3 ] || exit 1
for e in $ENTRIES; do
    cmp -s "$BACKUP/loader/entries/$e" "/boot/loader/entries/$e" || exit 1
done
cmp -s "$BACKUP/loader/loader.conf" /boot/loader/loader.conf || exit 1
```

Whether the live modules for `OLDREL` are intact is a separate question:
`(cd /usr/lib/modules && sha256sum -c "$BACKUP/live-module-hashes.txt")`. A
failure means the modules need their own restore, which this procedure does not
cover.

Stage, compare, rename, then check again. Stop if a staging file already exists.

```sh
for n in "$KERNEL" "$INITRAMFS" "$DTB"; do
    [ ! -e "/boot/$n.rollback-new" ] && [ ! -L "/boot/$n.rollback-new" ] || exit 1
done
for n in "$KERNEL" "$INITRAMFS" "$DTB"; do
    install -m 0755 "$BACKUP/$n" "/boot/$n.rollback-new" || exit 1
    cmp -s "$BACKUP/$n" "/boot/$n.rollback-new" || exit 1
done
sync
for n in "$KERNEL" "$INITRAMFS" "$DTB"; do
    mv -f "/boot/$n.rollback-new" "/boot/$n" || {
        echo "ERROR: normal payloads are now mixed; stay in safe mode" >&2
        exit 1
    }
done
sync
echo 3 > /proc/sys/vm/drop_caches
(cd "$BACKUP" && sha256sum -c SHA256SUMS) || exit 1
for n in "$KERNEL" "$INITRAMFS" "$DTB"; do
    cmp -s "$BACKUP/$n" "/boot/$n" || exit 1
done
(cd /boot && sha256sum -c "$BACKUP/safe-fsck-payload-hashes.txt") || exit 1
```

The `SHA256SUMS` check after the renames covers the backup copies, so the
`cmp` loop is what ties the installed files to them. FAT renames are not
crash-atomic and the three renames are not a transaction: after a power loss or
an error, stay in safe mode and look at all three files before a normal boot.
After the first normal boot, check that `uname -r` equals `OLDREL`, that the
payload and loader checks above still pass, and that `/boot/loader/entries`
still holds exactly three entries. Do not call the rollback working before that
boot has been done.
