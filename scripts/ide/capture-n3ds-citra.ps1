param(
  [string]$CitraPath = "citra-windows-msvc-20240303-0ff3440\citra-qt.exe",
  [string]$ElfPath = "dist\starbound",
  [string]$SmdhPath = "dist\starbound.smdh",
  [string]$RomfsPath = "assets",
  [string]$ThreeDsxToolPath = "C:\devkitPro\tools\bin\3dsxtool.exe",
  [string]$ThreeDsxPath = "build\citra-repack\starbound.3dsx",
  [string]$CaptureDir = "build\citra-captures\n3ds-current",
  [string]$BasePakPath = "",
  [int]$WindowWidth = 1280,
  [int]$WindowHeight = 900,
  [int]$WindowTimeoutSeconds = 30,
  [int]$CaptureDelaySeconds = 15,
  [switch]$NoRepack,
  [switch]$AutoStartSinglePlayer,
  [switch]$KeepCitraOpen
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Set-Location $repoRoot

function Resolve-RepoPath([string]$path) {
  if ([System.IO.Path]::IsPathRooted($path)) { return $path }
  return Join-Path $repoRoot $path
}

$citraExe = Resolve-RepoPath $CitraPath
$elf = Resolve-RepoPath $ElfPath
$smdh = Resolve-RepoPath $SmdhPath
$romfs = Resolve-RepoPath $RomfsPath
$threeDsxTool = Resolve-RepoPath $ThreeDsxToolPath
$threeDsx = Resolve-RepoPath $ThreeDsxPath
$capture = Resolve-RepoPath $CaptureDir
$usingDefaultAssetsRomfs = $RomfsPath -eq "assets"

if ($BasePakPath) {
  $basePak = (Resolve-Path -LiteralPath $BasePakPath).Path
  if (!(Test-Path -LiteralPath $basePak)) { throw "Base packed.pak not found: $BasePakPath" }

  $sdmcAssets = Join-Path $env:APPDATA "Citra\sdmc\OpenStarbound\assets"
  $sdmcPak = Join-Path $sdmcAssets "packed.pak"
  New-Item -ItemType Directory -Path $sdmcAssets -Force | Out-Null

  $sourceLength = (Get-Item -LiteralPath $basePak).Length
  $needsStage = $true
  if (Test-Path $sdmcPak) {
    $needsStage = (Get-Item $sdmcPak).Length -ne $sourceLength
    if ($needsStage) { Remove-Item -Force $sdmcPak }
  }

  if ($needsStage) {
    try {
      New-Item -ItemType HardLink -Path $sdmcPak -Target $basePak | Out-Null
      Write-Host "Staged base assets as hardlink: $sdmcPak"
    } catch {
      Write-Warning "Hardlink failed, copying base assets to Citra SDMC: $($_.Exception.Message)"
      Copy-Item -LiteralPath $basePak -Destination $sdmcPak -Force
      Write-Host "Staged base assets as copy: $sdmcPak"
    }
  } else {
    Write-Host "Base assets already staged: $sdmcPak"
  }

  if ($usingDefaultAssetsRomfs) {
    $romfs = Join-Path $repoRoot "build\citra-repack\romfs-sdmc-base"
    if (Test-Path $romfs) { Remove-Item -Recurse -Force $romfs }
    New-Item -ItemType Directory -Path $romfs -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot "assets\opensb") -Destination (Join-Path $romfs "opensb") -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $repoRoot "assets\sbinit.config") -Destination (Join-Path $romfs "sbinit.config") -Force
    Write-Host "Using SDMC base asset RomFS overlay: $romfs"
  }
}

if (!(Test-Path $citraExe)) { throw "Citra executable not found: $citraExe" }
if (!$NoRepack) {
  if (!(Test-Path $threeDsxTool)) { throw "3dsxtool not found: $threeDsxTool" }
  if (!(Test-Path $elf)) { throw "N3DS ELF not found: $elf" }
  if (!(Test-Path $smdh)) { throw "SMDH not found: $smdh" }
  if (!(Test-Path $romfs)) { throw "RomFS path not found: $romfs" }
  New-Item -ItemType Directory -Path (Split-Path $threeDsx -Parent) -Force | Out-Null
  & $threeDsxTool $elf $threeDsx "--smdh=$smdh" "--romfs=$romfs"
  if ($LASTEXITCODE -ne 0) { throw "3dsxtool failed with exit code $LASTEXITCODE" }
}

if (!(Test-Path $threeDsx)) { throw "3DSX not found: $threeDsx" }
if (Test-Path $capture) { Remove-Item -Recurse -Force $capture }
New-Item -ItemType Directory -Path $capture -Force | Out-Null

