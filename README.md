# HapWallpaper

**English** · [Español](README.es.md)

Animated wallpaper for Windows 11 that plays **HAP video straight through Direct3D 11**. Frames are stored in the compressed texture format the GPU reads natively (BC1/DXT1), so it uses **no video decoder and no media player**.

[**Download the latest release**](https://github.com/ramiro-soler/HapWallpaper/releases/latest) — one small `.exe`, no runtime to install.

## Why it's different

[HAP](https://hap.video/) isn't new: it's an open format by Vidvox for playing video as GPU-compressed textures, used for years in live shows and VJ software (Resolume, TouchDesigner, VDMX…).

HapWallpaper brings that path to the **Windows desktop**, instead of relying on a general-purpose player (mpv, VLC, a browser or Media Foundation) like other wallpaper apps:

| | Typical animated wallpaper | HapWallpaper |
|---|---|---|
| Decoding | GPU video decoder (NVDEC, VCN, Quick Sync) or CPU | **None**: the frame is copied as-is into a BC1 texture |
| Color conversion | YUV → RGB every frame | Not needed: BC1 is already RGB |
| High-refresh G-Sync / FreeSync monitors | A 30/60 fps video drags the panel down to 30/60 Hz | Presents on every refresh; when the frame didn't change it draws nothing and hands Windows a 1×1 dirty rect, so the monitor stays at its maximum rate |
| Maximized or fullscreen windows | Often keeps playing (or stutters on resume) | Pauses **per monitor** instantly; the last frame stays visible behind a transparent taskbar |
| Paused | Varies | 0 % GPU and the file is released from memory |

Measured on an RTX 3060, 2560×1440 @ 200 Hz, 30 fps video:

| | H.264 (Media Foundation) | AV1 (Media Foundation) | **HAP** |
|---|---|---|---|
| GPU video decoder | 28–44 % | 6–18 % | **0 %** |
| CPU | 2.4 % | 0.4 % | 0.5 % |
| GPU board power (idle: 18 W) | 22.2 W | 20.7 W | **19.5–20 W** |

The trade-off: a HAP file is large (~1.8 MB per frame at 1440p, ~1 GB for 18 s at 30 fps) and is memory-mapped while playing. If you prefer a small file, HapWallpaper also plays any MP4/AV1 Windows can decode, with the same presentation.

## Features

- **Every monitor**, each with its own window and its own pause state. One HAP file fills any resolution and aspect ratio (scaled and center-cropped on the GPU).
- **Automatic pause** with maximized or fullscreen windows, exclusive-fullscreen games, a locked session or the display off. It always shows a frame before pausing, so the taskbar never turns black.
- **Transparent taskbar** (optional, see the note below).
- **Fallback to regular video**: without a `.hap`, it plays `wallpaper.mp4` (H.264, AV1…) through Media Foundation with hardware decoding.
- Tray icon (double-click pauses/resumes, with a warning if the `.hap` can't be used) and start with Windows.

## Prepare your video

The hard part of HAP is making the file, so `prepare_video.ps1` does it in one step with [ffmpeg](https://ffmpeg.org/) (`winget install --id Gyan.FFmpeg -e` if you don't have it):

```powershell
.\prepare_video.ps1 -Video C:\videos\my_wallpaper.mp4
```

It detects your primary monitor's resolution, scales and crops the video to fill it, converts it to 30 fps, writes `wallpaper.hap` next to the script, checks the result and tells you how much disk it will use.

| Option | What for |
|---|---|
| `-Format hap` / `av1` / `both` | `hap` = 0 % decoder, large file. `av1` = small `wallpaper.mp4` that uses the GPU decoder. `both` keeps the MP4 as a fallback |
| `-Fps 30` | Frame rate stored in the file (the player reads it). 60 doubles the size |
| `-Start 5 -Duration 20` | Use only part of the video (seconds). Shorter = smaller file |
| `-ClockShadow` | Softly darkens the clock/tray corner so it reads better over a transparent taskbar |
| `-Width 1920 -Height 1080` | Force a resolution (e.g. your largest monitor) |
| `-Output C:\path` | Output folder (default: the script's folder) |

Use videos you made or have permission to use. Seamless loops look best.

<details>
<summary>File format / doing it by hand</summary>

`wallpaper.hap` is the stream of HAP packets (DXT1, no Snappy) written by `ffmpeg -f rawvideo` (width and height multiples of 4):

```powershell
ffmpeg -i video.mp4 -vf "scale=2560:1440:force_original_aspect_ratio=increase,crop=2560:1440,fps=30" -pix_fmt rgba -c:v hap -format hap -compressor none -f rawvideo wallpaper.hap
```

`prepare_video.ps1` then appends a 32-byte trailer: `HAPWALL1` followed by six 32-bit integers (width, height, fps numerator, fps denominator, frame count, bytes per frame). Without the trailer the player assumes 30 fps and the primary monitor's resolution.
</details>

## Use

Put `wallpaper.hap` (and/or `wallpaper.mp4`) next to `HapWallpaper.exe` and run it. If both exist, the `.hap` wins.

```
HapWallpaper.exe [video.mp4 | wallpaper.hap] [--debug] [--nopause] [--notaskbar] [--noautostart]
```

| Option | Effect |
|---|---|
| `--debug` | Writes `log.txt` (and `tb_log.txt` for the taskbar DLL) next to the `.exe` |
| `--nopause` | Never pauses for maximized/fullscreen windows |
| `--notaskbar` | Leaves the taskbar alone (the DLL is not loaded) |
| `--noautostart` | Doesn't register itself to start with Windows |

To quit: right-click the tray icon → Exit.

### About the taskbar DLL

The transparent taskbar works like [TranslucentTB](https://github.com/TranslucentTB/TranslucentTB): `HapWallpaperTB.dll` is loaded into Explorer through Windows' XAML diagnostics API and sets the taskbar background's opacity to 0. Because it runs inside Explorer, **some antivirus products may flag it**. It's optional: run with `--notaskbar` (or delete the DLL) and nothing is injected. The source is in `hapwallpaper_tb.cpp`.

## Build

With MSYS2 (UCRT64):

```powershell
g++ -O2 -s -municode -mwindows -static -o HapWallpaper.exe hapwallpaper.cpp -ld3d11 -ldxgi -lmfplat -lmfuuid -ldwmapi -lshell32 -lwtsapi32 -lole32 -loleaut32 -luuid -luser32 -lgdi32
g++ -O2 -s -shared -static -municode -o HapWallpaperTB.dll hapwallpaper_tb.cpp -lole32 -loleaut32 -luuid -luser32
```

## Credits and license

- [HAP](https://hap.video/) — open format created by Tom Butterworth for Vidvox.
- [ffmpeg](https://ffmpeg.org/) — HAP encoder used by `prepare_video.ps1`.

Code under the [MIT](LICENSE) license. No videos included.
