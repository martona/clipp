# SPDX-License-Identifier: MIT
#
# Exit-code smoke test: replays the winget validation harness's launch sequence against a
# clipp.exe and fails if the process exits nonzero. The harness treats ANY nonzero exit code
# as a failed run — see winget-pkgs PR #408205 (martona.clipp 1.4.0.150,
# "App Clipp returned exit code: -1073741189" = 0xC000027B, STATUS_STOWED_EXCEPTION) and
# PR #393605, where ../WM_NIGHT hit the identical signature.
#
# Why a stowed exception needs a test at all: it is a WinRT/COM fail-fast
# (RoFailFastWithErrorContext). It does NOT run unhandled-exception filters, so Clipp's own
# crash handler never sees it and no minidump lands. The only reliable signal is the exit
# code — hence this script.
#
# Sequence, mirroring the harness: launch the exe with NO arguments; wait for the main XAML-
# Islands window (on a clean profile there is no group key, so Clipp opens it by itself —
# the same first-run path a validation VM takes, and the riskiest surface in the app); close
# it with WM_CLOSE; exit through the tray window (WM_COMMAND/ID_TRAY_EXIT, the full teardown
# path); assert exit code 0.
#
# NOTE on the first-run window: it appears only when no group key is configured. On a
# developer box that HAS one, Clipp starts to the tray with no window at all and this script
# still exercises the teardown path (it reports the exit code either way) — but only a clean
# profile reproduces what the harness sees. -RequireWindow makes the missing window fatal,
# for CI on a fresh runner.
#
# On any timeout the script enumerates every window the process owns and writes a full
# minidump of the LIVE process into -DumpDir (defaults to $env:WER_DUMP_DIR) before killing
# it, so a hang is as diagnosable as a crash.
#
# The exe must be launchable in the calling environment. A uiAccess-manifested build will not
# launch AT ALL where UAC is disabled (GitHub-hosted runners: EnableLUA=0 means Windows
# cannot mint a UIAccess token, so CreateProcess fails outright) — the CI workflow strips
# uiAccess from the binary first. On a normal desktop, point this at any signed build.

