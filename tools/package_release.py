"""Package only the freshly verified DLL and portable release documentation."""
import argparse
import hashlib
import json
import os
import re
import shutil
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DIST = ROOT / 'dist'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', default='v0.1.1-experimental')
    parser.add_argument('--commit', default=os.environ.get('GITHUB_SHA', 'local'))
    args = parser.parse_args()
    version = args.version if args.version.startswith('v') else 'v0.1.1-dev'
    if not re.fullmatch(r'v[0-9A-Za-z._-]+', version):
        raise ValueError('Invalid release version')
    if args.commit != 'local' and not re.fullmatch(r'[0-9a-f]{40}', args.commit):
        raise ValueError('Invalid commit SHA')
    verification = json.loads((DIST / 'build-verification.json').read_text(encoding='utf-8-sig'))
    tests = json.loads((DIST / 'ci-test-results.json').read_text(encoding='utf-8-sig'))
    dll_hash = digest(DIST / 'dwmapi.dll')
    if verification['result'] != 'passed' or not tests['passed']:
        raise ValueError('Packaging requires successful verification and CI tests')
    if dll_hash != verification['dll_sha256'] or dll_hash != tests['dllSha256']:
        raise ValueError('DLL changed after verification or testing')
    compiler = json.loads((ROOT / 'ci/zig.json').read_text(encoding='utf-8'))
    info = {
        'version': version,
        'source_commit': args.commit,
        'compiler': {'name': 'Zig', 'version': compiler['version'],
                     'archive_sha256': compiler['archive_sha256']},
        'build_verification': verification,
        'synthetic_ci_tests': tests,
        'not_verified': ['Logged-in Oopz advertising UI', 'voice channels',
                         'screen-sharing workflow', 'newer client versions',
                         'all streaming-software detection paths'],
    }
    repo, run = os.environ.get('GITHUB_REPOSITORY'), os.environ.get('GITHUB_RUN_ID')
    if repo and run:
        info['actions_run'] = f'https://github.com/{repo}/actions/runs/{run}'
    (DIST / 'build-info.json').write_text(json.dumps(info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    shutil.copyfile(ROOT / 'docs/使用说明.txt', DIST / '使用说明.txt')
    shutil.copyfile(ROOT / 'oopz_guard.ini.example', DIST / 'oopz_guard.ini.example')
    members = ['dwmapi.dll', '使用说明.txt', 'oopz_guard.ini.example', 'build-info.json']
    inner_checksums = ''.join(f'{digest(DIST / name)}  {name}\n' for name in members)
    archive_name = f'OopzGuard-{version}-windows-x64.zip'
    archive = DIST / archive_name
    with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in members:
            item = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(item, (DIST / name).read_bytes())
        item = zipfile.ZipInfo('SHA256SUMS.txt', date_time=(1980, 1, 1, 0, 0, 0))
        item.compress_type = zipfile.ZIP_DEFLATED
        z.writestr(item, inner_checksums.encode('utf-8'))
    release_files = [archive_name, 'dwmapi.dll', 'build-info.json']
    checksums = ''.join(f'{digest(DIST / name)}  {name}\n' for name in release_files)
    (DIST / 'SHA256SUMS.txt').write_bytes(checksums.encode('utf-8'))
    print(f'Packaged {archive_name}; DLL SHA256 {dll_hash}')


if __name__ == '__main__':
    main()
