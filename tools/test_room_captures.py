#!/usr/bin/env python3
"""Replay captured gameplay rooms at each supported fixed aspect.

Native-center equality is an oracle only for native pixels. Retained wider
images still require visual inspection; this is not a full-game fidelity test.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--exe', type=Path, required=True)
    parser.add_argument('--rom', type=Path, required=True)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--artifacts', type=Path, required=True)
    args = parser.parse_args()
    exe = args.exe.resolve(strict=True)
    rom = args.rom.resolve(strict=True)
    captures = sorted(args.captures.resolve(strict=True).glob('room.*.capture'))
    if not captures:
        parser.error('no room captures found')
    args.artifacts.mkdir(parents=True, exist_ok=True)
    output = Path(tempfile.mkdtemp(prefix='sm-rooms-', dir=args.artifacts.resolve()))
    env = os.environ.copy()
    for key in list(env):
        if key.startswith(('SM_RENDER_', 'SM_CAPTURE_')) or key == 'SM_XRAY_TILE_REPORT':
            env.pop(key)
    env['SM_RENDER_REQUIRE_NATIVE_MATCH'] = '1'
    rows = []
    for capture in captures:
        for aspect in ('4:3', '16:9', '21:9', '32:9'):
            stem = capture.stem + '.' + aspect.replace(':', 'x')
            bitmap = output / (stem + '.bmp')
            result = subprocess.run([str(exe), str(capture), str(rom), aspect, str(bitmap)],
                                    env=env, capture_output=True, timeout=60)
            log = result.stdout + result.stderr
            (output / (stem + '.log')).write_bytes(log)
            passed = result.returncode == 0
            rows.append(dict(capture=str(capture), aspect=aspect, passed=passed,
                             exit_code=result.returncode, bitmap=str(bitmap)))
            print(f'{capture.name} {aspect}: {"PASS" if passed else "FAIL"}', flush=True)
    passed = all(row['passed'] for row in rows)
    report = dict(passed=passed, runs=rows, exe=str(exe), rom=str(rom),
                  exe_sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                  rom_sha256=hashlib.sha256(rom.read_bytes()).hexdigest(),
                  captures_sha256={str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                   for path in captures})
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Artifacts: {output}', flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
