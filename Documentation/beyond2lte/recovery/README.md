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
