<#
.SYNOPSIS
  Sets up a Windows development environment for AviCap Studio.

.DESCRIPTION
  Installs (if missing) the official tools with winget:
    - Visual Studio 2022 Build Tools with the C++ workload (MSVC, Windows SDK)
    - CMake, Ninja, Git, Inno Setup 6
  Then clones vcpkg at a pinned release tag and bootstraps it, and installs
  the FFmpeg (LGPL) dependencies from vcpkg.json.

  Nothing is downloaded from unofficial sources and no security setting is
  changed. Run from a normal PowerShell; winget may ask for elevation for the
  Visual Studio installer.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File .\setup_dev.ps1
  powershell -ExecutionPolicy Bypass -File .\setup_dev.ps1 -SkipDependencies
#>
param(
    [switch]$SkipTools,
    [switch]$SkipDependencies,
    [string]$VcpkgRoot = "$PSScriptRoot\tools\vcpkg",
    [string]$VcpkgTag = "2026.07.29"
)

$ErrorActionPreference = "Stop"
function Step($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

if (-not $IsWindows -and $PSVersionTable.PSEdition -eq "Core") { throw "setup_dev.ps1 must run on Windows." }

if (-not $SkipTools) {
    if (-not (Have "winget")) {
        throw "winget (App Installer) is required. Install it from the Microsoft Store, then run this script again."
    }
    $packages = @(
        @{ Id = "Kitware.CMake";       Cmd = "cmake" },
        @{ Id = "Ninja-build.Ninja";   Cmd = "ninja" },
        @{ Id = "Git.Git";             Cmd = "git" },
        @{ Id = "JRSoftware.InnoSetup"; Cmd = "" }
    )
    foreach ($p in $packages) {
        if ($p.Cmd -and (Have $p.Cmd)) { Write-Host "    $($p.Id): already installed"; continue }
        Step "Installing $($p.Id)"
        winget install --id $p.Id --exact --silent --accept-package-agreements --accept-source-agreements
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $hasMsvc = (Test-Path $vswhere) -and (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $hasMsvc) {
        Step "Installing Visual Studio 2022 Build Tools (C++ workload)"
        winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --silent --accept-package-agreements --accept-source-agreements `
            --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
    } else {
        Write-Host "    MSVC: found"
    }
    # New tools are only on PATH in new shells; refresh this session's PATH.
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "Machine") + ";" + [Environment]::GetEnvironmentVariable("Path", "User")
}

if (-not $SkipDependencies) {
    if (-not (Test-Path "$VcpkgRoot\.git")) {
        Step "Cloning vcpkg $VcpkgTag (https://github.com/microsoft/vcpkg)"
        git clone --branch $VcpkgTag --depth 1 https://github.com/microsoft/vcpkg.git $VcpkgRoot
    }
    if (-not (Test-Path "$VcpkgRoot\vcpkg.exe")) {
        Step "Bootstrapping vcpkg"
        & "$VcpkgRoot\bootstrap-vcpkg.bat" -disableMetrics
    }
    [Environment]::SetEnvironmentVariable("VCPKG_ROOT", $VcpkgRoot, "User")
    $env:VCPKG_ROOT = $VcpkgRoot
    Step "Installing dependencies from vcpkg.json (FFmpeg LGPL build; the first run takes a while)"
    Push-Location $PSScriptRoot
    try {
        & "$VcpkgRoot\vcpkg.exe" install --triplet x64-windows --x-install-root="$PSScriptRoot\build\vcpkg_installed"
    } finally { Pop-Location }
}

Step "Done. Next: build_release.bat  (or open the folder in Visual Studio / VS Code with CMake)"
