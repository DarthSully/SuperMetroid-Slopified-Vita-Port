[CmdletBinding()]
param(
    # Supplying a ROM bypasses the Mods launcher. Omit to configure mods.
    [string]$DirectRomPath,
    [string]$RuntimeBin = 'C:\msys64\mingw64\bin',
    [switch]$CheckOnly
)

$ErrorActionPreference = 'Stop'
$rendererRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$rendererExe = Join-Path $rendererRoot 'build-custom\SuperMetroidSNESRecomp.exe'
if (-not (Test-Path -LiteralPath $rendererExe -PathType Leaf)) {
    throw "Build the custom renderer first: $rendererExe"
}
if (-not (Test-Path -LiteralPath $RuntimeBin -PathType Container)) {
    throw "Runtime DLL directory does not exist: $RuntimeBin"
}
# An explicit config argument disables the runtime's exe-directory anchoring.
$rendererArguments = @('--config', 'config.ini')
if ($DirectRomPath) {
    $resolvedRom = (Resolve-Path -LiteralPath $DirectRomPath).Path
    if (-not (Test-Path -LiteralPath $resolvedRom -PathType Leaf)) {
        throw "ROM is not a file: $resolvedRom"
    }
    $rendererArguments += $resolvedRom
}
$rendererData = Join-Path $rendererRoot 'build-custom\playtest'
if ($CheckOnly) {
    Write-Output "Executable: $rendererExe"
    Write-Output "Settings and saves: $rendererData"
    Write-Output "Runtime DLLs: $RuntimeBin"
    return
}

# The runtime uses its working directory for settings and saves. Never start
# the experimental build in the original checkout or reuse the user's saves.
[void](New-Item -ItemType Directory -Path $rendererData -Force)
$savedRendererPath = $env:PATH
Push-Location -LiteralPath $rendererData
try {
    $env:PATH = "$RuntimeBin;$savedRendererPath"
    & $rendererExe @rendererArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Custom renderer exited with code $LASTEXITCODE. See $rendererData for diagnostics."
    }
} finally {
    $env:PATH = $savedRendererPath
    Pop-Location
}
