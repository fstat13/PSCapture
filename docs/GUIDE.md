# PSCapture — the complete step-by-step guide

🇮🇹 Versione italiana: [GUIDE.it.md](GUIDE.it.md)

From zero to your PS5 inside Discord. Every step says exactly what you should
see on screen. If something doesn't match, jump to the troubleshooting table
at the bottom.

---

## 0. What you need

**On the console** — all standard in current jailbreak setups:

| Thing | Port | Why |
|---|---|---|
| Payload loader (`elfldr`-style) | TCP **9021** | runs the payload we send |
| Anonymous FTP | TCP **2121** | the app reads/writes files on the console |
| `pldmgr` autoloader (optional) | — | makes the payload start at every boot |

**On the PC**: Windows 10/11 64-bit, wired network recommended.

**The app**: `PSCapture.exe` — **one single file**. Download it from the
repository's Releases page. That's the whole package: the console payload and
the Windows capture-card driver are inside it.

---

## 1. First run on the PC

1. Put `PSCapture.exe` anywhere (the Desktop is fine) and **double-click it**.

2. Windows asks *"Do you want to allow this app to make changes to your
   device?"* → click **Yes**.
   *Why:* registering the virtual camera for every app on the machine needs
   administrator rights, once. The app elevates itself — no right-click needed.

3. The window opens (dark, with a big blue **START STREAMING** button).
   If your PS5 is not at `192.168.0.36`: type its address in the **PS5 IP**
   field and press **Apply**.

4. From here the app does everything alone. Watch the dark log pane:

   | You should read | Meaning |
   |---|---|
   | `driver gia' installato` | camera already registered — fine |
   | `driver estratto dall'eseguibile (… byte)` + `driver registrato - la webcam 'PS5 Capture' e' pronta` | first run: driver installed |
   | `payload dall'eseguibile (… byte)` + `payload avviato - la PS5 deve mostrare la notifica PSCapture` | the payload is now running on the console |
   | `porta stream: 5599` then `collegato a 192.168.0.36:5599` | stream connected |
   | status pill turns **green**: `STREAMING - scegli 'PS5 Capture' in Discord` | you're live |

5. The PS5 shows a system notification: **PSCapture is streaming on:PORT**.
   (That toast is the console telling you the payload is alive.)

> If the pill stays on *"IN ATTESA DELLA PS5 — accesa e col jailbreak avviato?"*
> the console is off, not jailbroken, or the IP is wrong. See section 5.

## 2. Use it

**Discord (in a call)** — *Settings → Voice & Video → Camera* → **PS5 Capture**.
The in-call image is 720p: that's Discord's engine, it asks *every* camera
(capture cards included) for 1280x720.

**OBS** — *Sources → **+** → Video Capture Device* → **PS5 Capture**.
This runs at the full **1920x1080**.

**ffmpeg** — `ffmpeg -f dshow -i video="PS5 Capture" capture.mkv`

## 3. Daily use

- Open `PSCapture.exe` → **Yes** to the admin prompt. The app reconnects by
  itself: if the payload is already on the console (autoloader) the stream
  starts by itself; if the console is off the pill says
  *"IN ATTESA DELLA PS5"* and connects the moment the console is ready.
- Press **STOP** (or close the app) to stop streaming. The camera goes black /
  freezes on the last frame; consumers keep the device listed.
- **Deploy payload** re-sends the payload by hand (useful after a console
  reboot without autoloader). **Install driver** re-registers the camera by
  hand (useful if the log shows a driver error).

## 4. The two knobs

| Knob | What it does | Rules |
|---|---|---|
| `gain` | chroma (color) strength | `1.0` = raw, the correct default. Only touch it if colors look wrong. |
| `shift px` | moves the image left/right | **even numbers only** (an odd number inverts the colors). Tune until the picture is centered; the console's title text is a good anchor. |

Both apply live (about a second) and are saved in `pscapture.ini` next to the
exe, so they survive restarts.

## 5. Troubleshooting — symptom by symptom

| Symptom | Cause | Fix |
|---|---|---|
| Pill stays `AVVIO...` / `IN ATTESA DELLA PS5` | console off, **not jailbroken**, or wrong IP | turn the console on, run the jailbreak (ports 9021/2121 must be up), check the IP field |
| Log repeats `PS5 non raggiungibile (...)`, riprova | same as above | same |
| No `PS5 Capture` camera appears in Discord/OBS | driver never registered (install failed) | press **Install driver**; read the log line after it — it now reports the *real* error |
| Log: `ERRORE: driver non scrivibile in C:\Program Files\PSCapture (serve admin?)` | app not elevated | close it, restart `PSCapture.exe` and accept the UAC prompt |
| Log: `ERRORE caricamento dll (err=126)` | antivirus quarantined the extracted dll | check the antivirus quarantine for `pscapture.dll`, allow it, press **Install driver** again |
| Camera listed but the image is black | stream not running | check the pill; press **Deploy payload** and/or **START STREAMING** |
| Colors look wrong (skin reddish, sky orange) | `gain` was changed | set `gain` back to `1.0` → Apply |
| Image has a black bar / is off-center | alignment | tune `shift px` (even values) until centered |
| Discord keeps showing 720p | Discord's engine caps every camera at 720p | use OBS for 1080p, or Discord screen-share (1080p60 with Nitro) |
| OBS shows the camera, Discord doesn't | Discord was already open when the driver was installed | fully quit Discord (also from the tray) and reopen it |
| Everything else | — | read `pscapture-app.log` (next to the exe, or in `%TEMP%`) and the console log `/data/pscapture.log` (FTP) |

## 6. Uninstall

1. Close the app.
2. `rundll32 "%ProgramFiles%\PSCapture\pscapture.dll",DllUnregisterServer`
   (removes the camera registration).
3. Delete `C:\Program Files\PSCapture`.
4. Optional, on the console: delete `/data/pldmgr/payloads/pscapture/` so the
   autoloader stops starting it.
