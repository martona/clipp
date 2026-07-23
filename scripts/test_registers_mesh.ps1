#!/usr/bin/env pwsh
#
# Two-daemon acceptance for the mesh semantics `test_registers.ps1` deliberately
# can't reach: register replication between daemons, tombstone propagation, and
# `put` making a register the live clipboard EVERYWHERE. Pin two different
# daemons with --host and assert each sees the other's writes.
#
# Usage:   ./test_registers_mesh.ps1 <host-A> <host-B> [path-to-clipp.com]
# Example: ./test_registers_mesh.ps1 devbox2 mac-mini
#
# Both daemons must run THIS build and be members of the same group, connected
# to each other. Settle sleeps cover broadcast latency; bump $Settle on a slow
# LAN.
#
# NOT covered here (GUI-only triggers — verify by hand on the A3+ build):
#   * MRU re-share: click a mid-list item in host-A's Clipp page -> it moves to
#     the top there AND on host-B, and host-B's OS clipboard now holds it.
#   * Mesh delete: right-click -> Delete on host-A -> the row disappears on
#     host-B too (best-effort: only currently-connected peers).
#   * Zero-anchor resync: restart host-A's daemon mid-churn -> its activity
#     list repopulates (up to clipboardSyncMaxItems) including any relocations.
#   * Binary registers: (after the popup's promote ships) promote an image ->
#     `ls -v` shows "[image/png, ...]" from both hosts; `put` of it pastes the
#     image on the other host; old CLI `paste` of it refuses a tty.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$HostA,
    [Parameter(Mandatory = $true, Position = 1)][string]$HostB,
    [Parameter(Position = 2)][string]$Clipp,
    [int]$Settle = 2
)

$ErrorActionPreference = 'Stop'

if (-not $Clipp) {
    $cand = Join-Path $PSScriptRoot '..\build\windows-release\clipp.com'
    if (Test-Path $cand)                               { $Clipp = (Resolve-Path $cand).Path }
    elseif (Get-Command clipp.com -ErrorAction Ignore) { $Clipp = 'clipp.com' }
    else { Write-Host 'clipp.com not found; pass its path as the 3rd argument.' -ForegroundColor Red; exit 2 }
}

Write-Host "clipp : $Clipp"
Write-Host "A     : $HostA"
Write-Host "B     : $HostB`n"

$tmp  = New-Item -ItemType Directory -Force -Path (Join-Path ([System.IO.Path]::GetTempPath()) "clipp-meshtest-$PID")
$script:pass = 0
$script:fail = 0
function Ok($m) { $script:pass++; Write-Host "  PASS  $m" -ForegroundColor Green }
function No($m) { $script:fail++; Write-Host "  FAIL  $m" -ForegroundColor Red }

function ClippOn {
    param([string]$AtHost, [string[]]$ClippArgs, [string]$InFile, [string]$OutFile)
    if (-not $OutFile) { $OutFile = Join-Path $tmp 'scratch.out' }
    $sp = @{
        FilePath               = $Clipp
        ArgumentList           = @('--host', $AtHost) + $ClippArgs
        NoNewWindow            = $true
        Wait                   = $true
        PassThru               = $true
        RedirectStandardOutput = $OutFile
        RedirectStandardError  = "$OutFile.err"
    }
    if ($InFile) { $sp['RedirectStandardInput'] = $InFile }
    (Start-Process @sp).ExitCode
}

$U8 = [System.Text.Encoding]::UTF8

# --- write via A, read via B: the REGW broadcast replicated the record ---
$payload = 'mesh-rt-' + [guid]::NewGuid().ToString('N')
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'rt.in'), $U8.GetBytes($payload))
[void](ClippOn $HostA @('copy', 'test.mesh.rt') (Join-Path $tmp 'rt.in'))
Start-Sleep -Seconds $Settle
$rtOut = Join-Path $tmp 'rt.out'
$rc = ClippOn $HostB @('paste', 'test.mesh.rt') $null $rtOut
if (($rc -eq 0) -and ((Get-Content -Raw $rtOut) -eq $payload)) { Ok 'A-write readable via B (replicated)' }
else { No "A-write not readable via B (exit $rc)" }

# --- wide name replicates too ---
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'wide.in'), $U8.GetBytes('wide mesh payload'))
[void](ClippOn $HostA @('copy', 'test wide mesh!') (Join-Path $tmp 'wide.in'))
Start-Sleep -Seconds $Settle
$wideOut = Join-Path $tmp 'wide.out'
$rc = ClippOn $HostB @('paste', 'test wide mesh!') $null $wideOut
if (($rc -eq 0) -and ((Get-Content -Raw $wideOut) -eq 'wide mesh payload')) { Ok 'wide-named register replicated' }
else { No "wide-named register not replicated (exit $rc)" }

# --- put via A: the register becomes the live clipboard on BOTH daemons ---
$putPayload = 'mesh-put-' + [guid]::NewGuid().ToString('N')
[System.IO.File]::WriteAllBytes((Join-Path $tmp 'put.in'), $U8.GetBytes($putPayload))
[void](ClippOn $HostA @('copy', 'test.mesh.put') (Join-Path $tmp 'put.in'))
Start-Sleep -Seconds $Settle
[void](ClippOn $HostA @('put', 'test.mesh.put'))
Start-Sleep -Seconds $Settle
$curA = Join-Path $tmp 'cur.a'; $curB = Join-Path $tmp 'cur.b'
[void](ClippOn $HostA @('paste') $null $curA)
[void](ClippOn $HostB @('paste') $null $curB)
$onA = (Get-Content -Raw $curA -ErrorAction Ignore) -match [regex]::Escape($putPayload)
$onB = (Get-Content -Raw $curB -ErrorAction Ignore) -match [regex]::Escape($putPayload)
if ($onA -and $onB) { Ok 'put made the register current on both daemons' }
else { No "put propagation (A=$onA B=$onB)" }

# --- rm via B: the tombstone reaches A ---
$rc = ClippOn $HostB @('rm', 'test.mesh.rt')
Start-Sleep -Seconds $Settle
$ls = Join-Path $tmp 'ls.a'
[void](ClippOn $HostA @('ls', 'test.mesh.rt') $null $ls)
$gone = -not ((Get-Content -Raw $ls -ErrorAction Ignore) -match 'test\.mesh\.rt')
if (($rc -eq 0) -and $gone) { Ok 'B-side rm tombstoned the register on A' }
else { No "B-side rm did not reach A (rmExit=$rc gone=$gone)" }

# --- cleanup on both sides (idempotent; tombstones replicate anyway) ---
[void](ClippOn $HostA @('rm', 'test*'))
[void](ClippOn $HostB @('rm', 'test*'))
Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue

Write-Host "`n$($script:pass) passed, $($script:fail) failed." -ForegroundColor ($(if ($script:fail) { 'Red' } else { 'Green' }))
exit ($(if ($script:fail) { 1 } else { 0 }))
