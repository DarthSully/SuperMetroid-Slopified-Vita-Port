<# Package an already compiled Windows game using the common runtime packager. #>
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [string]$BuildDir = 'build-recompui',
  [string]$RuntimeBinDir = 'C:\msys64\mingw64\bin',
  [string]$Bash = 'C:\msys64\usr\bin\bash.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Version -ne (Get-Content -LiteralPath (Join-Path $root 'VERSION') -Raw).Trim()) {
  throw 'The requested package version must match VERSION.'
}
if (-not (Test-Path -LiteralPath $Bash)) {
  throw "MSYS2 bash executable missing: $Bash"
}
$previousPath = $env:PATH
try {
  $env:PATH = "$RuntimeBinDir;$previousPath"
  Push-Location -LiteralPath $root
  try {
    & $Bash scripts/package_release.sh $BuildDir windows-x64
    if ($LASTEXITCODE -ne 0) { throw "Runtime packaging failed: $LASTEXITCODE" }
  } finally { Pop-Location }
} finally { $env:PATH = $previousPath }
