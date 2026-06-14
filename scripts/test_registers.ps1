#!/usr/bin/env pwsh
#
# Acceptance test for clipp named registers. Exercises the CLI <-> daemon round
# trip that the C++ unit tests can't reach: argument parsing, the OneShot socket,
# stdio/pipe handling, exit codes, and the rich RLST list path.
#
# This is NOT a CRDT test -- it pins ONE daemon with --host, so copy and the
# following paste/ls hit the same store deterministically. Convergence across two
# daemons needs a different harness.
#
# Usage:   ./test_registers.ps1 <device-name-or-ip> [path-to-clipp.com]
# Example: ./test_registers.ps1 devbox2
#
# Notes:
#   * The target daemon must run THIS build (the RLST list frame gained a field;
#     ls / ls -v / rm-glob won't decode against an older daemon). copy/paste of a
#     single name work cross-version.
#   * Register names are all "test.*"; everything is rm'd at the end, leaving only
#     tombstones (they persist ~90 days). Point this at a throwaway group if you
#     don't want the traffic on your real mesh.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$TargetHost,
    [Parameter(Position = 1)][string]$Clipp
)

$ErrorActionPreference = 'Stop'

if (-not $Clipp) {
    $cand = Join-Path $PSScriptRoot '..\build\windows-release\clipp.com'
    if (Test-Path $cand)                               { $Clipp = (Resolve-Path $cand).Path }
    elseif (Get-Command clipp.com -ErrorAction Ignore) { $Clipp = 'clipp.com' }
    else { Write-Host 'clipp.com not found; pass its path as the 2nd argument.' -ForegroundColor Red; exit 2 }
}

Write-Host "clipp : $Clipp"
Write-Host "host  : $TargetHost`n"

$tmp  = New-Item -ItemType Directory -Force -Path (Join-Path ([System.IO.Path]::GetTempPath()) "clipp-regtest-$PID")
$script:pass = 0
$script:fail = 0
function Ok($m) { $script:pass++; Write-Host "  PASS  $m" -ForegroundColor Green }
function No($m) { $script:fail++; Write-Host "  FAIL  $m" -ForegroundColor Red }

# Run clipp with --host pinned. Returns the exit code; stdout -> $OutFile (a scratch
# file if omitted), stderr alongside, optional $InFile fed as byte-exact stdin.
function Clipp {
    param([string[]]$ClippArgs, [string]$InFile, [string]$OutFile)
    if (-not $OutFile) { $OutFile = Join-Path $tmp 'scratch.out' }
    $sp = @{
        FilePath               = $Clipp
        ArgumentList           = @('--host', $TargetHost) + $ClippArgs
        NoNewWindow            = $true
        Wait                   = $true
        PassThru               = $true
        RedirectStandardOutput = $OutFile
        RedirectStandardError  = "$OutFile.err"
    }
    if ($InFile) { $sp['RedirectStandardInput'] = $InFile }
    (Start-Process @sp).ExitCode
}

function FilesEqual($a, $b) {
    (Test-Path $a) -and (Test-Path $b) -and ((Get-FileHash $a).Hash -eq (Get-FileHash $b).Hash)
}

# copy <payload> into <name>, paste it back, assert byte-exact.
function RoundTrip($name, [byte[]]$payload) {
    $in  = Join-Path $tmp "$name.in"
    $out = Join-Path $tmp "$name.out"
    [System.IO.File]::WriteAllBytes($in, $payload)
    $rc = Clipp @('copy', $name) $in
    if ($rc -ne 0) { No "$name copy (exit $rc)"; return }
    $rc = Clipp @('paste', $name) $null $out
    if ($rc -ne 0) { No "$name paste (exit $rc)"; return }
    if (FilesEqual $in $out) { Ok "$name round-trips ($($payload.Length) bytes)" }
    else                     { No "$name round-trip mismatch" }
}

$U8 = [System.Text.Encoding]::UTF8

