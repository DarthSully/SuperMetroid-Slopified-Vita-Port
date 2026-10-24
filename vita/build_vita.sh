#!/usr/bin/env bash
# Build SuperMetroidRecomp.vpk for the PS Vita from YOUR Super Metroid ROM.
#
#   bash vita/build_vita.sh "/path/to/Super Metroid (JU) [!].smc"
#
# Runs on Linux or WSL. Needs: git, cmake, ninja (or make), python3, rustup,
# and VitaSDK (the script tells you how to install whatever is missing).
#
# Steps: verify ROM -> regenerate src/gen from it -> configure with VitaSDK
# -> build eboot.bin -> pack build-vita/SuperMetroidRecomp.vpk.
# The VPK contains code generated from your ROM. Do not share it.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build-vita}"
EXPECTED_SHA256="12b77c4bc9c1832cee8881244659065ee1d84c70c3d29e6eaf92e6798cc2ca72"
CANONICAL="$ROOT/Super Metroid (Japan, USA) (En,Ja).sfc"

die() { printf '\n[vita] ERROR: %s\n' "$*" >&2; exit 1; }
say() { printf '[vita] %s\n' "$*"; }

# ── 1. Tools ────────────────────────────────────────────────────────────────
for tool in git cmake python3; do
  command -v "$tool" >/dev/null || die "'$tool' is not installed."
done
if command -v ninja >/dev/null; then GEN=(-G Ninja); else GEN=(); fi

if [ -z "${VITASDK:-}" ] || [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ]; then
  cat >&2 <<'EOF'

[vita] VitaSDK not found. Install it once (takes a few minutes):

    export VITASDK=/usr/local/vitasdk
    export PATH=$VITASDK/bin:$PATH        # add both lines to ~/.bashrc
    git clone https://github.com/vitasdk/vdpm
    cd vdpm && ./bootstrap-vitasdk.sh && ./install-all.sh

then run this script again.
EOF
  exit 1
fi
export PATH="$VITASDK/bin:$PATH"

if [ ! -f "$VITASDK/arm-vita-eabi/lib/libSDL2.a" ]; then
  say "installing SDL2 for Vita (vdpm sdl2)"
  command -v vdpm >/dev/null || die "vdpm not on PATH; run: vdpm sdl2"
  vdpm sdl2
fi

if ! command -v cargo >/dev/null; then
  die "Rust is needed for the code generator. Install it from https://rustup.rs then re-run."
fi

# ── 2. ROM ──────────────────────────────────────────────────────────────────
ROM="${1:-}"
if [ -z "$ROM" ]; then
  [ -f "$CANONICAL" ] && ROM="$CANONICAL" || die "usage: $0 <path to your Super Metroid ROM>"
fi
[ -f "$ROM" ] || die "ROM not found: $ROM"
sum="$(sha256sum "$ROM" | cut -d' ' -f1)"
[ "$sum" = "$EXPECTED_SHA256" ] || die "ROM checksum $sum is not the supported dump
       (Super Metroid (Japan, USA), unheadered, SHA-256 $EXPECTED_SHA256)."
say "ROM verified"
if [ "$(cd "$(dirname "$ROM")" && pwd)/$(basename "$ROM")" != "$CANONICAL" ]; then
  cp -f "$ROM" "$CANONICAL"
fi

# ── 3. Generate the recompiled C from the ROM ───────────────────────────────
if [ -e "$ROOT/.git" ]; then
  git -C "$ROOT" submodule update --init --recursive
fi
[ -f "$ROOT/snesrecomp/runner/runner.cmake" ] || die "snesrecomp/ is missing (clone with --recursive)."
if [ -z "$(ls -A "$ROOT/src/gen" 2>/dev/null)" ] || [ "${REGEN:-0}" = 1 ]; then
  say "generating src/gen from your ROM (first run builds the Rust analyzer)"
  bash "$ROOT/tools/regen.sh" --rom "$CANONICAL"
else
  say "src/gen already present (REGEN=1 to regenerate)"
fi

# ── 4. Build ────────────────────────────────────────────────────────────────
say "configuring $BUILD"
cmake "${GEN[@]}" -S "$ROOT" -B "$BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DVITA=ON \
  -DCMAKE_BUILD_TYPE=Release

JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
say "building with $JOBS jobs (the generated code is large; this takes a while)"
cmake --build "$BUILD" -j "$JOBS"

VPK="$BUILD/SuperMetroidRecomp.vpk"
[ -f "$VPK" ] || die "build finished but $VPK is missing"
say "done: $VPK"
cat <<EOF

Next:
  1. Install the VPK with VitaShell.
  2. Copy your ROM to ux0:data/SuperMetroidRecomp/ (any *.sfc / *.smc name).
  3. Launch "Super Metroid Recomp". Double-tap the left half of the screen
     to switch 16:9 widescreen on/off.
EOF
