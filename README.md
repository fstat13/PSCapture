<p align="center">
  <img src="docs/pscapture.png" width="120" alt="PSCapture">
</p>

<h1 align="center">PSCapture</h1>

<p align="center">
Turn a <b>jailbroken PlayStation 5</b> into a real <b>capture card for your PC</b>.<br>
The console screen is streamed over your LAN and shows up in Discord, OBS or ffmpeg<br>
as a virtual camera called <b>PS5 Capture</b>.
</p>

<p align="center">
No capture card. No Remote Play. No cloud. Just your console and your network.<br>
<b>by fstat13</b>
</p>

<p align="center">
📖 <b><a href="docs/GUIDE.md">How to use it — step-by-step guide</a></b>
&nbsp;·&nbsp;
🇮🇹 <b><a href="docs/GUIDE.it.md">Guida passo-passo in italiano</a></b>
&nbsp;·&nbsp;
🔧 <b><a href="docs/BUILD.md">Build from source</a></b>
&nbsp;·&nbsp;
🇮🇹 <b><a href="docs/BUILD.it.md">Build dai sorgenti</a></b>
</p>

<p align="center">
🌐 Illustrated guide:
<b><a href="https://fstat13.github.io/PSCapture/">English</a></b>
&nbsp;·&nbsp;
<b><a href="https://fstat13.github.io/PSCapture/index.it.html">Italiano</a></b>
</p>

<p align="center">
  <a href="https://github.com/fstat13/PSCapture/releases"><img src="https://img.shields.io/github/v/release/fstat13/PSCapture?style=flat-square&color=0070d1&label=release" alt="Release"></a>
  <a href="https://github.com/fstat13/PSCapture/stargazers"><img src="https://img.shields.io/github/stars/fstat13/PSCapture?style=flat-square&color=0070d1" alt="Stars"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-source--available-35a3ff?style=flat-square" alt="License"></a>
  <img src="https://img.shields.io/badge/platform-Windows%2010%2F11-0070d1?style=flat-square" alt="Platform">
</p>

---

## ✨ What you get

- **One executable** — `PSCapture.exe` carries the console payload inside itself:
  it installs the virtual capture driver, pushes the payload to the PS5, registers
  it in the autoloader and starts the stream. One window, one button.
- **A real camera device** — a DirectShow source filter named *PS5 Capture*:
  pick it in Discord calls, record it in OBS at 1920x1080, open it from ffmpeg.
- **Low latency** — frames are read straight from the console's display engine
  (NV12 over TCP); nothing is re-encoded on the console.
- **Live tuning** — chroma `gain` and horizontal `shift` knobs, saved in
  `pscapture.ini`, applied without restarts.

## 🚀 Quick start

1. **Jailbreak your PS5** — you need the standard setup: a payload loader on
   TCP `9021` (`elfldr`-style), anonymous FTP on TCP `2121`, and optionally
   `pldmgr` so the payload starts on every boot.
2. **Double-click `PSCapture.exe`** (one file — the console payload and the
   Windows driver travel inside it). Accept the UAC prompt. If your PS5 isn't
   at `192.168.0.36`, type its IP in the *PS5 IP* field and press *Apply*.
3. **Watch the log pane** — the app installs the capture driver (first run
   only), sends the payload to the console and connects by itself. The status
   pill turns green: `STREAMING`. The console shows the system toast
   **PSCapture is streaming on:PORT**.
4. In Discord (*Settings → Voice & Video → Camera*) or OBS pick the camera
   **PS5 Capture**.

Stuck on *“IN ATTESA DELLA PS5”*? Console off, not jailbroken yet, or wrong
IP — the app retries by itself every 2 seconds.

📖 Step-by-step with every log line explained: [`docs/GUIDE.md`](docs/GUIDE.md)
— or the [illustrated HTML guide](docs/index.html).
🔧 What the app does under the hood when you press Start:
[`docs/INSTALLER.md`](docs/INSTALLER.md).

## 📦 Repository layout

```
payload/    PS5 side: display-engine reader + NV12 streamer (pscapture.elf)
pc/         PSCapture.exe — GUI + installer + payload deployer + feeder
pc/ps5cam.c the virtual capture-card driver (DirectShow source filter)
pc/shm.h    shared-memory contract between feeder and driver
docs/       guides: HTML, fool-proof step-by-step, build, installer, RE, log
```

## 🔧 Build

Full walk-through (toolchain install, verification, gotchas):
**[`docs/BUILD.md`](docs/BUILD.md)**. The short version, in the **mandatory
order** (the `.res` embeds the driver and the payload as they are on disk):

**Console payload** → `payload/pscapture.elf` (needs Docker + the PS5 payload
SDK image; any tag works, here `ps5sdk:18`):

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload ps5sdk:18 make
```

**PC side** → `pscapture.exe` + `pscapture.dll` (needs MinGW-w64 with `gcc` +
`windres`):

```bash
cd pc
cp ../payload/pscapture.elf .   # gets embedded as-is
# 1) the capture-card driver (-static is mandatory, see BUILD.md)
gcc -O2 -shared -static -static-libgcc -o pscapture.dll ps5cam.c \
    -lole32 -loleaut32 -luser32 -ladvapi32 -lws2_32
# 2) resources = icon + payload + driver
windres pscapture.rc -O coff -o pscapture.res
# 3) the app (one self-contained executable)
gcc -O2 -mwindows -o pscapture.exe pscapture.c pscapture.res \
    -lws2_32 -lshell32 -ladvapi32 -luxtheme
```

Rebuild rule: **DLL → windres → exe, every time** — rebuilding only the exe
ships the old driver inside it.

## ❓ FAQ

**Does it work on firmwares older than 13.60?**
The PC side works everywhere. The console payload was built and tuned on
**13.60**; other firmwares may need a small porting pass (re-finding the same
display-engine anchors). Open an issue with your firmware version.

**720p in Discord but 1080p in OBS?**
That's Discord's voice engine — it requests 1280x720 from every camera
(capture cards included). Use OBS for full quality, or Discord screen-share
(up to 1080p60 with Nitro).

**Remote Play involved?**
Never. The payload reads the display engine on the console itself.

## 🔬 Reverse engineering

The whole story of how the console's display engine was mapped — the scan-out
ring, the pan, the chroma plane, the kernel-side notes and the delivery format
for kernel payloads — lives in
[`docs/REVERSE_ENGINEERING.md`](docs/REVERSE_ENGINEERING.md).
The bench used to get there is in [`tools/`](tools/): the library dumper
payload, the disassembly hunters, the deterministic frame renderers.

> No Sony binaries are distributed with this project: `tools/dumplib/` extracts
> what you need **from your own console** to reproduce every finding.

## 📄 License

**Source-available**, copyright © 2026 **fstat13**. You may read and study the
code and use the official binaries for personal projects; redistribution,
reuse in other projects and commercial use are not allowed without permission.
See [LICENSE](LICENSE) for the full terms.
