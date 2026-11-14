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

## Camera power (rear main, S5K2L4)

Source: stock overlay 09 (`ro.boot.dtbo_idx` = 9) for the regulators, and the Samsung 4.14 driver `fimc-is-device-module-2l4.c` (LineageOS commit `4bcfb0263b21`) for the order. This is stock data, not a voltage measurement on this unit.

Camera PMIC `s2mpb03_pmic@56` (`samsung,s2mpb03pmic`, I2C `0x56`); its parent bus is not resolved in the overlay:

| LDO | Name | Voltage |
| --- | --- | --- |
| ldo1 | `VDDD_1.0V_CAM` | 1.05 V |
| ldo2 | `VDDQ_1.1V_CAM` | 1.1 V |
| ldo3 | `VDDIO_1.8V_CAM` | 1.8 V |
| ldo4 | `VDDD_RET_1.0V_CAM` | 0.7-1.025 V range |
| ldo5 | `VDDA_2.4V_CAM` | 2.4 V |
| ldo6 | `VDDA_2.8V_CAM` | 2.8 V |
| ldo7 | `VDD_VM_2.8V_OIS` | 2.8 V (OIS only) |

Autofocus supply `VDDAF_COMMON_CAM` is `s2mpb02-ldo14` at 3.3 V, on the other PMIC.

Stock power-on order for the 2L4: reset low; `VDDD_RET` set to 1.025 V; `VDDA_2.8V`; `VDDA_2.4V`; `VDDD_1.0V`; `VDDAF_COMMON`; `VDDIO_1.8V`; `VDDQ_1.1V` (1 ms); OIS supply; I2C pins; MCLK (1.5 ms); reset high (3 ms). Stock power-off runs the reverse. Retention mode drops `VDDD_RET` to 0.7 V.

Caveats: the stock logs from this unit show `power_seq_id read is fail(-22)`, so the driver used its default sequence. The reference DTS for `beyond1lte` wires the same S2MPB03 names to the sensor (`retention-supply`, `avdd28-supply`, `avdd24-supply`, `core-supply`, `iovdd-supply`, `vddq-supply`). The S10+ PMIC parent bus and the real voltages at the sensor are still to be measured once the PMIC probes.

## Audio — [#17](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/17)

Stock DT describes a CS47L93 at SPI2 (`0x10470000`, reset `gpg1-0`) and two CS35L40 amplifiers at I2C38 (`0x109a0000`) addresses `0x40` and `0x41`, resets `gph1-1` and `gph1-2`. The stock kernel on this unit read CS47L93 silicon revision 1 at `spi2.0` and then logged `BOOT_DONE`. Mainline registers both CS47L92 and CS47L93 under the component name `cs47l92-codec`, so that name is not evidence of a different part. The same logs identify both amplifiers as `Cirrus Logic CS35L41 (35a40), Revision: A0` at stock I2C addresses `11-0040` and `11-0041`. Their DT compatible string remains `cirrus,cs35l40`, which does not match the silicon ID. The reference [audio implementation](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/drivers/s10-abox-asoc) reports playback on `beyond1lte`; its `s10-abox-session` source warns that switching the AUD domain off after UAIF use can hang the system. The reference microphone card hard-codes the shared mainline component name `cs47l92-codec`. On this unit the compatible is `cirrus,cs47l93` and the stock probe reports CS47L93 rev 1, so the device type stays CS47L93.

Measured on one SM-G975F on 2026-11-14, kernel `7.2.0-rc2-unified-mb007-audio-g6882ccf13539`, branch `issue-17-audio-cs47l93`. Raw captures stay private.

