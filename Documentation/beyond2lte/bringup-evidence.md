# Galaxy S10+ (`beyond2lte`) bring-up evidence

Hardware evidence for the implementation issues. Measurements refer to a Galaxy S10+ SM-G975F, not to the ordinary S10 (`beyond1lte`). Public logs and notes must not contain serial numbers, MAC/IP addresses, credentials or firmware payloads. Reference results must be tested on this device before they are called working.

The original S10 port [butztill/s10-linux](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303) is credited in the root README; retain SPDX and attribution when adapting its GPL code.

## Status measured on this device (2026-11-13)

- Bluetooth: the controller came up and a scan found 40 devices after boot. PR [#23](https://github.com/omnipha/linux-samsung-s10-exynos9820/pull/23), issue [#15](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/15).
- Phosh/Phoc 0.58: with simple-framebuffer output `Unknown-1` at 1440×3040 and scale 3, `wlr-randr --output Unknown-1 --transform 90` rotated the UI. The owner confirmed touch still hit the intended targets; `--transform normal` restored the original orientation. Automatic rotation remains unavailable until an orientation provider exists. Reboot persistence was not tested.
- No sound card (`/proc/asound/cards`), `/dev/video*`, `/dev/media*`, backlight device, IIO sensor, GNSS device or NFC class device was exposed in that boot. `CONFIG_GNSS=y` and `CONFIG_SPI_SPIDEV=y`, but neither proves a device is bound.

## Pin registers read on the current boot (2026-11-13)

Read-only `debugfs` pinctrl dump on kernel `7.2.0-rc2-unified-mb007-bt-ga260b60c67d9-dirty`. No GPIO was exported or driven. `CON`/`DAT`/`PUD` are the raw Samsung pin-controller fields.

| Pin | Stock role | CON | DAT | PUD |
| --- | --- | ---: | ---: | ---: |
| `gpg2-6` | rear-camera reset | 0 | 0 | 1 |
| `gpc1-4` | rear-camera MCLK | 0 | 0 | 1 |
| `gpg1-0` | CS47L93 reset | 1 | 0 | 1 |
| `gph1-1` | speaker-amplifier reset | 0 | 0 | 1 |
| `gph1-2` | earpiece-amplifier reset | 0 | 0 | 1 |
| `gpp6-2` | NFC enable | 0 | 0 | 1 |
| `gpp6-3` | NFC firmware mode | 0 | 0 | 1 |
| `gpa2-6` | NFC clock request | 0 | 0 | 1 |
| `gpa1-2` | NFC IRQ pin | 0 | 0 | 1 |
| `gpg0-2` | GPS-power pinctrl group | 0 | 0 | 1 |
| `gpg0-6` | pin named by the GPS GPIO property | 0 | 0 | 1 |
| `gpa1-7` | hub host request | 0 | 0 | 1 |
| `gpg4-1` | hub MCU request | 0 | 0 | 1 |
| `gpg4-2` | hub MCU response | 0 | 0 | 1 |
| `gpa4-0` | hub interrupt | 0 | 0 | 1 |
| `gpa0-5` | hub batch wake | 0 | 0 | 1 |
| `gph0-3` | hub chip select | 0 | 0 | 1 |

Only `gpg1-0` is configured as an output, and its data bit is 0. Every other pin above is function 0 with data bit 0. The camera, amplifier, NFC and hub controllers are still absent, so these are boot-time pin states, not proof that a driver toggled them.

A full debugfs pin-file read hit an external abort in `samsung_pinconf_rw` twice. The targeted reads above completed. Display, touch and WLAN stayed up. Do not repeat an unbounded pinctrl dump on this kernel.

## Stock-kernel probe on this S10+ (2026-11-10 recovery logs)

Two independent `last_kmsg` captures from this SM-G975F, both identifying `BEYOND2LTE EUR OPEN 26` on Exynos 9820. SHA-256: `5498bedbfa62702216139615d0b413e1cdafceb79fe25238a0a940f86aa8b61e` and `09c8127e6d8d97daeb603690629b6163a98c40d537e6607fdf9b4b516ae9f8e1`. These are stock-driver probe results, not measurements under the current mainline kernel. The numeric GPIOs are the stock kernel's global numbers, not the mainline bases that start at 512.

- Audio: `madera spi2.0: CS47L93 silicon revision 1`, followed by `BOOT_DONE`, in both logs. This is a silicon-ID read: the codec is CS47L93 rev 1. Mainline uses the shared component name `cs47l92-codec` for CS47L92 and CS47L93. Both logs also read the amplifiers as `Cirrus Logic CS35L41 (35a40), Revision: A0` at stock I2C `11-0040` and `11-0041`. The DT compatible `cirrus,cs35l40` does not match that ID.
- NFC: `sec_nfc` probe succeeded. The driver resolved IRQ 10, enable 188, firmware 189 and clock-request 22.
- Hub: probe succeeded. It resolved host-request 15, MCU-request 167, MCU-response 168 and GPS-power 145. The separate power GPIO lookup returned `-2`. Batch-wake was 5 and hub-interrupt was 32. Accelerometer position was 4, magnetometer position 7, accelerometer type 0, magnetometer type 1, and `ap-rev` was 26. Recovery status reported `GPS: false`. Neither log names `LSM6DSO`, `AK09918C` or `LPS22H`.
- Panel: ID `0x00A11013` selected `s6e3ha9_beyond2`, 1440×3040. The intact log then selected `s6e3ha9_beyond2_a3_s0_default`, ID `0xA01010`, revision 2. The second log's corresponding line is byte-corrupted. Stock brightness notifications occurred, but no AOR curve was read from the panel.
- Camera: the `fimc-is_sensor_2l4@20` module probed. `power_seq_id` failed with `-22`. `cis_2l4_probe` reported sensor ID 0, f-number 150 and `setfile_A`, then completed. That is not a read of model ID `0x20c4`.

## Camera — [#16](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/16)

The stock S10+ DTBO describes `fimc-is-2l4@10` at `reg = <0x10>` on `hsi2c0` (`0x10860000`); reset is `gpg2-6` and MCLK is `gpc1-4`. The associated `fimc-is_sensor_2l4@20` node names sensor channel 0. No camera lane count was established from that stock sensor node. The four-lane camera graph in the reference DTS is for `beyond1lte` and must be verified here.

The S10 reference provides [s5k-cis.c](https://github.com/butztill/s10-linux/blob/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-camera-v4l2/s5k-cis.c) and [exynos9820-csis.c](https://github.com/butztill/s10-linux/blob/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-camera-v4l2/exynos9820-csis.c). Its external sensor tables and loader require separate validation on S10+.

## Audio — [#17](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/17)

Stock DT describes a CS47L93 at SPI2 (`0x10470000`, reset `gpg1-0`) and two CS35L40 amplifiers at I2C38 (`0x109a0000`) addresses `0x40` and `0x41`, resets `gph1-1` and `gph1-2`. The stock kernel on this unit read CS47L93 silicon revision 1 at `spi2.0` and then logged `BOOT_DONE`. Mainline registers both CS47L92 and CS47L93 under the component name `cs47l92-codec`, so that name is not evidence of a different part. The same logs identify both amplifiers as `Cirrus Logic CS35L41 (35a40), Revision: A0` at stock I2C addresses `11-0040` and `11-0041`. Their DT compatible string remains `cirrus,cs35l40`, which does not match the silicon ID. The reference [audio implementation](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-abox-asoc) reports playback on `beyond1lte`; its `s10-abox-session` source warns that switching the AUD domain off after UAIF use can hang the system. The reference microphone card hard-codes codec name `cs47l92-codec`, while the stock node is `cs47l93`. Read the codec ID before choosing the driver.

## Sensor hub and GNSS — [#18](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/18), [#22](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/22)

Stock DT names `ssp,BCM4773` on SPI17 (`0x10970000`), mode 3, max 25 MHz. Its pinctrl group for GPS power selects `gpg0-2`, but the separate GPIO property refers to a local pinctrl node instead of a GPIO controller: the actual power GPIO remains to be verified. Stock vendor-image strings name `LSM6DSO`, `AK09918C` and `LPS22H`; those strings are not in this unit's two recovery logs, so they remain unproven here. The same logs do record accelerometer position 4 and magnetometer position 7.

The reference [sensor service](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/sensors) publishes SensorProxy orientation after hub initialization. Its [GNSS stack](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/gnss) reports fixes on `beyond1lte` with external proprietary Android daemons; that is not an open standalone GNSS implementation.

## NFC — [#19](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/19)

Stock DT calls the device `sec-nfc@27` on I2C20 (`0x108e0000`). Stock Android reports board revision 26 for this unit (read from a read-only ADB backup); stock overlay 09 covers revisions 26-255, so it is the matching overlay. There, enable is `gpp6-2` and firmware-mode is `gpp6-3`. Overlays 00-08 use other pins (PCAL6524 expander indices `0x0b`/`0x09` in 00-03). Overlay selection by the bootloader is inferred from the revision range, not observed. The S10 reference instead uses `samsung,s3fwrn5-i2c`; compatibility must be validated.

## Brightness — [#21](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/21)

The stock display description names panel `s6e3ha9_beyond2_a3_s0_default`. The S10 reference DECON driver uses `Beyond1 A3 S0` brightness/AOR points, not a validated S10+ curve. S10+ panel and dimming sources are available in the [pinned LineageOS Exynos 9820 kernel](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/4bcfb0263b21b553760a3bdce542452db3184820/drivers/video/fbdev/exynos/panel/s6e3ha9). The applicable panel table and visible brightness changes remain to be verified on this device.

## Licensing and private data

Do not commit device-specific or proprietary firmware, extracted stock images, generated camera payloads, personal identifiers, network addresses or credentials. Public issue evidence must distinguish live measurements, stock descriptions and results from other devices. Individual source files retain their original license and authorship.
