# HapWallpaper

Fondo de pantalla animado para Windows 11 que reproduce video **HAP directamente con Direct3D 11**: los cuadros ya vienen en el formato de textura comprimida que la placa de video lee de forma nativa (BC1/DXT1), así que **no usa el decodificador de video ni un reproductor multimedia**.

*Animated wallpaper for Windows 11 that plays HAP video straight through Direct3D 11 (BC1 textures): no video decoder, no general-purpose media player.*

## Qué tiene de distinto

[HAP](https://hap.video/) no es nuevo: es un formato abierto de Vidvox pensado para reproducir video como texturas comprimidas por GPU, y se usa hace años en shows en vivo y software de VJ (Resolume, TouchDesigner, VDMX, etc.).

Lo que aporta HapWallpaper es llevar esa ruta a un **fondo de escritorio de Windows**, sin depender de un reproductor generalista (mpv, VLC, navegador) como hacen otros programas de fondos animados:

| | Fondo animado típico (mpv/VLC/navegador) | HapWallpaper |
|---|---|---|
| Decodificación | Decodificador de video de la placa (NVDEC, etc.) o CPU | Ninguna: el cuadro se copia tal cual a una textura BC1 |
| Conversión de color | YUV → RGB en cada cuadro | No hace falta: BC1 ya es RGB |
| Presentación | La que imponga el reproductor | Swap chain propio en el escritorio (WorkerW), sin copias intermedias |
| Monitores G-Sync/FreeSync | Un video de 30/60 fps arrastra la pantalla a 30/60 Hz | Presenta en cada refresco; si el cuadro no cambió, no dibuja y le pasa a Windows un rectángulo sucio mínimo, así el monitor sigue a su frecuencia máxima |
| En pausa | Varía | 0 % de GPU y suelta la memoria del archivo |

Medido en una RTX 3060 con monitor 2560×1440 a 200 Hz y un video de 30 fps:

| | Video H.264 (Media Foundation) | Video AV1 (Media Foundation) | **HAP** |
|---|---|---|---|
| Decodificador de video | 28–44 % | 6–18 % | **0 %** |
| CPU | 2,4 % | 0,4 % | 0,5 % |
| Consumo de la placa (reposo: 18 W) | 22,2 W | 20,7 W | **19,95 W** |

Los porcentajes del Administrador de tareas se calculan sobre la placa a su velocidad mínima (210 MHz), por eso parecen altos para tan poco consumo real.

## Otras funciones

- **Pausa automática** con ventanas maximizadas o a pantalla completa (juegos, videos), sesión bloqueada o pantalla apagada. Antes de pausarse siempre muestra un cuadro, así detrás de la barra transparente nunca queda negro.
- **Barra de tareas transparente** (opcional): `HapWallpaperTB.dll` se carga en Explorer con la API de diagnóstico XAML (como TranslucentTB) y oculta el fondo de la barra.
- **Respaldo con video común:** si no hay archivo HAP, reproduce cualquier video que Windows soporte (MP4 H.264, AV1, etc.) con Media Foundation y decodificación por hardware, con la misma presentación a frecuencia máxima.
- Ícono en la bandeja (doble clic pausa/reanuda) y arranque automático con Windows.

## Preparar el video

Lo más complicado de usar HAP es preparar el archivo, así que `preparar_video.ps1` lo hace en un paso con [ffmpeg](https://ffmpeg.org/) (si no lo tenés: `winget install --id Gyan.FFmpeg -e`):

```powershell
.\preparar_video.ps1 -Video C:\videos\mi_fondo.mp4
```

El script:

- detecta la resolución del monitor principal y escala/recorta el video para llenar la pantalla sin bordes;
- lo pasa a 30 fps (cambialo con `-Fps`; el reproductor lee los fps del archivo);
- genera `fondo.hap` junto al script, verifica que el reproductor lo pueda leer y le agrega un bloque final con resolución, fps y cantidad de cuadros;
- avisa cuánto va a ocupar y si no hay espacio en el disco.

| Opción | Para qué |
|---|---|
| `-Formato hap` / `av1` / `ambos` | `hap` = 0 % de decodificador pero archivo grande (~1,8 MB por cuadro a 1440p). `av1` = `fondo.mp4` chico que usa el decodificador de la placa. `ambos` deja el MP4 como respaldo |
| `-Fps 30` | Cuadros por segundo del archivo (30 alcanza para fondos lentos; 60 duplica el tamaño) |
| `-Inicio 5 -Duracion 20` | Usar solo un tramo del video (en segundos). Menos duración = archivo más chico |
| `-SombraReloj` | Oscurece suavemente la esquina del reloj y la bandeja, para leerlos mejor con la barra transparente |
| `-Ancho 1920 -Alto 1080` | Forzar una resolución (si la detección falla o es para otra PC) |
| `-Salida C:\ruta` | Carpeta de salida (por defecto, la del script) |

Usá videos propios o que tengas permiso de usar como fondo. Los que hacen bucle sin corte se ven mejor.

<details>
<summary>Formato del archivo y cómo hacerlo a mano</summary>

`fondo.hap` es la secuencia de paquetes HAP (DXT1, sin compresión Snappy) que escribe `ffmpeg -f rawvideo`, con la resolución exacta del monitor principal (múltiplos de 4):

```powershell
ffmpeg -i video.mp4 -vf "scale=2560:1440:force_original_aspect_ratio=increase,crop=2560:1440,fps=30" -pix_fmt rgba -c:v hap -format hap -compressor none -f rawvideo fondo.hap
```

Al final, `preparar_video.ps1` agrega 32 bytes: `HAPWALL1` y seis enteros de 32 bits (ancho, alto, fps numerador, fps denominador, cuadros, bytes por cuadro). Si falta ese bloque, el reproductor asume 30 fps.
</details>

## Compilar

Con MSYS2 (UCRT64):

```powershell
g++ -O2 -s -municode -mwindows -static -o HapWallpaper.exe hapwallpaper.cpp -ld3d11 -ldxgi -lmfplat -lmfuuid -ldwmapi -lshell32 -lwtsapi32 -lole32 -loleaut32 -luuid -luser32 -lgdi32
g++ -O2 -s -shared -static -municode -o HapWallpaperTB.dll hapwallpaper_tb.cpp -lole32 -loleaut32 -luuid -luser32
```

## Uso

Dejá `fondo.hap` (y/o `fondo.mp4`) junto a `HapWallpaper.exe` y abrilo. Si están los dos, usa el `.hap`.

```
HapWallpaper.exe [video.mp4 | fondo.hap] [--debug] [--nopause] [--notaskbar] [--noautostart]
```

| Opción | Efecto |
|---|---|
| `--debug` | Escribe `log.txt` junto al `.exe` |
| `--nopause` | No se pausa con ventanas maximizadas ni pantalla completa |
| `--notaskbar` | No toca la barra de tareas |
| `--noautostart` | No se anota en el inicio de Windows |

Para cerrarlo: clic derecho en el ícono de la bandeja → Salir.

## Créditos y licencia

- [HAP](https://hap.video/) — formato abierto creado por Tom Butterworth para Vidvox.
- [ffmpeg](https://ffmpeg.org/) — codificador HAP usado por `preparar_video.ps1`.

Código bajo licencia [MIT](LICENSE). No incluye videos.
