
## Audio volume (Phosh slider and volume keys)

The S10 speaker card has no master control: the volume lives in the two CS35L41
`Bottom Digital PCM` / `Receiver Digital PCM` elements. Without a PulseAudio mixer
path, Phosh only scales in software and the amplifiers stay at their boot gain.

Install on the phone:

- `mb007-s10-speaker.conf` -> `/usr/share/pulseaudio/alsa-mixer/paths/`
- `mb007-s10.profile-set` -> `/usr/share/pulseaudio/alsa-mixer/profile-sets/mb007-s10.conf`
- `91-mb007-s10-pulse.rules` -> `/etc/udev/rules.d/`
- `mb007-audio-route-volume.conf` -> `/etc/systemd/system/mb007-audio-route.service.d/volume.conf`
- `mb007-audio.default` -> `/etc/default/mb007-audio`

Then `udevadm control --reload; udevadm trigger --subsystem-match=sound --action=change`
and restart PulseAudio. Measured: sink `analog-stereo` with HW_VOLUME_CTRL; 30 % ->
Bottom -7.4 dB, 10 % -> -36 dB, 0 % -> both amplifiers 0; mute not mapped to hardware.
Analog gain stays at 20 and `vol_factor` at 0 dB (drop-in); the slider scales the digital gain.
Not yet verified: Receiver follows only at higher levels (merge order); `mb007-audio-route --check`
reports digital gain above its -12 dB ceiling once the slider is raised.
