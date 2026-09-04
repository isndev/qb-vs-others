# Imports the newest MSVC toolchain into the current PowerShell session.
#
# Uses vswhere rather than whatever `cl` happens to be on PATH -- the same rule qb-dev's own
# verify-windows.ps1 states: a benchmark that cannot name its compiler is not reproducible, and a
# stray cl.exe from an older toolset is exactly how that happens silently.
#
#   . tools/msvc-env.ps1        # dot-sourced, so the environment lands in the caller

$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found -- no Visual Studio installation" }

$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "vswhere found no installation carrying the C++ x64 toolset" }

$devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
if (-not (Test-Path $devcmd)) { throw "VsDevCmd.bat missing under $vs" }

& "${env:COMSPEC}" /s /c "`"$devcmd`" -arch=amd64 -host_arch=amd64 -no_logo && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}

foreach ($t in @('cl', 'cmake', 'ninja')) {
    if (-not (Get-Command $t -ErrorAction SilentlyContinue)) {
        throw "$t is not on PATH after importing the VS environment"
    }
}

if (-not $env:VCPKG_ROOT -and (Test-Path 'D:\repo\vcpkg')) { $env:VCPKG_ROOT = 'D:\repo\vcpkg' }

Write-Host ("qvo: MSVC {0}, cmake {1}" -f `
    ((& cl 2>&1 | Select-String 'Version (\S+)').Matches[0].Groups[1].Value), `
    ((& cmake --version | Select-Object -First 1) -replace 'cmake version ',''))
