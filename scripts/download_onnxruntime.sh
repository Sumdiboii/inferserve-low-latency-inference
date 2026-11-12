#!/usr/bin/env bash
# Requires Bash, wget, sha256sum, and Python >=3.8 (standard library only).
set -euo pipefail
readonly VERSION=1.20.1 PACKAGE=onnxruntime-linux-x64-1.20.1 URL=https://github.com/microsoft/onnxruntime/releases/download/v1.20.1/onnxruntime-linux-x64-1.20.1.tgz SHA256=67db4dc1561f1e3fd42e619575c82c601ef89849afc7ea85a003abbac1a1a105
script_dir=$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd); repository_dir=$(CDPATH= cd -- "$script_dir/.." && pwd); third_party="$repository_dir/third_party"; install_dir="$third_party/$PACKAGE"
fail(){ printf 'download_onnxruntime.sh: %s\n' "$*" >&2; exit 1; }
for tool in wget sha256sum python3; do command -v "$tool" >/dev/null || fail "required tool not found: $tool"; done
python3 -c 'import sys; assert sys.version_info >= (3,8)' || fail 'Python 3.8 or newer is required'
validator=$(mktemp "${TMPDIR:-/tmp}/onnx-validator.XXXX.py"); temp=''; trap 'rm -f "$validator"; [ -z "$temp" ] || rm -rf -- "$temp"' EXIT
cat >"$validator" <<'PY'
import os,sys,tarfile,shutil
from pathlib import Path,PurePosixPath
V,P,mode,src,*rest=sys.argv[1:]
def bad(s): raise SystemExit('ONNX Runtime archive validation failed: '+s)
def check(root):
 r=Path(root)
 if r.name!=P or not (r/'include/onnxruntime_c_api.h').is_file() or not (r/f'lib/libonnxruntime.so.{V}').is_file() or not (r/'lib/libonnxruntime_providers_shared.so').is_file(): bad('package is missing expected headers or shared libraries')
 if (r/'VERSION_NUMBER').read_text().strip()!=V: bad('VERSION_NUMBER does not match requested release')
if mode=='validate': check(src); raise SystemExit
dest=Path(rest[0]); a=tarfile.open(src,'r:gz'); entries={}; dirs=[]; links=[]
for m in a.getmembers():
 q=PurePosixPath(m.name); k=q.parts
 if not k or q.is_absolute() or any(x in ('','.','..') for x in k) or k[0]!=P: bad('unsafe or out-of-root member: '+m.name)
 if k in entries: bad('duplicate member path: '+m.name)
 if m.isdir(): typ='d'; dirs.append(k)
 elif m.isreg(): typ='f'
 elif m.issym():
  typ='s'; t=PurePosixPath(m.linkname)
  if t.is_absolute(): bad('absolute symbolic-link target: '+m.name)
  resolved=[]
  for x in k[:-1]+t.parts:
   if x=='..':
    if not resolved: bad('symbolic-link target escapes extraction root: '+m.name)
    resolved.pop()
   elif x not in ('','.'): resolved.append(x)
  if not resolved or resolved[0]!=P: bad('symbolic-link target escapes extraction root: '+m.name)
  links.append((k,tuple(resolved),m))
 elif m.islnk(): bad('hard links are not permitted: '+m.name)
 else: bad('unsupported member type: '+m.name)
 entries[k]=(typ,m)
if entries.get((P,),('',))[0]!='d': bad('missing package root directory')
for k,(typ,m) in entries.items():
 for i in range(1,len(k)):
  if k[:i] in entries and entries[k[:i]][0]!='d': bad('non-directory member has descendants: '+'/'.join(k[:i]))
for k,t,m in links:
 seen=set()
 while entries.get(t,('',))[0]=='s':
  if t in seen: bad('symbolic-link cycle: '+m.name)
  seen.add(t)
  lm=entries[t][1]; q=PurePosixPath(lm.linkname); resolved=[]
  for x in t[:-1]+q.parts:
   if x=='..':
    if not resolved: bad('symbolic-link target escapes extraction root: '+m.name)
    resolved.pop()
   elif x not in ('','.'): resolved.append(x)
  t=tuple(resolved)
 if entries.get(t,('',))[0]!='f': bad('symbolic link does not target a regular archive file: '+m.name)
dest.mkdir()
for k in sorted(dirs,key=lambda x:(len(x),x)): (dest.joinpath(*k)).mkdir(mode=entries[k][1].mode&0o777)
for k,(typ,m) in entries.items():
 if typ=='f':
  inp=a.extractfile(m)
  with inp,open(dest.joinpath(*k),'xb') as out: shutil.copyfileobj(inp,out)
  os.chmod(dest.joinpath(*k),m.mode&0o777)
for k,t,m in links: os.symlink(m.linkname,dest.joinpath(*k))
check(dest/P)
PY
if [ -d "$install_dir" ] && python3 "$validator" "$VERSION" "$PACKAGE" validate "$install_dir" >/dev/null 2>&1; then printf 'ONNX Runtime %s is already installed and verified at %s\n' "$VERSION" "$install_dir"; exit 0; fi
mkdir -p "$third_party"; temp=$(mktemp -d "$third_party/.onnxruntime.XXXXXX") || fail 'could not create temporary directory'
wget --https-only --secure-protocol=TLSv1_2 -O "$temp/archive.tgz" "$URL" || fail "download failed: $URL"
printf '%s  %s\n' "$SHA256" "$temp/archive.tgz" | sha256sum -c --status || fail 'archive SHA-256 verification failed'
python3 "$validator" "$VERSION" "$PACKAGE" extract "$temp/archive.tgz" "$temp/extract" || fail 'safe archive validation or extraction failed'
if [ -e "$install_dir" ] || [ -L "$install_dir" ]; then mv "$install_dir" "$temp/previous" || fail 'could not preserve existing installation'; fi
if ! mv "$temp/extract/$PACKAGE" "$install_dir"; then [ -e "$temp/previous" ] && mv "$temp/previous" "$install_dir"; fail 'installation failed; previous installation restored'; fi
printf 'Installed and verified ONNX Runtime CPU %s at %s\n' "$VERSION" "$install_dir"
