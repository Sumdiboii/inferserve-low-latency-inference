#!/usr/bin/env bash
# Requires Bash, wget, sha256sum, and Python >=3.8 (standard library only).
set -euo pipefail

readonly VERSION=1.20.1
readonly PACKAGE=onnxruntime-linux-x64-1.20.1
readonly URL=https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/1.20.1/microsoft.ml.onnxruntime.1.20.1.nupkg
readonly SHA256=9c32108e9a1c78b090a95c2a219f244dd5f067d4caf68fb0f967103f3f20839c

script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repository_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
third_party="$repository_dir/third_party"
install_dir="$third_party/$PACKAGE"

fail() { printf 'download_onnxruntime.sh: %s\n' "$*" >&2; exit 1; }

for tool in wget sha256sum python3; do
    command -v "$tool" >/dev/null || fail "required tool not found: $tool"
done
python3 -c 'import sys; assert sys.version_info >= (3,8)' ||
    fail 'Python 3.8 or newer is required'

validator=$(mktemp "${TMPDIR:-/tmp}/onnx-validator.XXXX.py")
temp=''
trap 'rm -f "$validator"; [ -z "$temp" ] || rm -rf -- "$temp"' EXIT

cat >"$validator" <<'PY'
import os
import shutil
import sys
import zipfile
from pathlib import Path

version, package, mode, source, *rest = sys.argv[1:]


def bad(message):
    raise SystemExit("ONNX Runtime archive validation failed: " + message)


def check(root):
    root = Path(root)
    if root.name != package:
        bad("package directory has an unexpected name")
    if not (root / "include/onnxruntime_c_api.h").is_file():
        bad("package is missing ONNX Runtime headers")
    if not (root / f"lib/libonnxruntime.so.{version}").is_file():
        bad("package is missing the versioned ONNX Runtime library")
    if not (root / "lib/libonnxruntime.so.1").is_file():
        bad("package is missing the ONNX Runtime SONAME symlink")
    if not (root / "lib/libonnxruntime_providers_shared.so").is_file():
        bad("package is missing the shared providers library")
    if (root / "VERSION_NUMBER").read_text().strip() != version:
        bad("VERSION_NUMBER does not match requested release")


if mode == "validate":
    check(source)
    raise SystemExit

destination = Path(rest[0]) / package
with zipfile.ZipFile(source) as archive:
    members = {}
    for member in archive.infolist():
        name = Path(member.filename)
        if name.is_absolute() or ".." in name.parts:
            bad("unsafe archive member: " + member.filename)
        if member.filename.startswith("build/native/include/"):
            target = Path("include") / Path(member.filename).relative_to("build/native/include")
        elif member.filename in {
            "runtimes/linux-x64/native/libonnxruntime.so",
            "runtimes/linux-x64/native/libonnxruntime_providers_shared.so",
        }:
            target = Path("lib") / Path(member.filename).name
        else:
            continue
        if target in members:
            bad("duplicate archive member: " + str(target))
        members[target] = member

    required_header = Path("include/onnxruntime_c_api.h")
    required_library = Path("lib/libonnxruntime.so")
    required_provider = Path("lib/libonnxruntime_providers_shared.so")
    if not {required_header, required_library, required_provider} <= members.keys():
        bad("package is missing required Linux x64 files")

    destination.mkdir(parents=True)
    for target, member in members.items():
        output = destination / target
        output.parent.mkdir(parents=True, exist_ok=True)
        with archive.open(member) as source_file, output.open("xb") as output_file:
            shutil.copyfileobj(source_file, output_file)
        os.chmod(output, member.external_attr >> 16 & 0o777 or 0o644)

    (destination / "VERSION_NUMBER").write_text(version + "\n")
    os.symlink("libonnxruntime.so", destination / f"lib/libonnxruntime.so.{version}")
    os.symlink("libonnxruntime.so", destination / "lib/libonnxruntime.so.1")

check(destination)
PY

if [ -d "$install_dir" ] &&
    python3 "$validator" "$VERSION" "$PACKAGE" validate "$install_dir" >/dev/null 2>&1; then
    printf 'ONNX Runtime %s is already installed and verified at %s\n' "$VERSION" "$install_dir"
    exit 0
fi

mkdir -p "$third_party"
temp=$(mktemp -d "$third_party/.onnxruntime.XXXXXX") ||
    fail 'could not create temporary directory'
wget --https-only --secure-protocol=TLSv1_2 -O "$temp/archive.nupkg" "$URL" ||
    fail "download failed: $URL"
printf '%s  %s\n' "$SHA256" "$temp/archive.nupkg" | sha256sum -c --status ||
    fail 'archive SHA-256 verification failed'
python3 "$validator" "$VERSION" "$PACKAGE" extract "$temp/archive.nupkg" "$temp/extract" ||
    fail 'safe archive validation or extraction failed'
if [ -e "$install_dir" ] || [ -L "$install_dir" ]; then
    mv "$install_dir" "$temp/previous" || fail 'could not preserve existing installation'
fi
if ! mv "$temp/extract/$PACKAGE" "$install_dir"; then
    [ -e "$temp/previous" ] && mv "$temp/previous" "$install_dir"
    fail 'installation failed; previous installation restored'
fi
printf 'Installed and verified ONNX Runtime CPU %s at %s\n' "$VERSION" "$install_dir"
