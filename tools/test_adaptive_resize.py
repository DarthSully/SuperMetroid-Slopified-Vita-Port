#!/usr/bin/env python3
"""Windows live-window Fit regression; isolated artifacts and caller-owned ROM.

Moves/resizes only the child process's window. Captured BMPs require visual HUD
review; trace equality is a regression probe, not a complete emulation oracle.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    parser.add_argument('--reference-trace', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('Windows window APIs are required')
    exe, rom = args.exe.resolve(strict=True), args.rom.resolve(strict=True)
    reference = args.reference_trace.read_bytes().splitlines()
    if len(reference) != 3200:
        parser.error('expected the fresh 3200-frame stock reference')
    args.artifacts.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix='sm-resize-', dir=args.artifacts.resolve()))
    print(f'Artifacts: {root}', flush=True)
    (root / 'config.ini').write_text('[General]\nAutosave=0\nSkipLauncher=1\n'
        'DisableFrameDelay=0\n[Graphics]\nWindowScale=2\nNewRenderer=1\n'
        'NoSpriteLimits=0\n[Sound]\nEnableAudio=1\n')
    (root / 'sm-video.ini').write_text('[SuperMetroidVideo]\nEnhancedRenderer=1\n'
        'Aspect=Fit\nPresentationEnabled=1\nPresentationFPS=120\nHudAnchored=1\n')
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('SM_CAPTURE_', 'SM_VIDEO_', 'SM_STATE_', 'SNESRECOMP_FRAME_BMP')):
            env.pop(key)
    env.pop('SNESRECOMP_WIDESCREEN', None)
    env.pop('LNG_SCRIPT', None)
    env.update(SM_RUN_FRAMES='3600', SM_STATE_TRACE='state.csv',
               SNESRECOMP_FRAME_BMP='live.bmp', SNESRECOMP_DEBUG_PORT='4399')
    user = c.WinDLL('user32', use_last_error=True)
    user.SetProcessDPIAware()
    callback_type = c.WINFUNCTYPE(w.BOOL, w.HWND, w.LPARAM)
    user.EnumWindows.argtypes = [callback_type, w.LPARAM]
    user.GetWindowThreadProcessId.argtypes = [w.HWND, c.POINTER(w.DWORD)]
    user.IsWindowVisible.argtypes = [w.HWND]
    user.GetClientRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.GetWindowRect.argtypes = [w.HWND, c.POINTER(w.RECT)]
    user.SetWindowPos.argtypes = [w.HWND, w.HWND, c.c_int, c.c_int, c.c_int, c.c_int, w.UINT]
    samples = []
    with (root / 'stdout.log').open('wb') as out, (root / 'stderr.log').open('wb') as err:
        process = subprocess.Popen([str(exe), '--config', 'config.ini', str(rom)],
                                   cwd=root, env=env, stdout=out, stderr=err)
        started = time.monotonic()
        try:
            windows = []
            @callback_type
            def visit(hwnd, _):
                owner = w.DWORD()
                user.GetWindowThreadProcessId(hwnd, c.byref(owner))
                if owner.value == process.pid and user.IsWindowVisible(hwnd):
                    windows.append(hwnd)
                return True
            while not windows:
                if process.poll() is not None or time.monotonic() - started > 20:
                    raise RuntimeError('child game window unavailable')
                user.EnumWindows(visit, 0)
                time.sleep(.1)
            hwnd = windows[0]
            # Wait for the landing-site HUD, retaining normal guest timing.
            while time.monotonic() - started < 50:
                if process.poll() is not None:
                    raise RuntimeError('game exited before resize checks')
                time.sleep(1)
                elapsed = int(time.monotonic() - started)
                if elapsed % 20 == 0:
                    print(f'child pid={process.pid} live, {elapsed}s', flush=True)
            for index, (width, height) in enumerate(((800, 600), (960, 540),
                    (1260, 540), (1440, 405), (600, 700), (960, 540))):
                owner = w.DWORD()
                user.GetWindowThreadProcessId(hwnd, c.byref(owner))
                if owner.value != process.pid or process.poll() is not None:
                    raise RuntimeError('owned window no longer live')
                client, outer = w.RECT(), w.RECT()
                user.GetClientRect(hwnd, c.byref(client))
                user.GetWindowRect(hwnd, c.byref(outer))
                frame_w = outer.right - outer.left - client.right
                frame_h = outer.bottom - outer.top - client.bottom
                if not user.SetWindowPos(hwnd, None, 20, 40, width + frame_w,
                                         height + frame_h, 0x0014):
                    raise c.WinError(c.get_last_error())
                time.sleep(.4)
                user.GetClientRect(hwnd, c.byref(client))
                if (client.right, client.bottom) != (width, height):
                    raise RuntimeError(f'client size differs: {client.right}x{client.bottom}')
                aspect = max(4 / 3, min(32 / 9, width / height))
                expected = 2 * int(96 * aspect + .5)
                deadline = time.monotonic() + 3
                while True:
                    try:
                        bitmap = (root / 'live.bmp').read_bytes()
                        actual = struct.unpack_from('<ii', bitmap, 18)
                        complete = struct.unpack_from('<I', bitmap, 2)[0] == len(bitmap)
                        if complete and actual == (expected, -224):
                            break
                    except (OSError, struct.error):
                        pass
                    if time.monotonic() > deadline:
                        raise RuntimeError(f'no complete {expected}x224 presentation after resize')
                    time.sleep(.05)
                name = f'{index}-{width}x{height}.bmp'
                (root / name).write_bytes(bitmap)
                samples.append(dict(client=[width, height], render_width=expected, image=name))
                print(f'{width}x{height}: render width {expected}', flush=True)
            while process.poll() is None:
                if time.monotonic() - started > 120:
                    raise TimeoutError('game exceeded 120 seconds')
                time.sleep(1)
            if process.returncode:
                raise RuntimeError(f'game exited {process.returncode}')
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
    trace = (root / 'state.csv').read_bytes().splitlines()
    if len(trace) != 3600 or trace[:3200] != reference:
        raise RuntimeError('simulation trace missing or differs from stock')
    report = dict(passed=True, samples=samples, compared_stock_frames=3200,
                  total_frames=3600, exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest())
    (root / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: live Fit resizing and 3200-frame stock trace equality; review HUD BMPs.', flush=True)


if __name__ == '__main__':
    main()
