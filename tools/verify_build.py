"""Verify a standalone build without any installed Oopz or system DLL input."""

import json
import re
import sys
from hashlib import sha256
from pathlib import Path

from inspect_aot_objects import dart_string_hash
from inspect_pe import PE


ROOT = Path(__file__).resolve().parents[1]
DIAGNOSTIC_NAMES = {"OopzGuardStatus", "OopzGuardVersion"}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def required_path(relative):
    path = ROOT / relative
    require(path.is_file(), f"Required build input missing: {relative}")
    return path


def read_text(relative):
    return required_path(relative).read_text(encoding="utf-8-sig")


def definition_exports(text):
    """The checked-in DEF is the ABI baseline, independent of runner OS."""
    entries = {}
    export_line = re.compile(
        r"(?P<name>[A-Za-z_]\w*)\s*=\s*proxy_(?P<stub>\d+)"
        r"\s+@(?P<ordinal>\d+)(?P<noname>\s+NONAME)?"
    )
    for line in text.splitlines():
        line = line.split(";", 1)[0].strip()
        if not line or line.upper() == "EXPORTS" or line.upper().startswith("LIBRARY "):
            continue
        match = export_line.fullmatch(line)
        require(match is not None, "Unexpected export syntax in src/dwmapi.def")
        ordinal = int(match["ordinal"])
        require(ordinal == int(match["stub"]), "DEF ordinal does not match proxy symbol")
        require(ordinal not in entries, "Duplicate DEF ordinal")
        if match["noname"]:
            require(match["name"] == f"proxy_{ordinal}", "Unexpected NONAME proxy alias")
        entries[ordinal] = None if match["noname"] else match["name"]
    require(set(entries) == set(range(100, 199)), "DEF must contain exactly ordinals 100..198")
    require(len(entries) == 99, "DEF must contain exactly 99 DWM exports")
    named = [name for name in entries.values() if name is not None]
    require(len(named) == len(set(named)), "Duplicate named DEF export")
    return entries


def header_integer(text, name):
    match = re.search(rf"^#define\s+{name}\s+(\d+)[uUlL]*\s*$", text, re.MULTILINE)
    require(match is not None, f"Header macro missing: {name}")
    return int(match[1])


def header_hash(text, name):
    match = re.search(rf"\b{name}\s*\[32\]\s*=\s*\{{([^}}]+)\}}\s*;", text)
    require(match is not None, f"Header SHA256 array missing: {name}")
    tokens = [value.strip() for value in match[1].split(",")]
    require(len(tokens) == 32 and all(re.fullmatch(r"0x[0-9a-fA-F]{2}", token)
                                    for token in tokens), f"Invalid SHA256 array: {name}")
    return bytes(int(token, 16) for token in tokens).hex()


def validate_profile(header, profile):
    require(isinstance(profile, dict), "Profile JSON must be an object")
    for name in ("source_sha256", "patched_sha256"):
        require(isinstance(profile.get(name), str) and
                re.fullmatch(r"[0-9a-f]{64}", profile[name]) is not None,
                f"Invalid profile field: {name}")
    require(isinstance(profile.get("version"), str) and
            re.fullmatch(r"\d+(?:\.\d+){2,3}", profile["version"]) is not None,
            "Invalid profile version")
    require(type(profile.get("size")) is int and profile["size"] > 0,
            "Invalid profile source size")
    require(header_integer(header, "PROFILE_SIZE") == profile["size"],
            "Header/JSON source size mismatch")
    require(header_hash(header, "profile_sha256") == profile["source_sha256"],
            "Header/JSON source SHA256 mismatch")
    require(header_hash(header, "profile_patched_sha256") == profile["patched_sha256"],
            "Header/JSON patched SHA256 mismatch")
    patches = profile.get("patches")
    require(isinstance(patches, list) and len(patches) > 0, "Missing profile patches")
    require(header_integer(header, "PROFILE_PATCH_COUNT") == len(patches),
            "Header/JSON patch count mismatch")
    array = re.search(r"\bprofile_patches\s*\[\]\s*=\s*\{(.*?)\};", header, re.DOTALL)
    require(array is not None, "Header patch table missing")
    entry = re.compile(r'\{\s*(\d+)[uUlL]*\s*,\s*"([^"\\]*)"\s*,\s*"([^"\\]*)"\s*\}\s*,?')
    parsed = [(int(match[1]), match[2], match[3]) for match in entry.finditer(array[1])]
    require(not entry.sub("", array[1]).strip(), "Unexpected header patch table syntax")
    require(len(parsed) == len(patches), "Header patch entry count mismatch")
    offsets = set()
    for index, (header_entry, patch) in enumerate(zip(parsed, patches)):
        require(isinstance(patch, dict), f"Invalid patch object at index {index}")
        require(type(patch.get("offset")) is int, f"Invalid patch offset at index {index}")
        before, after = patch.get("before"), patch.get("after")
        require(isinstance(before, str) and isinstance(after, str) and
                before.isascii() and after.isascii() and
                0 < len(before) == len(after) and before != after,
                f"Patch strings must be different ASCII strings of equal length at index {index}")
        require(header_entry == (patch["offset"], before, after),
                f"Header/JSON patch entry mismatch at index {index}")
        require(16 <= patch["offset"] <= profile["size"] - len(before),
                f"Patch exceeds source bounds at index {index}")
        require(patch["offset"] not in offsets, "Duplicate patch offset")
        offsets.add(patch["offset"])
        require(type(patch.get("new_hash")) is int and
                patch["new_hash"] == dart_string_hash(after),
                f"Incorrect Dart replacement string hash at index {index}")
    cache = re.search(r'^#define\s+PROFILE_CACHE_FILENAME\s+L"([^"\\]+)"\s*$',
                      header, re.MULTILINE)
    expected_cache = f'snapshot-{profile["version"]}-{profile["patched_sha256"][:16]}.so'
    require(cache is not None and cache[1] == expected_cache,
            "Header cache filename does not match profile version/hash")


