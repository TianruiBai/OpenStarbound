param(
  [string]$DevkitProRoot = $env:DEVKITPRO
)

$ErrorActionPreference = "Stop"

if (-not $DevkitProRoot) {
  if (Test-Path "C:\devkitPro") {
    $DevkitProRoot = "C:\devkitPro"
  }
}

if (-not $DevkitProRoot) {
  throw "DEVKITPRO was not set and C:\devkitPro was not found. Install devkitPro first."
}

$DevkitArmRoot = Join-Path $DevkitProRoot "devkitARM"
$BinPath = Join-Path $DevkitArmRoot "bin"
$MsysBinPath = Join-Path $DevkitProRoot "msys2\usr\bin"

if (-not (Test-Path $BinPath)) {
  throw "devkitARM bin folder not found at '$BinPath'. Install the 3ds-dev package group first."
}

$env:DEVKITPRO = $DevkitProRoot
$env:DEVKITARM = $DevkitArmRoot

$pathParts = $env:Path -split ';'
if ($pathParts -notcontains $BinPath) {
  $env:Path = "$BinPath;$($env:Path)"
}

if ((Test-Path $MsysBinPath) -and ($pathParts -notcontains $MsysBinPath)) {
  $env:Path = "$MsysBinPath;$($env:Path)"
}

$pkgConfig = Join-Path $MsysBinPath "pkg-config.exe"
if (Test-Path $pkgConfig) {
  $env:PKG_CONFIG_EXECUTABLE = $pkgConfig
  $env:PKG_CONFIG_PATH = "$DevkitProRoot\portlibs\3ds\lib\pkgconfig;$DevkitProRoot\libctru\lib\pkgconfig"
}

Write-Host "Configured devkitPro environment for this terminal session:" -ForegroundColor Green
Write-Host "  DEVKITPRO=$env:DEVKITPRO"
Write-Host "  DEVKITARM=$env:DEVKITARM"
Write-Host "  Added to PATH: $BinPath"
if (Test-Path $MsysBinPath) {
  Write-Host "  Added to PATH: $MsysBinPath"
}
if (Test-Path $pkgConfig) {
  Write-Host "  PKG_CONFIG_EXECUTABLE=$env:PKG_CONFIG_EXECUTABLE"
}
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Cyan
Write-Host "  1) cd source"
Write-Host "  2) cmake --preset=n3ds-devkitarm-bootstrap"
Write-Host "  3) cmake --build --preset=n3ds-devkitarm-bootstrap"
