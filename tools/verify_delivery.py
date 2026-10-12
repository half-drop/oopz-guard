"""Verify real snapshot edits and current local test records without a whitelist."""
import argparse
import hashlib
import json
import os
import struct
from datetime import datetime, timezone
from pathlib import Path
from test_adaptive import dart_hash

ROOT = Path(__file__).resolve().parents[1]


def changed_string(original, patched, first):
    for distance in range(4, 16):
        at = first - distance
        if at < 0 or at % 8:
            continue
        tags = struct.unpack_from('<Q', original, at)[0]
        for layout in range(3):
            header = 24 if layout == 2 else 16
            if layout == 0:
                raw_length = struct.unpack_from('<Q', original, at + 8)[0]
                old_hash, new_hash = tags >> 32, struct.unpack_from('<I', patched, at + 4)[0]
                hash_at, hash_size = at + 4, 4
            else:
                raw_length = struct.unpack_from('<I' if layout == 1 else '<Q', original, at + (12 if layout == 1 else 16))[0]
                fmt = '<I' if layout == 1 else '<Q'
                old_hash = struct.unpack_from(fmt, original, at + 8)[0] >> 1
                new_hash = struct.unpack_from(fmt, patched, at + 8)[0] >> 1
                hash_at, hash_size = at + 8, 4 if layout == 1 else 8
            if raw_length & 1 or not 0 < raw_length <= 8192:
                continue
            for width in (1, 2):
                end = at + header + raw_length // 2 * width
                if end > len(original):
                    continue
                before, after = original[at + header:end], patched[at + header:end]
                encoding = 'latin1' if width == 1 else 'utf-16-le'
                try:
                    old, new = before.decode(encoding), after.decode(encoding)
                except UnicodeError:
                    continue
                if dart_hash(old) != old_hash or dart_hash(new) != new_hash:
                    continue
                changed_characters = [i for i, pair in enumerate(zip(old, new)) if pair[0] != pair[1]]
                if len(changed_characters) != 1 or new[changed_characters[0]] not in ('z', 'Z'):
                    continue
                expected = bytearray(original[at:end])
                expected[header:] = after
                expected[hash_at - at:hash_at - at + hash_size] = patched[hash_at:hash_at + hash_size]
                if bytes(expected) != patched[at:end]:
                    continue
                return at, end, {'before': old, 'after': new, 'classId': (tags >> 12) & 0xfffff, 'layout': layout, 'width': width}
    raise ValueError(f'Unexpected modification at byte {first}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--aot', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '.')) / 'oopz/data/app.so')
    parser.add_argument('--patched', type=Path, default=ROOT / 'tests/runtime/real.patched.so')
    args = parser.parse_args()
    original, patched = args.aot.read_bytes(), args.patched.read_bytes()
    assert len(original) == len(patched), 'Length changed'
    differences = [i for i, pair in enumerate(zip(original, patched)) if pair[0] != pair[1]]
    remaining = set(differences)
    changes = []
    while remaining:
        start, end, change = changed_string(original, patched, min(remaining))
        remaining.difference_update(range(start, end))
        changes.append(change)
    local = json.loads((ROOT / 'dist/local-test-results.json').read_text(encoding='utf-8'))
    ci = json.loads((ROOT / 'dist/ci-test-results.json').read_text(encoding='utf-8'))
    dll_hash = hashlib.sha256((ROOT / 'dist/dwmapi.dll').read_bytes()).hexdigest()
    assert local['passed'] and local['originalUnchanged'] and ci['passed']
    assert local['dllSha256'] == ci['dllSha256'] == dll_hash
    assert len(changes) == local['scan']['patched'] and len(changes) > 0
    result = {
        'passed': True, 'date': datetime.now(timezone.utc).isoformat(), 'release': '0.2.0',
        'dllSha256': dll_hash, 'sourceUnchanged': True,
        'snapshotBytes': len(original), 'changedBytes': len(differences),
        'onlyEqualLengthStringAndHashChanges': True, 'changes': changes,
        'scan': local['scan'], 'ciTestCount': len(ci['tests']),
        'localTestCount': len(local['tests']), 'realEngineTested': local['realEngineTested'],
        'notVerified': ['logged-in advertising UI', 'voice and sharing workflows', 'all future framework layouts'],
    }
    (ROOT / 'dist/验证记录.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({key: value for key, value in result.items() if key != 'changes'}, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
