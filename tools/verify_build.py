"""Verify a standalone build without any installed Oopz or system DLL input."""

import json
import re
import sys
from hashlib import sha256
from pathlib import Path

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


def validate_dwm_name_table(header, expected):
    """Check the explicit name/NULL table against every fixed proxy ordinal."""
    require(header_integer(header, "DWM_EXPORT_FIRST_ORDINAL") == 100,
            "DWM name table must start at ordinal 100")
    require(header_integer(header, "DWM_EXPORT_LAST_ORDINAL") == 198,
            "DWM name table must end at ordinal 198")
    require(header_integer(header, "DWM_EXPORT_NAME_COUNT") == 99,
            "DWM name table must declare exactly 99 entries")
    uncommented = re.sub(r"/\*.*?\*/|//[^\n]*", "", header, flags=re.DOTALL)
    table = re.search(
        r"\bstatic\s+const\s+char\s*\*\s*const\s+dwm_export_names"
        r"\s*\[\s*99\s*\]\s*=\s*\{(.*?)\};", uncommented, re.DOTALL)
    require(table is not None, "Fixed 99-entry DWM name table missing")
    entry = re.compile(r'(NULL|"([A-Za-z_]\w*)")\s*,?')
    matches = list(entry.finditer(table[1]))
    require(not entry.sub("", table[1]).strip(), "Unexpected DWM name table syntax")
    names = [None if match[1] == "NULL" else match[2] for match in matches]
    require(len(names) == 99, "DWM name table must contain exactly 99 name/NULL values")
    mapping = {100 + index: name for index, name in enumerate(names)}
    require(mapping == expected, "DWM header name/NULL mapping differs from DEF name/NONAME entries")
    return mapping


def main():
    expected = definition_exports(read_text("src/dwmapi.def"))
    name_table = validate_dwm_name_table(read_text("src/dwmapi_names.h"), expected)
    policy = header_integer(read_text("src/snapshot_patch.h"), "SNAPSHOT_POLICY_VERSION")
    require(policy == 2, "Unexpected adaptive snapshot policy")
    require(not (ROOT / "src/profile.h").exists(), "Obsolete client whitelist must not ship")
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
        "dwm_name_table_mapping_count": len(name_table),
        "dwm_name_table_mapping_matches": True,
        "ordinal_only_dwm_cross_windows_verified": False,
        "dwm_forwarding_compatibility": {
            "named_exports": "Name table matches DEF; runtime forwarding is checked separately by loader tests",
            "ordinal_only_exports": "Fixed ordinal forwarding; meanings and ABI across different Windows versions are unverified",
        },
        "diagnostic_exports": sorted(DIAGNOSTIC_NAMES),
        "pe_forwarded_exports": 0,
        "definition_export_mapping_matches": True,
        "adaptive_snapshot_policy": policy,
        "client_version_whitelist": False,
        "fixed_patch_offsets": False,
        "verification_scope": "Standalone DLL structure, DWM forwarding ABI and adaptive policy; behavior checked by CI tests",
        "not_verified": ["Oopz desktop UI", "voice or screen-sharing behavior",
                         "installed Oopz AOT content", "real Flutter AOT parsing",
                         "ordinal-only DWM export compatibility across different Windows versions"],
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