param(
    [Parameter(Mandatory)] [string] $ExePath,
    [string] $DumpDir = $env:WER_DUMP_DIR,
    [int] $WindowTimeoutSec = 90,
    [int] $ExitTimeoutSec = 60,
    [switch] $RequireWindow
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class ClippSmoke
{
    [DllImport("user32.dll", CharSet = CharSet.Unicode, EntryPoint = "FindWindowW")]
    public static extern IntPtr FindWindow(string lpClassName, string lpWindowName);

    // PowerShell binds $null to "" for string parameters, and FindWindowW(cls, "") matches
    // only windows with an EMPTY title. Pass the real null from here.
    public static IntPtr FindWindowByClass(string lpClassName)
    {
        return FindWindow(lpClassName, null);
    }

    [DllImport("user32.dll", EntryPoint = "PostMessageW")]
    public static extern bool PostMessage(IntPtr hWnd, uint Msg, UIntPtr wParam, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")]
    static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetClassNameW(IntPtr hWnd, StringBuilder buf, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    static extern int GetWindowTextW(IntPtr hWnd, StringBuilder buf, int max);
    [DllImport("user32.dll")]
    static extern bool IsWindowVisible(IntPtr hWnd);

    // Every top-level window owned by pid: "class | title | visible".
    public static List<string> GetProcessWindows(uint pid)
    {
        var result = new List<string>();
        EnumWindows((h, l) =>
        {
            uint wpid;
            GetWindowThreadProcessId(h, out wpid);
            if (wpid == pid)
            {
                var cls = new StringBuilder(256);
                GetClassNameW(h, cls, 256);
                var txt = new StringBuilder(256);
                GetWindowTextW(h, txt, 256);
                result.Add(string.Format("class={0} | title=\"{1}\" | visible={2}",
                                         cls, txt, IsWindowVisible(h)));
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    [DllImport("dbghelp.dll", SetLastError = true)]
    public static extern bool MiniDumpWriteDump(IntPtr hProcess, uint ProcessId, IntPtr hFile,
        int DumpType, IntPtr ExceptionParam, IntPtr UserStreamParam, IntPtr CallbackParam);
}
'@

$WM_CLOSE     = 0x0010
$WM_COMMAND   = 0x0111
$ID_TRAY_EXIT = 1002                        # src/platform/win32/tray.cpp
$TRAY_CLASS   = 'ClippHiddenTrayWindow'     # src/platform/win32/tray.cpp
$DIALOG_CLASS = 'ClippMainXamlDialog'       # src/platform/win32/xaml_dialog.cpp

# On a timeout: list the process's windows, dump it live, kill it (unless -KeepAlive).
function Invoke-HangAutopsy([System.Diagnostics.Process] $proc, [string] $label, [switch] $KeepAlive)
{
    Write-Host "--- $label ---"
    if ($proc.HasExited) {
        Write-Host ("Process already exited: {0} (0x{1:X8})" -f $proc.ExitCode, $proc.ExitCode)
        return
    }
    Write-Host "Windows owned by PID $($proc.Id):"
    $wins = [ClippSmoke]::GetProcessWindows([uint32]$proc.Id)
    if ($wins.Count -eq 0) { Write-Host '  (none)' }
    else { $wins | ForEach-Object { Write-Host "  $_" } }

    if ($DumpDir) {
        New-Item -ItemType Directory -Force $DumpDir | Out-Null
        $dumpPath = Join-Path $DumpDir "clipp-hang-$label.dmp"
        $file = [System.IO.File]::Create($dumpPath)
        try {
            # 2 = MiniDumpWithFullMemory
            $ok = [ClippSmoke]::MiniDumpWriteDump($proc.Handle, [uint32]$proc.Id,
                $file.SafeFileHandle.DangerousGetHandle(), 2,
                [IntPtr]::Zero, [IntPtr]::Zero, [IntPtr]::Zero)
        } finally { $file.Dispose() }
        if ($ok) { Write-Host "Live minidump written: $dumpPath" }
        else     { Write-Host "MiniDumpWriteDump failed (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))" }
    }
    if (-not $KeepAlive) {
        Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    }
}

# Report an exit code and fail on anything nonzero. Single place so every exit path in this
# script speaks the same language.
function Assert-CleanExit([int] $code)
{
    Write-Host ("Exit code: {0} (0x{1:X8})" -f $code, $code)
    if ($code -eq 0) {
        Write-Host 'PASS: clean exit.'
        return
    }
    if ($code -eq -1073741189) {
        throw 'FAIL: exit code 0xC000027B (stowed exception) — the winget validation failure, reproduced.'
    }
    throw ("FAIL: nonzero exit code {0} (0x{1:X8})" -f $code, $code)
}

if (-not (Test-Path $ExePath)) { throw "Not found: $ExePath" }

# Launch like the winget harness: by path, no arguments.
Write-Host "Launching $ExePath"
$p = Start-Process -FilePath $ExePath -PassThru

# On a clean profile (no group key) Clipp opens its main XAML-Islands window unprompted.
# Islands cold-start can be slow on a runner, hence the generous timeout.
$dialog = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($WindowTimeoutSec)
while ((Get-Date) -lt $deadline) {
    $dialog = [ClippSmoke]::FindWindowByClass($DIALOG_CLASS)
    if ($dialog -ne [IntPtr]::Zero) { break }
    if ($p.HasExited) {
        throw ("App exited before the main window appeared: exit code {0} (0x{1:X8})" -f $p.ExitCode, $p.ExitCode)
    }
    Start-Sleep -Milliseconds 500
}

if ($dialog -eq [IntPtr]::Zero) {
    # No window: either this profile already has a group key (Clipp went straight to the
    # tray — legitimate on a dev box) or the island failed to come up (the bug we hunt).
    # Either way the teardown path is still worth measuring, so exit gracefully and report.
    Write-Host "No '$DIALOG_CLASS' window within ${WindowTimeoutSec}s."
    Invoke-HangAutopsy $p 'no-main-window' -KeepAlive
    $tray = [ClippSmoke]::FindWindowByClass($TRAY_CLASS)
    if ($tray -ne [IntPtr]::Zero -and -not $p.HasExited) {
        Write-Host 'Tray window exists; exiting via ID_TRAY_EXIT for an exit-code reading...'
        [void][ClippSmoke]::PostMessage($tray, $WM_COMMAND, [UIntPtr]::new($ID_TRAY_EXIT), [IntPtr]::Zero)
        if ($p.WaitForExit($ExitTimeoutSec * 1000)) {
            if ($RequireWindow) {
                Assert-CleanExit $p.ExitCode   # reports, then fails below on the window
                throw "FAIL: no main window appeared (-RequireWindow). Exit path itself was clean."
            }
            Write-Host '(Tray-only launch: this profile has a group key, so the first-run window is expected to be absent.)'
            Assert-CleanExit $p.ExitCode
            return
        }
        Invoke-HangAutopsy $p 'teardown-hang-no-window'
        throw "App did not exit within ${ExitTimeoutSec}s of ID_TRAY_EXIT."
    }
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "Neither the main window nor the tray window was found (see window list / minidump above)."
}
Write-Host 'Main window is up.'

# Let the island's async content settle before tearing it down.
Start-Sleep -Seconds 5

Write-Host 'Closing the main window (WM_CLOSE)...'
[void][ClippSmoke]::PostMessage($dialog, $WM_CLOSE, [UIntPtr]::Zero, [IntPtr]::Zero)
$deadline = (Get-Date).AddSeconds(15)
while ((Get-Date) -lt $deadline -and [ClippSmoke]::IsWindow($dialog)) {
    Start-Sleep -Milliseconds 250
}
if ([ClippSmoke]::IsWindow($dialog)) {
    Invoke-HangAutopsy $p 'main-window-wont-close'
    throw 'Main window did not close on WM_CLOSE.'
}
if ($p.HasExited) {
    # Closing the window should leave the app alive in the tray; dying here is itself a bug.
    Write-Host 'App exited on WM_CLOSE of the main window (expected: stays resident in tray).'
    Assert-CleanExit $p.ExitCode
    throw 'FAIL: the app terminated when its main window closed; it should remain in the tray.'
}

Write-Host 'Exiting via the tray window (WM_COMMAND / ID_TRAY_EXIT)...'
$tray = [ClippSmoke]::FindWindowByClass($TRAY_CLASS)
if ($tray -eq [IntPtr]::Zero) {
    Invoke-HangAutopsy $p 'no-tray-window'
    throw 'Tray window not found after closing the main window.'
}
[void][ClippSmoke]::PostMessage($tray, $WM_COMMAND, [UIntPtr]::new($ID_TRAY_EXIT), [IntPtr]::Zero)

if (-not $p.WaitForExit($ExitTimeoutSec * 1000)) {
    Invoke-HangAutopsy $p 'teardown-hang'
    throw "App did not exit within ${ExitTimeoutSec}s of ID_TRAY_EXIT (teardown hang?)."
}

Assert-CleanExit $p.ExitCode
