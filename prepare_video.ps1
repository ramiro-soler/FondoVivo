<#
.SYNOPSIS
  Turns any video into a HapWallpaper wallpaper (wallpaper.hap and/or wallpaper.mp4).

.DESCRIPTION
  - Detects the primary monitor's resolution (or uses -Width/-Height).
  - Scales and center-crops the video to fill the screen with no black bars.
  - Writes wallpaper.hap (BC1 frames: 0 % GPU video decoder, ~1.8 MB per frame at
    2560x1440) and/or wallpaper.mp4 in AV1 (small file, uses the GPU decoder).
  - Optional: use only part of the video, darken the clock/tray corner.
  - Only use videos you made or have permission to use as a wallpaper.

.EXAMPLE
  .\prepare_video.ps1 -Video C:\videos\my_wallpaper.mp4
.EXAMPLE
  .\prepare_video.ps1 -Video my_wallpaper.mp4 -Format both -Fps 30 -Start 5 -Duration 20
.EXAMPLE
  .\prepare_video.ps1 -Video my_wallpaper.mp4 -ClockShadow
#>
param(
    [Parameter(Mandatory = $true)] [string] $Video,
    [ValidateSet('hap', 'av1', 'both')] [string] $Format = 'hap',
    [int] $Fps = 30,
    [int] $Width = 0,
    [int] $Height = 0,
    [double] $Start = 0,
    [double] $Duration = 0,
    # softly darkens the bottom-right corner (clock and tray)
    [switch] $ClockShadow,
    [string] $Output = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

# --- ffmpeg
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    Write-Host "ffmpeg was not found. Install it with:" -ForegroundColor Red
    Write-Host "  winget install --id Gyan.FFmpeg -e"
    exit 1
}
$encoders = & $ffmpeg -hide_banner -encoders 2>$null | Out-String
if ($Format -ne 'av1' -and $encoders -notmatch '\bhap\b') { Write-Host "Your ffmpeg has no HAP encoder. Use the 'full' build (winget install Gyan.FFmpeg)." -ForegroundColor Red; exit 1 }
if ($Format -ne 'hap' -and $encoders -notmatch 'libsvtav1') { Write-Host "Your ffmpeg has no libsvtav1 (AV1). Use the 'full' build (winget install Gyan.FFmpeg)." -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Video)) { Write-Host "Video not found: $Video" -ForegroundColor Red; exit 1 }
$Video = (Resolve-Path $Video).Path

# --- primary monitor resolution
if (-not $Width -or -not $Height) {
    $vc = Get-CimInstance Win32_VideoController | Where-Object CurrentHorizontalResolution | Select-Object -First 1
    $Width = [int]$vc.CurrentHorizontalResolution; $Height = [int]$vc.CurrentVerticalResolution
    if (-not $Width) { Write-Host "Couldn't detect the resolution. Pass -Width and -Height." -ForegroundColor Red; exit 1 }
}
if ($Format -ne 'av1' -and ($Width % 4 -or $Height % 4)) { Write-Host "Width and height must be multiples of 4 for HAP." -ForegroundColor Red; exit 1 }

# --- source info
$probe = & (Join-Path (Split-Path $ffmpeg) 'ffprobe.exe') -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate:format=duration -of default=nw=1 $Video
$info = @{}; $probe | ForEach-Object { $k, $v = $_ -split '=', 2; $info[$k] = $v }
$total = [double]::Parse($info['duration'], [Globalization.CultureInfo]::InvariantCulture)
$dur = if ($Duration -gt 0) { $Duration } else { $total - $Start }
$frameCount = [math]::Floor($dur * $Fps)
Write-Host ("Source: {0}x{1}, {2:N1} s  ->  screen {3}x{4}, {5} fps, {6} frames" -f $info['width'], $info['height'], $total, $Width, $Height, $Fps, $frameCount)
New-Item -ItemType Directory -Force $Output | Out-Null
if ($Format -ne 'av1') {
    $mb = $frameCount * ($Width * $Height / 2 + 8) / 1MB
    Write-Host ("wallpaper.hap will take ~{0:N0} MB" -f $mb)
    $free = (Get-PSDrive ((Resolve-Path $Output).Drive.Name)).Free / 1MB
    if ($mb * 1.1 -gt $free) { Write-Host "Not enough free space on the output drive." -ForegroundColor Red; exit 1 }
}

# --- filters: fill the screen (scale + center crop), fps, extras
$vf = "scale=${Width}:${Height}:force_original_aspect_ratio=increase:flags=lanczos,crop=${Width}:${Height},setsar=1,fps=$Fps"
if ($ClockShadow) {
    # dark gradient in the bottom-right corner (clock and tray area)
    $sw = [int]($Width * 0.35); $sh = [int]($Height * 0.12)
    $vf += ",format=yuv420p,geq=lum='lum(X,Y)*(1-0.55*clip((X-($Width-$sw))/$sw,0,1)*clip((Y-($Height-$sh))/$sh,0,1))':cb='cb(X,Y)':cr='cr(X,Y)'"
}
$trim = @()
if ($Start -gt 0) { $trim += @('-ss', "$Start") }
$trimOut = @()
if ($Duration -gt 0) { $trimOut += @('-t', "$Duration") }

$timer = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = 'Continue' # ffmpeg writes its progress to stderr
if ($Format -ne 'av1') {
    $out = Join-Path $Output 'wallpaper.hap'
    Write-Host "Writing $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -pix_fmt rgba -c:v hap -format hap -compressor none -an -f rawvideo $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg failed." -ForegroundColor Red; exit 1 }
    $stride = $Width * $Height / 2 + 8
    $len = (Get-Item $out).Length
    if ($len % $stride) { Write-Host "Warning: the HAP frames don't have a fixed size; HapWallpaper will fall back to the MP4." -ForegroundColor Yellow }
    else {
        # 32-byte trailer so the player knows the format:
        # "HAPWALL1" + width, height, fps numerator, fps denominator, frames, bytes per frame.
        $frames = [uint32]($len / $stride)
        $fs = [IO.File]::Open($out, 'Append', 'Write')
        $bw = New-Object IO.BinaryWriter -ArgumentList $fs
        $bw.Write([Text.Encoding]::ASCII.GetBytes('HAPWALL1'))
        foreach ($v in @($Width, $Height, $Fps, 1, $frames, $stride)) { $bw.Write([uint32]$v) }
        $bw.Close()
        Write-Host ("OK: {0} frames at {1} fps, {2:N0} MB" -f $frames, $Fps, ($len / 1MB)) -ForegroundColor Green
    }
}
if ($Format -ne 'hap') {
    $out = Join-Path $Output 'wallpaper.mp4'
    Write-Host "Writing $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -c:v libsvtav1 -preset 6 -crf 30 -g ($Fps * 4) -pix_fmt yuv420p -an $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg failed." -ForegroundColor Red; exit 1 }
    Write-Host ("OK: {0:N1} MB" -f ((Get-Item $out).Length / 1MB)) -ForegroundColor Green
}
Write-Host ("Done in {0:N0} s. Restart HapWallpaper (right-click the tray icon > Exit, then run it again)." -f $timer.Elapsed.TotalSeconds)