def main():
    expected = definition_exports(read_text("src/dwmapi.def"))
    profile = json.loads(read_text("research/profile.json"))
    validate_profile(read_text("src/profile.h"), profile)
    dll = PE(required_path("dist/dwmapi.dll"))
    require(dll.machine == 0x8664 and dll.bits == 64, "DLL must be an AMD64 PE32+ binary")
    exports = dll.exports()
    require(len(exports) == 101, "DLL must have exactly 101 exports")
    require(all(export["forwarder"] is None for export in exports),
            "All DLL exports must be implemented locally, without PE forwarders")
    actual = {export["ordinal"]: export["name"] for export in exports
              if 100 <= export["ordinal"] <= 198}
    require(actual == expected, "DLL ordinal/name/NONAME mapping differs from checked-in DEF")
    diagnostics = [export for export in exports if not 100 <= export["ordinal"] <= 198]
    require(len(diagnostics) == 2 and {export["name"] for export in diagnostics} == DIAGNOSTIC_NAMES,
            "Unexpected diagnostic exports or extra export entries")
    result = {
        "result": "passed",
        "dll": "dwmapi.dll",
        "dll_architecture": "Windows AMD64 / PE32+",
        "dll_bytes": len(dll.data),
        "dll_sha256": sha256(dll.data).hexdigest(),
        "total_exports": len(exports),
        "dwm_export_mapping_count": len(actual),
        "named_dwm_exports": sum(name is not None for name in actual.values()),
        "ordinal_only_dwm_exports": sum(name is None for name in actual.values()),
        "diagnostic_exports": sorted(DIAGNOSTIC_NAMES),
        "pe_forwarded_exports": 0,
        "definition_export_mapping_matches": True,
        "profile_header_json_matches": True,
        "profile_version": profile["version"],
        "supported_source_aot_bytes": profile["size"],
        "supported_source_aot_sha256": profile["source_sha256"],
        "expected_patched_aot_sha256": profile["patched_sha256"],
        "profile_patch_count": len(profile["patches"]),
        "verification_scope": "Standalone build structure and checked-in profile consistency",
        "not_verified": ["Oopz desktop UI", "voice or screen-sharing behavior",
                         "installed Oopz AOT content", "real Flutter AOT parsing"],
    }
    output = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    (ROOT / "dist/build-verification.json").write_text(output, encoding="utf-8")
    print(output, end="")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError) as error:
        print(f"Build verification failed: {error}", file=sys.stderr)
        sys.exit(1)
    except OSError as error:
        # Error.filename may contain a local checkout path; omit it from logs.
        print(f"Build verification failed: filesystem error (errno={error.errno})", file=sys.stderr)
        sys.exit(1)
    except Exception as error:
        print(f"Build verification failed: {type(error).__name__}", file=sys.stderr)
        sys.exit(1)
