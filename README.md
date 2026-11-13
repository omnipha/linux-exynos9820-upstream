# Linux for Samsung Exynos 9820

A community kernel tree for bringing Linux to Samsung Exynos 9820 devices, including the Galaxy S10 series. Our aim is to share tested code, report device support honestly, and work toward upstream Linux support.

**Status: experimental, not a finished phone OS or an installation image.** The WLAN changes are in this repository's `main` branch; they have not been submitted to or accepted by the upstream Linux kernel.

## Hardware status

The results below were measured on a **Galaxy S10+ SM-G975F (`beyond2lte`)**, using a postmarketOS-based system and a custom U-Boot/initramfs setup. Wi-Fi used kernel `7.2.0-rc2-g160cc32e597f` ([tested commit](https://github.com/omnipha/linux-samsung-s10-exynos9820/commit/160cc32e597f)); the GPU test used the experimental `issue-8-mali-g76` branch at `f31200d85813`. They do not establish support for other models or all hardware revisions.

| Component | Observed status |
| --- | --- |
| Boot and storage | Kernel boots and mounts the root filesystem with the custom boot setup. The test system experienced ext4 corruption; long-term storage reliability is not established. |
| USB networking | USB gadget networking and SSH worked with the test initramfs and device-specific setup. |
| Display and touch | Simple framebuffer console works (`/dev/fb0` present). Touchscreen (`s6sy761`) is built in. On kernel `7.2.0-rc2-unified-mb007-tsp-gbddb73611a09` the driver power-cycles its `vdd`/`avdd` supplies before the first probe read, and the touch input device was present in `/proc/bus/input/devices` on four consecutive warm boots (`c2d3f036`, `1d8dbd5f`, `0178f631`, `37dda04e`). A desktop and touch events have not been verified. |
| BCM4375 Wi-Fi | PCIe Gen1 x1 link, firmware 18.41.117, scan, WPA2 association, DHCP and SSH over Wi-Fi were verified after manual setup. |
| GPU | On `beyond2lte`, the Mali-G76 (MP12, GPU ID `0x7211`) probes with panfrost (`/dev/dri/renderD128`). `kmscube -D /dev/dri/card0 -c 600` reported `Mali-G76 MC12 (Panfrost)`, rendered 599 frames in 19.14 seconds, and exited 0; afterward `/proc/interrupts` showed 1201 `panfrost-job` interrupts and no GPU faults. A passive 88 °C G3D cooling trip is wired to the devfreq cooling device but has not been exercised (the device stayed near 40 °C). Higher-than-433 MHz rates, suspend/resume and thermal throttling are not verified. |
| Mobile network, audio, Bluetooth, sensors, cameras | Not verified by this project. |

The tree also contains a device tree for `beyond1lte` (Galaxy S10); we have not verified it on hardware. Other S10-series models need their own verification.

### Known Wi-Fi limitations

- The test used Broadcom firmware and a board-specific Semco CS01 NVRAM candidate installed under `brcmfmac4375b1-pcie.txt`. Other hardware variants may require different calibration data; firmware and NVRAM are not provided here. Correct regulatory configuration still needs verification.
- Network setup was manual/transient. Automatic connection across reboot has not been verified: [issue #3](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/3).
- Creating the P2P device timed out (`err=-5`): [issue #4](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/4). Suspend/resume is unverified.
- The PCIe binding treats `reset-gpios` as optional while the driver requires it: [issue #5](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/5).

The detailed Wi-Fi bring-up evidence is in [issue #1](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/1#issuecomment-6782996563) and [PR #2](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/2). PCIe and WLAN changes from that PR were merged into `main`.

## Building

The source is based on Linux 7.2-rc2, via the [`exynos9820-7.2-rc2` branch](https://github.com/chiffathefox/exynos-9820-mainline-linux/tree/exynos9820-7.2-rc2). This is a kernel source tree, not a complete bootable image.

`arch/arm64/configs/beyond2lte_defconfig` is a minimal defconfig, generated with `make savedefconfig` from the configuration of a unified kernel built from this tree at commit `9267a2137168`. That build, and the verification of the defconfig, used Ubuntu clang/LLD 17.0.6 (`LLVM=-17`); regenerating the configuration with `make O=out ARCH=arm64 LLVM=-17 beyond2lte_defconfig` reproduced the built configuration exactly, apart from `CONFIG_LOCALVERSION` (the built kernel carried a private local version string, which is not part of the defconfig). The defconfig keeps the tested base-kernel settings — display (`CONFIG_DRM_SIMPLEDRM=y`) and touch (`CONFIG_TOUCHSCREEN_S6SY761=y`) built in — and adds the WLAN pieces as modules or built-ins (`CONFIG_PCIE_EXYNOS9820=m`, `CONFIG_BRCMFMAC=m`, `CONFIG_BRCMFMAC_PCIE=y`).

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

Kernel licensing information is in [COPYING](COPYING) and [license-rules.rst](Documentation/process/license-rules.rst). The original general kernel introduction is preserved in [README](README).
