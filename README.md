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

1. **Jailbreak your PS5** (payload loader on `:9021`, anonymous FTP on `:2121`,
   `pldmgr` autoloader — all standard in today's setups).
2. **Run `PSCapture.exe`** (admin is requested automatically; set your PS5 IP
   if it isn't the default).
3. **Press Start.** The app does the rest. The console pops a notification:
   `PSCapture is streaming on:PORT`
4. In Discord/OBS pick the camera **PS5 Capture**.

📖 The illustrated guide lives in [`docs/index.html`](docs/index.html)
(open it in a browser, or enable GitHub Pages on `/docs`).

## 📦 Repository layout

```
payload/    PS5 side: display-engine reader + NV12 streamer (ps5capture.elf)
pc/         PSCapture.exe  — GUI + installer + payload deployer + feeder
pc/shm.h    shared-memory contract between feeder and driver
pc/*.dll    the virtual capture-card driver (DirectShow source filter)
docs/       HTML guide (dark theme, ready for GitHub Pages)
```

## 🔧 Build

**Console payload** (needs the PS5 payload SDK; any Linux/docker with it):

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload <ps5-payload-sdk-image> make
```

**PC app** (mingw-w64, no dependencies):

```bash
windres pscapture.rc -O coff -o pscapture.res      # icon + embedded payload
gcc -O2 -mwindows -o pscapture.exe pscapture.c pscapture.res -lws2_32 -lshell32 -ladvapi32
```

**Driver** (mingw-w64):

```bash
gcc -O2 -shared -o pscapture.dll ps5cam.c -lole32 -loleaut32 -luser32 -ladvapi32 -lws2_32
```

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

## 📄 License

**Source-available**, copyright © 2026 **fstat13**. You may read and study the
code and use the official binaries for personal projects; redistribution,
reuse in other projects and commercial use are not allowed without permission.
See [LICENSE](LICENSE) for the full terms.
