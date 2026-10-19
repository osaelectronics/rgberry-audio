#!/bin/bash
# RGBerry SCART - one-line installer for Raspberry Pi 5
#
#   curl -fsSL https://raw.githubusercontent.com/osaelectronics/rgberry-audio/main/get.sh | sudo bash
#
# Optional:  ... | sudo RGBERRY_REF=v2.4.0 bash     install a specific tag/branch
#            ... | sudo bash -s -- --test          also play a test tone
set -e

REPO="${RGBERRY_REPO:-osaelectronics/rgberry-audio}"
REF="${RGBERRY_REF:-main}"

RED='\033[1;31m'; GRN='\033[1;32m'; NC='\033[0m'
die() { echo -e "${RED}ERROR:${NC} $*"; exit 1; }

[ "$(id -u)" = 0 ] || die "ejecuta con sudo:  curl -fsSL ... | sudo bash"
command -v curl >/dev/null || { apt-get update -qq && apt-get install -y -qq curl; }
command -v tar  >/dev/null || die "falta 'tar'"

echo -e "${GRN}==>${NC} Descargando RGBerry audio ($REPO @ $REF)"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
curl -fsSL "https://github.com/$REPO/archive/$REF.tar.gz" | tar xz -C "$TMP" --strip-components=1 \
    || die "no se pudo descargar https://github.com/$REPO (¿hay conexión a Internet?)"

cd "$TMP"
bash ./install.sh "$@"
