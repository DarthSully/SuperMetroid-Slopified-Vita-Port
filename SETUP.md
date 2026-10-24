# Building the Super Metroid Vita Port

This guide covers building the Vita port from source.

## Prerequisites

### ROM
You'll need a legally-dumped copy of **Super Metroid (Japan, USA) (En,Ja).sfc** — the SNES ROM file with the standard filename and hash. No ROM is included in this repo; you must provide your own.

### Build Environment (Windows PC with WSL)

1. **WSL 2** with Ubuntu 20.04 LTS or newer
2. **ARM toolchain** (aarch64-linux-gnu):
   ```bash
   sudo apt-get update
   sudo apt-get install -y \
     build-essential cmake ninja-build \
     aarch64-linux-gnu-binutils aarch64-linux-gnu-gcc \
     libvita-toolchain-bin vita-headers vita-libraries
   ```
3. **VitaSDK** or compatible Vita cross-compiler
4. **Python 3.8+** for build scripts
5. **Git** (with long paths enabled on Windows)

### Optional: Desktop Build (for testing)

If you want to test the build on Windows x64 before Vita deployment:

- **MSVC** or **MinGW-w64** (gcc)
- **SDL3** + **OpenGL**
- **CMake 3.20+**

## Build Steps

### 1. Prepare the workspace

On your PC, create a build directory:

```cmd
mkdir C:\VitaBuild
cd C:\VitaBuild
```

Clone this repo (or extract it):

```cmd
git clone https://github.com/DarthSully/SuperMetroid-Slopified-Vita-Port.git .
```

### 2. Place your ROM

Copy your ROM file to the repo root:

```cmd
copy "path\to\Super Metroid (Japan, USA) (En,Ja).sfc" .
```

Verify the filename is exactly `Super Metroid (Japan, USA) (En,Ja).sfc` (case-sensitive in WSL).

### 3. Enter WSL and configure

In PowerShell or CMD, launch WSL:

```cmd
wsl
cd /mnt/c/VitaBuild
```

Configure the CMake build for Vita (aarch64):

```bash
cmake -G Ninja -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE=snesrecomp/vita_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -S .
```

### 4. Build

```bash
cmake --build build-vita -j $(nproc)
```

The build generates ~93 MB of C code from the ROM, recompiles it to native code, links against the SNES emulation layer, and produces `build-vita/SuperMetroidRecomp.elf` and `build-vita/SuperMetroidRecomp.vpk`.

Build time varies (10–30 minutes depending on your system).

### 5. Retrieve the VPK

The compiled VPK is at:

```
build-vita/SuperMetroidRecomp.vpk
```

Copy it from WSL back to Windows:

```bash
cp build-vita/SuperMetroidRecomp.vpk /mnt/c/VitaBuild/
```

### Alternative: Pre-built VPK

A pre-compiled VPK is included in [`downloads/SuperMetroid-Vita-r18.vpk`](downloads/SuperMetroid-Vita-r18.vpk). Skip the build steps above and download it directly from the repo.

## Installation on PS Vita

1. **Connect your Vita** to your PC via USB.
2. **Open VitaShell** on the Vita.
3. **Copy the VPK** to your Vita (USB storage or ux0: directly).
4. **Install the VPK** via VitaShell (highlight → Triangle → Install).

After installation:

1. **Locate your ROM file** on your Vita:
   - Create the folder: `ux0:data/SuperMetroidRecomp/`
   - Copy `Super Metroid (Japan, USA) (En,Ja).sfc` into it

2. **Launch the game** from the Vita home screen.

## Controls (Vita)

| Action | Button/Gesture |
|--------|-----------------|
| Menu | D-Pad, Analog, Buttons (SNES layout) |
| Start (skips intro) | Start button |
| Toggle 16:9 widescreen | Double-tap left half of touch screen |
| Toggle infinite energy | Double-tap right half of touch screen |
| Save/Load | In-game (same as SNES) |

Saves are written to `ux0:data/SuperMetroidRecomp/` after each in-game save.

## Troubleshooting

### Build fails with "ROM not found"
- Ensure the ROM filename is exactly `Super Metroid (Japan, USA) (En,Ja).sfc` (case-sensitive).
- Place it in the repo root (same directory as `CMakeLists.txt`).

### WSL path issues
- Ensure the path uses forward slashes in WSL: `/mnt/c/VitaBuild/`, not `C:\VitaBuild`.
- Long paths on Windows may need `git config core.longpaths true` before cloning.

### VPK won't install on Vita
- Ensure your Vita's PSM Dev Assistant or HENkaku is running.
- Try re-extracting the repo and rebuilding.

### Game crashes on Vita
- Confirm the ROM is in the correct folder: `ux0:data/SuperMetroidRecomp/Super Metroid (Japan, USA) (En,Ja).sfc`.
- Check that the ROM file is exactly the expected version (the build is tuned to this ROM).

## Next Steps

For performance tuning and rebuilding for optimization, see the main [README.md](README.md) and the snesrecomp documentation in `snesrecomp/docs/`.
