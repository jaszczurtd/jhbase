[CmdletBinding()]
param(
    [string]$HalRoot = '',
    [string]$ToolsRoot = '',
    [ValidateSet('x64', 'Win32')][string[]]$Architecture = @('x64', 'Win32'),
    [ValidateRange(1, 256)][int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
# Windows PowerShell 5.1 leaves $PSScriptRoot empty in parameter defaults.
if (-not $HalRoot) { $HalRoot = Join-Path $PSScriptRoot '../JaszczurHAL' }
if (-not $ToolsRoot) { $ToolsRoot = Join-Path $PSScriptRoot '.build/tools' }

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

$hal = (Resolve-Path -LiteralPath $HalRoot).Path
# The pinned Unity sources come from the transfer bundle's tools directory or
# from scripts/ensure_tools.sh on a machine with Bash; nothing is downloaded here.
if (-not (Test-Path -LiteralPath (Join-Path $ToolsRoot 'Unity/src/unity.c'))) {
    throw "Pinned Unity is missing below $ToolsRoot; pass -ToolsRoot to the bundle's tools directory."
}
$tools = (Resolve-Path -LiteralPath $ToolsRoot).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 Build Tools.' }
$installation = & $vswhere -latest -products '*' -version '[17.0,18.0)' `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC v143 x86/x64 tools are missing.' }
$devShell = Join-Path $installation 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShell

foreach ($arch in $Architecture) {
    $targetArch = if ($arch -eq 'Win32') { 'x86' } else { 'x64' }
    # Use the same developer-shell module as the existing HAL Windows workflow.
    Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation `
        -DevCmdArguments "-arch=$targetArch -host_arch=x64"
    if ($env:VSCMD_ARG_TGT_ARCH -ne $targetArch -or
        -not (Get-Command cl.exe -CommandType Application -ErrorAction SilentlyContinue)) {
        throw "Cannot initialize MSVC for $arch"
    }
    $build = Join-Path $PSScriptRoot ".build/msvc-$arch"
    Invoke-Checked 'cmake' @('-S', $PSScriptRoot, '-B', $build,
        '-G', 'Visual Studio 17 2022', '-A', $arch, "-DJHBASE_HAL_ROOT=$hal",
        "-DJHBASE_TOOLS_ROOT=$tools")
    Invoke-Checked 'cmake' @('--build', $build, '--config', 'Release', '--parallel', "$Jobs")
    Invoke-Checked 'ctest' @('--test-dir', $build, '-C', 'Release', '--output-on-failure',
        '--output-junit', (Join-Path $build 'ctest.xml'))
}
Write-Host 'PASS: all requested native MSVC configurations'
