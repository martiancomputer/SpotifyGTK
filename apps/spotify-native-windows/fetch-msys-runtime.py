#!/usr/bin/env python3
"""Download a consistent UCRT64 SDK/runtime for a Linux cross-build.

Uses MSYS2's official repository metadata and verifies each archive's SHA256
before extraction. Does not install host packages or execute Windows binaries.
"""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile
import urllib.request

REPOSITORY = "https://repo.msys2.org/mingw/ucrt64/"
PREFIX = "mingw-w64-ucrt-x86_64-"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--extra-package", action="append", default=[],
                        help="Additional UCRT64 package name without the architecture prefix")
    parser.add_argument("--cached-database", action="store_true",
                        help="Reuse the downloaded repository snapshot without updating versions")
    args = parser.parse_args()
    root = args.directory.resolve()
    root.mkdir(parents=True, exist_ok=True)
    archives = root / "archives"
    archives.mkdir(exist_ok=True)
    database = root / "ucrt64.db"
    if not args.cached_database:
        urllib.request.urlretrieve(REPOSITORY + "ucrt64.db", database)
    elif not database.is_file():
        parser.error("--cached-database requires an existing ucrt64.db")
    packages = {}
    with tarfile.open(database) as archive:
        for member in archive:
            if not member.name.endswith("/desc"):
                continue
            text = archive.extractfile(member).read().decode("utf-8")
            fields = {}
            for field in text.strip().split("\n\n"):
                lines = field.splitlines()
                fields[lines[0].strip("%")] = lines[1:]
            packages[fields["NAME"][0]] = fields
    providers = {}
    for name, fields in packages.items():
        for provided in fields.get("PROVIDES", []):
            providers[re.split(r"[<>=]", provided)[0]] = name
    selected = {}

    def select(name):
        name = re.split(r"[<>=]", name)[0]
        if name in selected:
            return
        name = name if name in packages else providers.get(name, name)
        if name not in packages:
            raise RuntimeError(f"Unresolved runtime dependency: {name}")
        if name in selected:
            return
        selected[name] = packages[name]
        for dep in packages[name].get("DEPENDS", []):
            select(dep)

    for name in ("gtk4", "libadwaita", "libsoup3", "json-glib", "openssl",
                 "libogg", "libvorbis", "ffmpeg", "librsvg",
                 "glib-networking", "ca-certificates", "adwaita-icon-theme",
                 "adwaita-icon-theme-legacy"):
        select(PREFIX + name)
    for name in args.extra_package:
        select(PREFIX + name)

    def fetch(fields):
        filename = fields["FILENAME"][0]
        target = archives / filename
        expected = fields["SHA256SUM"][0]
        cached = False
        if target.exists():
            with target.open("rb") as stream:
                cached = hashlib.file_digest(stream, "sha256").hexdigest() == expected
        if not cached:
            urllib.request.urlretrieve(REPOSITORY + filename, target)
        with target.open("rb") as stream:
            if hashlib.file_digest(stream, "sha256").hexdigest() != expected:
                raise RuntimeError(f"Checksum mismatch: {filename}")
        print(f"Verified {filename}", flush=True)
        return target

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        downloaded = list(pool.map(fetch, selected.values()))
    for target in downloaded:
        subprocess.run(["bsdtar", "-xf", str(target), "-C", str(root),
                        "--exclude=.PKGINFO", "--exclude=.BUILDINFO",
                        "--exclude=.MTREE", "--exclude=.INSTALL"], check=True)
    (root / "runtime-packages.json").write_text(json.dumps([
        {"name": name, "version": fields["VERSION"][0],
         "repository": REPOSITORY,
         "sha256": fields["SHA256SUM"][0]}
        for name, fields in sorted(selected.items())
    ], indent=2) + "\n")
    print(f"Ready: {len(selected)} packages in {root / 'ucrt64'}", flush=True)


if __name__ == "__main__":
    main()
