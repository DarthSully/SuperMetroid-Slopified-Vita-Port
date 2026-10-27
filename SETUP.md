# Installing and building the Super Metroid Vita port

## Playing (no build needed)

1. Download [downloads/SuperMetroid-Vita-r21.vpk](downloads/SuperMetroid-Vita-r21.vpk) and
   install it with **VitaShell** (highlight the file → Cross → Yes).
2. Put **your own** legally dumped ROM in `ux0:data/SuperMetroidRecomp/`.
   The supported dump is `Super Metroid (Japan, USA) (En,Ja).sfc`; a single `.sfc` or `.smc`
   file of that exact dump under another name works too. No ROM is included here.
3. Start the game from the LiveArea.

**Controls and extras**

| | |
|---|---|
| Buttons | SNES layout |
| Start (new game) | Skips the intro |
| Double-tap left half of the touch screen | 16:9 on/off |
| Double-tap right half of the touch screen | Infinite energy on/off |

Saves, settings and the log (`log.txt`) live in `ux0:data/SuperMetroidRecomp/`. Saves are
written shortly after every in-game save, so closing with the PS button is safe.

An empty file named `profile` in that folder adds a timing line every 300 frames to the
log — useful when reporting performance.

## Building from source

The build runs on Linux; on Windows use WSL (Ubuntu). The recompiled C is generated from
your ROM on your machine and is never committed.

### 1. One-time setup (WSL Ubuntu)

```bash
sudo apt-get update
sudo apt-get install -y build-essential git cmake ninja-build python3 curl xz-utils bzip2
# Rust (the recompiler's analyzer is written in Rust)
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
. "$HOME/.cargo/env"
# VitaSDK and its libraries (SDL2 included), via vdpm
export VITASDK=/usr/local/vitasdk PATH=/usr/local/vitasdk/bin:$PATH
git clone --depth 1 https://github.com/vitasdk/vdpm ~/vdpm
cd ~/vdpm && ./bootstrap-vitasdk.sh && ./install-all.sh
```

Add `export VITASDK=/usr/local/vitasdk` and `export PATH=$VITASDK/bin:$PATH` to `~/.bashrc`.

### 2. Get the source and your ROM

```bash
git clone --recurse-submodules https://github.com/DarthSully/SuperMetroid-Slopified-Vita-Port.git
cd SuperMetroid-Slopified-Vita-Port
cp "/path/to/Super Metroid (Japan, USA) (En,Ja).sfc" .
```

Build inside the WSL filesystem (for example `~/`), not under `/mnt/c`: it is much faster.

### 3. Generate the recompiled C

```bash
./tools/regen.sh
```

This checks the ROM against the expected dump and writes `src/gen/` (about 230 MB). It
takes a couple of minutes; the first run also builds the Rust analyzer.

### 4. Build the VPK

```bash
cmake -G Ninja -S . -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake \
  -DVITA=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-vita -j"$(nproc)"
```

The result is `build-vita/SuperMetroid-Vita-r21.vpk`. Install it as above.

## Troubleshooting

- **`regen.sh`: ROM does not match** — only the exact `Super Metroid (Japan, USA) (En,Ja)`
  dump is supported.
- **Link errors mentioning `std::`, `sceShaccCg…` or `tai…`** — your VitaSDK is older or
  newer than expected; update it with `~/vdpm/install-all.sh` and rebuild.
- **The game is slow and the log shows `render driver in use: opengles2`** — your SDL2
  build has no native GXM renderer; please open an issue with your log.
- **Black screen / no game on start** — check that the ROM is in
  `ux0:data/SuperMetroidRecomp/` and read `log.txt` there.
