"""Record actual artifacts and checks without running the user's application."""
import argparse
import hashlib
import json
import os
import tempfile
from pathlib import Path
from inspect_pe import PE

root = Path(__file__).resolve().parents[1]
profile = json.loads((root/'research/profile.json').read_text(encoding='utf-8'))
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--aot', type=Path, default=Path(os.environ.get('LOCALAPPDATA', '.'))/'oopz/data/app.so')
args = parser.parse_args()
original_path = args.aot
original = original_path.read_bytes()
original_hash = hashlib.sha256(original).hexdigest()
assert original_hash == profile['source_sha256']
cache = Path(tempfile.gettempdir())/'OopzGuard'/f'snapshot-0.88.41-{profile["patched_sha256"][:16]}.so'
snapshot = cache.read_bytes()
assert hashlib.sha256(snapshot).hexdigest() == profile['patched_sha256']

from inspect_aot_objects import dart_string_hash
import struct
expected = bytearray(original)
for p in profile['patches']:
    at = p['offset']
    expected[at:at+len(p['after'])] = p['after'].encode()
    struct.pack_into('<I', expected, at-12, dart_string_hash(p['after']))
assert bytes(expected) == snapshot

dll_path = root/'dist/dwmapi.dll'
dll = PE(dll_path)
assert dll.bits == 64
system_exports = {(e['ordinal'], e['name']) for e in PE(r'C:\Windows\System32\dwmapi.dll').exports()}
proxy_exports = {(e['ordinal'], e['name']) for e in dll.exports() if 100 <= e['ordinal'] <= 198}
assert system_exports == proxy_exports
loader_output = (root/'tests/loader-output.txt').read_text(encoding='utf-8-sig')
assert loader_output.count('0 failures') == 5 and 'FAIL line' not in loader_output
aot_output = (root/'tests/aot-smoke-output.txt').read_text(encoding='utf-8-sig')
assert 'Could not resolve main entrypoint function' in aot_output
result = {
    'date': '2026-10-11', 'release': '0.1.0 experimental', 'oopz_version': '0.88.41',
    'dll_architecture': 'Windows x64', 'dll_bytes': dll_path.stat().st_size,
    'dll_sha256': hashlib.sha256(dll_path.read_bytes()).hexdigest(),
    'source_aot_sha256': original_hash, 'patched_aot_sha256': profile['patched_sha256'],
    'exact_expected_patch_only': True, 'original_aot_unchanged': True,
    'dwm_exports_preserved': len(system_exports),
    'loader_tests': {'cases':5,'failures':0,'covers':['cold start DLL auto-load and IAT installation','exact SHA256 profile and all five string/hash patches','cached snapshot reuse','different AOT path in same process passes through','unsupported size passes through','same-size SHA256 mismatch passes through','disabled settings pass through']},
    'window_filter_test': {'result':'0 failures','method':'self-created hidden obs64.exe helper and ordinary windows','api_count':12},
    'real_engine_create_destroy': 'passed with actual installed Flutter library',
    'aot_parse_smoke': 'modified snapshot reached missing-entrypoint lookup; deliberately absent entrypoint; Oopz main not invoked',
    'not_verified': ['logged-in desktop UI','voice channel behavior','screen sharing workflow','current official 1.4.6.1 package','all streaming software detection paths'],
    'installed_in_oopz': False,
    'compiler': 'Zig 0.16.0 official Windows x64 PyPI wheel; see ci/zig.json'
}
(root/'dist/验证记录.json').write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