- Cards `S10` (speaker, playback) and `S10Mic` (CS47L93 to UAIF0 to WDMA1, capture) registered after a reboot. Both amplifiers (`cs35l41`, A0) and the codec (`CS47L93` rev 1) bound. The AUD PMU status (`0x1904`) read `1` on the earlier boot, and on both boots the kernel log reports the domain held until reboot.
- Test: `speaker-test -t sine -f 440` on `hw:S10,0` (48 kHz S16_LE stereo, amplifier digital gain 721 = -12 dB, `vol_factor` -18 dB) with a 6 s capture on `hw:S10Mic,0`, five segments: baseline, unmuted, muted (`Bottom Digital PCM` and `Receiver Digital PCM`, numids 1 and 19, set to 0; the analog volumes were not touched), unmuted, baseline. The phone microphone's 440 Hz amplitude in LSB, left / right: baseline 0.0057 / 0.0015, unmuted 4.97 / 0.86, muted 0.0039 / 0.0045, unmuted 4.97 / 0.86, baseline 0.0052 / 0.0017. Broadband capture noise was 31-38 LSB RMS in every segment (about -59 dBFS); the tone is far below it and was found by narrowband analysis. Per second, the left channel carried about 10 LSB in seconds 3 to 5 of the window and about 1 LSB otherwise, while the right channel peaked at about 3 LSB in the other seconds. That fits `speaker-test` alternating channels; the mapping from channel to speaker was not verified, and the 6 s averages mix loud and quiet intervals.
- The host computer's built-in microphone, a separate device, showed 440 Hz amplitudes of 55.5 / 37.5 (left / right) over the interior 4 s of the first unmuted segment and 54.5 / 38.7 over the second, and at most 3.3 in the other segments. It is uncalibrated.
- An earlier run on the same kernel, before the I2C alias change and after the amplifier ASP TX sources were set to Zero, gave unmuted amplitudes of 5.05 / 0.877 against 4.97 / 0.861 (both within 2%). On that boot the Zero setting had been applied by hand earlier in the session, and after the reboot the same four controls still read Zero. The reboot also changed the DTB and the MUIC unit, so the comparison does not isolate the alias change. That earlier run's muted and baseline values are all below 0.01 LSB, at the analysis floor.
- The self-test passed repeatedly. It requires at least 90% of the requested frames, a span of at least 2 LSB on each channel, and at least 1% of frames that differ between channels, so it shows that capture delivers varying data, not that sound was heard. The first run after the reboot, at 12 s of uptime, failed at the microphone step. `arecord -q` still prints its errors on stderr, but the caller filtered the output with `grep -E "PASS|FAIL"`, and the script keeps its capture in a temporary directory that it deletes on exit, so neither the ALSA error nor the capture was kept and the cause is unknown. A run at 20 s and five more at 41 s passed.

Limits: playback audibility: owner check pending (joint test). Muting the amplifier also removes electrical coupling, so the phone-microphone data do not separate an acoustic path from coupling; the host microphone result is consistent with sound at that microphone but was not isolated. One unit, one level, 6 s windows averaging loud and quiet `speaker-test` channel intervals.

Source-review findings. Open: on a PLL or PMU initialisation failure, the session probe logs the error, keeps its runtime-PM reference and clock, and leaves the sound cards deferred. The `beyond2lte` provider separately marks the AUD domain always on. The session's manual test attributes (`uaif_test`, `rdma_test`, `reg`) and the speaker driver share `0x40`, `0x510`, `DIV_UAIF1` and the RDMA0 registers without an interlock, so those attributes must not be used while a PCM is open. The card remove paths also need a lifecycle review. The speaker, microphone and session paths that set bits in `ABOX_QCH_DISABLE` (`0x38`) now go through one spinlock-protected helper, which serializes those set-only updates. That kernel, `7.2.0-rc2-unified-mb007-audio-ga4a43b7073f4`, booted on this unit. No lost update was observed, and the concurrent playback/capture race was not stress-tested. The `reg` poke still replaces the whole word and can clear bits needed by an active stream.

A later boot of that kernel, with no playback, registered both cards, bound CS47L93 rev 1 and both CS35L41 A0 amplifiers, and kept the AUD domain on. Four capture-only self-tests, started at 10, 20, 30 and 41 s of uptime, all passed, as did a fifth run later in the same boot. A 10 s capture with no playback open had summary RMS values of 24.59 LSB left and 29.41 LSB right, with means of -18.77 and 4.85 LSB. The summary does not state whether the mean was removed, so these are not a dBFS-after-DC figure, and room silence was not verified. The self-test only checks that samples vary, so none of this is an acoustic measurement. USB SSH, WLAN association, one access-point ping, the touchscreen driver, Phosh and phoc, the three loader entries, the safe-mode and fsck payloads, and the absence of EXT4 errors were checked. Touchscreen hits, visible panel output and WLAN throughput were not. `nftables.service` and `systemd-vconsole-setup.service` failed as before. The self-test now skips its playback step unless `MB007_AUDIO_PLAYBACK_TEST=1`. The normal-entry rollback procedure is documented and has not been executed.

Three further reboots of the same QCH kernel ran six capture-only self-tests each, at 8, 12, 20, 30, 40 and 60 s. All 8 s and 12 s runs passed. The 20 s run failed on the first two boots with `arecord: audio open error: Resource busy`; its pre-capture snapshot showed PulseAudio owning the running capture PCM. The same run passed on the third boot, and every later run passed. No oops, panic, lockup, ASoC error or EXT4 error appeared. This explains those two failures as a userspace device conflict, but it does not identify the earlier discarded 12 s failure. Suspend and resume were not tried: the ABOX session refuses system suspend while bound, and powering the AUD domain off after UAIF use is documented to hang the phone.

