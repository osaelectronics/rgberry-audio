#!/bin/bash
# RGBerry SCART - remove the Pi 5 audio daemon
set -e
[ "$(id -u)" = 0 ] || { echo "ejecuta con sudo"; exit 1; }
systemctl disable --now rgberry-audio.service rgberry-audio-select.service 2>/dev/null || true
# restore the original ALSA config if ours is active
if grep -qE "RGBERRY-AUDIO-PI[45]" /etc/asound.conf 2>/dev/null; then
    if [ -e /etc/asound.conf.rgberry-orig ]; then
        mv -f /etc/asound.conf.rgberry-orig /etc/asound.conf
    else
        rm -f /etc/asound.conf
    fi
fi
rm -f /etc/systemd/system/rgberry-audio.service /etc/systemd/system/rgberry-audio-select.service \
      /etc/systemd/system/replay.service.d/10-rgberry-audio.conf /etc/asound.conf.rgberry-none \
      /usr/local/bin/rgberry-audio /usr/local/sbin/rgberry-audio-select \
      /etc/modprobe.d/rgberry-audio.conf /usr/lib/*/alsa-lib/libasound_module_pcm_rgberry.so /usr/lib/*/alsa-lib/libasound_module_ctl_rgberry.so /dev/shm/rgberry-audio
rm -rf /etc/rgberry-audio /var/lib/rgberry-audio
systemctl daemon-reload
echo "rgberry-audio desinstalado (se conserva /etc/default/rgberry-audio)."
