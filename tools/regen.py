#!/usr/bin/env python3
"""Generate the release profile locally from a verified owned ROM."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--rom', default=os.environ.get('SNESRECOMP_ROM'))
    parser.add_argument('--out-dir', default='src/gen')
    parser.add_argument('--strict-idempotent', action='store_true')
    parser.add_argument('--profile-manifest', action='append', default=[])
    parser.add_argument('--cfg-roots', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    os.chdir(ROOT)
    profile = json.loads((ROOT / 'profiles/release.json').read_text())
    rom = Path(args.rom or 'Super Metroid (Japan, USA) (En,Ja).sfc').resolve()
    if hashlib.sha256(rom.read_bytes()).hexdigest() != profile['rom_sha256']:
        parser.error('ROM does not match the verified JU release profile')
    engine = Path(os.environ.get('SNESRECOMP_ROOT', ROOT / 'snesrecomp')).resolve()
    out = Path(args.out_dir).resolve()
    command = [sys.executable, str(engine / 'tools/v2_emit.py'), '--rom', str(rom),
               '--cfg-dir', str(ROOT / 'recomp'), '--source-root', str(ROOT / 'src')]
    if profile['cfg_roots']:
        command += ['--cfg-roots']
    for path in profile['historical_profiles']:
        command += ['--historical-profile-manifest', str(ROOT / path)]
    for path in args.profile_manifest:
        command += ['--profile-manifest', path]
    # Host alias/interrupt discovery reads the generated declarations too.
    # Sync before emission so a clean checkout and a repeat build agree.
    subprocess.run([sys.executable, str(engine / 'tools/v2_sync_funcs_h.py'),
                    '--cfg-dir', str(ROOT / 'recomp'), '--out', str(ROOT / 'recomp/funcs.h')], check=True)
    subprocess.run(command + ['--out-dir', str(out)], check=True)
    if args.strict_idempotent:
        with tempfile.TemporaryDirectory(prefix='sm-idempotency-') as scratch:
            second = Path(scratch) / 'gen'
            subprocess.run(command + ['--out-dir', str(second)], check=True)
            def hashes(directory):
                return {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                        for p in directory.iterdir() if p.is_file()
                        and p.name not in ('.gitkeep', '.snesrecomp-cache.json')}
            first_hashes, second_hashes = hashes(out), hashes(second)
            if first_hashes != second_hashes:
                changed = sorted(n for n in first_hashes.keys() | second_hashes.keys()
                                 if first_hashes.get(n) != second_hashes.get(n))
                raise RuntimeError(f'Two generations differ: {changed}')
        print('Two generations produced byte-identical program output')


if __name__ == '__main__':
    main()
