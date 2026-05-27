[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$BuildType = "Release",

    [string]$Triplet = "",

    [string]$VcpkgRoot = $env:VCPKG_ROOT,

    [string]$VcVarsAll = "",

    [string]$VcVarsArch = "amd64",

    [string]$Generator = "",

    [int]$Parallel = [Environment]::ProcessorCount,

    [switch]$DisableCodeSigning
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

if (-not $Triplet) {
    $Triplet = "x64-windows-static"
}

function Find-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates
    )

    $fromPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($fromPath) {
        return $fromPath.Source
    }

    foreach ($candidate in $Candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    return $null
}

function Resolve-Executable {
    param(
        [string]$Name,
        [string[]]$Candidates
    )

    $resolved = Find-Executable -Name $Name -Candidates $Candidates
    if ($resolved) {
        return $resolved
    }

    throw "Could not find $Name. Install it or add it to PATH."
}

function Invoke-NativeCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "'$FilePath' failed with exit code $LASTEXITCODE."
    }
}

function Resolve-VcVarsAll {
    param([string]$RequestedPath)

    $candidates = @()

    if ($RequestedPath) {
        $candidates += $RequestedPath
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($installPath) {
            $candidates += Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
        }
    }

    $candidates += @(
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
    )

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path $candidate)) {
            return $candidate
        }
    }

    throw "Could not find vcvarsall.bat. Pass -VcVarsAll C:\path\to\vcvarsall.bat."
}

