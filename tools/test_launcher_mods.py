#!/usr/bin/env python3
"""Exercise actual launcher Mods controls and Play using recomp-ui's test hook.

Creates a fresh config/save directory. Screenshots need visual inspection;
settings and the launched game's state trace are checked automatically.
"""
import argparse
import configparser
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--rom", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--reference-trace", type=Path, required=True)
    args = parser.parse_args()
    exe, rom = args.exe.resolve(strict=True), args.rom.resolve(strict=True)
    reference = args.reference_trace.read_bytes()
    if len(reference.splitlines()) != 3200:
        parser.error("reference trace must contain 3200 frames")
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="sm-launcher-", dir=args.artifacts.resolve()))
    print(f"Artifacts: {root}", flush=True)
    (root / "config.ini").write_text(
        "[General]\nAutosave=0\nSkipLauncher=0\n[Graphics]\n"
        "WindowScale=2\nNewRenderer=1\nNoSpriteLimits=0\n[Sound]\nEnableAudio=1\n")
    (root / "rom.cfg").write_text(str(rom) + "\n", encoding="utf-8")
    (root / "sm-video.ini").write_text(
        "[SuperMetroidVideo]\nEnhancedRenderer=0\nAspect=Fit\n"
        "PresentationEnabled=0\nPresentationFPS=0\nHudAnchored=1\n")
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(("SM_", "SNESRECOMP_FRAME_BMP", "LNG_")):
            env.pop(key)
    env.pop("SNESRECOMP_NO_LAUNCHER", None)
    env.pop("SNESRECOMP_WIDESCREEN", None)
    # Coordinates are logical pixels at the explicitly requested window size.
    # Wait after Play: the synthetic click is consumed over subsequent frames.
    env["LNG_SCRIPT"] = (
        "size:1280x800;wait:10;shot:dashboard.png;click:1200,48;wait:10;"
        "shot:mods-off.png;click:750,424;wait:5;shot:aspects.png;"
        "click:670,553;wait:5;click:64,363;wait:5;click:64,304;wait:5;"
        "click:170,313;wait:5;click:750,424;wait:5;shot:rates.png;"
        "click:660,552;wait:5;shot:fps-120.png;click:170,372;wait:5;"
        "shot:wide-32.png;click:1150,740;wait:20")
    env.update(SM_RUN_FRAMES="3200", SM_STATE_TRACE="state.csv",
               SNESRECOMP_FRAME_BMP="game.bmp", SNESRECOMP_FRAME_BMP_FRAME="3200",
               SNESRECOMP_DEBUG_PORT="4399")
    with (root / "stdout.log").open("wb") as out, (root / "stderr.log").open("wb") as err:
        process = subprocess.Popen([str(exe), "--config", "config.ini"],
                                   cwd=root, env=env, stdout=out, stderr=err)
        try:
            for elapsed in range(20, 241, 20):
                try:
                    code = process.wait(timeout=20)
                    break
                except subprocess.TimeoutExpired:
                    print(f"pid={process.pid} live, wait={elapsed}s", flush=True)
            else:
                raise RuntimeError("launcher/game did not finish within 240 seconds")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    if code:
        raise RuntimeError(f"game exited {code}; see {root}")
    saved = configparser.ConfigParser()
    saved.read(root / "sm-video.ini")
    for key, value in {"EnhancedRenderer": "1", "Aspect": "32:9",
                       "PresentationEnabled": "1", "PresentationFPS": "120",
                       "HudAnchored": "1"}.items():
        if saved["SuperMetroidVideo"].get(key) != value:
            raise RuntimeError(f"UI did not commit {key}={value}")
    if (root / "state.csv").read_bytes() != reference:
        raise RuntimeError("UI-launched game state differs from stock")
    for name in ("dashboard", "mods-off", "aspects", "rates", "fps-120", "wide-32"):
        if (root / f"{name}.png").read_bytes()[:8] != b"\x89PNG\r\n\x1a\n":
            raise RuntimeError(f"missing screenshot {name}")
    print("PASS: actual Mods clicks committed 32:9/120 FPS and launched an unchanged 3200-frame simulation.")


if __name__ == "__main__":
    main()
