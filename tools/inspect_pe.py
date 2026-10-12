"""Read-only PE import/export and string inspection; no external dependencies."""
import argparse
import json
import re
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3c)[0]
        if d[pe:pe+4] != b'PE\0\0':
            raise ValueError('Not PE')
        self.machine, nsections = struct.unpack_from('<HH', d, pe+4)
        optional_size = struct.unpack_from('<H', d, pe+20)[0]
        opt = pe+24
        magic = struct.unpack_from('<H', d, opt)[0]
        self.bits = 64 if magic == 0x20b else 32
        self.image_base = struct.unpack_from('<Q' if self.bits == 64 else '<I', d, opt+(24 if self.bits == 64 else 28))[0]
        self.dirs = [struct.unpack_from('<II', d, opt+(112 if self.bits == 64 else 96)+i*8) for i in range(16)]
        self.sections = []
        for i in range(nsections):
            s = opt+optional_size+i*40
            vs, va, rawsize, raw = struct.unpack_from('<IIII', d, s+8)
            self.sections.append((d[s:s+8].rstrip(b'\0').decode('ascii', 'replace'), va, vs, raw, rawsize))

    def offset(self, rva):
        for _, va, vs, raw, rawsize in self.sections:
            if va <= rva < va + max(vs, rawsize):
                return raw+rva-va
        return rva

    def string(self, rva):
        p = self.offset(rva)
        end = self.data.find(b'\0', p)
        return self.data[p:end].decode('ascii', 'replace')

    def imports(self):
        result = {}
        rva, size = self.dirs[1]
        if not rva:
            return result
        at = self.offset(rva)
        for _ in range(size//20+1):
            original, _, _, name, first = struct.unpack_from('<IIIII', self.data, at)
            if not name:
                break
            names = []
            thunk = self.offset(original or first)
            width = self.bits//8
            while True:
                value = struct.unpack_from('<Q' if width == 8 else '<I', self.data, thunk)[0]
                if not value:
                    break
                names.append('#'+str(value & 0xffff) if value & (1 << (self.bits-1)) else self.string(value+2))
                thunk += width
            result[self.string(name)] = names
            at += 20
        return result

    def exports(self):
        rva, size = self.dirs[0]
        if not rva:
            return []
        at = self.offset(rva)
        base, nfunc, nnames, functions, names, ordinals = struct.unpack_from('<IIIIII', self.data, at+16)
        named = {}
        for i in range(nnames):
            name = struct.unpack_from('<I', self.data, self.offset(names)+i*4)[0]
            ordinal = struct.unpack_from('<H', self.data, self.offset(ordinals)+i*2)[0]
            named[ordinal] = self.string(name)
        result = []
        for i in range(nfunc):
            frva = struct.unpack_from('<I', self.data, self.offset(functions)+i*4)[0]
            if frva:
                result.append({'name':named.get(i), 'ordinal':base+i, 'rva':hex(frva), 'forwarder':self.string(frva) if rva <= frva < rva+size else None})
        return result


def relevant_strings(path, pattern, limit):
    data = Path(path).read_bytes()
    found = []
    for enc, expression in [('ascii', rb'[\x20-\x7e]{5,}'), ('utf-16le', rb'(?:[\x20-\x7e]\x00){5,}')]:
        for m in re.finditer(expression, data):
            s = m.group().decode(enc)
            if re.search(pattern, s, re.I):
                found.append({'offset':hex(m.start()), 'encoding':enc, 'value':s[:500]})
                if len(found) >= limit:
                    return found
    return found


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('paths', nargs='+')
    p.add_argument('--strings')
    p.add_argument('--limit', type=int, default=150)
    a = p.parse_args()
    for path in a.paths:
        pe = PE(path)
        result = {'path':path, 'bits':pe.bits, 'base':hex(pe.image_base), 'sections':pe.sections, 'imports':pe.imports(), 'exports':pe.exports()}
        if a.strings:
            result['strings'] = relevant_strings(path, a.strings, a.limit)
        print(json.dumps(result, ensure_ascii=False, indent=2))