The microphone card's world-readable `codec_regs` attribute was removed in `7f58fe95d838`. It read the codec component pointer without a lifetime check. The card can register before the CS47L93 binds, and unbinding the codec can leave the pointer stale. `regs` and `pads` remain. The pointer is still stored for GPIO restoration in card removal, so that separate lifetime hazard remains. Kernel `7.2.0-rc2-unified-mb007-audio-g7f58fe95d838` booted once: both cards registered, the AUD domain stayed on, `codec_regs` was absent, all 47 route tests passed, and three capture-only self-tests passed. The safe-mode and fsck payloads and the three loader entries were unchanged. Playback audibility remains an owner check pending (joint test).

Bus numbers: the amplifier bus renumbered the touch and MAX77705 adapters and sent `mb007-muic-route.service` into a restart loop on the first audio boot. `i2c0` to `i2c2` aliases in the board DTS pin touch as 0, MAX77705 as 1 and the amplifiers as 2, and the unit now skips when a kernel driver is bound to `1-0025`. After the next reboot the unit stayed inactive with 0 restarts and there were no EXT4 errors. The same three loader entries were present, and `nftables.service` and `systemd-vconsole-setup.service` failed as they had on earlier boots; the FAT dirty-volume message is a kernel warning printed on every checked boot. Phosh and phoc were seen running; touch input and Wi-Fi throughput were not re-tested.

## Sensor hub and GNSS — [#18](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/18), [#22](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/22)

Stock DT names `ssp,BCM4773` on SPI17 (`0x10970000`), mode 3, max 25 MHz. Its pinctrl group for GPS power selects `gpg0-2`, but the separate GPIO property refers to a local pinctrl node instead of a GPIO controller: the stock hub probe resolved `ssp-gps-pwr-en` to GPIO 145 and logged a separate `ssp-pwr-en=-2`. Those are stock GPIO numbers, not the mainline bases. Stock vendor-image strings name `LSM6DSO`, `AK09918C` and `LPS22H`; those strings are not in this unit's two recovery logs, so they remain unproven here. The same logs do record accelerometer position 4 and magnetometer position 7.

The reference [sensor service](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/sensors) publishes SensorProxy orientation after hub initialization. Its [GNSS stack](https://github.com/butztill/s10-linux/tree/d8135b2465b3a9f1c0e7f1f6be71451ad185b303/userspace/gnss) reports fixes on `beyond1lte` with external proprietary Android daemons; that is not an open standalone GNSS implementation.

## NFC — [#19](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/19)

Stock DT calls the device `sec-nfc@27` on I2C20 (`0x108e0000`). Stock Android reports board revision 26 for this unit (read from a read-only ADB backup); stock overlay 09 covers revisions 26-255, so it is the matching overlay. There, enable is `gpp6-2` and firmware-mode is `gpp6-3`. Overlays 00-08 use other pins (PCAL6524 expander indices `0x0b`/`0x09` in 00-03). Stock Android reported `ro.boot.dtbo_idx` = 9 (read-only ADB backup), so overlay 09 was the one selected on this unit. The S10 reference instead uses `samsung,s3fwrn5-i2c`; compatibility must be validated.

## Brightness — [#21](https://github.com/omnipha/linux-samsung-s10-exynos9820/issues/21)

The stock display description names panel `s6e3ha9_beyond2_a3_s0_default`. The S10 reference DECON driver uses `Beyond1 A3 S0` brightness/AOR points, not a validated S10+ curve. S10+ panel and dimming sources are available in the [pinned LineageOS Exynos 9820 kernel](https://github.com/LineageOS/android_kernel_samsung_exynos9820/tree/4bcfb0263b21b553760a3bdce542452db3184820/drivers/video/fbdev/exynos/panel/s6e3ha9).

On 2026-11-14, a one-shot read-only diagnostic on kernel `7.2.0-rc2-unified-mb007-audio-g7f58fe95d838` reported the display power domain on, DSIM version `0x02030000`, command-mode configuration, empty transmit FIFOs and the D-PHY PLL lock bit set. The separate read-only test left the framebuffer and USB SSH running and did not send panel commands. No brightness control, panel readback or visible change was measured in that test. The applicable panel table and visible brightness changes remain to be verified on this device.

## Licensing and private data

Do not commit device-specific or proprietary firmware, extracted stock images, generated camera payloads, personal identifiers, network addresses or credentials. Public issue evidence must distinguish live measurements, stock descriptions and results from other devices. Individual source files retain their original license and authorship.
