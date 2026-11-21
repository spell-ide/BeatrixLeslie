#!/usr/bin/env python3
"""Generate Resources/THIRD_PARTY_LICENSES.txt for BeatrixLeslie IDE from Cargo metadata.

Resolves normal/build dependencies of `rust/spell_core` using standalone Cargo
metadata (no vendored crate directory or local inference engine required).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CRATE_DIR = ROOT / "rust" / "spell_core"
OUTPUT = ROOT / "Resources" / "THIRD_PARTY_LICENSES.txt"
NOTICES = ROOT / "THIRD_PARTY_NOTICES.md"
FIRST_PARTY = {"spell_core"}
LICENSE_PREFIXES = ("LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE", "UNLICENSE")


def shipped_packages() -> list[dict]:
    raw = subprocess.run(
        ["cargo", "metadata", "--format-version", "1"],
        cwd=CRATE_DIR,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    meta = json.loads(raw)
    pkgs = []
    seen = set()
    for pkg in meta.get("packages", []):
        name = pkg.get("name", "")
        version = pkg.get("version", "")
        if not name or name in FIRST_PARTY:
            continue
        key = (name, version)
        if key in seen:
            continue
        seen.add(key)
        pkgs.append(pkg)
    pkgs.sort(key=lambda p: (p["name"], p["version"]))
    return pkgs


def render() -> str:
    pkgs = shipped_packages()
    header = "BeatrixLeslie IDE (Standalone) — Third-Party Licenses"
    out: list[str] = [
        header,
        "=" * len(header),
        "",
        "This file lists open-source Rust crates compiled into BeatrixLeslieIDE",
        "and libspell_core (libspell_core.dylib / libspell_core.so / spell_core.dll).",
        "",
        "Rust crates",
        "-----------",
    ]
    for pkg in pkgs:
        lic = pkg.get("license") or "see license files"
        out.append(f"  {pkg['name']} {pkg['version']}  ({lic})")
    out.append("")

    seen_hashes: dict[str, str] = {}
    for pkg in pkgs:
        name = pkg["name"]
        version = pkg["version"]
        lic_expr = pkg.get("license") or "see license files"
        manifest_path = Path(pkg["manifest_path"])
        cdir = manifest_path.parent
        out += ["", "#" * 78, f"# {name} {version}", f"# License: {lic_expr}", "#" * 78]
        files = sorted(
            p for p in cdir.iterdir()
            if p.is_file() and p.name.upper().startswith(LICENSE_PREFIXES)
        ) if cdir.is_dir() else []
        if not files:
            out.append(f"(No standalone license file distributed; Cargo.toml license: {lic_expr}.)")
        for lic in files:
            text = lic.read_text(encoding="utf-8", errors="replace").rstrip()
            digest = hashlib.sha256(text.encode()).hexdigest()
            out += ["", f"--- {lic.name} ---"]
            if digest in seen_hashes:
                out.append(f"(Identical to the {seen_hashes[digest]} text above.)")
            else:
                seen_hashes[digest] = f"{name} {lic.name}"
                out.append(text)

    if NOTICES.is_file():
        out += [
            "",
            "",
            "#" * 78,
            "# Additional Notices",
            "#" * 78,
            "",
            NOTICES.read_text(encoding="utf-8").rstrip(),
        ]
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    text = render()
    if args.check:
        current = OUTPUT.read_text(encoding="utf-8") if OUTPUT.is_file() else ""
        if current != text:
            print(
                f"{OUTPUT.relative_to(ROOT)} is out of date; run scripts/generate_third_party_licenses.py",
                file=sys.stderr,
            )
            return 1
        return 0
    OUTPUT.write_text(text, encoding="utf-8")
    print(f"wrote {OUTPUT.relative_to(ROOT)} ({len(text.splitlines())} lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
