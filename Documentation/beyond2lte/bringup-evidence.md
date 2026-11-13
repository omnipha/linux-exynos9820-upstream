# Galaxy S10+ (`beyond2lte`) bring-up evidence

Hardware evidence for the implementation issues. Measurements refer to a Galaxy S10+ SM-G975F, not to the ordinary S10 (`beyond1lte`). Public logs and notes must not contain serial numbers, MAC/IP addresses, credentials or firmware payloads. Reference results must be tested on this device before they are called working.

The original S10 port [butztill/s10-linux](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303) is credited in the root README; retain SPDX and attribution when adapting its GPL code.

## Status measured on this device (2026-11-13)

- Bluetooth: the controller came up and a scan found 40 devices after boot. PR [#23](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/23), issue [#15](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/15).
- Camera: one 2016×1512 BA10 frame and an exposure/gain sweep were captured on a running system. A covered comparison and persistence through reboot are not yet established. Issue [#16](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/16).
- Phosh/Phoc 0.58: with simple-framebuffer output `Unknown-1` at 1440×3040 and scale 3, `wlr-randr --output Unknown-1 --transform 90` rotated the UI. The owner confirmed touch still hit the intended targets; `--transform normal` restored the original orientation. Automatic rotation remains unavailable until an orientation provider exists. Reboot persistence was not tested.
- No sound card (`/proc/asound/cards`), `/dev/video*`, `/dev/media*`, backlight device, IIO sensor, GNSS device or NFC class device was exposed in that boot. `CONFIG_GNSS=y` and `CONFIG_SPI_SPIDEV=y`, but neither proves a device is bound.

## Camera — [#16](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/16)

The stock S10+ DTBO describes `fimc-is-2l4@10` at `reg = <0x10>` on `hsi2c0` (`0x10860000`); reset is `gpg2-6` and MCLK is `gpc1-4`. The associated `fimc-is_sensor_2l4@20` node names sensor channel 0. No camera lane count was established from that stock sensor node. The four-lane camera graph in the reference DTS is for `beyond1lte` and must be verified here.

The S10 reference provides [s5k-cis.c](https://github.com/butztill/s10-linux/blob/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-camera-v4l2/s5k-cis.c) and [exynos9820-csis.c](https://github.com/butztill/s10-linux/blob/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-camera-v4l2/exynos9820-csis.c). Its external sensor tables and loader require separate validation on S10+.

An experimental, uncommitted port based on `b74704a41343` booted on this S10+ on 2026-11-13. CSIS registered `/dev/video0` but reported that it was waiting for the sensor; the S2MPB03 at `0x56` responded with ID `0x02` and was left in read-only identification mode. The sensor driver failed with `-ENOENT` because its runtime table was unavailable during the early initramfs probe; this was not a sensor chip-ID read. No raw frame was captured, so lane count, sensor supply wiring and streaming remain unverified.

A subsequent reboot after changing the initramfs did not regain USB or WLAN access; no kernel journal from that attempt was available. The owner reported a visible desktop, then recovered via Safe Mode. The previously verified Bluetooth boot entry was restored; the current default has USB SSH, WLAN, Phosh, framebuffer and the touch driver present, three boot entries, and no observed EXT4 errors. The cause of the failed reboot attempt remains unknown.

## Audio — [#17](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/17)

Stock DT describes a CS47L93 at SPI2 (`0x10470000`, reset `gpg1-0`) and two CS35L40 amplifiers at I2C38 (`0x109a0000`) addresses `0x40` and `0x41`, resets `gph1-1` and `gph1-2`. These are DT descriptions, not live chip-ID reads. The reference [audio implementation](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-abox-asoc) reports playback on `beyond1lte`; its `s10-abox-session` source warns that switching the AUD domain off after UAIF use can hang the system.

## Sensor hub and GNSS — [#18](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/18), [#22](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/22)

Stock DT names `ssp,BCM4773` on SPI17 (`0x10970000`), mode 3, max 25 MHz. Its pinctrl group for GPS power selects `gpg0-2`, but the separate GPIO property refers to a local pinctrl node instead of a GPIO controller: the actual power GPIO remains to be verified. Stock vendor-image strings name `LSM6DSO`, `AK09918C` and `LPS22H`; those strings alone do not prove which sensors are fitted to this unit.

The reference [sensor service](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/sensors) publishes SensorProxy orientation after hub initialization. Its [GNSS stack](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/gnss) reports fixes on `beyond1lte` with external proprietary Android daemons; that is not an open standalone GNSS implementation.

## NFC — [#19](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/19)

Stock DT calls the device `sec-nfc@27` on I2C20 (`0x108e0000`). Enable/firmware GPIOs differ across stock overlays: indices `0x0b`/`0x09` on the PCAL6524 expander in overlays 00–03; `gpp6-2`/`gpp6-3` in overlays 04–09. The matching active stock overlay must be established before selecting pins. The S10 reference instead uses `samsung,s3fwrn5-i2c`; compatibility must be validated.

## Brightness — [#21](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/21)

The stock display description names panel `s6e3ha9_beyond2_a3_s0_default`. The S10 reference DECON driver uses `Beyond1 A3 S0` brightness/AOR points, not a validated S10+ curve. S10+ panel and dimming sources are available in the [pinned LineageOS Exynos 9820 kernel](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/4bcfb0263b21b553760a3bdce542452db3184820/drivers/video/fbdev/exynos/panel/s6e3ha9). The applicable panel table and visible brightness changes remain to be verified on this device.

## Licensing and private data

Do not commit device-specific or proprietary firmware, extracted stock images, generated camera payloads, personal identifiers, network addresses or credentials. Public issue evidence must distinguish live measurements, stock descriptions and results from other devices. Individual source files retain their original license and authorship.
