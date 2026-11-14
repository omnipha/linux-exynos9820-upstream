# Linux on the Samsung Exynos 9820

[![Kernel](https://img.shields.io/badge/Kernel-Linux%207.2--rc2-green.svg)](https://github.com/omnipha/linux-samsung-s10-exynos9820)
[![SoC](https://img.shields.io/badge/SoC-Exynos%209820-blue.svg)](https://github.com/omnipha/linux-samsung-s10-exynos9820)
[![Status](https://img.shields.io/badge/Status-active%20bring--up-orange.svg)](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues)
[![License](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](https://github.com/omnipha/linux-samsung-s10-exynos9820/blob/main/COPYING)
[![Help wanted](https://img.shields.io/badge/Help%20wanted-open%20issues-brightgreen.svg)](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues)

Mainline Linux for the 2019 Samsung phones built on the **Exynos 9820**: the Galaxy S10 family (Exynos models). One tree, tested in the open, aimed at upstream.

> **Tested so far: one Galaxy S10+ (SM-G975F).** The other models share the SoC and are welcome here, but nothing below has been measured on them.

## Devices

| Phone | Model | Codename | In this tree |
|---|---|---|---|
| Galaxy S10e | SM-G970F | `beyond0lte` | not yet |
| Galaxy S10 | SM-G973F | `beyond1lte` | device tree, untested |
| Galaxy S10+ | SM-G975F | `beyond2lte` | booting, see below |
| Galaxy S10 5G | SM-G977B | `beyondx` | not yet |

Exynos models only. The Snapdragon variants (SM-G970U, SM-G973U, SM-G975U) and the Galaxy S10 Lite (Snapdragon 855) are a different SoC and will not boot this kernel. The Note10 family uses the Exynos 9825, a refresh of the 9820; Samsung's vendor tree builds it from the same platform code. We have not checked it.

## What already runs on the S10+

| | | |
|---|---|---|
| 🖥️ Display and touch | framebuffer console, Phosh on the panel, touch hits | [status](https://github.com/omnipha/linux-samsung-s10-exynos9820/blob/main/Documentation/beyond2lte/STATUS.md) |
| 📡 Wi-Fi | BCM4375, scan, WPA2, SSH over Wi-Fi | [#1](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/1) |
| 🔵 Bluetooth | controller up, 40 devices discovered | [#15](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/15) |
| 🎮 GPU | Mali-G76, panfrost, kmscube | [#8](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/8) |
| 🔌 USB | gadget networking and SSH, no cable replug | [recovery](https://github.com/omnipha/linux-samsung-s10-exynos9820/tree/main/Documentation/beyond2lte/recovery) |

## Help wanted

These are open and measured against the stock hardware. Pick one.

| | |
|---|---|
| 📷 Camera | [#16](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/16) rear main sensor, raw capture |
| 🔊 Audio | [#17](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/17) speaker and microphone |
| ☀️ Brightness | [#21](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/21) backlight control |
| 📶 Mobile data | [#25](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/25) Shannon modem, LTE |
| 🧭 NFC | [#19](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/19) tag detection |
| 📐 Sensors | [#18](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/18) orientation |
| 📍 GPS | [#22](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/22) fixes |

Own an S10e or S10 5G? A boot log from your exact model is useful on its own. Open an issue with the model, the kernel commit and the log. Strip serial numbers, credentials and addresses first.

## Build

```sh
git clone https://github.com/omnipha/linux-samsung-s10-exynos9820.git
cd linux-samsung-s10-exynos9820
make O=out ARCH=arm64 LLVM=-17 beyond2lte_defconfig
make -j"$(nproc)" O=out ARCH=arm64 LLVM=-17 Image modules dtbs
```

Needs Clang/LLVM 17. Output: `out/arch/arm64/boot/Image` and `out/arch/arm64/boot/dts/exynos/exynos9820-beyond2lte.dtb`. The S10 device tree is built alongside it. The S5K2L4 camera register tables are in `firmware/samsung/s10/`. Details, firmware and the measured results: [Documentation/beyond2lte/STATUS.md](https://github.com/omnipha/linux-samsung-s10-exynos9820/blob/main/Documentation/beyond2lte/STATUS.md).

There is no installer. Flashing without your own backup and recovery path can wipe the phone.

## Credits

Built on [chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux). Wi-Fi references: [LineageOS/android_kernel_samsung_exynos9820](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/a412cfaf86b980ce35cae96a35f678c8ea551575), [pulpoff/linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte/tree/3c142b4624f3caf9cb0ee90324ef7d62616d2a64). Thanks to [butztill/s10-linux](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303) for the S10 bring-up notes. Results on another model are not results on this one. Keep SPDX and authorship when adapting code.

Kernel license: [COPYING](https://github.com/omnipha/linux-samsung-s10-exynos9820/blob/main/COPYING).
