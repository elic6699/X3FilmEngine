#!/usr/bin/env bash
# setup-libraw.sh -- the POSIX twin of setup-libraw.ps1.
#
# Fetches the one LibRaw tree the native RAW path is validated against into a
# gitignored dependency directory, and verifies it before it is used.
#
# 2026-11-16: the source is the product's own fork, github.com/elic6699/LibRaw,
# pinned to commit cf02d66e -- the 0.22.3 base, which is where X3Lumina-side
# LibRaw work lands from now on. The pin is checked either way:
#
#   1. the archive from codeload.github.com for that exact commit, checked
#      against a SHA-256 before anything is extracted; or, when that host is
#      unreachable (some egress proxies refuse it),
#   2. a shallow `git clone` of the fork, checked against the commit id *and*
#      its tree id -- a content hash over every file -- so a moved branch or a
#      different tree is refused exactly as a bad archive is.
#
# An existing tree is re-verified and never overwritten, so a locally patched
# LibRaw cannot silently become "the pinned source".
#
#   engine/setup-libraw.sh [deps-dir]      (default: <repo>/build/deps)
#
# The last line of stdout is the source directory, for SPEKTRALAB_LIBRAW_SOURCE.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
deps="${1:-$here/../build/deps}"
mkdir -p "$deps"
deps="$(cd "$deps" && pwd)"

version=0.22.3
fork=elic6699/LibRaw
url="https://codeload.github.com/$fork/zip/cf02d66eb5c19d4e46df7aee0582e47a4918509a"
zip_sha256=003effd3f93e1b8d961b2bf42b34cd60768efd02d3dbc5e547be8c4cde4169c6
git_url="https://github.com/$fork.git"
git_commit=cf02d66eb5c19d4e46df7aee0582e47a4918509a
git_tree=e1ff88e6cc1d03e5e067aaf482893dc61abcc0b6
archive="$deps/LibRaw-$version.zip"
source="$deps/LibRaw-$version"

verify_git_tree() {   # $1 = a checkout; succeeds only on the pinned commit+tree, clean
  local dir="$1"
  [[ "$(git -C "$dir" rev-parse HEAD)" == "$git_commit" ]] || return 1
  [[ "$(git -C "$dir" rev-parse 'HEAD^{tree}')" == "$git_tree" ]] || return 1
  [[ -z "$(git -C "$dir" status --porcelain --ignored)" ]] || return 1
}

verify_zip_tree() {   # the extracted tree against the verified archive, byte for byte
  python3 - "$archive" "$deps" "$version" "$git_commit" <<'PY'
import hashlib, os, sys, zipfile
archive, deps, version, commit = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
z = zipfile.ZipFile(archive)
top = z.namelist()[0].split('/')[0]           # LibRaw-<version> or LibRaw-<commit>
# On disk the tree is always LibRaw-<version> (the extract step renames the
# commit-named archive directory), so rewrite the archive's prefix to match.
root_dir = os.path.join(deps, f"LibRaw-{version}")
expected = set()
for info in z.infolist():
    if info.is_dir():
        continue
    rel = info.filename.split('/', 1)[1] if '/' in info.filename else ''
    path = os.path.normpath(os.path.join(root_dir, rel))
    expected.add(path)
    if not os.path.isfile(path):
        sys.exit(f"Incomplete LibRaw source tree: {path}")
    # engine/cmake/LibRaw.cmake overwrites exactly one file with the X3Lumina
    # OpenMP patch at configure time; that one is allowed to differ, and only
    # when it carries the marker. Everything else must still match the archive.
    if path.endswith(os.path.join("src", "postprocessing", "postprocessing_utils.cpp")):
        with open(path, "rb") as fh:
            if b"(X3Lumina)" in fh.read():
                continue
    if hashlib.sha256(z.read(info)).digest() != hashlib.sha256(open(path, 'rb').read()).digest():
        sys.exit(f"Modified LibRaw source file: {path}. Existing files were not overwritten.")
for root, _, files in os.walk(root_dir):
    for f in files:
        p = os.path.normpath(os.path.join(root, f))
        if p not in expected:
            sys.exit(f"Unexpected file in pinned LibRaw source: {p}")
PY
}

if [[ -d "$source" ]]; then
  if [[ -d "$source/.git" ]]; then
    verify_git_tree "$source" || { echo "LibRaw at $source is not the pinned, clean $version tree; not touched." >&2; exit 1; }
  elif [[ -f "$archive" ]]; then
    verify_zip_tree
  else
    echo "LibRaw at $source has no archive or git metadata to verify it against; remove it and re-run." >&2
    exit 1
  fi
  echo "LibRaw $version ($fork): existing source verified." >&2
  echo "$source"
  exit 0
fi

if [[ ! -f "$archive" ]] && curl -fsSL --retry 2 -o "$archive.partial" "$url" 2>/dev/null; then
  mv "$archive.partial" "$archive"
fi
rm -f "$archive.partial"

if [[ -f "$archive" ]]; then
  actual="$(sha256sum "$archive" | cut -d' ' -f1)"
  if [[ "$actual" != "$zip_sha256" ]]; then
    echo "LibRaw archive checksum mismatch ($actual); nothing was extracted or changed." >&2
    exit 1
  fi
  python3 - "$archive" "$deps" "$version" "$git_commit" <<'PY'
import os, sys, zipfile
archive, deps, version, commit = sys.argv[1:]
z = zipfile.ZipFile(archive)
# codeload names a commit archive after the commit, a tag archive after the tag.
allowed = {f"LibRaw-{version}", f"LibRaw-{commit}"}
for n in z.namelist():
    parts = n.rstrip('/').split('/')
    if parts[0] not in allowed or '..' in parts or n.startswith('/'):
        sys.exit(f"Unsafe LibRaw archive path: {n}")
z.extractall(deps)
sha_dir = os.path.join(deps, f"LibRaw-{commit}")
version_dir = os.path.join(deps, f"LibRaw-{version}")
if os.path.isdir(sha_dir) and not os.path.exists(version_dir):
    os.rename(sha_dir, version_dir)
PY
  echo "LibRaw $version ($fork): archive SHA256 verified and extracted." >&2
else
  echo "codeload.github.com unreachable; falling back to a pinned git clone of $fork." >&2
  tmp="$source.partial"
  rm -rf "$tmp"
  mkdir -p "$tmp"
  if git -C "$tmp" init -q && git -C "$tmp" remote add origin "$git_url" &&
     git -C "$tmp" -c advice.detachedHead=false fetch -q --depth 1 origin "$git_commit" &&
     git -C "$tmp" -c advice.detachedHead=false checkout -q FETCH_HEAD && verify_git_tree "$tmp"; then
    mv "$tmp" "$source"
    echo "LibRaw $version ($fork): git commit and tree id verified." >&2
  else
    rm -rf "$tmp"
    echo "LibRaw git fetch does not match the pinned commit $git_commit / tree $git_tree." >&2
    exit 1
  fi
fi
echo "$source"

