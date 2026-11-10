"""Finish an SDK installed from the fork: add PDBs and record what was built.

Usage: python tools/stage_renderer_sdk.py SDK_SOURCE OUTPUT
       [--configuration Release|Debug] [--codegen-stamp PATH]
OUTPUT is the prefix passed to cmake --install. The native application must be
rebuilt against it. Game dumps and saves are never packaged.
"""
import argparse
import hashlib
import json
import platform
from pathlib import Path
import shutil
import subprocess
import sys


def sdk_platform():
    """Return the SDK output-tree name for the host (REX_PLATFORM naming)."""
    if sys.platform == "win32":
        return "win-amd64"
    if sys.platform == "darwin":
        return "mac-arm64" if platform.machine().lower() in ("aarch64", "arm64") else "mac-amd64"
    arch = "arm64" if platform.machine().lower() in ("aarch64", "arm64") else "amd64"
    return f"linux-{arch}"


def artifact_names(configuration):
    """Install artifact names for the host platform (also used by the tests)."""
    suffix = "d" if configuration == "Debug" else ""
    if sys.platform == "win32":
        return {
            "stem": (f"rexruntime{suffix}", f"rexgpu-xenos{suffix}"),
            "runtime": f"rexruntime{suffix}.dll",
            "plugin": f"rexgpu-xenos{suffix}.dll",
            "runtime_lib": f"rexruntime{suffix}.lib",
            "plugin_lib": f"rexgpu-xenos{suffix}.lib",
            "tool": "rexglue.exe",
        }
    ext = "dylib" if sys.platform == "darwin" else "so"
    return {
        "stem": (f"librexruntime{suffix}", f"librexgpu-xenos{suffix}"),
        "runtime": f"librexruntime{suffix}.{ext}",
        "plugin": f"librexgpu-xenos{suffix}.{ext}",
        "runtime_lib": None,
        "plugin_lib": None,
        "tool": "rexglue",
    }


def lib_dirs(output):
    """Candidate install lib dirs (GNUInstallDirs picks lib or lib64)."""
    return [output / "lib64", output / "lib"]


def find_one(output, name, kind):
    """Locate an installed artifact, returning its path or None."""
    if kind == "bin":
        candidates = [output / "bin" / name]
    elif kind == "lib":
        candidates = [d / name for d in lib_dirs(output)]
    elif kind == "include":
        candidates = [output / "include" / name]
    else:  # cmake package
        candidates = [d / "cmake/rexglue" / name for d in lib_dirs(output)]
    for path in candidates:
        if path.is_file():
            return path
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--configuration", choices=("Release", "Debug"), default="Release")
    parser.add_argument("--codegen-stamp", type=Path)
    args = parser.parse_args()
    source, output = args.source.resolve(), args.output.resolve()
    native = source / "out" / sdk_platform()
    names = artifact_names(args.configuration)
    runtime, plugin, tool = names["runtime"], names["plugin"], names["tool"]
    runtime_lib, plugin_lib = names["runtime_lib"], names["plugin_lib"]
    required = [find_one(output, runtime, "lib"), find_one(output, plugin, "lib"),
                find_one(output, "rex/version.h", "include"),
                find_one(output, "rexglueConfig.cmake", "cmake")]
    if sys.platform == "win32":
        required += [find_one(output, runtime_lib, "lib"), find_one(output, plugin_lib, "lib")]
    # Only Release installs the codegen tool.
    if args.configuration == "Release":
        required.append(find_one(output, tool, "bin"))
    for path in required:
        if path is None or not path.is_file():
            raise SystemExit(f"Missing build input: {path or 'required artifact not found under ' + str(output)}")
    runtime_path, plugin_path = required[0], required[1]
    # Carry the PDBs (when present) so the host build can stage them next to
    # the exe for debugger/profiler symbolization (PE builds only; ELF/Mach-O
    # builds carry their debug info differently).
    if sys.platform == "win32":
        for name in (f"{names['stem'][0]}.pdb", f"{names['stem'][1]}.pdb"):
            if (native / name).is_file():
                shutil.copy2(native / name, output / "bin" / name)
    revision = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    provenance = {
        "configuration": args.configuration,
        "sdk_commit": revision,
        "note": "Native headers, integration sources and DLLs must stay matched.",
        "runtime_sha256": hashlib.sha256(runtime_path.read_bytes()).hexdigest(),
        "plugin_sha256": hashlib.sha256(plugin_path.read_bytes()).hexdigest(),
    }
    provenance_path = output / "renderer-sdk-provenance.json"
    if args.configuration == "Release":
        tool_hash = hashlib.sha256((output / "bin" / tool).read_bytes()).hexdigest()
        provenance["codegen_tool_sha256"] = tool_hash
        # The tool's up-to-date check doesn't cover its own code, so a new tool
        # has to clear it or the old generated code would be kept.
        previous = json.loads(provenance_path.read_text()) if provenance_path.is_file() else {}
        if args.codegen_stamp and previous.get("codegen_tool_sha256") != tool_hash:
            args.codegen_stamp.unlink(missing_ok=True)
    provenance_path.write_text(json.dumps(provenance, indent=2), encoding="utf-8")
    print(f"Matched native SDK staged at {output}")


if __name__ == "__main__":
    main()
