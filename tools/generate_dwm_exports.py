"""Generate DWM name forwarding and trampolines from the checked-in DEF ABI."""
import json
import re
from pathlib import Path


def read_dwm_exports(definition_path):
    """Keep the checked-in 100..198 ABI independent of host Windows ordinals."""
    exports = []
    pattern = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=proxy_(\d+)\s+@(\d+)(?:\s+(NONAME))?')
    for line_number, line in enumerate(definition_path.read_text(encoding='utf-8').splitlines(), 1):
        line = line.split(';', 1)[0].strip()
        if not line or line in ('LIBRARY dwmapi', 'EXPORTS'):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError(f'Invalid fixed DWM DEF entry on line {line_number}')
        symbol, stub_ordinal, ordinal, noname = match.groups()
        ordinal = int(ordinal)
        if int(stub_ordinal) != ordinal or (noname and symbol != f'proxy_{ordinal}'):
            raise ValueError(f'DWM DEF stub mismatch on line {line_number}')
        exports.append({'ordinal': ordinal, 'name': None if noname else symbol})
    exports.sort(key=lambda export: export['ordinal'])
    if [export['ordinal'] for export in exports] != list(range(100, 199)):
        raise ValueError('The fixed DWM DEF must contain exactly ordinals 100..198')
    names = [export['name'] for export in exports if export['name'] is not None]
    if len(names) != len(set(names)):
        raise ValueError('The fixed DWM DEF contains duplicate named exports')
    return exports


def make_dwm_names_header(exports):
    lines = [
        '/* Generated from src/dwmapi.def by tools/generate_dwm_exports.py. */',
        '#pragma once', '#include <stddef.h>',
        '#define DWM_EXPORT_FIRST_ORDINAL 100u',
        '#define DWM_EXPORT_LAST_ORDINAL 198u',
        '#define DWM_EXPORT_NAME_COUNT 99u',
        'static const char *const dwm_export_names[99] = {',
    ]
    for export in exports:
        name = json.dumps(export['name']) if export['name'] is not None else 'NULL'
        lines.append(f'    {name}, /* {export["ordinal"]} */')
    lines.extend(['};', ''])
    return '\n'.join(lines)


root = Path(__file__).resolve().parents[1]
exports = read_dwm_exports(root/'src/dwmapi.def')
(root/'src/dwmapi_names.h').write_bytes(make_dwm_names_header(exports).encode('utf-8'))
assembly = ['.text', '.extern ResolveDwmExport']
for e in exports:
    n = e['ordinal']
    stub = f'proxy_{n}'
    assembly += [f'.globl {stub}', f'.def {stub}; .scl 2; .type 32; .endef', f'.seh_proc {stub}',f'{stub}:',
        '  subq $136, %rsp', '  .seh_stackalloc 136', '  .seh_endprologue',
        '  movq %rcx, 32(%rsp)', '  movq %rdx, 40(%rsp)', '  movq %r8, 48(%rsp)', '  movq %r9, 56(%rsp)',
        '  movdqu %xmm0, 64(%rsp)', '  movdqu %xmm1, 80(%rsp)', '  movdqu %xmm2, 96(%rsp)', '  movdqu %xmm3, 112(%rsp)',
        f'  movl ${n}, %ecx', '  call ResolveDwmExport',
        '  movq 32(%rsp), %rcx', '  movq 40(%rsp), %rdx', '  movq 48(%rsp), %r8', '  movq 56(%rsp), %r9',
        '  movdqu 64(%rsp), %xmm0', '  movdqu 80(%rsp), %xmm1', '  movdqu 96(%rsp), %xmm2', '  movdqu 112(%rsp), %xmm3',
        '  addq $136, %rsp', '  jmp *(%rax)', '.seh_endproc', '']
(root/'src/dwmapi_stubs.S').write_bytes(('\n'.join(assembly).rstrip()+'\n').encode('utf-8'))
print(f'Generated {len(exports)} DWM forwarding exports; no Oopz input required.')
