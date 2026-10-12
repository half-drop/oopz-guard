"""Read-only ELF64 + observed Dart image string object inspector."""
import argparse
from collections import Counter
import hashlib
import json
import re
import struct
from pathlib import Path


def dart_string_hash(s):
    encoded = s.encode('utf-16le', errors='surrogatepass')
    units = struct.unpack('<' + 'H'*(len(encoded)//2), encoded)
    value = 0
    for unit in units:
        value = (value+unit) & 0xffffffff
        value = (value+(value << 10)) & 0xffffffff
        value ^= value >> 6
    value = (value+(value << 3)) & 0xffffffff
    value ^= value >> 11
    value = (value+(value << 15)) & 0xffffffff
    value &= 0x3fffffff
    return value or 1


def main():
    p = argparse.ArgumentParser()
    p.add_argument('path')
    p.add_argument('--pattern', default=r'OBS|obs-studio|yyobs|obsWatcher|isObsRunning|obsBroadcaster|obsBroadcasting|^/.*advert')
    p.add_argument('--neighbors', type=int, default=0)
    p.add_argument('--verify-hashes', action='store_true')
    p.add_argument('--output')
    a = p.parse_args()
    d = Path(a.path).read_bytes()
    shoff = struct.unpack_from('<Q', d, 40)[0]
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 58)
    sections = []
    raw_sections = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i*shentsize) for i in range(shnum)]
    names = raw_sections[shstrndx]
    name_data = d[names[4]:names[4]+names[5]]
    for fields in raw_sections:
        nameoff = fields[0]
        name = name_data[nameoff:name_data.find(b'\0', nameoff)].decode()
        sections.append({'name': name, 'type': fields[1], 'va': fields[3], 'file_offset': fields[4], 'size': fields[5]})
    print(json.dumps({'size': len(d), 'sha256': hashlib.sha256(d).hexdigest(), 'sections': sections}, ensure_ascii=False))
    objects = []
    for at in range(0, len(d)-16, 16):
        tags, raw_length = struct.unpack_from('<QQ', d, at)
        cid = (tags >> 12) & 0xfffff
        if cid not in (94, 95) or raw_length & 1 or not (0 < raw_length <= 200000):
            continue
        length = raw_length // 2
        end = at + 16 + length*(1 if cid==94 else 2)
        if end > len(d):
            continue
        try:
            s = d[at+16:end].decode('latin1' if cid==94 else 'utf-16le')
        except UnicodeError:
            continue
        if '\0' in s:
            continue
        objects.append({'header': hex(at), 'payload': hex(at+16), 'cid': cid, 'length': length, 'hash': hex(tags>>32), 'value':s})
    print(json.dumps({'recognized': len(objects), 'cids': dict(Counter(x['cid'] for x in objects))}))
    if a.verify_hashes:
        good = sum(dart_string_hash(x['value']) == int(x['hash'],16) for x in objects)
        bad = [{**x, 'computed_hash': hex(dart_string_hash(x['value']))} for x in objects if dart_string_hash(x['value']) != int(x['hash'],16)]
        print(json.dumps({'hash_matches': good, 'hash_mismatches': len(bad), 'mismatch_examples': bad[:10]}))
    regex = re.compile(a.pattern)
    hits = []
    for i, ob in enumerate(objects):
        if regex.search(ob['value']):
            hit = {'object': ob, 'neighbors': objects[max(0,i-a.neighbors):i+a.neighbors+1]}
            hits.append(hit)
            print(json.dumps(hit))
    if a.output:
        Path(a.output).write_text(json.dumps(hits, ensure_ascii=False, indent=2), encoding='utf-8')


if __name__ == '__main__':
    main()
