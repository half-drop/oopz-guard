"""Generate an exact-version profile and ABI-preserving dwmapi trampolines."""
import argparse
import hashlib
import json
import os
import struct
from pathlib import Path
from inspect_pe import PE
from inspect_aot_objects import dart_string_hash

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--aot', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '.'))/'oopz/data/app.so')
parser.add_argument('--dwm', type=Path, default=Path(os.environ.get('SystemRoot', r'C:\Windows'))/'System32/dwmapi.dll')
args = parser.parse_args()
original = args.aot.read_bytes()
expected = '8384ee1a0a3c39194755034328c34ec9dc1f76842d38144cc78b43c3153cc8aa'
assert hashlib.sha256(original).hexdigest() == expected
changes = [('/uni/advertisement/v1/list', '/uni/advertisemenz/v1/list'),
           ('/advertisement/v2', '/advertisemenz/v2'),
           ('advertisementBannerList', 'advertisemenzBannerList'),
           ('adBanner', 'xxBanner'),
           ('adSection', 'xxSection')]
patched = bytearray(original)
patches = []
for before, after in changes:
    b, a = before.encode(), after.encode()
    assert len(b) == len(a)
    candidates = []
    start = 0
    while True:
        at = original.find(b, start)
        if at < 0:
            break
        if at >= 16:
            tag, length = struct.unpack_from('<QQ', original, at-16)
            if (tag >> 12) & 0xfffff == 94 and length == 2*len(b) and tag >> 32 == dart_string_hash(before):
                candidates.append(at)
        start = at+1
    assert len(candidates) == 1, (before, candidates)
    at = candidates[0]
    tag, length = struct.unpack_from('<QQ', original, at-16)
    assert (tag >> 12) & 0xfffff == 94 and length == 2*len(b)
    assert tag >> 32 == dart_string_hash(before)
    patched[at:at+len(a)] = a
    struct.pack_into('<I', patched, at-12, dart_string_hash(after))
    patches.append({'offset':at,'before':before,'after':after,'new_hash':dart_string_hash(after)})

profile = '#pragma once\n#include <stdint.h>\n'
profile += 'typedef struct { uint32_t offset; const char *before, *after; } ProfilePatch;\n'
profile += f'#define PROFILE_SIZE {len(original)}u\n#define PROFILE_PATCH_COUNT {len(patches)}u\n'
profile += 'static const unsigned char profile_sha256[32] = {' + ','.join('0x'+expected[i:i+2] for i in range(0,64,2)) + '};\n'
patched_hash = hashlib.sha256(patched).hexdigest()
profile += 'static const unsigned char profile_patched_sha256[32] = {' + ','.join('0x'+patched_hash[i:i+2] for i in range(0,64,2)) + '};\n'
profile += f'#define PROFILE_CACHE_FILENAME L"snapshot-0.88.41-{patched_hash[:16]}.so"\n'
profile += 'static const ProfilePatch profile_patches[] = {\n'
for p in patches:
    profile += f'  {{{p["offset"]}u, "{p["before"]}", "{p["after"]}"}},\n'
profile += '};\n'
(root/'src/profile.h').write_text(profile)
(root/'research/profile.json').write_text(json.dumps({'version':'0.88.41','source_sha256':expected,'size':len(original),'patched_sha256':hashlib.sha256(patched).hexdigest(),'patches':patches},indent=2),encoding='utf-8')

exports = PE(args.dwm).exports()
definition = ['LIBRARY dwmapi', 'EXPORTS']
assembly = ['.text', '.extern ResolveDwmExport']
for e in exports:
    n = e['ordinal']
    stub = f'proxy_{n}'
    definition.append(f'  {e["name"] or stub}={stub} @{n}' + (' NONAME' if not e['name'] else ''))
    assembly += [f'.globl {stub}', f'.def {stub}; .scl 2; .type 32; .endef', f'.seh_proc {stub}',f'{stub}:',
        '  subq $136, %rsp', '  .seh_stackalloc 136', '  .seh_endprologue',
        '  movq %rcx, 32(%rsp)', '  movq %rdx, 40(%rsp)', '  movq %r8, 48(%rsp)', '  movq %r9, 56(%rsp)',
        '  movdqu %xmm0, 64(%rsp)', '  movdqu %xmm1, 80(%rsp)', '  movdqu %xmm2, 96(%rsp)', '  movdqu %xmm3, 112(%rsp)',
        f'  movl ${n}, %ecx', '  call ResolveDwmExport',
        '  movq 32(%rsp), %rcx', '  movq 40(%rsp), %rdx', '  movq 48(%rsp), %r8', '  movq 56(%rsp), %r9',
        '  movdqu 64(%rsp), %xmm0', '  movdqu 80(%rsp), %xmm1', '  movdqu 96(%rsp), %xmm2', '  movdqu 112(%rsp), %xmm3',
        '  addq $136, %rsp', '  jmp *(%rax)', '.seh_endproc', '']
(root/'src/dwmapi.def').write_text('\n'.join(definition)+'\n')
(root/'src/dwmapi_stubs.S').write_text('\n'.join(assembly).rstrip()+'\n')
print(json.dumps({'source_sha256':expected,'patched_sha256':hashlib.sha256(patched).hexdigest(),'dwm_exports':len(exports),'patches':patches},indent=2))