function Import-VcVarsEnvironment {
    param(
        [string]$VcVarsAllPath,
        [string]$Arch
    )

    $resolvedVcVarsAll = Resolve-VcVarsAll -RequestedPath $VcVarsAllPath
    Write-Host "Importing Visual Studio environment ($Arch)..."

    $command = "call `"$resolvedVcVarsAll`" $Arch >nul && set"
    $environment = & cmd.exe /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "vcvarsall.bat failed for architecture '$Arch'."
    }

    foreach ($line in $environment) {
        if ($line -match "^([^=]+)=(.*)$") {
            Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

function Resolve-VcpkgRoot {
    param([string]$RequestedRoot)

    $candidates = @()

    if ($RequestedRoot) {
        $candidates += $RequestedRoot
    }

    if ($env:VCPKG_ROOT) {
        $candidates += $env:VCPKG_ROOT
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -property installationPath
        if ($installPath) {
            $candidates += Join-Path $installPath "VC\vcpkg"
        }
    }

    $candidates += @(
        (Join-Path $repoRoot "vcpkg"),
        (Join-Path (Split-Path $repoRoot -Parent) "vcpkg"),
        (Join-Path $env:USERPROFILE "vcpkg"),
        (Join-Path $env:LOCALAPPDATA "vcpkg"),
        "C:\Program Files\Microsoft Visual Studio\18\Community\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\18\BuildTools\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\18\Professional\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\vcpkg",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\vcpkg",
        "C:\src\vcpkg",
        "C:\dev\vcpkg",
        "C:\tools\vcpkg",
        "C:\vcpkg"
    )

    $fromPath = Get-Command vcpkg -ErrorAction SilentlyContinue
    if ($fromPath) {
        $candidates += Split-Path $fromPath.Source -Parent
    }

    foreach ($candidate in $candidates) {
        if (-not $candidate) {
            continue
        }

        $resolved = Resolve-Path $candidate -ErrorAction SilentlyContinue
        if (-not $resolved) {
            continue
        }

        $toolchain = Join-Path $resolved "scripts\buildsystems\vcpkg.cmake"
        if (Test-Path $toolchain) {
            return $resolved.Path
        }
    }

    throw "Could not find vcpkg. Set VCPKG_ROOT or pass -VcpkgRoot C:\path\to\vcpkg."
}

$cmakeCandidates = @(
    "C:\Program Files\CMake\bin\cmake.exe",
    "C:\Program Files (x86)\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\18\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
)

if (-not (Find-Executable -Name "cmake" -Candidates $cmakeCandidates) -or -not (Get-Command cl -ErrorAction SilentlyContinue)) {
    Import-VcVarsEnvironment -VcVarsAllPath $VcVarsAll -Arch $VcVarsArch
}

$resolvedVcpkgRoot = Resolve-VcpkgRoot -RequestedRoot $VcpkgRoot
$env:VCPKG_ROOT = $resolvedVcpkgRoot
$toolchainFile = Join-Path $resolvedVcpkgRoot "scripts\buildsystems\vcpkg.cmake"
$buildDir = Join-Path $repoRoot "build\windows-$($BuildType.ToLowerInvariant())"
$vcpkgManifestDir = Join-Path $repoRoot "src"
$vcpkgInstalledDir = Join-Path $repoRoot "build\vcpkg_installed"
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
$cmakeExe = Resolve-Executable -Name "cmake" -Candidates $cmakeCandidates

$configureArgs = @(
    "-S", $repoRoot,
    "-B", $buildDir,
    "-DVCPKG_TARGET_TRIPLET=$Triplet",
    "-DVCPKG_MANIFEST_DIR=$vcpkgManifestDir",
    "-DVCPKG_INSTALLED_DIR=$vcpkgInstalledDir",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
    "-DCMAKE_BUILD_TYPE=$BuildType",
    "-DVCPKG_INSTALL_OPTIONS=--clean-buildtrees-after-build;--clean-packages-after-build"
)

if (-not (Test-Path $cacheFile)) {
    $configureArgs += "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile"
}

$ninja = Get-Command ninja -ErrorAction SilentlyContinue
if ($Generator) {
    $configureArgs += @("-G", $Generator)
}
elseif ($ninja) {
    $configureArgs += @("-G", "Ninja")
    $configureArgs += "-DCMAKE_MAKE_PROGRAM=$($ninja.Source)"
}
else {
    $nmake = Get-Command nmake -ErrorAction SilentlyContinue
}

if (-not $Generator -and -not $ninja -and $nmake) {
    $configureArgs += @("-G", "NMake Makefiles")
    $configureArgs += "-DCMAKE_MAKE_PROGRAM=$($nmake.Source)"
}
elseif (-not $Generator -and -not $ninja) {
    Write-Host "Ninja and NMake were not found; letting CMake choose the default generator."
}

Write-Host "Configuring clipp ($BuildType, $Triplet)..."
Invoke-NativeCommand -FilePath $cmakeExe -Arguments $configureArgs

Write-Host "Building clipp..."
Invoke-NativeCommand -FilePath $cmakeExe -Arguments @("--build", $buildDir, "--config", $BuildType, "--parallel", "$Parallel")

Write-Host "Built: $buildDir"

$signingVars = [ordered]@{
    "ARTIFACT_SIGNING_ENDPOINT"            = $env:ARTIFACT_SIGNING_ENDPOINT
    "ARTIFACT_SIGNING_ACCOUNT"             = $env:ARTIFACT_SIGNING_ACCOUNT
    "ARTIFACT_SIGNING_CERTIFICATE_PROFILE" = $env:ARTIFACT_SIGNING_CERTIFICATE_PROFILE
}
$presentVars = @($signingVars.GetEnumerator() | Where-Object { $_.Value })
$missingVars = @($signingVars.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })

if ($DisableCodeSigning) {
    Write-Host "Skipping artifact signing (-DisableCodeSigning was passed)."
}
elseif ($presentVars.Count -eq 0) {
    # No signing env vars set; skip silently.
}
elseif ($missingVars.Count -gt 0) {
    Write-Warning "Skipping artifact signing; missing env var(s): $($missingVars -join ', ')"
}
else {
    Write-Host "Signing artifacts in $buildDir..."
    $signArgs = @(
        "code", "artifact-signing",
        "-b", $buildDir,
        "-ase", $env:ARTIFACT_SIGNING_ENDPOINT,
        "-asa", $env:ARTIFACT_SIGNING_ACCOUNT,
        "*.exe",
        "*.com",
        "-v", "Information",
        "-ascp", $env:ARTIFACT_SIGNING_CERTIFICATE_PROFILE
    )
    Invoke-NativeCommand -FilePath "sign.exe" -Arguments $signArgs
}
