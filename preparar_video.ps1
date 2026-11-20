<#
.SYNOPSIS
  Convierte cualquier video en el fondo de HapWallpaper (fondo.hap y/o fondo.mp4).

.DESCRIPTION
  - Detecta la resolucion del monitor principal (o usa -Ancho/-Alto).
  - Escala y recorta el video para llenar la pantalla sin bordes negros.
  - Genera fondo.hap (cuadros BC1: 0 % de decodificador de video, ~1,8 MB por
    cuadro a 2560x1440) y/o fondo.mp4 en AV1 (archivo chico, usa el decodificador).
  - Opcional: recorta un tramo y oscurece la zona del reloj.
  - Usa solo videos propios o que tengas permiso de usar como fondo.

.EXAMPLE
  .\preparar_video.ps1 -Video C:\videos\mi_fondo.mp4
.EXAMPLE
  .\preparar_video.ps1 -Video mi_fondo.mp4 -Formato ambos -Fps 30 -Inicio 5 -Duracion 20
.EXAMPLE
  .\preparar_video.ps1 -Video mi_fondo.mp4 -SombraReloj
#>
param(
    [Parameter(Mandatory = $true)] [string] $Video,
    [ValidateSet('hap', 'av1', 'ambos')] [string] $Formato = 'hap',
    [int] $Fps = 30,
    [int] $Ancho = 0,
    [int] $Alto = 0,
    [double] $Inicio = 0,
    [double] $Duracion = 0,
    # oscurece suavemente la esquina inferior derecha (reloj y bandeja)
    [switch] $SombraReloj,
    [string] $Salida = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'

# --- ffmpeg
$ffmpeg = (Get-Command ffmpeg -ErrorAction SilentlyContinue).Source
if (-not $ffmpeg) {
    Write-Host "No se encontro ffmpeg. Instalalo con:" -ForegroundColor Red
    Write-Host "  winget install --id Gyan.FFmpeg -e"
    exit 1
}
$encoders = & $ffmpeg -hide_banner -encoders 2>$null | Out-String
if ($Formato -ne 'av1' -and $encoders -notmatch '\bhap\b') { Write-Host "Tu ffmpeg no trae el codificador HAP. Usa la version 'full' (winget install Gyan.FFmpeg)." -ForegroundColor Red; exit 1 }
if ($Formato -ne 'hap' -and $encoders -notmatch 'libsvtav1') { Write-Host "Tu ffmpeg no trae libsvtav1 (AV1). Usa la version 'full' (winget install Gyan.FFmpeg)." -ForegroundColor Red; exit 1 }
if (-not (Test-Path $Video)) { Write-Host "No existe el video: $Video" -ForegroundColor Red; exit 1 }
$Video = (Resolve-Path $Video).Path

# --- resolucion del monitor principal
if (-not $Ancho -or -not $Alto) {
    $vc = Get-CimInstance Win32_VideoController | Where-Object CurrentHorizontalResolution | Select-Object -First 1
    $Ancho = [int]$vc.CurrentHorizontalResolution; $Alto = [int]$vc.CurrentVerticalResolution
    if (-not $Ancho) { Write-Host "No pude detectar la resolucion. Pasala con -Ancho y -Alto." -ForegroundColor Red; exit 1 }
}
if ($Ancho % 4 -or $Alto % 4) { Write-Host "Ancho y alto deben ser multiplos de 4 para HAP." -ForegroundColor Red; exit 1 }

# --- info del video de origen
$probe = & (Join-Path (Split-Path $ffmpeg) 'ffprobe.exe') -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate:format=duration -of default=nw=1 $Video
$info = @{}; $probe | ForEach-Object { $k, $v = $_ -split '=', 2; $info[$k] = $v }
$durTotal = [double]::Parse($info['duration'], [Globalization.CultureInfo]::InvariantCulture)
$dur = if ($Duracion -gt 0) { $Duracion } else { $durTotal - $Inicio }
$cuadros = [math]::Floor($dur * $Fps)
Write-Host ("Origen: {0}x{1}, {2:N1} s  ->  pantalla {3}x{4}, {5} fps, {6} cuadros" -f $info['width'], $info['height'], $durTotal, $Ancho, $Alto, $Fps, $cuadros)
New-Item -ItemType Directory -Force $Salida | Out-Null
if ($Formato -ne 'av1') {
    $mb = $cuadros * ($Ancho * $Alto / 2 + 8) / 1MB
    Write-Host ("fondo.hap va a ocupar ~{0:N0} MB" -f $mb)
    $libre = (Get-PSDrive ((Resolve-Path $Salida).Drive.Name)).Free / 1MB
    if ($mb * 1.1 -gt $libre) { Write-Host "No hay espacio suficiente en el disco de salida." -ForegroundColor Red; exit 1 }
}

# --- filtros: llenar la pantalla (escalar + recortar al centro), fps, extras
$vf = "scale=${Ancho}:${Alto}:force_original_aspect_ratio=increase:flags=lanczos,crop=${Ancho}:${Alto},setsar=1,fps=$Fps"
if ($SombraReloj) {
    # degradado oscuro en la esquina inferior derecha (zona del reloj y la bandeja)
    $sw = [int]($Ancho * 0.35); $sh = [int]($Alto * 0.12)
    $vf += ",format=yuv420p,geq=lum='lum(X,Y)*(1-0.55*clip((X-($Ancho-$sw))/$sw,0,1)*clip((Y-($Alto-$sh))/$sh,0,1))':cb='cb(X,Y)':cr='cr(X,Y)'"
}
$trim = @()
if ($Inicio -gt 0) { $trim += @('-ss', "$Inicio") }
$trimOut = @()
if ($Duracion -gt 0) { $trimOut += @('-t', "$Duracion") }

$sw = [Diagnostics.Stopwatch]::StartNew()
$ErrorActionPreference = 'Continue' # ffmpeg escribe el progreso por stderr
if ($Formato -ne 'av1') {
    $out = Join-Path $Salida 'fondo.hap'
    Write-Host "Generando $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -pix_fmt rgba -c:v hap -format hap -compressor none -an -f rawvideo $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg fallo." -ForegroundColor Red; exit 1 }
    $stride = $Ancho * $Alto / 2 + 8
    $len = (Get-Item $out).Length
    if ($len % $stride) { Write-Host "Aviso: el archivo HAP no tiene cuadros de tamano fijo; HapWallpaper usara el video MP4." -ForegroundColor Yellow }
    else {
        # Bloque final de 32 bytes con los datos del video, para que el reproductor
        # sepa los fps: "HAPWALL1" + ancho, alto, fps (num/den), cuadros, bytes por cuadro.
        $frames = [uint32]($len / $stride)
        $fs = [IO.File]::Open($out, 'Append', 'Write')
        $bw = New-Object IO.BinaryWriter($fs)
        $bw.Write([Text.Encoding]::ASCII.GetBytes('HAPWALL1'))
        foreach ($v in @($Ancho, $Alto, $Fps, 1, $frames, $stride)) { $bw.Write([uint32]$v) }
        $bw.Close()
        Write-Host ("OK: {0} cuadros a {1} fps, {2:N0} MB" -f $frames, $Fps, ($len / 1MB)) -ForegroundColor Green
    }
}
if ($Formato -ne 'hap') {
    $out = Join-Path $Salida 'fondo.mp4'
    Write-Host "Generando $out ..."
    & $ffmpeg -v error -stats -y @trim -i $Video @trimOut -vf $vf -c:v libsvtav1 -preset 6 -crf 30 -g ($Fps * 4) -pix_fmt yuv420p -an $out
    if ($LASTEXITCODE) { Write-Host "ffmpeg fallo." -ForegroundColor Red; exit 1 }
    Write-Host ("OK: {0:N1} MB" -f ((Get-Item $out).Length / 1MB)) -ForegroundColor Green
}
Write-Host ("Listo en {0:N0} s. Reinicia HapWallpaper (clic derecho en la bandeja > Salir, y abrilo de nuevo)." -f $sw.Elapsed.TotalSeconds)
