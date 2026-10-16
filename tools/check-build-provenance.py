#!/usr/bin/env python3
"""Check an already-built QVO result against its cache and real compile command.

Does not configure, compile, run a process, import QVO implementation helpers, or measure time.
Usage (already-built output):
  python3 tools/check-build-provenance.py --result result.json \
      --compile-commands build/compile_commands.json --config Debug
CMakeCache.txt is read beside compile_commands unless --cache names another path.

Windows command-line validation runs natively on Windows. The common flags are checked as
actual command arguments, never by substring search through
old QVO_CXX_FLAGS macro definitions (which themselves contain the falsely reported flags).
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import sys


def arguments(entry, windows):
    if isinstance(entry.get("arguments"), list):
        argv = entry["arguments"]
        if not argv or not all(isinstance(arg, str) for arg in argv) or not argv[0].strip():
            raise ValueError("compiler arguments must be a nonempty string array")
        return argv
    command = entry["command"]
    if not isinstance(command, str) or not command.strip():
        raise ValueError("compiler command must be a nonempty string")
    if not windows:
        return shlex.split(command)
    if os.name != "nt":
        raise ValueError("Windows command-line validation requires native Windows")
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    parse = shell.CommandLineToArgvW
    parse.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    parse.restype = ctypes.POINTER(ctypes.c_wchar_p)
    count = ctypes.c_int()
    ptr = parse(command, ctypes.byref(count))
    if not ptr:
        raise OSError("CommandLineToArgvW failed")
    try:
        return [ptr[i] for i in range(count.value)]
    finally:
        free = ctypes.WinDLL("kernel32", use_last_error=True).LocalFree
        free.argtypes = [ctypes.c_void_p]
        free.restype = ctypes.c_void_p
        free(ptr)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--result", required=True, type=Path)
    ap.add_argument("--compile-commands", required=True, type=Path)
    ap.add_argument("--config", required=True)
    ap.add_argument("--cache", type=Path, help="defaults beside compile_commands.json")
    ap.add_argument("--source-suffix", default="harness/src/harness.cpp")
    args = ap.parse_args()
    try:
        result = json.loads(args.result.read_text(encoding="utf-8"))
        env = result["env"]
        if not isinstance(env, dict):
            raise ValueError("result env must be an object")
        for field in ("os", "compiler", "build_type", "cxx_flags"):
            if not isinstance(env.get(field), str):
                raise ValueError(f"env.{field} must be a string")
        cache = {}
        cache_path = args.cache or args.compile_commands.with_name("CMakeCache.txt")
        for line in cache_path.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith(("#", "//")) or "=" not in line:
                continue
            name_type, value = line.split("=", 1)
            cache[name_type.split(":", 1)[0]] = value
        expected_flags = (cache["CMAKE_CXX_FLAGS"] + " "
                          + cache["CMAKE_CXX_FLAGS_" + args.config.upper()]).strip()
        windows = "windows" in str(env.get("os", "")).lower() or env.get("compiler") == "MSVC"
        selected = []
        commands = json.loads(args.compile_commands.read_text(encoding="utf-8"))
        if not isinstance(commands, list):
            raise ValueError("compile commands must be an array")
        for entry in commands:
            if not isinstance(entry, dict) or not isinstance(entry.get("file"), str):
                raise ValueError("each compile command must name a source file")
            if not entry["file"].replace("\\", "/").endswith(args.source_suffix):
                continue
            argv = arguments(entry, windows)
            configurations = [a.split("=", 1)[1].strip('"') for a in argv
                              if a.startswith(("-DCMAKE_INTDIR=", "/DCMAKE_INTDIR="))]
            if configurations and args.config not in configurations:
                continue
            selected.append((entry, argv))
        if len(selected) != 1:
            raise ValueError(f"expected exactly one {args.config} harness command, found {len(selected)}")
        entry, argv = selected[0]
        expected_argv = arguments({"command": "compiler " + expected_flags}, windows)[1:]
        # Metadata macro payloads are not real optimization/debug flags.
        actual = [a for a in argv if not a.startswith(tuple(
            prefix + name + "=" for prefix in ("-D", "/D") for name in
            ("QVO_COMPILER", "QVO_COMPILER_VERSION", "QVO_CXX_FLAGS", "QVO_BUILD_TYPE")))]
        errors = []
        if env.get("build_type") != args.config:
            errors.append(f"build_type {env.get('build_type')!r} != built config {args.config!r}")
        if env.get("cxx_flags", "").strip() != expected_flags:
            errors.append(f"cxx_flags {env.get('cxx_flags')!r} != common configured flags {expected_flags!r}")
        remaining = iter(actual)
        for flag in expected_argv:
            if not any(arg == flag for arg in remaining):
                errors.append(f"configured flag absent from actual compiler argv in order: {flag!r}")
                break
        if not cache.get("CMAKE_CONFIGURATION_TYPES") and cache.get("CMAKE_BUILD_TYPE") != args.config:
            errors.append("requested config differs from the single-config cache")
        receipt = {"ok": not errors, "result": str(args.result), "config": args.config,
                   "source": entry["file"], "expected_common_flags": expected_flags,
                   "reported_build_type": env.get("build_type"), "reported_flags": env.get("cxx_flags"),
                   "scope": env.get("cxx_flags_scope", "unspecified"), "errors": errors,
                   "processes_launched": 0, "timings_used": False}
        print(json.dumps(receipt, indent=2))
        return 1 if errors else 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"check-build-provenance: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
