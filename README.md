# Linux for Samsung Exynos 9820

A community kernel tree for bringing Linux to Samsung Exynos 9820 devices, including the Galaxy S10 series. Our aim is to share tested code, report device support honestly, and work toward upstream Linux support.

**Status: experimental, not a finished phone OS or an installation image.** The WLAN changes are in this repository's `main` branch; they have not been submitted to or accepted by the upstream Linux kernel.

## Hardware status

The results below were measured on a **Galaxy S10+ SM-G975F (`beyond2lte`)**, using a postmarketOS-based system, kernel `7.2.0-rc2-g160cc32e597f` ([tested commit](https://github.com/omnipha/linux-samsung-s10-exynos9820/commit/160cc32e597f)) and a custom U-Boot/initramfs setup. They do not establish support for other models or all hardware revisions.

| Component | Observed status |
| --- | --- |
| Boot and storage | Kernel boots and mounts the root filesystem with the custom boot setup. The test system experienced ext4 corruption; long-term storage reliability is not established. |
| USB networking | USB gadget networking and SSH worked with the test initramfs and device-specific setup. |
| Display and touch | Simple framebuffer console worked; touch was demonstrated in the initramfs. A desktop and accelerated graphics have not been verified. |
| BCM4375 Wi-Fi | PCIe Gen1 x1 link, firmware 18.41.117, scan, WPA2 association, DHCP and SSH over Wi-Fi were verified after manual setup. |
| GPU, mobile network, audio, Bluetooth, sensors, cameras | Not verified by this project. |

The tree also contains a device tree for `beyond1lte` (Galaxy S10); we have not verified it on hardware. Other S10-series models need their own verification.

### Known Wi-Fi limitations

- The test used Broadcom firmware and a board-specific Semco CS01 NVRAM candidate installed under `brcmfmac4375b1-pcie.txt`. Other hardware variants may require different calibration data; firmware and NVRAM are not provided here. Correct regulatory configuration still needs verification.
- Network setup was manual/transient. Automatic connection across reboot has not been verified: [issue #3](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/3).
- Creating the P2P device timed out (`err=-5`): [issue #4](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/4). Suspend/resume is unverified.
- The PCIe binding treats `reset-gpios` as optional while the driver requires it: [issue #5](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/5).

The detailed Wi-Fi bring-up evidence is in [issue #1](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/1#issuecomment-6782996563) and [PR #2](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/2). PCIe and WLAN changes from that PR were merged into `main`.

## Building

The source is based on Linux 7.2-rc2, via the [`exynos9820-7.2-rc2` branch](https://github.com/chiffathefox/exynos-9820-mainline-linux/tree/exynos9820-7.2-rc2). An ARM64 image, modules and device tree were previously built with Clang 17 for the tested commit. This is a kernel source tree, not a complete bootable image.

Example cross-build with Clang/LLVM 17 and the usual kernel build dependencies:

```sh
git clone https://github.com/omnipha/linux-samsung-s10-exynos9820.git
cd linux-samsung-s10-exynos9820
make O=out ARCH=arm64 LLVM=-17 defconfig
make -j"$(nproc)" O=out ARCH=arm64 LLVM=-17 Image modules dtbs
```

The S10+ device tree is `out/arch/arm64/boot/dts/exynos/exynos9820-beyond2lte.dtb`. On the merged tree we rechecked `defconfig`, that device-tree target, and compilation of the Exynos PCIe and brcmfmac PCIe objects. This example has not been validated as a complete, reproducible phone installation; it does not by itself reproduce the custom boot environment used for the hardware test.

## Getting involved

- Browse the [open issues](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues) for known problems and ongoing work.
- Report hardware results with the exact model and variant, kernel commit, configuration, logs and what you tested. Please remove serial numbers, credentials, MAC/IP addresses and other personal data from public logs.
- Contributions and corrections are welcome through GitHub issues and pull requests. For eventual Linux mainline submission, follow the [kernel contribution process](Documentation/process/submitting-patches.rst); merging here does not merge anything upstream.

There is no tested general-purpose installer or recovery guide yet. Bootloader and storage changes can make a device unbootable or erase data; do not flash images based only on this README. Confirm your exact device, backups and recovery path first.

## Credits and license

This tree builds on [chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux). The PCIe/Wi-Fi work used Samsung vendor sources from [LineageOS/android_kernel_samsung_exynos9820](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/a412cfaf86b980ce35cae96a35f678c8ea551575) and [pulpoff/linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte/tree/3c142b4624f3caf9cb0ee90324ef7d62616d2a64) as references; see [PR #2](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/2) for details.

Kernel licensing information is in [COPYING](COPYING) and [license-rules.rst](Documentation/process/license-rules.rst). The original general kernel introduction is preserved in [README](README).
