#!/usr/bin/env python3
"""Build the launcher package: one zip a player extracts and runs.

  HarmonyLauncher.exe
  HarmonyRecomp/          this repository's tracked files (git ls-files)
  rexglue-sdk/            the prebuilt SDK: out/install/<platform> only

With the prebuilt SDK the player's first build skips cloning and compiling
the SDK (the longest step) and goes straight to codegen and the game build.
The package contains no game files and no code derived from the game: only
this repository, the launcher and the SDK (its own code and licenses).

Prerequisites:
  dotnet publish launcher -c Release             # the launcher exe
  python scripts/build.py --toolchain portable   # an SDK built with the portable tools

Usage:
  python scripts/make_launcher_package.py                    # -> ../HarmonyLauncher.zip
  python scripts/make_launcher_package.py --out D:/x.zip
  python scripts/make_launcher_package.py --no-sdk           # players build the SDK themselves
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import build  # noqa: E402  (scripts/ is not a package)

ROOT = build.ROOT
DEFAULT_EXE = ROOT / "launcher" / "bin" / "Release" / "net10.0" / "win-x64" / "publish" / "HarmonyLauncher.exe"


def tracked_files() -> list[str]:
    out = subprocess.run(build.git("ls-files", "--cached", "-z"), cwd=ROOT,
                         capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE, help="published HarmonyLauncher.exe")
    parser.add_argument("--out", type=Path, default=ROOT.parent / "HarmonyLauncher.zip")
    parser.add_argument("--no-sdk", action="store_true", help="leave the prebuilt SDK out")
    parser.add_argument("--sdk-dir", type=Path, default=build.SDK_DIR,
                        help=f"SDK folder to take the build from (default: {build.SDK_DIR})")
    args = parser.parse_args()
    build.SDK_DIR = args.sdk_dir.resolve()
    build.PATCH_STAMP = build.SDK_DIR / build.PATCH_STAMP.name

    if not args.exe.exists():
        sys.exit(f"error: {args.exe} not found; run: dotnet publish launcher -c Release")
    pid = build.platform_id()
    install = build.sdk_install_dir(pid)
    if not args.no_sdk:
        # The launcher builds with the portable tools, so the SDK must have been
        # built with them too (CMake and the C++ runtime must match).
        build.TOOLCHAIN = "portable"
        stamp = build.PATCH_STAMP.read_text().strip() if build.PATCH_STAMP.exists() else ""
        if stamp != f"{build.sdk_patch_hash()} portable" or build.find_rexglue(pid) is None:
            sys.exit(f"error: no SDK built with the portable tools and the current SDK changes in {build.SDK_DIR}\n"
                     "  run: python scripts/build.py --toolchain portable")

    status = subprocess.run(build.git("status", "--porcelain", "--untracked-files=no"), cwd=ROOT,
                            capture_output=True, text=True).stdout
    if status.strip():
        print("warning: uncommitted changes in tracked files are packaged as they are on disk")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with zipfile.ZipFile(args.out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.write(args.exe, "HarmonyLauncher.exe")
        for rel in tracked_files():
            path = ROOT / rel
            if path.is_file():
                z.write(path, f"HarmonyRecomp/{rel}")
                count += 1
        if not args.no_sdk:
            for path in sorted(install.rglob("*")):
                if path.is_file():
                    z.write(path, f"rexglue-sdk/{path.relative_to(build.SDK_DIR).as_posix()}")
            z.writestr("rexglue-sdk/.harmonyrecomp-prebuilt", build.prebuilt_stamp_text() + "\n")
    print(f"wrote {args.out} ({args.out.stat().st_size / 1e6:.1f} MB): launcher, {count} source files"
          + ("" if args.no_sdk else ", prebuilt SDK"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
