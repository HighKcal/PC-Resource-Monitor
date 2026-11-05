"""Read PE RT_MANIFEST #1 as data; never execute the inspected binaries."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
from pathlib import Path
import xml.etree.ElementTree as ET

k = C.WinDLL("kernel32", use_last_error=True)
k.LoadLibraryExW.argtypes = [W.LPCWSTR, W.HANDLE, W.DWORD]
k.LoadLibraryExW.restype = W.HMODULE
k.FindResourceW.argtypes = [W.HMODULE, C.c_void_p, C.c_void_p]
k.FindResourceW.restype = W.HANDLE
k.SizeofResource.argtypes = [W.HMODULE, W.HANDLE]
k.SizeofResource.restype = W.DWORD
k.LoadResource.argtypes = [W.HMODULE, W.HANDLE]
k.LoadResource.restype = W.HANDLE
k.LockResource.argtypes = [W.HANDLE]
k.LockResource.restype = C.c_void_p
k.FreeLibrary.argtypes = [W.HMODULE]
k.FreeLibrary.restype = W.BOOL


def read_manifest(path):
    # AS_DATAFILE | AS_IMAGE_RESOURCE: no executable entry point or imports run.
    module = k.LoadLibraryExW(str(path), None, 0x00000002 | 0x00000020)
    if not module: raise C.WinError(C.get_last_error())
    try:
        resource = k.FindResourceW(module, C.c_void_p(1), C.c_void_p(24))
        if not resource:
            error = C.get_last_error()
            if error in (1812, 1813, 1814):
                sidecar = Path(str(path) + ".manifest")
                return (sidecar.read_bytes(), "external_sidecar") if sidecar.is_file() else (None, "absent")
            raise C.WinError(error)
        size = k.SizeofResource(module, resource)
        loaded = k.LoadResource(module, resource)
        address = k.LockResource(loaded) if loaded else None
        if not size or not address: raise C.WinError(C.get_last_error())
        return C.string_at(address, size), "embedded_rt_manifest_1"
    finally: k.FreeLibrary(module)


def levels(root):
    return [item.attrib for item in root.iter() if item.tag.rsplit("}", 1)[-1] == "requestedExecutionLevel"]


def canonical(root, omit_level=False):
    attributes = dict(root.attrib)
    if omit_level and root.tag.rsplit("}", 1)[-1] == "requestedExecutionLevel": attributes.pop("level", None)
    children = tuple(canonical(child, omit_level) for child in root)
    # mt.exe may reorder independent assembly dependency/trustInfo sections.
    # Preserve every element/value and nested order; normalize only root sections.
    if root.tag == "{urn:schemas-microsoft-com:asm.v1}assembly": children = tuple(sorted(children, key=repr))
    return (root.tag, tuple(sorted(attributes.items())), (root.text or "").strip(),
            children)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gui", required=True, type=Path)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--console", type=Path, nargs="*", default=[])
    parser.add_argument("--require-sidecar", action="store_true")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    records = []; errors = []; roots = []
    for index, path in enumerate([args.before, args.gui, *args.console]):
        path = path.resolve(strict=True)
        data, source = read_manifest(path); root = ET.fromstring(data) if data else None; roots.append(root)
        role = "before" if index == 0 else "gui" if index == 1 else "console"
        if data: (output / (str(index) + "-" + role + ".manifest")).write_bytes(data)
        expected_level = "requireAdministrator" if role == "gui" else "asInvoker"
        found = levels(root) if root is not None else []
        if role == "gui" and source != "embedded_rt_manifest_1": errors.append("New GUI manifest must be embedded as RT_MANIFEST #1")
        if not (role == "console" and source == "absent") and found != [dict(level=expected_level, uiAccess="false")]:
            errors.append(role + ": unexpected execution level " + repr(found))
        old_source = None
        sidecar_record = None
        if role == "gui":
            sidecar_path = Path(str(path) + ".manifest")
            if sidecar_path.is_file():
                sidecar_data = sidecar_path.read_bytes()
                sidecar_equal = canonical(ET.fromstring(sidecar_data)) == canonical(root)
                sidecar_record = dict(path=str(sidecar_path), sha256=hashlib.sha256(sidecar_data).hexdigest(),
                                      equal_to_embedded=sidecar_equal)
                if not sidecar_equal: errors.append("GUI external sidecar differs from embedded manifest")
            elif args.require_sidecar: errors.append("GUI extracted sidecar is missing")
        if role == "console":
            old_path = args.before.resolve(strict=True).parent / path.name
            if not old_path.is_file(): errors.append("Missing baseline console: " + str(old_path))
            else:
                old_data, old_source = read_manifest(old_path)
                old_root = ET.fromstring(old_data) if old_data else None
                if (old_root is None) != (root is None) or (old_root is not None and canonical(old_root) != canonical(root)):
                    errors.append("Console manifest changed: " + path.name)
        records.append(dict(path=str(path), role=role, exe_sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                            manifest_source=source, baseline_manifest_source=old_source,
                            sidecar=sidecar_record,
                            manifest_sha256=hashlib.sha256(data).hexdigest() if data else None, execution_levels=found,
                            absence_meaning="No requested elevation; OS default launch context. Existing console manifest absence is preserved." if source == "absent" else None))
    before, current = roots[:2]
    if before is None or current is None: raise ValueError("Both GUI versions must have an inspectable manifest")
    if canonical(before, True) != canonical(current, True): errors.append("Manifest differs beyond requestedExecutionLevel.level")
    controls = [item.attrib for item in current.iter() if item.tag.rsplit("}", 1)[-1] == "assemblyIdentity" and item.attrib.get("name") == "Microsoft.Windows.Common-Controls"]
    if len(controls) != 1 or controls[0].get("version") != "6.0.0.0": errors.append("Common-Controls v6 dependency not retained exactly once")
    if any(item.tag.rsplit("}", 1)[-1] in ("dpiAware", "dpiAwareness") for item in current.iter()): errors.append("Unexpected manifest DPI override")
    report = dict(files=records, manifest_equal_except_execution_level=canonical(before, True) == canonical(current, True),
                  comparison_normalization="XML whitespace/attribute order and independent root assembly section order only",
                  common_controls=controls, errors=errors, executed_binaries=False,
                  parser_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (output / "result.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