# --- byte-exact round trips through the daemon ---
RoundTrip 'test.basic'     ($U8.GetBytes('hello world'))
RoundTrip 'test.multiline' ($U8.GetBytes("line1`nline2`nline3`n"))
# 'cafe' + COMBINING ACUTE (U+0301) + ' ' + party-popper. Decomposed on purpose:
# proves registers are byte-exact and do NOT NFC-normalise. Explicit bytes so the
# script's own encoding can't taint the payload.
RoundTrip 'test.unicode'   ([byte[]]@(0x63,0x61,0x66,0x65,0xCC,0x81,0x20,0xF0,0x9F,0x8E,0x89))

# --- overwrite is last-writer-wins ---
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'ow1'), $U8.GetBytes('first'))
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'ow2'), $U8.GetBytes('second'))
[void](Clipp @('copy', 'test.overwrite') (Join-Path $tmp 'ow1'))
[void](Clipp @('copy', 'test.overwrite') (Join-Path $tmp 'ow2'))
$owOut = Join-Path $tmp 'ow.out'
[void](Clipp @('paste', 'test.overwrite') $null $owOut)
if ((Get-Content -Raw $owOut) -eq 'second') { Ok 'test.overwrite keeps the newer value' }
else                                         { No 'test.overwrite did not keep newer value' }

# --- private: masked in ls -v, still readable to a non-tty (a redirected pipe) ---
$secret = 'sup3rsecret-' + [guid]::NewGuid().ToString('N')
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'priv.in'), $U8.GetBytes($secret))
[void](Clipp @('copy', 'test.priv', '--private') (Join-Path $tmp 'priv.in'))
$lsv = Join-Path $tmp 'priv.lsv'
[void](Clipp @('ls', '-v', 'test.priv') $null $lsv)
$lsvText = Get-Content -Raw $lsv
if (($lsvText -match '\[private\]') -and ($lsvText -notmatch [regex]::Escape($secret))) { Ok 'test.priv masked in ls -v' }
else                                                                                     { No 'test.priv NOT masked in ls -v' }
$privOut = Join-Path $tmp 'priv.out'
$rc = Clipp @('paste', 'test.priv') $null $privOut
if (($rc -eq 0) -and ((Get-Content -Raw $privOut) -eq $secret)) { Ok 'test.priv readable to a non-tty' }
else                                                            { No "test.priv not readable to a non-tty (exit $rc)" }

# --- rm lifecycle: created -> listed -> removed -> paste fails ---
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'rmc.in'), $U8.GetBytes('removable'))
[void](Clipp @('copy', 'test.rmcheck') (Join-Path $tmp 'rmc.in'))
$ls1 = Join-Path $tmp 'rm.ls1'; [void](Clipp @('ls', 'test.rmcheck') $null $ls1)
$present  = (Get-Content -Raw $ls1) -match 'test\.rmcheck'
$rmRc     = Clipp @('rm', 'test.rmcheck')
$ls2 = Join-Path $tmp 'rm.ls2'; [void](Clipp @('ls', 'test.rmcheck') $null $ls2)
$gone     = -not ((Get-Content -Raw $ls2) -match 'test\.rmcheck')
$pasteRc  = Clipp @('paste', 'test.rmcheck')
if ($present -and ($rmRc -eq 0) -and $gone -and ($pasteRc -ne 0)) { Ok 'test.rmcheck created/listed/removed, paste then fails' }
else { No "test.rmcheck lifecycle (present=$present rmExit=$rmRc gone=$gone pasteExit=$pasteRc)" }

# --- rm of an absent register errors ---
$rc = Clipp @('rm', 'test.never-existed-zzz')
if ($rc -ne 0) { Ok 'rm of absent register errors' } else { No 'rm of absent register did NOT error' }

# --- invalid register name rejected (no register created) ---
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'bad.in'), $U8.GetBytes('x'))
$rc = Clipp @('copy', 'test.BAD!') (Join-Path $tmp 'bad.in')
if ($rc -ne 0) { Ok 'invalid register name rejected' } else { No 'invalid register name accepted' }

# --- cleanup: tombstone everything we made ---
[void](Clipp @('rm', 'test.*'))
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host "`n$($script:pass) passed, $($script:fail) failed." -ForegroundColor ($(if ($script:fail) { 'Red' } else { 'Green' }))
exit ($(if ($script:fail) { 1 } else { 0 }))
