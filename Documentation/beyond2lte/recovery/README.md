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

Keep a normal entry and `safemode.conf`. Do not make a test kernel the
default. The templates use `<BOOT_UUID>` and `<ROOT_UUID>`; do not publish the
values from one phone.

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
`Documentation/staging/xz.rst`). That CRC32 fixes the boot is not yet
confirmed by a successful boot.
