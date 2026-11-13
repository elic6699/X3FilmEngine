#Requires -Version 5.1
<#
setup-lensfun.ps1 -- fetch and verify the pinned Lensfun 0.3.4 sources.

    engine\setup-lensfun.ps1 [deps-dir]     (default: <repo>\build\deps)

The Windows host compiles the UNMODIFIED official Lensfun 0.3.4 sources
against third_party\lensfun_shim (a small Win32 GLib-compatible layer), so
no GLib DLL has to ship. This script pins the upstream tarball by SHA-256
and verifies an existing tree against it byte for byte; a locally patched
Lensfun is therefore refused instead of silently becoming "the pinned
source". Everything lands in the gitignored build\deps directory.

The last line of stdout is the source directory, suitable for
-DSPEKTRALAB_LENSFUN_SOURCE=<that path>.
#>
[CmdletBinding()]
param([string]$DepsDir)

$ErrorActionPreference = 'Stop'

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $DepsDir) { $DepsDir = Join-Path $here '..\build\deps' }
if (-not (Test-Path $DepsDir)) { New-Item -ItemType Directory -Force -Path $DepsDir | Out-Null }
$DepsDir = (Resolve-Path $DepsDir).Path

$version = '0.3.4'
$top     = "lensfun-$version"
$url     = "https://github.com/lensfun/lensfun/archive/refs/tags/v$version.tar.gz"
$sha256  = 'dafb39c08ef24a0e2abd00d05d7341b1bf1f0c38bfcd5a4c69cf5f0ecb6db112'
$archive = Join-Path $DepsDir "$top.tar.gz"
$source  = Join-Path $DepsDir $top

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) { throw 'python is required to verify/extract the Lensfun tarball' }

# Verify an existing tree against the pinned archive, byte for byte.
function Test-ExtractedTree {
    $verify = @'
import os, sys, tarfile
archive, deps, top = sys.argv[1:4]
dst = os.path.join(deps, top)
if not os.path.isfile(os.path.join(dst, "libs", "lensfun", "lens.cpp")):
    sys.exit(1)
t = tarfile.open(archive, "r:gz")
same = 0
for m in t.getmembers():
    if not m.isfile() or not m.name.startswith(top + "/"):
        continue
    rel = m.name[len(top) + 1:]
    p = os.path.join(dst, *rel.split("/"))
    if not os.path.isfile(p):
        sys.exit(1)
    if t.extractfile(m).read() != open(p, "rb").read():
        sys.exit(1)
    same += 1
print(same)
'@
    $n = & $py.Source -c $verify $archive $DepsDir $top 2>$null
    return ($LASTEXITCODE -eq 0 -and $n)
}

if (Test-Path $source) {
    if (-not (Test-Path $archive)) {
        throw "Lensfun source exists at $source but the pinned archive is missing; remove the directory and re-run."
    }
    $actual = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actual -ne $sha256) {
        throw "Lensfun archive checksum mismatch ($actual); nothing was changed."
    }
    if (-not (Test-ExtractedTree)) {
        throw "Lensfun source at $source differs from the pinned 0.3.4 archive; not touched."
    }
    Write-Host "Lensfun $version : existing source verified against the pinned archive."
    Write-Output $source
    return
}

if (-not (Test-Path $archive)) {
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile "$archive.partial" -UseBasicParsing
    Move-Item -Force "$archive.partial" $archive
}

$actual = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($actual -ne $sha256) {
    Remove-Item -Force $archive
    throw "Lensfun archive checksum mismatch ($actual); archive deleted, nothing extracted."
}

# Extract with path-traversal checks, mirroring setup-libraw.sh.
$extract = @'
import os, sys, tarfile
archive, deps, top = sys.argv[1:4]
t = tarfile.open(archive, "r:gz")
for m in t.getmembers():
    parts = m.name.rstrip("/").split("/")
    if parts[0] != top or ".." in parts or m.name.startswith("/"):
        sys.exit("Unsafe Lensfun archive path: " + m.name)
t.extractall(deps)
'@
& $py.Source -c $extract $archive $DepsDir $top
if ($LASTEXITCODE -ne 0) { throw 'Lensfun archive extraction failed' }
if (-not (Test-ExtractedTree)) { throw 'Lensfun extraction did not reproduce the pinned archive' }

Write-Host "Lensfun $version : SHA-256 verified and extracted."
Write-Output $source
