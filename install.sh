#!/bin/bash
# RGBerry SCART - audio installer for Raspberry Pi 5
# Usage:  sudo ./install.sh            install and start
#         sudo ./install.sh --test     install, then play a 1 kHz test tone (L, then R)
set -e

RED='\033[1;31m'; GRN='\033[1;32m'; YEL='\033[1;33m'; NC='\033[0m'
say()  { echo -e "${GRN}==>${NC} $*"; }
warn() { echo -e "${YEL}AVISO:${NC} $*"; }
die()  { echo -e "${RED}ERROR:${NC} $*"; exit 1; }

[ "$(id -u)" = 0 ] || die "ejecuta con sudo:  sudo ./install.sh"
cd "$(dirname "$0")"

IS_PI5=0
grep -q bcm2712 /proc/device-tree/compatible 2>/dev/null && IS_PI5=1

if [ $IS_PI5 = 0 ]; then
    warn "Esto no es una Raspberry Pi 5. Se instala igualmente para que la"
    warn "tarjeta SD funcione si la mueves a una Pi 5; en Pi 2/3/4 no hace nada."
fi

if [ $IS_PI5 = 1 ] && [ ! -e /dev/pio0 ]; then
    die "/dev/pio0 no existe. Actualiza el sistema y reinicia:
       sudo apt update && sudo apt full-upgrade && sudo reboot"
fi

# stop any previous instance (a stuck one keeps the PIO state machines)
systemctl stop rgberry-audio.service 2>/dev/null || true
pkill -9 -x rgberry-audio 2>/dev/null || true

say "Instalando dependencias de compilacion"
PKGS="build-essential libasound2-dev"
apt-get install -y -qq --no-install-recommends $PKGS >/dev/null 2>&1 || {
    apt-get update -qq && apt-get install -y -qq --no-install-recommends $PKGS >/dev/null
} || die "no se pudieron instalar $PKGS (¿hay conexión a Internet?)"

say "Compilando rgberry-audio"
make clean >/dev/null
make >/dev/null
make install >/dev/null

say "Instalando configuracion"
# 1.x/2.0 replaced /etc/asound.conf entirely: put the distro original back
# first, so the selector appends to it instead of backing up our own file.
if grep -q "RGBERRY-AUDIO-PI5" /etc/asound.conf 2>/dev/null && [ -e /etc/asound.conf.rgberry-orig ] \
   && ! grep -q "RGBERRY-AUDIO-PI5" /etc/asound.conf.rgberry-orig; then
    cp -a /etc/asound.conf.rgberry-orig /etc/asound.conf
fi
install -D -m 644 config/asound-pi5.conf      /etc/rgberry-audio/asound-pi5.conf
install -D -m 644 config/modprobe-rgberry.conf /etc/modprobe.d/rgberry-audio.conf
[ -e /etc/default/rgberry-audio ] || install -D -m 644 config/rgberry-audio.default /etc/default/rgberry-audio
install -D -m 755 scripts/rgberry-audio-select /usr/local/sbin/rgberry-audio-select
install -D -m 644 systemd/rgberry-audio.service        /etc/systemd/system/rgberry-audio.service
install -D -m 644 systemd/rgberry-audio-select.service /etc/systemd/system/rgberry-audio-select.service
# RePlay OS starts its frontend very early: create the card before it
if systemctl cat replay.service >/dev/null 2>&1; then
    install -D -m 644 systemd/replay.service.d/10-rgberry-audio.conf \
        /etc/systemd/system/replay.service.d/10-rgberry-audio.conf
    REPLAY=1
fi

# Pi 4-era overlays that would fight over GPIO 12/13/18/19 on a Pi 5
CFG=/boot/firmware/config.txt; [ -e $CFG ] || CFG=/boot/config.txt
if [ -e $CFG ]; then
    awk '
        /^\[/ { sect=$0 }
        /^[[:space:]]*dtoverlay=(audremap|pwm|pwm-2chan)(,|[[:space:]]|$)/ &&
            (sect=="" || sect=="[all]" || sect=="[pi5]") { print FILENAME ": " $0; found=1 }
        END { exit !found }' $CFG && {
        warn "config.txt tiene overlays de audio PWM que se aplicarian en la Pi 5."
        warn "Ponlos bajo [pi4] (o [pi3]/[pi2]) para que no se carguen en la Pi 5:"
        warn "   [pi4]"
        warn "   dtoverlay=audremap,pins_18_19"
        warn "   [all]"
    } || true
fi

systemctl daemon-reload
systemctl disable rgberry-audio-select.service >/dev/null 2>&1 || true   # old WantedBy
systemctl enable rgberry-audio-select.service rgberry-audio.service >/dev/null 2>&1

if [ $IS_PI5 = 1 ]; then
    systemctl stop rgberry-audio.service 2>/dev/null || true
    # reload the loopback card with the new id if an older install loaded it
    if [ -e /proc/asound/RGBerry ]; then
        modprobe -r snd-aloop 2>/dev/null || NEED_REBOOT=1
    fi
    systemctl restart rgberry-audio-select.service
    [ -e /proc/asound/sndrpihifiberry ] || NEED_REBOOT=1

    if [ "$1" = "--test" ]; then
        systemctl stop rgberry-audio.service 2>/dev/null || true
        say "Tono de prueba 1 kHz: 3 s por el izquierdo, luego 3 s por el derecho"
        timeout -s INT 3 /usr/local/bin/rgberry-audio -t 1000 --tone-ch L || true
        timeout -s INT 3 /usr/local/bin/rgberry-audio -t 1000 --tone-ch R || true
    fi

    if [ "$NEED_REBOOT" = 1 ]; then
        say "Instalado. REINICIA la Pi (reboot) para activar la tarjeta de audio."
    else
        systemctl restart rgberry-audio.service
        sleep 1
        if systemctl is-active --quiet rgberry-audio.service; then
            say "Listo. El audio sale por GPIO18 (L) y GPIO19 (R)."
        else
            die "el servicio no arranco: prueba  rgberry-audio -T"
        fi
    fi
    if [ "$REPLAY" = 1 ]; then
        if [ "$NEED_REBOOT" != 1 ] && systemctl is-active --quiet replay.service; then
            systemctl restart replay.service
        fi
        say "RePlay OS: el audio sale por el SCART con 'Primary HDMI' (por defecto)"
    fi

    if command -v pipewire >/dev/null 2>&1 && pgrep -x pipewire >/dev/null; then
        warn "PipeWire esta activo (escritorio). Elige la salida 'Loopback'"
        warn "en el control de volumen del escritorio."
    fi
else
    say "Instalado. Se activara automaticamente al arrancar en una Pi 5."
fi
