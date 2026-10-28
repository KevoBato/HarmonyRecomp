#!/usr/bin/env python3
"""Portable Windows build tools for HarmonyRecomp.

Downloads everything build.py needs into one folder, without installing
anything system-wide and without Visual Studio:

  LLVM/Clang, CMake, Ninja, MinGit, 7-Zip (to unpack LLVM's installer),
  the Microsoft C/C++ runtime headers and libraries (two .vsix packages from
  Microsoft's Visual Studio CDN) and the Windows SDK (two NuGet packages from
  nuget.org).

Every file is pinned in toolchain.lock.json with its size and digest and is
checked before it is unpacked. The Microsoft packages are downloaded from
Microsoft on your own PC; this repository never redistributes them.

The tools go into ../harmonyrecomp-tools, next to this folder (like the SDK,
they must not live inside the project: codegen scans the project's files).
Set HARMONYRECOMP_TOOLS to use another folder.

Usage:
  python scripts/toolchain.py fetch     # download and unpack what is missing
  python scripts/toolchain.py status    # show what is installed
  python scripts/build.py               # uses the tools automatically once fetched

Only the Python standard library is used, so the embeddable Python that the
launcher downloads can run this script.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LOCK_FILE = ROOT / "toolchain.lock.json"
STAMP_NAME = ".harmonyrecomp-tool"


def tools_dir() -> Path:
    env = os.environ.get("HARMONYRECOMP_TOOLS")
    return Path(env).resolve() if env else ROOT.parent / "harmonyrecomp-tools"


def load_lock() -> list[dict]:
    return json.loads(LOCK_FILE.read_text(encoding="utf-8"))["tools"]


def digest_of(entry: dict) -> tuple[str, str]:
    for algorithm in ("sha256", "sha512"):
        if algorithm in entry:
            return algorithm, entry[algorithm].lower()
    sys.exit(f"error: {entry['name']} has no sha256/sha512 in {LOCK_FILE.name}")


def stamp_path(tools: Path, entry: dict) -> Path:
    # One stamp per download: several packages unpack into the same folder (msvc/).
    return tools / entry["dest"] / f"{STAMP_NAME}-{entry['name']}"


def installed(tools: Path, entry: dict) -> bool:
    stamp = stamp_path(tools, entry)
    return ((tools / entry["probe"]).exists() and stamp.exists()
            and stamp.read_text().strip() == digest_of(entry)[1])


def ready(tools: Path | None = None) -> bool:
    """True when every build tool (everything except the launcher's Python) is in place."""
    tools = tools or tools_dir()
    return all(installed(tools, e) for e in load_lock() if not e.get("launcher"))


def file_digest(path: Path, algorithm: str) -> str:
    h = hashlib.new(algorithm)
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(entry: dict, target: Path) -> None:
    """Download to target, reusing a complete, verified earlier download."""
    algorithm, expected = digest_of(entry)
    if target.exists() and target.stat().st_size == entry["size"]:
        if file_digest(target, algorithm) == expected:
            print(f"[tools] {entry['name']}: using the earlier download", flush=True)
            return
    partial = target.with_name(target.name + ".partial")
    request = urllib.request.Request(entry["url"], headers={"User-Agent": "HarmonyRecomp-toolchain"})
    print(f"[tools] downloading {entry['name']} ({entry['size'] / 1e6:.0f} MB)", flush=True)
    h = hashlib.new(algorithm)
    done, last_report = 0, 0.0
    with urllib.request.urlopen(request, timeout=60) as response, partial.open("wb") as out:
        while block := response.read(1 << 20):
            out.write(block)
            h.update(block)
            done += len(block)
            now = time.monotonic()
            if now - last_report > 1 or done == entry["size"]:
                last_report = now
                # The launcher reads these lines for its progress bar.
                print(f"[tools] progress {entry['name']} {done} {entry['size']}", flush=True)
    if done != entry["size"] or h.hexdigest() != expected:
        partial.unlink(missing_ok=True)
        sys.exit(f"error: {entry['name']} failed its size/{algorithm} check "
                 f"(got {done} bytes); try again")
    partial.replace(target)


def extract_zip(archive: Path, dest: Path, prefix: str) -> None:
    with zipfile.ZipFile(archive) as z:
        for member in z.infolist():
            name = member.filename
            if prefix:
                if not name.startswith(prefix):
                    continue
                name = name[len(prefix):]
            if not name or member.is_dir():
                continue
            # NuGet and VSIX packages escape spaces and other characters in member names.
            name = urllib.parse.unquote(name)
            out = (dest / name).resolve()
            if not out.is_relative_to(dest.resolve()):
                sys.exit(f"error: {archive.name} contains an unsafe path: {member.filename}")
            out.parent.mkdir(parents=True, exist_ok=True)
            with z.open(member) as src, out.open("wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)


def install(tools: Path, entry: dict) -> None:
    downloads = tools / "downloads"
    downloads.mkdir(parents=True, exist_ok=True)
    archive = downloads / entry["name"]
    download(entry, archive)
    dest = tools / entry["dest"]
    dest.mkdir(parents=True, exist_ok=True)
    print(f"[tools] unpacking {entry['name']}", flush=True)
    kind = entry["kind"]
    if kind == "file":
        shutil.copy2(archive, dest / entry["name"])
    elif kind == "zip":
        extract_zip(archive, dest, entry.get("prefix", ""))
    elif kind == "7z":
        # 7-Zip's own installer and LLVM's NSIS installer are both plain archives to 7-Zip.
        result = subprocess.run([str(tools / entry["extractor"]), "x", "-y", f"-o{dest}", str(archive)],
                                stdout=subprocess.DEVNULL)
        if result.returncode != 0:
            sys.exit(f"error: unpacking {entry['name']} failed (exit code {result.returncode})")
    else:
        sys.exit(f"error: unknown kind {kind!r} for {entry['name']}")
    if not (tools / entry["probe"]).exists():
        sys.exit(f"error: {entry['name']} unpacked, but {entry['probe']} is missing")
    stamp_path(tools, entry).write_text(digest_of(entry)[1] + "\n")


def fetch(with_python: bool) -> Path:
    if sys.platform != "win32":
        sys.exit("error: the portable toolchain is for Windows; install clang, cmake, ninja and git instead")
    tools = tools_dir()
    entries = [e for e in load_lock() if with_python or not e.get("launcher")]
    todo = [e for e in entries if not installed(tools, e)]
    print(f"[tools] folder: {tools}", flush=True)
    for i, entry in enumerate(todo, 1):
        print(f"[tools] step {i}/{len(todo)}: {entry['name']}", flush=True)
        install(tools, entry)
    print("[tools] all build tools are ready", flush=True)
    return tools


def _single_subdir(parent: Path) -> Path:
    dirs = sorted(p for p in parent.iterdir() if p.is_dir())
    if not dirs:
        sys.exit(f"error: nothing in {parent}; run python scripts/toolchain.py fetch")
    return dirs[-1]


def environment(tools: Path | None = None) -> dict[str, str]:
    """Environment variables that make clang, lld-link, CMake and Ninja use the
    portable tools instead of an installed Visual Studio.

    Clang reads VCToolsInstallDir before it looks for Visual Studio, and both
    clang and lld-link honour INCLUDE and LIB like a Developer Command Prompt.
    """
    tools = tools or tools_dir()
    msvc = _single_subdir(tools / "msvc" / "VC" / "Tools" / "MSVC")
    winsdk = tools / "winsdk"
    sdk_version = _single_subdir(winsdk / "Include").name
    sdk_inc = winsdk / "Include" / sdk_version
    sdk_lib = tools / "winsdk-x64"
    path = [tools / "llvm" / "bin", tools / "cmake" / "bin", tools / "ninja",
            tools / "git" / "cmd", winsdk / "bin" / sdk_version / "x64"]
    return {
        "PATH": os.pathsep.join(str(p) for p in path) + os.pathsep + os.environ.get("PATH", ""),
        "INCLUDE": os.pathsep.join(str(p) for p in (
            msvc / "include", sdk_inc / "ucrt", sdk_inc / "shared", sdk_inc / "um",
            sdk_inc / "winrt", sdk_inc / "cppwinrt")),
        "LIB": os.pathsep.join(str(p) for p in (
            msvc / "lib" / "x64", sdk_lib / "um" / "x64", sdk_lib / "ucrt" / "x64")),
        "VCToolsInstallDir": str(msvc) + os.sep,
        "WindowsSdkDir": str(winsdk) + os.sep,
        "WindowsSDKVersion": sdk_version + os.sep,
    }


def status() -> int:
    tools = tools_dir()
    print(f"folder: {tools}")
    missing = 0
    for entry in load_lock():
        ok = installed(tools, entry)
        missing += not ok and not entry.get("launcher")
        print(f"  {'ok     ' if ok else 'missing'}  {entry['name']}")
    return 1 if missing else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p_fetch = sub.add_parser("fetch", help="download and unpack the missing tools")
    p_fetch.add_argument("--with-python", action="store_true",
                         help="also fetch the embeddable Python the launcher uses")
    sub.add_parser("status", help="show which tools are installed")
    args = parser.parse_args()
    if args.command == "fetch":
        fetch(args.with_python)
        return 0
    return status()


if __name__ == "__main__":
    sys.exit(main())