$sdmcStorage = Join-Path $env:APPDATA "Citra\sdmc\OpenStarbound\storage"
$autoStartMarker = Join-Path $sdmcStorage "n3ds_autostart_singleplayer"
if ($AutoStartSinglePlayer) {
  New-Item -ItemType Directory -Path $sdmcStorage -Force | Out-Null
  Set-Content -LiteralPath $autoStartMarker -Value "1" -NoNewline
  Write-Host "N3DS singleplayer autostart marker staged: $autoStartMarker"
} else {
  Remove-Item -LiteralPath $autoStartMarker -Force -ErrorAction SilentlyContinue
}

Get-Process citra-qt -ErrorAction SilentlyContinue | Stop-Process -Force

if (-not ("OpenStarboundN3dsCitraCapture.NativeMethods" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace OpenStarboundN3dsCitraCapture {
  public static class NativeMethods {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
      public int Left;
      public int Top;
      public int Right;
      public int Bottom;
    }

    [DllImport("user32.dll")]
    public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);

    [DllImport("user32.dll")]
    public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out int processId);

    [DllImport("user32.dll")]
    public static extern bool IsWindowVisible(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    private static int s_targetProcessId;
    private static IntPtr s_foundWindow;

    public static IntPtr FindVisibleWindowForProcess(int processId) {
      s_targetProcessId = processId;
      s_foundWindow = IntPtr.Zero;
      EnumWindows(EnumWindow, IntPtr.Zero);
      return s_foundWindow;
    }

    private static bool EnumWindow(IntPtr hWnd, IntPtr lParam) {
      int processId;
      GetWindowThreadProcessId(hWnd, out processId);
      if (processId == s_targetProcessId && IsWindowVisible(hWnd)) {
        StringBuilder title = new StringBuilder(256);
        GetWindowText(hWnd, title, title.Capacity);
        if (title.Length > 0) {
          s_foundWindow = hWnd;
          return false;
        }
      }
      return true;
    }
  }
}
"@
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$process = $null
try {
  $process = Start-Process -FilePath $citraExe -ArgumentList "`"$threeDsx`"" -PassThru
  $window = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($WindowTimeoutSeconds)

  while ((Get-Date) -lt $deadline) {
    $process.Refresh()
    if ($process.HasExited) { throw "Citra exited before a visible window was found" }
    $window = [OpenStarboundN3dsCitraCapture.NativeMethods]::FindVisibleWindowForProcess($process.Id)
    if ($window -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
  }

  if ($window -eq [IntPtr]::Zero) { throw "Timed out waiting for a visible Citra window for PID $($process.Id)" }

  $rect = New-Object OpenStarboundN3dsCitraCapture.NativeMethods+RECT
  [OpenStarboundN3dsCitraCapture.NativeMethods]::ShowWindow($window, 9) | Out-Null
  [OpenStarboundN3dsCitraCapture.NativeMethods]::MoveWindow($window, 10, 10, $WindowWidth, $WindowHeight, $true) | Out-Null
  [OpenStarboundN3dsCitraCapture.NativeMethods]::SetForegroundWindow($window) | Out-Null
  Start-Sleep -Seconds $CaptureDelaySeconds

  [OpenStarboundN3dsCitraCapture.NativeMethods]::GetWindowRect($window, [ref]$rect) | Out-Null
  $captureWidth = [Math]::Max(1, $rect.Right - $rect.Left)
  $captureHeight = [Math]::Max(1, $rect.Bottom - $rect.Top)
  $bitmap = New-Object System.Drawing.Bitmap $captureWidth, $captureHeight
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
  $windowPng = Join-Path $capture "window.png"
  $bitmap.Save($windowPng, [System.Drawing.Imaging.ImageFormat]::Png)
  $graphics.Dispose()
  $bitmap.Dispose()

  if (!$KeepCitraOpen) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
} finally {
  if ($process -and !$process.HasExited -and !$KeepCitraOpen) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
}

foreach ($logRoot in @((Join-Path $env:APPDATA "Citra"), (Join-Path $env:APPDATA "Citra\log"))) {
  if (Test-Path $logRoot) {
    Get-ChildItem -Path $logRoot -Filter "citra_log*.txt" -File -ErrorAction SilentlyContinue | Copy-Item -Destination $capture -Force
  }
}

$windowPng = Join-Path $capture "window.png"
if (!(Test-Path $windowPng)) { throw "Capture failed: $windowPng was not created" }

Write-Host "Citra window found: True"
Write-Host "Capture: $windowPng"
Write-Host "Capture exists: $(Test-Path $windowPng)"
Get-ChildItem $capture