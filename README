# Linux for Samsung Exynos 9820 (Galaxy S10 series)

Actively maintained kernel tree for Samsung **Exynos 9820** devices
(Galaxy S10 / S10+ / S10 5G / S10e: `beyond0lte`, `beyond1lte`, `beyond2lte`).

This is a fork of [chiffathefox/exynos-9820-mainline-linux](https://github.com/chiffathefox/exynos-9820-mainline-linux)
(branch `exynos9820-7.2-rc2`, Linux 7.2-rc2 base), extended with device trees,
PCIe host support and BCM4375 WLAN — all verified on real hardware.
The stock kernel `README` still applies for general kernel questions.

## Hardware status (measured, not claimed)

Verified on **SM-G975F (Galaxy S10+, `beyond2lte`)** — evidence in
[issue #1](https://github.com/omnipha/linux-exynos9820-upstream/issues/1) and
[PR #2](https://github.com/omnipha/linux-exynos9820-upstream/pull/2):

| Subsystem | Status |
|---|---|
| Boot (U-Boot → systemd-boot → kernel) | working, incl. display console |
| UFS storage (`sda1`–`sda31`) | working |
| USB (gadget/NCM network, high-speed peripheral) | working |
| Display (SimpleFB console) + touchscreen | working |
| **WLAN BCM4375 (PCIe, brcmfmac)** | **working**: Gen1 x1 link up, firmware 18.41.117, scan, WPA2 association, DHCP, traffic |
| GPU (Mali G76), modem, audio, Bluetooth, sensors, camera | untested / not enabled |

Known WLAN gaps: first firmware load requires the correct board NVRAM as
`brcmfmac4375b1-pcie.txt` (Semco CS01 verified on S10+; other modules may need
their own NVRAM), P2P device creation fails (`err=-5`), suspend/resume not
implemented. The PCIe binding lists `reset-gpios` as optional while the driver
requires it — under review.

## What is in this tree (on top of the 7.2-rc2 base)

- `exynos9820-beyond1lte`/`beyond2lte` device trees (from U-Boot `dec4927b`)
- PCIe Gen2 root-complex driver for the WLAN PHY (`drivers/pci/controller/dwc/pcie-exynos9820.c`)
  + DT binding (`Documentation/devicetree/bindings/pci/samsung,exynos9820-pcie.yaml`)
- BCM4375 PCIe support in brcmfmac (IDs, firmware map, TCM base, protocol v6 handshake)
- USB2 peripheral mode for `beyond2lte`, defconfig enabling all of the above

## Build

Cross-build with Clang/LLVM (verified with clang 17):

```sh
make O=out ARCH=arm64 LLVM=-17 defconfig
make -j$(nproc) O=out ARCH=arm64 LLVM=-17 Image dtbs modules
```

Device tree: `out/arch/arm64/boot/dts/exynos/exynos9820-beyond2lte.dtb`.

WLAN additionally needs Broadcom firmware files in `/usr/lib/firmware/brcm/`
(`brcmfmac4375b1-pcie.bin`, `.clm_blob` and board NVRAM as `.txt`). These are
not part of this repository.

## Boot / install

These devices boot via a custom U-Boot on the `boot` partition loading
systemd-boot from a partition image on `userdata` — Samsung Exynos 9820 has no
fastboot; flashing goes through Download mode (Heimdall) or recovery.
There is no one-line installer yet. If you bring up one of these devices,
open an issue — tested reports are welcome.

## Contributing

PRs and issues here on GitHub. For anything intended for mainline, follow the
normal kernel process (`Documentation/process/submitting-patches.rst`); series
should go to the PCI / brcm80211 / Samsung-SoC / devicetree lists.

## License

GPL-2.0 (see `COPYING`), like the Linux kernel itself. The PCIe driver derives
from Samsung GPL vendor sources (LineageOS `android_kernel_samsung_exynos9820`)
and [pulpoff/linux-beyond0lte](https://github.com/pulpoff/linux-beyond0lte); see
the commit messages of PR #2 for exact provenance.
