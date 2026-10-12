"""Read-only Oopz ELF/Dart AOT string inspection, never modifies input."""
import argparse
import bisect
import hashlib
import json
import re
import struct
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('path')
    parser.add_argument('--pattern', default=r'obs|broadcast|advert|FindWindow|GetWindowText|Dart VM|release.*x64')
    parser.add_argument('--neighbors', type=int, default=5)
    parser.add_argument('--hex-context', type=int, default=32)
    parser.add_argument('--case-sensitive', action='store_true')
    parser.add_argument('--compact', action='store_true')
    args = parser.parse_args()
    data = Path(args.path).read_bytes()
    strings = [(m.start(), m.end(), m.group().decode('ascii')) for m in re.finditer(rb'[\x20-\x7e]{4,}', data)]
    result = {'path': args.path, 'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'hits': []}
    pattern = re.compile(args.pattern, 0 if args.case_sensitive else re.I)
    for i, (start, end, value) in enumerate(strings):
        if not pattern.search(value):
            continue
        result['hits'].append({
            'offset': hex(start), 'length': end-start, 'value': value,
            'preceding_hex': data[max(0,start-args.hex_context):start].hex(' '),
            'following_hex': data[end:end+args.hex_context].hex(' '),
            'neighbors': [{'offset': hex(p), 'value': s} for p, e, s in strings[max(0,i-args.neighbors):i+args.neighbors+1]],
        })
    if args.compact:
        print(json.dumps({k: result[k] for k in ['path', 'size', 'sha256']}, ensure_ascii=False))
        for hit in result['hits']:
            print(json.dumps(hit, ensure_ascii=False))
    else:
        print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
