# FondoVivo

Fondo de pantalla animado para Windows 11, chico y pensado para gastar lo mínimo posible.

*A tiny, low-overhead animated wallpaper for Windows 11 (C++, Direct3D 11, Media Foundation).*

## Qué hace

- Reproduce un video en bucle detrás de los íconos del escritorio.
- **Formato HAP (BC1/DXT1):** los cuadros ya están comprimidos en un formato que la placa lee directo, así que **no usa el decodificador de video** (0 % de NVDEC) y casi nada de CPU. Si no hay `.hap`, usa cualquier video que Windows pueda reproducir (MP4 H.264, AV1, etc.) con decodificación por hardware.
- **Monitores de alta frecuencia con G-Sync/FreeSync:** presenta en cada refresco del monitor, así un video de 30 o 60 fps no arrastra la pantalla a 30/60 Hz. En los refrescos en los que el cuadro no cambió no dibuja nada y le pasa a Windows un rectángulo sucio mínimo.
- **Se pausa solo** con ventanas maximizadas o a pantalla completa, con la sesión bloqueada y con la pantalla apagada. En pausa usa 0 % de GPU y suelta la memoria del archivo HAP. Antes de pausarse siempre muestra un cuadro, así la barra transparente nunca queda en negro.
- **Barra de tareas transparente** (opcional): `FondoVivoTB.dll` se carga en Explorer con la API de diagnóstico XAML, igual que TranslucentTB, y oculta el fondo de la barra.
- Ícono en la bandeja (doble clic pausa/reanuda) y arranque automático con Windows.

Medido en una RTX 3060 con un monitor 2560×1440 a 200 Hz y un video de 30 fps: 0 % de decodificación de video, ~8 % de 3D (con la placa a su velocidad mínima), 0,5 % de CPU y ~2 W más que sin fondo.

## Preparar el video

Lo más fácil es usar `preparar_video.ps1`, que hace todo con [ffmpeg](https://ffmpeg.org/) (si no lo tenés: `winget install --id Gyan.FFmpeg -e`):

```powershell
.\preparar_video.ps1 -Video C:\videos\mi_fondo.mp4
```

El script:

- detecta la resolución del monitor principal y escala/recorta el video para llenar la pantalla sin bordes;
- lo pasa a 30 fps (cambialo con `-Fps`);
- genera `fondo.hap` junto al script y verifica que FondoVivo lo pueda leer;
- avisa cuánto va a ocupar y si no hay espacio en el disco.

| Opción | Para qué |
|---|---|
| `-Formato hap` / `av1` / `ambos` | `hap` = 0 % de decodificador pero archivo grande (~1,8 MB por cuadro a 1440p). `av1` = `fondo.mp4` chico que usa el decodificador de la placa. `ambos` deja el MP4 como respaldo |
| `-Inicio 5 -Duracion 20` | Usar solo un tramo del video (en segundos). Menos duración = `.hap` más chico |
| `-SombraReloj` | Oscurece suavemente la esquina del reloj y la bandeja, para leerlos mejor con la barra transparente |
| `-Ancho 1920 -Alto 1080` | Forzar una resolución (si la detección falla o es para otra PC) |
| `-Salida C:\ruta` | Carpeta de salida (por defecto, la del script) |

Ejemplo completo:

```powershell
.\preparar_video.ps1 -Video mi_fondo.mp4 -Formato ambos -Inicio 2 -Duracion 18 -SombraReloj
```

Consejos: los videos que hacen bucle sin corte se ven mejor; 30 fps alcanza para fondos lentos; si el `.hap` queda muy grande, acortá la duración.

<details>
<summary>Hacerlo a mano con ffmpeg</summary>

El HAP debe tener exactamente la resolución del monitor principal (múltiplos de 4) y sin compresión Snappy:

```powershell
ffmpeg -i video.mp4 -vf "scale=2560:1440:force_original_aspect_ratio=increase,crop=2560:1440,fps=30" -pix_fmt rgba -c:v hap -format hap -compressor none -f rawvideo fondo.hap
ffmpeg -i video.mp4 -vf "scale=2560:1440:force_original_aspect_ratio=increase,crop=2560:1440,fps=30" -c:v libsvtav1 -crf 30 -an fondo.mp4
```
</details>

Dejá `fondo.hap` y/o `fondo.mp4` en la misma carpeta que `FondoVivo.exe` (si están los dos, usa el `.hap`).

## Compilar

Con MSYS2 (UCRT64):

```powershell
g++ -O2 -s -municode -mwindows -static -o FondoVivo.exe fondovivo.cpp -ld3d11 -ldxgi -lmfplat -lmfuuid -ldwmapi -lshell32 -lwtsapi32 -lole32 -loleaut32 -luuid -luser32 -lgdi32
g++ -O2 -s -shared -static -municode -o FondoVivoTB.dll fondovivo_tb.cpp -lole32 -loleaut32 -luuid -luser32
```

## Uso

```
FondoVivo.exe [video.mp4 | fondo.hap] [--debug] [--nopause] [--notaskbar] [--noautostart]
```

| Opción | Efecto |
|---|---|
| `--debug` | Escribe `log.txt` junto al `.exe` |
| `--nopause` | No se pausa con ventanas maximizadas ni pantalla completa |
| `--notaskbar` | No toca la barra de tareas |
| `--noautostart` | No se anota en el inicio de Windows |

Para cerrarlo: clic derecho en el ícono de la bandeja → Salir.

## Licencia

[MIT](LICENSE). Los videos no se incluyen: usá los tuyos o los que tengas permiso de usar.
