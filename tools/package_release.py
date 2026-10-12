"""Package the verified Guard release, portable documentation and source."""
import argparse
from datetime import datetime, timezone
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


def write_archive(path, prefix, members):
    """Use relative portable names and include checksums for every input file."""
    hashes = []
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, source in sorted(members.items()):
            data = source.read_bytes()
            item = zipfile.ZipInfo(f'{prefix}/{name}', date_time=(1980, 1, 1, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(item, data)
            hashes.append(f'{hashlib.sha256(data).hexdigest()}  {name}\n')
        item = zipfile.ZipInfo(f'{prefix}/FILE_HASHES.txt', date_time=(1980, 1, 1, 0, 0, 0))
        item.compress_type = zipfile.ZIP_DEFLATED
        archive.writestr(item, ''.join(hashes).encode('utf-8'))


def source_members():
    members = {name: ROOT / name for name in
               ('README.md', 'VALIDATION.md', 'oopz_guard.ini.example',
                'build.ps1', '.gitignore', '.gitattributes')}
    # Only source/documentation files from known flat directories are shipped.
    # Runtime test folders, caches, binaries and investigation output stay out.
    extensions = {
        '.github/workflows': {'.yml'}, 'ci': {'.ps1', '.json'},
        'docs': {'.md', '.txt'}, 'research': {'.md', '.json'},
        'src': {'.c', '.h', '.S', '.def'}, 'tests': {'.ps1', '.c'},
        'tools': {'.py'},
    }
    for directory, allowed in extensions.items():
        for source in sorted((ROOT / directory).iterdir()):
            if source.is_file() and source.suffix in allowed:
                members[source.relative_to(ROOT).as_posix()] = source
    return members


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--version', default='0.1.2')
    parser.add_argument('--commit', default=os.environ.get('GITHUB_SHA', 'local'))
    args = parser.parse_args()
    version = args.version.removeprefix('v')
    if not re.fullmatch(r'\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?', version):
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
    repo, run = os.environ.get('GITHUB_REPOSITORY'), os.environ.get('GITHUB_RUN_ID')
    info = {
        'schemaVersion': 1, 'suite': 'Guard', 'product': 'OopzGuard',
        'version': version, 'commit': args.commit,
        'actionsRun': f'https://github.com/{repo}/actions/runs/{run}' if repo and run else None,
        'builtAtUtc': datetime.now(timezone.utc).isoformat(), 'verified': True,
        'verification': {'build': verification, 'syntheticTests': tests},
        'compatibleClient': 'Oopz 0.88.41 exact AOT / Windows x64',
        'installation': 'Same-directory dwmapi.dll',
        'compiler': {'name': 'Zig', 'version': compiler['version'],
                     'archive_sha256': compiler['archive_sha256']},
        'unverified': ['Logged-in Oopz advertising UI', 'voice channels',
                       'screen-sharing workflow', 'newer client versions',
                       'all streaming-software detection paths'],
    }
    (DIST / 'build-info.json').write_text(json.dumps(info, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    # Preserve the standalone files offered by earlier releases as well.
    shutil.copyfile(ROOT / 'docs/使用说明.txt', DIST / '使用说明.txt')
    shutil.copyfile(ROOT / 'oopz_guard.ini.example', DIST / 'oopz_guard.ini.example')
    members = {
        'dwmapi.dll': DIST / 'dwmapi.dll', 'build-info.json': DIST / 'build-info.json',
        'README.md': ROOT / 'README.md', 'VALIDATION.md': ROOT / 'VALIDATION.md',
        '使用说明.txt': ROOT / 'docs/使用说明.txt',
        'docs/使用说明.txt': ROOT / 'docs/使用说明.txt',
        'oopz_guard.ini.example': ROOT / 'oopz_guard.ini.example',
    }
    prefix = f'OopzGuard-v{version}'
    archive_name = f'{prefix}-windows.zip'
    source_name = f'{prefix}-source.zip'
    write_archive(DIST / archive_name, f'{prefix}-windows', members)
    sources = source_members()
    sources['build-info.json'] = DIST / 'build-info.json'
    write_archive(DIST / source_name, f'{prefix}-source', sources)
    release_files = [archive_name, source_name, 'dwmapi.dll', 'build-info.json']
    checksums = ''.join(f'{digest(DIST / name)}  {name}\n' for name in release_files)
    (DIST / 'SHA256SUMS.txt').write_bytes(checksums.encode('utf-8'))
    print(f'Packaged {archive_name} and {source_name}; DLL SHA256 {dll_hash}')


if __name__ == '__main__':
    main()
