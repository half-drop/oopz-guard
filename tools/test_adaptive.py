"""Independent ELF/Dart fixtures for the native adaptive scanner.

The expected output is assembled independently, including the updated hash,
so assertions cover every byte rather than copying the scanner's decisions.
"""
import argparse
import json
import struct
import subprocess
from pathlib import Path


def dart_hash(text):
    value = 0
    units = struct.unpack('<' + 'H' * (len(text.encode('utf-16-le')) // 2), text.encode('utf-16-le'))
    for unit in units:
        value = (value + unit) & 0xffffffff
        value = (value + (value << 10)) & 0xffffffff
        value ^= value >> 6
    value = (value + (value << 3)) & 0xffffffff
    value ^= value >> 11
    value = (value + (value << 15)) & 0x3fffffff
    return value or 1


def object_bytes(text, cid, layout, wide):
    payload = text.encode('utf-16-le' if wide else 'ascii')
    length = len(payload) // (2 if wide else 1)
    header_size = 24 if layout == 2 else 16
    size = (header_size + len(payload) + 15) & ~15
    tags = (cid << 12) | ((size // 16 if size <= 240 else 0) << 8) | 0x31
    if layout == 0:
        header = struct.pack('<QQ', tags | (dart_hash(text) << 32), length * 2)
    elif layout == 1:
        header = struct.pack('<QII', tags, dart_hash(text) * 2, length * 2)
    else:
        header = struct.pack('<QQQ', tags, dart_hash(text) * 2, length * 2)
    return (header + payload).ljust(size, b'\0')


# Ad names and expected replacements are explicit test inputs, not calculated
# by reproducing the native matching logic. General banners and method/source
# names must remain untouched.
STRINGS = [
    ('/uni/advertisement/v17/list', '/uni/zdvertisement/v17/list'),
    ('/advertisement/v42', '/zdvertisement/v42'),
    ('/future/ads/v999', '/future/zds/v999'),
    ('https://cdn.example.com/advertisement/example.webp', 'https://cdn.example.com/zdvertisement/example.webp'),
    ('assets/images/ad-banner.webp', 'assets/images/zd-banner.webp'),
    ('advertisementBannerList', 'zdvertisementBannerList'),
    ('adSection', 'zdSection'), ('adBanner', 'zdBanner'), ('advs', 'zdvs'),
    ('hotRecommendAdItem', 'zotRecommendAdItem'),
    ('banner', None), ('banners', None), ('recommendations', None),
    ('address', None), ('adapter', None), ('loadAdvertisement@112', None),
    ('package:oopz/advertisement.dart', None),
    ('https://ads.example.com/normal/banner.webp', None),
    ('/download/v2', None), ('domainId', None), ('voiceChannel', None),
]


def fixture(cid=117, layout=0, wide=False, shift=0, no_ads=False):
    pairs = [(before, None if no_ads else after) for before, after in STRINGS if not no_ads or after is None]
    original = b''.join(object_bytes(before, cid, layout, wide) for before, _ in pairs)
    expected = b''.join(object_bytes(after or before, cid, layout, wide) for before, after in pairs)
    start = 256 + shift
    executable = object_bytes('/advertisement/v999', cid, layout, wide)
    end = start + len(original)
    image = bytearray(end + len(executable) + 48)
    image[:16] = b'\x7fELF\x02\x01\x01' + b'\0' * 9
    struct.pack_into('<HHIQQQIHHHHHH', image, 16, 3, 62, 1, 0, 64, 0, 0, 64, 56, 2, 0, 0, 0)
    struct.pack_into('<IIQQQQQQ', image, 64, 1, 4, start, 0, 0, len(original), len(original), 16)
    struct.pack_into('<IIQQQQQQ', image, 120, 1, 5, end, 0, 0, len(executable), len(executable), 16)
    image[start:end] = original
    image[end:end + len(executable)] = executable
    patched = bytearray(image)
    patched[start:end] = expected
    return bytes(image), bytes(patched), sum(after is not None for _, after in pairs)


def run(probe, source, output):
    result = subprocess.run([str(probe), str(source), str(output)], capture_output=True, text=True, check=True)
    return json.loads(result.stdout)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe', required=True, type=Path)
    parser.add_argument('--directory', required=True, type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    cases = []
    variants = [
        ('base', {}), ('shifted', {'shift': 4096}), ('cid-changed', {'cid': 287}),
        ('utf16', {'wide': True}), ('compressed-hash', {'cid': 233, 'layout': 1}),
        ('separate-hash', {'cid': 311, 'layout': 2}),
        ('wide-separate', {'cid': 402, 'layout': 2, 'wide': True, 'shift': 256}),
        ('no-ads', {'no_ads': True}),
    ]
    for name, options in variants:
        original, expected, count = fixture(**options)
        source, output = args.directory / f'{name}.so', args.directory / f'{name}.patched.so'
        source.write_bytes(original)
        result = run(args.probe, source, output)
        assert result['recognized'] and result['patched'] == count, (name, result)
        assert result['apiPaths'] == (0 if count == 0 else 3), (name, result)
        assert result['classId'] == options.get('cid', 117), (name, result)
        assert output.read_bytes() == expected, f'Unexpected changed byte: {name}'
        assert source.read_bytes() == original, f'Input modified: {name}'
        cases.append({'name': name, 'patched': count, 'passed': True})
    image, _, _ = fixture()
    corrupt = bytearray(image)
    # Break every object's verified hash, leaving advertising-looking bytes.
    at = 256
    for before, _ in STRINGS:
        corrupt[at + 4] ^= 1
        at += len(object_bytes(before, 117, 0, False))
    executable_only = bytearray(image)
    struct.pack_into('<I', executable_only, 68, 5)
    overlap = bytearray(image)
    struct.pack_into('<IIQQQQQQ', overlap, 120, 1, 4, 256, 0, 0, 64, 64, 16)
    for name, original in [
        ('truncated', image[:-120]), ('invalid-header', b'\1\2\3\4'),
        ('invalid-hashes', corrupt), ('executable-only', executable_only),
        ('overlapping-segments', overlap),
    ]:
        source, output = args.directory / f'{name}.so', args.directory / f'{name}.patched.so'
        source.write_bytes(original)
        result = run(args.probe, source, output)
        assert not result['recognized'] and result['patched'] == 0, (name, result)
        assert output.read_bytes() == original, f'Invalid image changed: {name}'
        cases.append({'name': name, 'passed': True})
    summary = {'passed': True, 'cases': cases, 'count': len(cases)}
    (args.directory / 'adaptive-results.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print(f"adaptive scanner tests: {len(cases)} cases, 0 failures")


if __name__ == '__main__':
    main()
