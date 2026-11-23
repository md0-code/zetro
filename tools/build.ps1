<#
.SYNOPSIS
    Configure, build and test ZetRo on Windows.

.DESCRIPTION
    Wraps the MSVC developer environment so one command works from a clean
    checkout. Two quirks of a stock Visual Studio
    install are handled here:

      * VsDevCmd.bat shells out to `vswhere.exe` by bare name, but the
        installer only puts it under "Program Files (x86)", not on PATH.
      * A toolset can be registered without shipping compiler binaries, in
        which case entering the environment silently leaves `cl` unavailable.
        We select the newest toolset that actually has the compiler for the
        target (HostX64/x64 or HostX64/arm64) and pin it with -vcvars_ver.

    -Arch arm64 cross-compiles for Windows on ARM64 from an x64 host
    (VsDevCmd -arch=arm64 -host_arch=amd64) into build/arm64; it builds
    only, the tests cannot run here. Without the ARM64 MSVC toolset (the
    Visual Studio component Microsoft.VisualStudio.Component.VC.Tools.ARM64)
    it says so and exits with code 3.

    Desktop builds fetch and build SDL2 statically. The tests write under
    work/. -Libretro builds the shared core in build/libretro-<arch>.

.EXAMPLE
    pwsh -File tools/build.ps1                 # configure, build and run the tests (build/x64)
    pwsh -File tools/build.ps1 -Clean          # wipe the build directory first
    pwsh -File tools/build.ps1 -NoTest         # build only
    pwsh -File tools/build.ps1 -Arch arm64     # cross-compile for ARM64 (build/arm64, no tests)
#>
[CmdletBinding()]
param(
    [ValidateSet("x64", "arm64")]
    [string]$Arch = "x64",
    [string]$Config = "RelWithDebInfo",
    [string]$BuildDir = "",
    [switch]$Libretro,
    [switch]$Clean,
    [switch]$NoTest,
    # Link the C runtime statically (/MT), so the binary needs no Visual C++
    # redistributable.
    [switch]$StaticRuntime,
    # Tests run side by side (ctest -j): default the physical cores - 1.
    [int]$Jobs = 0
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) {
    $targetDir = if ($Libretro) { "libretro-$Arch" } else { $Arch }
    $BuildDir = Join-Path $root "build/$targetDir"
}

# Every Visual Studio install with the C++ toolset, the newest first.
function Find-VisualStudios {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $vswhere = Join-Path $installer "vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at $vswhere - is Visual Studio installed?"
    }
    $latest = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $latest) { throw "No Visual Studio install with the C++ toolset was found." }
    $all = @($latest) + @(& $vswhere -all -products * -property installationPath | Where-Object { $_ -ne $latest })
    return $all | ForEach-Object { [PSCustomObject]@{ Root = $_; InstallerDir = $installer } }
}

# The newest toolset with the compiler for the target and its libraries;
# $null when none has them.
function Find-UsableToolset([string]$vsRoot, [string]$target) {
    $msvc = Join-Path $vsRoot "VC\Tools\MSVC"
    $usable = Get-ChildItem $msvc -Directory -ErrorAction SilentlyContinue |
        Where-Object { (Test-Path (Join-Path $_.FullName "bin\HostX64\$target\cl.exe")) -and
                       (Test-Path (Join-Path $_.FullName "lib\$target")) } |
        Sort-Object { [version]$_.Name } -Descending
    if (-not $usable) { return $null }
    $parts = $usable[0].Name.Split('.')
    return "$($parts[0]).$($parts[1])"   # -vcvars_ver wants major.minor
}

$installs = @(Find-VisualStudios)
$vs = $null
$toolset = $null
foreach ($candidate in $installs) {
    $toolset = Find-UsableToolset $candidate.Root $Arch
    if ($toolset) { $vs = $candidate; break }
}
if (-not $toolset) {
    $roots = ($installs | ForEach-Object { $_.Root }) -join ", "
    if ($Arch -eq "arm64") {
        Write-Host "The ARM64 MSVC toolset is not installed: no toolset in $roots has HostX64\arm64\cl.exe and lib\arm64." -ForegroundColor Yellow
        Write-Host "Install the Visual Studio component 'MSVC Build Tools for ARM64' (Microsoft.VisualStudio.Component.VC.Tools.ARM64) to build windows-arm64." -ForegroundColor Yellow
        exit 3
    }
    throw "No MSVC toolset in $roots contains HostX64\x64\cl.exe."
}
$devcmd = Join-Path $vs.Root "Common7\Tools\VsDevCmd.bat"
Write-Host "Visual Studio : $($vs.Root)"
Write-Host "MSVC toolset  : $toolset ($Arch)"

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning $BuildDir"
    Remove-Item $BuildDir -Recurse -Force
}

$runtimeFlag = if ($StaticRuntime) { "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded" } else { "" }
$devArch = if ($Arch -eq "x64") { "amd64" } else { "arm64" }
# Cross-compiling: CMake must not try to run what it builds.
$crossFlags = if ($Arch -eq "arm64") { "-DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=ARM64" } else { "" }
$src = Join-Path $root "src"
$test = -not $Libretro -and -not $NoTest -and $Arch -eq "x64"
$libretroFlag = if ($Libretro) { "ON" } else { "OFF" }

if ($Jobs -le 0) {
    $cores = (Get-CimInstance Win32_Processor | Measure-Object NumberOfCores -Sum).Sum
    $Jobs = [Math]::Max(1, $cores - 1)
}

$script = @"
@echo off
set "PATH=$($vs.InstallerDir);%PATH%"
call "$devcmd" -arch=$devArch -host_arch=amd64 -vcvars_ver=$toolset -no_logo || exit /b 1
cmake -S "$src" -B "$BuildDir" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DZR_LIBRETRO_ONLY=$libretroFlag $runtimeFlag $crossFlags || exit /b 1
cmake --build "$BuildDir" || exit /b 1
"@
if ($test) {
    $script += @"

ctest --test-dir "$BuildDir" --output-on-failure -j $Jobs || exit /b 1
"@
}

New-Item -ItemType Directory -Force $BuildDir | Out-Null
$batch = Join-Path $BuildDir "zetro_build_$PID.bat"
Set-Content -Path $batch -Value $script -Encoding ASCII
$originalPath = $env:Path
try {
    # The VS batch files expand PATH inside cmd.exe conditionals.
    $env:Path = ($originalPath -split ';' | Where-Object { $_ -and $_ -notmatch '[()&!^"]' }) -join ';'
    & cmd.exe /c $batch
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
} finally {
    $env:Path = $originalPath
    Remove-Item $batch -Force -ErrorAction SilentlyContinue
}
Write-Host "OK" -ForegroundColor Green
