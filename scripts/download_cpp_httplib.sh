#!/usr/bin/env bash
set -euo pipefail
readonly VERSION=0.58.0 URL=https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.58.0/httplib.h SHA256=aa14e7e7bd2703694e0a6b6855af3b8c406102ab1fc56ac905fe33619b31faa5
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd); repository_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
install_dir="$repository_dir/third_party/cpp-httplib-$VERSION"; install_file="$install_dir/httplib.h"
fail(){ printf 'download_cpp_httplib.sh: %s\n' "$*" >&2; exit 1; }
command -v wget >/dev/null || fail 'required tool not found: wget'; command -v sha256sum >/dev/null || fail 'required tool not found: sha256sum'
valid(){ [ -f "$install_file" ] && printf '%s  %s\n' "$SHA256" "$install_file" | sha256sum -c --status && grep -Fqx "#define CPPHTTPLIB_VERSION \"$VERSION\"" "$install_file"; }
if valid; then printf 'cpp-httplib %s is already installed and verified at %s\n' "$VERSION" "$install_dir"; exit 0; fi
mkdir -p "$repository_dir/third_party"; temp=$(mktemp -d "$repository_dir/third_party/.cpp-httplib.XXXXXX") || fail 'could not create temporary directory'; trap 'rm -rf -- "$temp"' EXIT
wget --https-only --secure-protocol=TLSv1_2 -O "$temp/httplib.h" "$URL" || fail "download failed: $URL"
printf '%s  %s\n' "$SHA256" "$temp/httplib.h" | sha256sum -c --status || fail 'SHA-256 verification failed'
grep -Fqx "#define CPPHTTPLIB_VERSION \"$VERSION\"" "$temp/httplib.h" || fail 'embedded version verification failed'
mkdir "$temp/package"; mv "$temp/httplib.h" "$temp/package/httplib.h"
if [ -e "$install_dir" ] || [ -L "$install_dir" ]; then mv "$install_dir" "$temp/previous" || fail 'could not preserve existing installation'; fi
if ! mv "$temp/package" "$install_dir"; then [ -e "$temp/previous" ] && mv "$temp/previous" "$install_dir"; fail 'installation failed; previous installation restored'; fi
printf 'Installed and verified cpp-httplib %s at %s\n' "$VERSION" "$install_dir"
