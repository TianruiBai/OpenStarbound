$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$captureDir = "build\citra-captures\capture_$timestamp"
New-Item -ItemType Directory -Path $captureDir -Force

$citraPath = "citra-windows-msvc-20240303-0ff3440\citra-qt.exe"
$cxiPath = "dist\starbound.cxi"

if (-not (Test-Path $citraPath)) { Write-Error "Citra not found at $citraPath"; exit 1 }
if (-not (Test-Path $cxiPath)) { Write-Error "CXI not found at $cxiPath"; exit 1 }

$process = Start-Process -FilePath $citraPath -ArgumentList "`"$cxiPath`"" -PassThru
$processId = $process.Id

Write-Host "Started Citra (PID: $processId). Waiting 20 seconds..."
Start-Sleep -Seconds 20

$stillRunning = Get-Process -Id $processId -ErrorAction SilentlyContinue 
$screenshotCreated = $false

if ($stillRunning) {
    Write-Host "Citra is still running. Capturing screenshot..."
    try {
        Add-Type -AssemblyName System.Windows.Forms
        Add-Type -AssemblyName System.Drawing
        $screen = [System.Windows.Forms.Screen]::PrimaryScreen
        $bitmap = New-Object System.Drawing.Bitmap $screen.Bounds.Width, $screen.Bounds.Height
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $graphics.CopyFromScreen($screen.Bounds.X, $screen.Bounds.Y, 0, 0, $bitmap.Size)
        $screenshotPath = Join-Path (Resolve-Path $captureDir).Path "screenshot.png"
        $bitmap.Save($screenshotPath, [System.Drawing.Imaging.ImageFormat]::Png)
        $graphics.Dispose()
        $bitmap.Dispose()
        $screenshotCreated = Test-Path $screenshotPath
    } catch {
        Write-Warning "Failed to capture screenshot: $_"
    }
    
    Write-Host "Terminating Citra..."
    Stop-Process -Id $processId -Force
} else {
    Write-Host "Citra terminated early."
}

$citraLogDir = "$env:APPDATA\Citra\log"
$copiedLogs = @()
if (Test-Path $citraLogDir) {
    $logs = Get-ChildItem -Path $citraLogDir -File | Where-Object { $_.LastWriteTime -gt (Get-Date).AddMinutes(-2) }
    foreach ($log in $logs) {
        Copy-Item $log.FullName -Destination $captureDir
        $copiedLogs += $log.Name
    }
}

Write-Host "--- Results ---"
Write-Host "Capture Directory: $captureDir"
Write-Host "Process stayed running: $([bool]$stillRunning)"
Write-Host "Screenshot created: $screenshotCreated"
Write-Host "Copied Logs: $($copiedLogs -join ', ')"
