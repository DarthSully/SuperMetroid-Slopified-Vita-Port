#!/usr/bin/env python3
"""Upload generated C privately and refresh the short-lived CI download secret.

Uses the existing gh login locally. Its credential is never copied to Actions;
only a signed download URL for this one asset is stored as a repository secret.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import urllib.error
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('archive', type=Path)
    parser.add_argument('--storage-repo', default='mstan/recomp-coverage-vault')
    parser.add_argument('--build-repo', default='mstan/SuperMetroidRecomp')
    args = parser.parse_args()
    gh = shutil.which('gh')
    def run(*command):
        return subprocess.check_output([gh, *command], text=True).strip()
    if not json.loads(run('api', f'repos/{args.storage_repo}'))['private']:
        raise ValueError('Generated build storage must be private')
    digest = hashlib.sha256(args.archive.read_bytes()).hexdigest()
    tag = 'sm-generated-' + digest[:16]
    found = subprocess.run([gh, 'release', 'view', tag, '--repo', args.storage_repo],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if found.returncode:
        run('release', 'create', tag, '--repo', args.storage_repo, '--title', tag,
            '--notes', 'Private Super Metroid generated build input. No ROM. Not for redistribution.')
        run('release', 'upload', tag, str(args.archive), '--repo', args.storage_repo)
    release = json.loads(run('api', f'repos/{args.storage_repo}/releases/tags/{tag}'))
    asset = next(a for a in release['assets'] if a['name'] == args.archive.name)
    if asset.get('digest') != 'sha256:' + digest:
        raise ValueError('Private asset SHA-256 differs from local input')
    token = run('auth', 'token')
    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            return None
    request = urllib.request.Request(asset['url'], headers={
        'Authorization': 'Bearer ' + token, 'Accept': 'application/octet-stream',
        'User-Agent': 'SuperMetroid-release'})
    try:
        urllib.request.build_opener(NoRedirect).open(request)
        raise RuntimeError('Expected a signed private asset redirect')
    except urllib.error.HTTPError as response:
        if response.code != 302:
            raise RuntimeError(f'Private asset download API returned {response.code}') from None
        url = response.headers['Location']
    if not url.startswith('https://release-assets.githubusercontent.com/'):
        raise ValueError('Unexpected signed asset host')
    subprocess.run([gh, 'secret', 'set', 'SM_GENERATED_CODE_URL', '--repo', args.build_repo],
                   input=url, text=True, check=True)
    print(json.dumps(dict(storage=args.storage_repo, tag=tag, sha256=digest,
                         note='Start the release workflow now; signed URL expires.')))


if __name__ == '__main__':
    main()
