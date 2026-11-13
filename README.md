# Linux for Samsung Exynos 9820

A community kernel tree for bringing Linux to Samsung Exynos 9820 devices, including the Galaxy S10 series. Our aim is to share tested code, report device support honestly, and work toward upstream Linux support.

**Status: experimental, not a finished phone OS or an installation image.** The WLAN changes are in this repository's `main` branch; they have not been submitted to or accepted by the upstream Linux kernel.

## Hardware status

The results below were measured on a **Galaxy S10+ SM-G975F (`beyond2lte`)**, using a postmarketOS-based system and a custom U-Boot/initramfs setup. Wi-Fi used kernel `7.2.0-rc2-g160cc32e597f` ([tested commit](https://github.com/omnipha/linux-samsung-s10-exynos9820/commit/160cc32e597f)); the GPU test used the experimental `issue-8-mali-g76` branch at `f31200d85813`. They do not establish support for other models or all hardware revisions.

| Component | Observed status |
| --- | --- |
| Boot and storage | Kernel boots and mounts the root filesystem with the custom boot setup. The test system experienced ext4 corruption; long-term storage reliability is not established. |
| USB networking | USB gadget networking and SSH worked with the test initramfs and device-specific setup. |
| Display and touch | Simple framebuffer console works (`/dev/fb0` present). Touchscreen (`s6sy761`) is built in. On kernel `7.2.0-rc2-unified-mb007-tsp-gbddb73611a09` the driver power-cycles its `vdd`/`avdd` supplies before the first probe read, and the touch input device was present in `/proc/bus/input/devices` on four consecutive warm boots (`c2d3f036`, `1d8dbd5f`, `0178f631`, `37dda04e`). On the later Bluetooth kernel `7.2.0-rc2-unified-mb007-bt-ga260b60c67d9-dirty` the owner confirmed correct touch hits, including in a 90-degree rotated Phosh session, and a visibly rendered Phosh desktop on the panel. Multi-touch, axis calibration and suspend/resume were not tested. |
| USB data routing | On kernel `7.2.0-rc2-unified-mb007-muic-gbd38316687d6` the MAX77705 CCIC driver routes the USB data lines itself. With the userspace `mb007-muic-route.service` disabled, the host enumerated the gadget and SSH worked on two consecutive warm boots (`3a5133cd`, `5cc42acc`), 64 seconds after each reboot. `EXTCON_USB` reported attached and the gadget was configured at high speed. Charging and power-role behaviour were not measured. |
| BCM4375 Wi-Fi | PCIe Gen1 x1 link, firmware 18.41.117, scan, WPA2 association, DHCP and SSH over Wi-Fi were verified after manual setup. |
| GPU | On `beyond2lte`, the Mali-G76 (MP12, GPU ID `0x7211`) probes with panfrost (`/dev/dri/renderD128`). `kmscube -D /dev/dri/card0 -c 600` reported `Mali-G76 MC12 (Panfrost)`, rendered 599 frames in 19.14 seconds, and exited 0; afterward `/proc/interrupts` showed 1201 `panfrost-job` interrupts and no GPU faults. A passive 88 °C G3D cooling trip is wired to the devfreq cooling device but has not been exercised (the device stayed near 40 °C). Higher-than-433 MHz rates, suspend/resume and thermal throttling are not verified. |
| Bluetooth | On 13 November 2026, one `beyond2lte` unit booted `7.2.0-rc2-unified-mb007-bt-ga260b60c67d9-dirty`. `bluetoothctl power on` succeeded and a discovery scan found 40 devices. On that same boot the owner confirmed that touch hits were correct, including in a 90-degree rotated Phosh session, and that Phosh rendered visibly on the panel. Generic public BD address handling, pairing and suspend/resume remain unverified. See Bluetooth notes below for the firmware and test conditions. |
| Mobile network, audio, sensors, cameras | Not verified by this project. |

The tree also contains a device tree for `beyond1lte` (Galaxy S10); we have not verified it on hardware. Other S10-series models need their own verification.

### Known Wi-Fi limitations

- The test used Broadcom firmware and a board-specific Semco CS01 NVRAM candidate installed under `brcmfmac4375b1-pcie.txt`. Other hardware variants may require different calibration data; firmware and NVRAM are not provided here. Correct regulatory configuration still needs verification.
- Network setup was manual/transient. Automatic connection across reboot has not been verified: [issue #3](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/3).
- Creating the P2P device timed out (`err=-5`): [issue #4](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/4). Suspend/resume is unverified.
- The PCIe binding treats `reset-gpios` as optional while the driver requires it: [issue #5](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/5).

The detailed Wi-Fi bring-up evidence is in [issue #1](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/1#issuecomment-6782996563) and [PR #2](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/2). PCIe and WLAN changes from that PR were merged into `main`.

### Bluetooth notes

- For this unit, stock device-tree overlay 9 confirmed the Bluetooth enable pin as `gpm8-0`. The Bluetooth controller is on serial UART1 (`serial1`, through `usi_uart_bt`) with the device-tree compatible `brcm,bcm4329-bt`. The tested config enables `CONFIG_BT_HCIUART=y`, `CONFIG_BT_HCIUART_BCM=y` and `CONFIG_SERIAL_DEV_BUS=y` in `beyond2lte_defconfig`; Kconfig also resolves `CONFIG_BT_BCM`, `CONFIG_BT_HCIUART_SERDEV`, `CONFIG_BT_HCIUART_H4` and `CONFIG_SERIAL_DEV_CTRL_TTYPORT` to `y`.
- Bluetooth firmware is not distributed here and no HCD file was added to this repository. The test used the device-only stock B1 Semco Sem HCD from the unit's own vendor firmware; its loaded firmware version matched stock, build 0173. The driver probes early, so the HCD must be in the private on-device initramfs as `usr/lib/firmware/brcm/BCM4362A2.samsung,beyond2lte.hcd`. That initramfs is not published; other hardware variants may need different firmware.
- After the reboot, USB SSH worked, WLAN had carrier and an access-point ping succeeded. A `phosh` process, `/dev/dri/renderD128`, `/dev/fb0` and the touch driver were present; there were no EXT4 errors and exactly three loader entries. On the same boot the owner confirmed correct touch hits, including in a 90-degree rotated Phosh session, and confirmed that Phosh rendered visibly on the panel.
- The run used the kernel built from `a260b60c67d9` plus the then-uncommitted six-file diff in `arch/arm64/boot/dts/exynos/exynos9820-beyond2lte.dts`, `arch/arm64/boot/dts/exynos/exynos9820-pinctrl.dtsi`, `arch/arm64/boot/dts/exynos/exynos9820.dtsi`, `arch/arm64/configs/beyond2lte_defconfig`, `drivers/clk/samsung/clk-exynos9820.c` and `include/dt-bindings/clock/exynos9820.h`. Those source changes are now committed as part of the Bluetooth bring-up; the booted kernel image itself was built before that commit and therefore reports `a260b60c67d9-dirty`.
- Only controller power-on and device discovery were measured. Generic public BD address handling, pairing, audio and suspend/resume have not been verified.

## Building

The source is based on Linux 7.2-rc2, via the [`exynos9820-7.2-rc2` branch](https://github.com/chiffathefox/exynos-9820-mainline-linux/tree/exynos9820-7.2-rc2). This is a kernel source tree, not a complete bootable image.

`arch/arm64/configs/beyond2lte_defconfig` is a minimal defconfig, generated with `make savedefconfig` from the configuration of a unified kernel built from this tree at commit `9267a2137168`. That build, and the verification of the defconfig, used Ubuntu clang/LLD 17.0.6 (`LLVM=-17`); regenerating the configuration with `make O=out ARCH=arm64 LLVM=-17 beyond2lte_defconfig` reproduced the built configuration exactly, apart from `CONFIG_LOCALVERSION` (the built kernel carried a private local version string, which is not part of the defconfig). The defconfig keeps the tested base-kernel settings — display (`CONFIG_DRM_SIMPLEDRM=y`) and touch (`CONFIG_TOUCHSCREEN_S6SY761=y`) built in — and adds the WLAN pieces as modules or built-ins (`CONFIG_PCIE_EXYNOS9820=m`, `CONFIG_BRCMFMAC=m`, `CONFIG_BRCMFMAC_PCIE=y`). It now also selects the Bluetooth UART options described above.

The tested Bluetooth kernel was built from the six-file dirty tree above, starting from the previous in-tree MUIC `.config`, not directly from the defconfig. The previous config was backed up and these in-tree commands were run:

```sh
cp .config /tmp/opencode/config.muic.bak
scripts/config --enable BT_HCIUART --enable BT_HCIUART_BCM --enable SERIAL_DEV_BUS --set-str LOCALVERSION '-unified-mb007-bt'
make ARCH=arm64 LLVM=-17 olddefconfig
make -j"$(nproc)" ARCH=arm64 LLVM=-17 Image modules dtbs
make ARCH=arm64 LLVM=-17 INSTALL_MOD_PATH="$PWD/out/bt-staging" modules_install
```

The backup path was specific to this build; choose an existing writable directory when repeating it. The tested source changes were uncommitted at build time, not part of `a260b60c67d9`; they are included in the Bluetooth bring-up commit.

That is not the command to use on a clean checkout, because it depends on the previous `.config`. On a clean source tree at `a260b60c67d9` with the six-file Bluetooth diff applied, the following reproduces the tested `.config` exactly, including `CONFIG_LOCALVERSION="-unified-mb007-bt"` and every dependency selected by `olddefconfig`. This was checked with Ubuntu clang/LLD 17.0.6 by generating only the configuration (the first three commands below) in scratch output directories from a copy of the tracked files plus the six-file diff, and comparing it byte for byte with the tested `.config`. The committed `HEAD` defconfig, with `CONFIG_LOCALVERSION` set to `-unified-mb007-muic`, likewise reproduced the recorded previous configuration. The full build and `modules_install` commands below were not re-run from a clean tree for this README.

```sh
make O=out ARCH=arm64 LLVM=-17 beyond2lte_defconfig
scripts/config --file out/.config --set-str LOCALVERSION '-unified-mb007-bt'
make O=out ARCH=arm64 LLVM=-17 olddefconfig
make -j"$(nproc)" O=out ARCH=arm64 LLVM=-17 Image modules dtbs
make O=out ARCH=arm64 LLVM=-17 INSTALL_MOD_PATH="$PWD/out/bt-staging" modules_install
```

`olddefconfig` reports no change after the local-version assignment. The tested kernel release gained `-ga260b60c67d9-dirty` from `CONFIG_LOCALVERSION_AUTO=y` because the six source files were uncommitted at build time. A clean tree at the Bluetooth bring-up commit gets its own commit suffix instead. The source tree must be clean before the first `O=out` build; an existing in-tree configuration makes the kernel reject an out-of-tree output directory.

Cross-build with Clang/LLVM 17 and the usual kernel build dependencies:

```sh
git clone https://github.com/omnipha/linux-samsung-s10-exynos9820.git
cd linux-samsung-s10-exynos9820
make O=out ARCH=arm64 LLVM=-17 beyond2lte_defconfig
make -j"$(nproc)" O=out ARCH=arm64 LLVM=-17 Image modules dtbs
make O=out ARCH=arm64 LLVM=-17 INSTALL_MOD_PATH="$PWD/out/staging" modules_install
```

The S10+ device tree is `out/arch/arm64/boot/dts/exynos/exynos9820-beyond2lte.dtb`; the Galaxy S10 tree `exynos9820-beyond1lte.dtb` is built alongside it but has not been verified on hardware. The kernel is `out/arch/arm64/boot/Image`. `modules_install` places the modules in `out/staging/lib/modules/<kernel release>/`, zstd-compressed (`CONFIG_MODULE_COMPRESS_ZSTD=y`), and runs `depmod`; copy that directory to `/lib/modules/` on the target root filesystem. The install step was run for the built kernel with a relative `INSTALL_MOD_PATH`; the exact command above was not re-run for this README.

Host tools used by the build: Ubuntu clang, LLD and llvm tools 17.0.6 (`clang-17`, `ld.lld-17`), flex, bison, bc, pahole for BTF (`CONFIG_DEBUG_INFO_BTF=y`) and the standard kmod `depmod`. The built modules are zstd-compressed; decompression uses the kernel's own zstd support (`CONFIG_MODULE_DECOMPRESS=y`), not host zstd.

The unified kernel built from this configuration compiled through with Ubuntu clang/LLD 17.0.6 (ARM64 `Image`, `dtbs`, and the WLAN/PCIe modules linked). Three consecutive warm boots of this unified kernel were measured on the device: `/dev/fb0` present, `brcmfmac` loaded, `wlan0` visible, no EXT4 errors, and `libgcc_s.so.1` hash equal to the original image. On the later kernel `7.2.0-rc2-unified-mb007-tsp-gbddb73611a09` the touchscreen probe succeeded on four consecutive warm boots (see hardware status above). For comparison, the configuration of the earlier base kernel built with Alpine clang 23.1.2 differs from the regenerated `beyond2lte_defconfig` configuration in the WLAN symbols above, `CONFIG_LOCALVERSION`, and symbols that Kconfig derives from the compiler (version and capability checks such as `CONFIG_CLANG_VERSION`, `CONFIG_CC_HAS_COUNTED_BY` and `CONFIG_ARM64_LSUI`); all other symbols match. This does not show whether the compiler or the configuration relates to the earlier wrong rootfs reads in [issue #6](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/6), and no cause is claimed.

## Getting involved

- Browse the [open issues](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues) for known problems and ongoing work.
- Report hardware results with the exact model and variant, kernel commit, configuration, logs and what you tested. Please remove serial numbers, credentials, MAC/IP addresses and other personal data from public logs.
- Contributions and corrections are welcome through GitHub issues and pull requests. For eventual Linux mainline submission, follow the [kernel contribution process](Documentation/process/submitting-patches.rst); merging here does not merge anything upstream.

There is no tested general-purpose installer or recovery guide yet. Bootloader and storage changes can make a device unbootable or erase data; do not flash images based only on this README. Confirm your exact device, backups and recovery path first.

## Credits and license

This tree builds on [chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux). The PCIe/Wi-Fi work used Samsung vendor sources from [LineageOS/android_kernel_samsung_exynos9820](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/a412cfaf86b980ce35cae96a35f678c8ea551575) and [pulpoff/linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte/tree/3c142b4624f3caf9cb0ee90324ef7d62616d2a64) as references; see [PR #2](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/2) for details.

We thank [butztill/s10-linux](https://github.com/butztill/s10-linux) for openly sharing its Galaxy S10 (`beyond1lte`) bring-up work. Its [Linux 7.2-rc2 sources](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303) are an important reference for our Bluetooth work and planned camera, audio, sensor-hub and NFC bring-up ([issues #15–#19](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues)). Results from that project do not establish support on our S10+ (`beyond2lte`) without testing here. When adapting code, we retain the original authorship, SPDX identifiers and applicable license terms. We welcome collaboration and sharing improvements across projects.

Kernel licensing information is in [COPYING](COPYING) and [license-rules.rst](Documentation/process/license-rules.rst). The original general kernel introduction is preserved in [README](README).
