param(
  [string]$CitraPath = "citra-windows-msvc-20240303-0ff3440\citra-qt.exe",
  [string]$ElfPath = "dist\starbound",
  [string]$SmdhPath = "dist\starbound.smdh",
  [string]$RomfsPath = "assets",
  [string]$ThreeDsxToolPath = "C:\devkitPro\tools\bin\3dsxtool.exe",
  [string]$ThreeDsxPath = "build\citra-repack\starbound.3dsx",
  [string]$MakeromPath = "makerom-bin\makerom.exe",
  [string]$CxiPath = "build\citra-repack\starbound.cxi",
  [Alias("OutputDir")]
  [string]$CaptureDir = "build\citra-captures\n3ds-current",
  [string]$BasePakPath = "",
  [int]$WindowWidth = 1280,
  [int]$WindowHeight = 900,
  [int]$WindowTimeoutSeconds = 30,
  [int]$CaptureDelaySeconds = 15,
  [switch]$NoRepack,
  [switch]$UseCxi,
  [switch]$AutoStartSinglePlayer,
  [switch]$UseExistingCitraLayout,
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
$makerom = Resolve-RepoPath $MakeromPath
$cxi = Resolve-RepoPath $CxiPath
$capture = Resolve-RepoPath $CaptureDir
$usingDefaultAssetsRomfs = $RomfsPath -eq "assets"
$stripTool = $null
if ($env:DEVKITARM) {
  $candidateStripTool = Join-Path $env:DEVKITARM "bin\arm-none-eabi-strip.exe"
  if (Test-Path $candidateStripTool) {
    $stripTool = $candidateStripTool
  }
}
if (!$stripTool) {
  $defaultStripTool = "C:\devkitPro\devkitARM\bin\arm-none-eabi-strip.exe"
  if (Test-Path $defaultStripTool) {
    $stripTool = $defaultStripTool
  }
}

function New-RepackElf([string]$sourceElf, [string]$outputPath) {
  if (!$stripTool) {
    Write-Warning "arm-none-eabi-strip not found; repack will use the original ELF and may fail on large debug images."
    return $sourceElf
  }

  New-Item -ItemType Directory -Path (Split-Path $outputPath -Parent) -Force | Out-Null
  Copy-Item -LiteralPath $sourceElf -Destination $outputPath -Force

  & $stripTool --strip-debug $outputPath
  if ($LASTEXITCODE -ne 0) { throw "arm-none-eabi-strip --strip-debug failed with exit code $LASTEXITCODE" }

  $strippedSize = (Get-Item -LiteralPath $outputPath).Length
  Write-Host "Using repack ELF: $outputPath ($strippedSize bytes)"
  return $outputPath
}

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
if (!$NoRepack -and $UseCxi) {
  if (!(Test-Path $makerom)) { throw "makerom not found: $makerom" }
  if (!(Test-Path $elf)) { throw "N3DS ELF not found: $elf" }
  if (!(Test-Path $romfs)) { throw "RomFS path not found: $romfs" }
  New-Item -ItemType Directory -Path (Split-Path $cxi -Parent) -Force | Out-Null
  $repackElf = New-RepackElf $elf (Join-Path (Split-Path $cxi -Parent) "starbound-stripdebug.elf")

  $romfsRoot = $romfs
  $repoPrefix = $repoRoot.Path.TrimEnd('\') + '\'
  if ($romfsRoot.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    $romfsRoot = $romfsRoot.Substring($repoPrefix.Length)
  }
  $romfsRoot = $romfsRoot.Replace('\', '/')

  $rsfPath = Join-Path (Split-Path $cxi -Parent) "starbound-citra-capture.rsf"
  $rsf = @"
BasicInfo:
  Title: OpenStarbound
  CompanyCode: OS
  ProductCode: CTR-P-OSB3D
  Logo: homebrew

RomFs:
  RootPath: $romfsRoot

TitleInfo:
  Platform: ctr
  Category: Application
  UniqueId: 0x0F0A1
  Version: 0

Option:
  UseOnSD: true

SystemControlInfo:
  StackSize: 0x40000
  RemasterVersion: 0
  SaveDataSize: 1MB

AccessControlInfo:
  CoreVersion: 2
  Priority: 48
  MemoryType: Application
  SystemMode: 64MB
  SystemModeExt: 178MB
  CpuSpeed: 804MHz
  EnableL2Cache: true
  FileSystemAccess:
    - DirectSdmc
    - DirectSdmcWrite
"@
  $utf8NoBom = New-Object System.Text.UTF8Encoding $false
  [System.IO.File]::WriteAllText($rsfPath, $rsf, $utf8NoBom)

  & $makerom -f cxi -target t -rsf $rsfPath -elf $repackElf -desc app:4 -o $cxi
  if ($LASTEXITCODE -ne 0) { throw "makerom failed with exit code $LASTEXITCODE" }
} elseif (!$NoRepack) {
  if (!(Test-Path $threeDsxTool)) { throw "3dsxtool not found: $threeDsxTool" }
  if (!(Test-Path $elf)) { throw "N3DS ELF not found: $elf" }
  if (!(Test-Path $smdh)) { throw "SMDH not found: $smdh" }
  if (!(Test-Path $romfs)) { throw "RomFS path not found: $romfs" }
  New-Item -ItemType Directory -Path (Split-Path $threeDsx -Parent) -Force | Out-Null
  $repackElf = New-RepackElf $elf (Join-Path (Split-Path $threeDsx -Parent) "starbound-stripdebug.elf")
  & $threeDsxTool $repackElf $threeDsx "--smdh=$smdh" "--romfs=$romfs"
  if ($LASTEXITCODE -ne 0) { throw "3dsxtool failed with exit code $LASTEXITCODE" }
}

if ($UseCxi) {
  if (!(Test-Path $cxi)) { throw "CXI not found: $cxi" }
  $launchPath = $cxi
} else {
  if (!(Test-Path $threeDsx)) { throw "3DSX not found: $threeDsx" }
  $launchPath = $threeDsx
}
if (Test-Path $capture) { Remove-Item -Recurse -Force $capture }
New-Item -ItemType Directory -Path $capture -Force | Out-Null

function Set-CitraIniValue([System.Collections.Generic.List[string]]$lines, [string]$section, [string]$key, [string]$value) {
  $sectionHeader = "[$section]"
  $sectionIndex = -1
  for ($i = 0; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -eq $sectionHeader) {
      $sectionIndex = $i
      break
    }
  }

  if ($sectionIndex -lt 0) {
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -ne "") { $lines.Add("") }
    $lines.Add($sectionHeader)
    $sectionIndex = $lines.Count - 1
  }

  $insertIndex = $lines.Count
  for ($i = $sectionIndex + 1; $i -lt $lines.Count; ++$i) {
    if ($lines[$i] -match '^\[.*\]$') {
      $insertIndex = $i
      break
    }

    if ($lines[$i] -like "$key=*") {
      $lines[$i] = "$key=$value"
      return
    }
  }

  $lines.Insert($insertIndex, "$key=$value")
}

$citraConfigPath = Join-Path $env:APPDATA "Citra\config\qt-config.ini"
$citraConfigBackupBytes = $null
if (!$UseExistingCitraLayout -and (Test-Path $citraConfigPath)) {
  $citraConfigBackupBytes = [System.IO.File]::ReadAllBytes($citraConfigPath)
  $lines = New-Object 'System.Collections.Generic.List[string]'
  $lines.AddRange([System.IO.File]::ReadAllLines($citraConfigPath))

  $layoutScale = [Math]::Max(1, [Math]::Min([Math]::Floor(($WindowWidth - 40) / 400), [Math]::Floor(($WindowHeight - 120) / 480)))
  $topWidth = 400 * $layoutScale
  $topHeight = 240 * $layoutScale
  $bottomWidth = 320 * $layoutScale
  $bottomHeight = 240 * $layoutScale
  $bottomLeft = [Math]::Floor(($topWidth - $bottomWidth) / 2)
  $bottomTop = $topHeight

  Set-CitraIniValue $lines "Layout" "layout_option\default" "false"
  Set-CitraIniValue $lines "Layout" "layout_option" "0"
  Set-CitraIniValue $lines "Layout" "swap_screen\default" "false"
  Set-CitraIniValue $lines "Layout" "swap_screen" "false"
  Set-CitraIniValue $lines "Layout" "upright_screen\default" "false"
  Set-CitraIniValue $lines "Layout" "upright_screen" "false"
  Set-CitraIniValue $lines "Layout" "custom_layout\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_layout" "true"
  Set-CitraIniValue $lines "Layout" "custom_top_left\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_top_left" "0"
  Set-CitraIniValue $lines "Layout" "custom_top_top\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_top_top" "0"
  Set-CitraIniValue $lines "Layout" "custom_top_right\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_top_right" "$topWidth"
  Set-CitraIniValue $lines "Layout" "custom_top_bottom\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_top_bottom" "$topHeight"
  Set-CitraIniValue $lines "Layout" "custom_bottom_left\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_bottom_left" "$bottomLeft"
  Set-CitraIniValue $lines "Layout" "custom_bottom_top\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_bottom_top" "$bottomTop"
  Set-CitraIniValue $lines "Layout" "custom_bottom_right\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_bottom_right" "$($bottomLeft + $bottomWidth)"
  Set-CitraIniValue $lines "Layout" "custom_bottom_bottom\default" "false"
  Set-CitraIniValue $lines "Layout" "custom_bottom_bottom" "$($bottomTop + $bottomHeight)"

  $utf8NoBom = New-Object System.Text.UTF8Encoding $false
  [System.IO.File]::WriteAllLines($citraConfigPath, $lines, $utf8NoBom)
  Write-Host "Using temporary Citra custom layout: top ${topWidth}x${topHeight}, bottom ${bottomWidth}x${bottomHeight}"
}

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

if (-not ("OpenStarboundN3dsCitraCaptureV3.NativeMethods" -as [type])) {
  Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace OpenStarboundN3dsCitraCaptureV3 {
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

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);

    public static string WindowTitle(IntPtr hWnd) {
      StringBuilder title = new StringBuilder(512);
      GetWindowText(hWnd, title, title.Capacity);
      return title.ToString();
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT rect);

    [DllImport("user32.dll")]
    public static extern bool MoveWindow(IntPtr hWnd, int x, int y, int width, int height, bool repaint);

    [DllImport("user32.dll")]
    public static extern bool ShowWindow(IntPtr hWnd, int command);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hWnd, IntPtr hdcBlt, uint nFlags);

    private static int s_targetProcessId;
    private static IntPtr s_foundWindow;
    private static int s_foundWindowArea;

    public static IntPtr FindVisibleWindowForProcess(int processId) {
      s_targetProcessId = processId;
      s_foundWindow = IntPtr.Zero;
      s_foundWindowArea = 0;
      EnumWindows(EnumWindow, IntPtr.Zero);
      return s_foundWindow;
    }

    private static bool EnumWindow(IntPtr hWnd, IntPtr lParam) {
      int processId;
      GetWindowThreadProcessId(hWnd, out processId);
      if (processId == s_targetProcessId && IsWindowVisible(hWnd)) {
        RECT rect;
        if (GetWindowRect(hWnd, out rect)) {
          int area = Math.Max(0, rect.Right - rect.Left) * Math.Max(0, rect.Bottom - rect.Top);
          if (area > s_foundWindowArea) {
            s_foundWindowArea = area;
            s_foundWindow = hWnd;
          }
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

function Copy-N3dsCaptureLogs() {
  foreach ($logRoot in @((Join-Path $env:APPDATA "Citra"), (Join-Path $env:APPDATA "Citra\log"))) {
    if (Test-Path $logRoot) {
      Get-ChildItem -Path $logRoot -Filter "citra_log*.txt" -File -ErrorAction SilentlyContinue | Copy-Item -Destination $capture -Force -ErrorAction SilentlyContinue
    }
  }

  $sdmcOpenStarbound = Join-Path $env:APPDATA "Citra\sdmc\OpenStarbound"
  if (Test-Path $sdmcOpenStarbound) {
    Get-ChildItem -Path $sdmcOpenStarbound -Filter "starbound_n3ds*.log" -File -ErrorAction SilentlyContinue | Copy-Item -Destination $capture -Force -ErrorAction SilentlyContinue
  }
}

$process = $null
try {
  $process = Start-Process -FilePath $citraExe -ArgumentList "`"$launchPath`"" -PassThru
  $window = [IntPtr]::Zero
  $deadline = (Get-Date).AddSeconds($WindowTimeoutSeconds)

  while ((Get-Date) -lt $deadline) {
    $process.Refresh()
    if ($process.HasExited) { throw "Citra exited before a visible window was found" }
    $window = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::FindVisibleWindowForProcess($process.Id)
    if ($window -ne [IntPtr]::Zero) { break }
    Start-Sleep -Milliseconds 250
  }

  if ($window -eq [IntPtr]::Zero) { throw "Timed out waiting for a visible Citra window for PID $($process.Id)" }

  $rect = New-Object OpenStarboundN3dsCitraCaptureV3.NativeMethods+RECT
  [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::ShowWindow($window, 9) | Out-Null
  [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::MoveWindow($window, 10, 10, $WindowWidth, $WindowHeight, $true) | Out-Null
  [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::SetForegroundWindow($window) | Out-Null
  Start-Sleep -Seconds $CaptureDelaySeconds

  $process.Refresh()
  if ($process.HasExited) { throw "Citra exited before capture" }

  $refreshedWindow = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::FindVisibleWindowForProcess($process.Id)
  if ($refreshedWindow -ne [IntPtr]::Zero) { $window = $refreshedWindow }

  [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::ShowWindow($window, 9) | Out-Null
  [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::SetForegroundWindow($window) | Out-Null

  if (![OpenStarboundN3dsCitraCaptureV3.NativeMethods]::IsWindow($window)) {
    $window = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::FindVisibleWindowForProcess($process.Id)
    if ($window -eq [IntPtr]::Zero) { throw "Citra window was no longer valid for PID $($process.Id)" }
  }

  if (![OpenStarboundN3dsCitraCaptureV3.NativeMethods]::GetWindowRect($window, [ref]$rect)) {
    throw "Could not query Citra window bounds"
  }
  $captureWidth = [Math]::Max(1, $rect.Right - $rect.Left)
  $captureHeight = [Math]::Max(1, $rect.Bottom - $rect.Top)
  $captureTitle = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::WindowTitle($window)
  $bitmap = New-Object System.Drawing.Bitmap $captureWidth, $captureHeight
  $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
  $captureMethod = "CopyFromScreen"
  $captured = $false
  try {
    $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    $captured = $true
  } catch {
    Write-Warning "CopyFromScreen failed, trying PrintWindow fallback: $($_.Exception.Message)"
    $captureMethod = "PrintWindow"
    $hdc = $graphics.GetHdc()
    try {
      $captured = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::PrintWindow($window, $hdc, 2)
      if (!$captured) {
        $captured = [OpenStarboundN3dsCitraCaptureV3.NativeMethods]::PrintWindow($window, $hdc, 0)
      }
    } finally {
      $graphics.ReleaseHdc($hdc)
    }
  }
  if (!$captured) {
    Write-Warning "Window capture failed; saving a fallback diagnostic image so logs are still collected."
    $captureMethod = "FallbackBlank"
    $graphics.Clear([System.Drawing.Color]::Black)
  }
  $windowPng = Join-Path $capture "window.png"
  $bitmap.Save($windowPng, [System.Drawing.Imaging.ImageFormat]::Png)
  $graphics.Dispose()
  $bitmap.Dispose()

  if (!$KeepCitraOpen) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue }
} finally {
  if ($process -and !$process.HasExited -and !$KeepCitraOpen) {
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
  }
  if ($citraConfigBackupBytes) {
    [System.IO.File]::WriteAllBytes($citraConfigPath, $citraConfigBackupBytes)
  }
  Copy-N3dsCaptureLogs
}

Copy-N3dsCaptureLogs

$windowPng = Join-Path $capture "window.png"
if (!(Test-Path $windowPng)) { throw "Capture failed: $windowPng was not created" }

Write-Host "Citra window found: True"
Write-Host "Citra window title: $captureTitle"
Write-Host "Citra window size: ${captureWidth}x${captureHeight}"
Write-Host "Capture: $windowPng"
Write-Host "Capture method: $captureMethod"
Write-Host "Capture exists: $(Test-Path $windowPng)"
Get-ChildItem $capture