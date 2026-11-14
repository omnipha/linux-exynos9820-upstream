
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
and restart PulseAudio. Each amplifier gets its own channel mask (Bottom = front-right,
Receiver = front-left), so both follow the slider equally. Measured via `pactl`/`amixer`
(no sound played): 10 % -> -48 dB, 30 % -> -19.4 dB, 60 % -> -1.25 dB, 100 % -> +12 dB on
both amplifiers; 0 % -> both at minimum. Analog gain stays at 20 and `vol_factor` at 0 dB.
`mb007-audio-route` no longer resets the digital gain, so the slider value survives.

Mute: the CS35L41 driver exposes no mute switch, so Phosh mute is PulseAudio software
mute of the sink; it does not change the amplifier gain. Not verified acoustically:
slider, volume keys and mute by ear (joint listening test pending).
