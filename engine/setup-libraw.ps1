# Download the pinned LibRaw tree from the product fork, verify it, extract it.
# The Windows twin of setup-libraw.sh; no Python needed.
#
# 2026-11-16: source is github.com/elic6699/LibRaw @ cf02d66e (the 0.22.3 base).
# The pin is a commit, not a tag, so the archive codeload hands back is named
# after the commit -- it is extracted to LibRaw-0.22.3 on disk either way.
[CmdletBinding()]
param([string]$DependencyDirectory = (Join-Path $PSScriptRoot '..\build\deps'))

$ErrorActionPreference = 'Stop'
$version = '0.22.3'
$fork    = 'elic6699/LibRaw'
$commit  = 'cf02d66eb5c19d4e46df7aee0582e47a4918509a'
$expected = '003effd3f93e1b8d961b2bf42b34cd60768efd02d3dbc5e547be8c4cde4169c6'

$DependencyDirectory = [IO.Path]::GetFullPath($DependencyDirectory)
New-Item -ItemType Directory -Path $DependencyDirectory -Force | Out-Null
$archive = Join-Path $DependencyDirectory "LibRaw-$version.zip"
$source  = Join-Path $DependencyDirectory "LibRaw-$version"
$url     = "https://codeload.github.com/$fork/zip/$commit"

if (-not (Test-Path -LiteralPath $archive)) { Invoke-WebRequest -Uri $url -OutFile $archive }
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $expected) {
    throw 'LibRaw archive checksum mismatch; existing files were not extracted or changed.'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archive)
try {
    $top = $zip.Entries[0].FullName.Split('/')[0]     # LibRaw-<commit>
    foreach ($entry in $zip.Entries) {
        $full = $entry.FullName
        if (-not $full.StartsWith($top, [StringComparison]::Ordinal)) { throw 'Unsafe LibRaw archive path' }
        if ($full -match '(^|/)\.\.(/|$)') { throw 'Unsafe LibRaw archive path' }
    }
    if (Test-Path -LiteralPath $source) {
        # Existing tree: re-verify it byte for byte and never overwrite it, so a
        # locally patched LibRaw cannot silently become "the pinned source".
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries) {
            if (-not $entry.Name) { continue }
            $rel = $entry.FullName.Substring($top.Length).TrimStart('/')
            $target = [IO.Path]::GetFullPath((Join-Path $source $rel))
            [void]$seen.Add($target)
            if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Incomplete LibRaw source tree: $target" }
            # engine/cmake/LibRaw.cmake overwrites exactly one file with the
            # X3Lumina OpenMP patch at configure time; that one may differ, and
            # only when it carries the marker.
            if ($rel -eq 'src/postprocessing/postprocessing_utils.cpp' -and
                (Get-Content -LiteralPath $target -Raw) -match '\(X3Lumina\)') { continue }
            $stream = $entry.Open()
            $hasher = [Security.Cryptography.SHA256]::Create()
            try { $digest = [BitConverter]::ToString($hasher.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
            finally { $hasher.Dispose(); $stream.Dispose() }
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -ne $digest) {
                throw "Modified LibRaw source file: $target. Existing files were not overwritten."
            }
        }
        foreach ($file in Get-ChildItem -LiteralPath $source -File -Recurse) {
            if (-not $seen.Contains($file.FullName)) { throw "Unexpected file in pinned LibRaw source: $($file.FullName)" }
        }
    }
} finally { $zip.Dispose() }

if (-not (Test-Path -LiteralPath $source)) {
    $tmp = Join-Path $DependencyDirectory '.libraw-extract'
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($archive, $tmp)
    Move-Item -LiteralPath (Join-Path $tmp $top) -Destination $source
    Remove-Item -LiteralPath $tmp -Recurse -Force
}

Write-Output "LibRaw $version ($fork): $source (archive SHA256 and source tree verified)."
