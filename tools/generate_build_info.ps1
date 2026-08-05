Param(
  [Parameter(Mandatory=$true)][string]$OutPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Runs unconditionally on every build (wired as a PreBuildEvent, not a CustomBuild gated on
# input changes) so the embedded identity always matches what was just compiled, even when
# the only files that changed are unrelated to dllmain.cpp -- MSVC's incremental build
# otherwise leaves __DATE__/__TIME__ stuck at whichever build last touched that translation
# unit, which made two genuinely different binaries report the same timestamp.
#
# Repo root is derived from this script's own location ($PSScriptRoot/tools/..) rather than
# taken as a command-line argument: $(ProjectDir) always ends in a trailing backslash, which
# escapes the closing quote in Windows command-line parsing and silently corrupts the arg.

$repoRoot = Split-Path -Parent $PSScriptRoot

$hash = "unknown"
$dirty = 0

try {
    Push-Location -LiteralPath $repoRoot
    $rawHash = git rev-parse --short=8 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($rawHash)) {
        $hash = $rawHash.Trim()
    }

    $status = git status --porcelain 2>$null
    if ($LASTEXITCODE -eq 0 -and $status) {
        $dirty = 1
    }
}
catch {
    $hash = "unknown"
}
finally {
    Pop-Location
}

$timestamp = Get-Date -Format "yyyy-MM-dd HH:mm:ss"

$content = @"
#pragma once
// Auto-generated on every build by tools/generate_build_info.ps1 (BeforeBuild target).
// Do not edit by hand -- changes are overwritten on the next build. Not checked into git.
#define BUILD_GIT_HASH "$hash"
#define BUILD_GIT_DIRTY $dirty
#define BUILD_TIMESTAMP "$timestamp"
"@

Set-Content -LiteralPath $OutPath -Value $content -Encoding ASCII
