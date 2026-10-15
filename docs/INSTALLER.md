# How the PSCapture installer works

There is no separate installer: **`PSCapture.exe` installs itself**. Everything
happens on the first launch, in four steps, from one window.

```
launch ─► self-elevate (UAC) ─► driver check ─► install (once) ─► deploy payload ─► stream
```

---

## Step 0 — Self-elevation (no manifest tricks)

On startup the app calls `CheckTokenMembership` against the *Administrators*
group. If it is not elevated it re-launches itself through
`ShellExecuteEx(…, "runas", …)` — the standard UAC prompt — and the elevated
instance takes over. No manifest is embedded, so the executable stays a plain
single file.

## Step 1 — Is the driver installed?

Checked by a single registry probe:

```
HKEY_CLASSES_ROOT\CLSID\{50533543-414d-4def-9b0e-192080102030}\InprocServer32
```

That CLSID is the virtual capture card's DirectShow filter. If the key exists,
the driver is considered installed and this step is skipped.

## Step 2 — Installing the driver (first run only)

1. Creates `%ProgramFiles%\PSCapture`.
2. Copies `pscapture.dll` there — the DirectShow source filter. (The app also
   copies itself, so the installed folder is self-contained.)
3. Registers the filter by running:

   ```
   rundll32.exe "%ProgramFiles%\PSCapture\pscapture.dll",DllRegisterServer
   ```

   `DllRegisterServer` writes the COM registration for the filter CLSID,
   including the **DevicePath** entry that modern capture consumers
   (Chromium/Discord media engine among them) use to pick the device.
   `DllUnregisterServer` removes it — that is the uninstall path today
   (delete the folder + call it).

After this step the system has a camera called **PS5 Capture**. It will show
a black frame until a stream arrives — the feeder is what fills it.

## Step 3 — Deploying the console payload

The payload (`pscapture.elf`) ships **inside the executable** as a Windows
`RCDATA` resource — no loose files to lose. Two paths are used, in order:

1. **Autoloader** (persistence): over anonymous FTP (port 2121) the app
   creates `/data/pldmgr/payloads/pscapture/` on the console and uploads
   `pscapture.elf` there, so the jailbreak's autoloader starts it on every
   boot.
2. **Immediate start** (this session): the same bytes are pushed to the
   payload loader on TCP port **9021**; the app reads back the console log
   and prints it in the window. The console shows the system notification
   **“PSCapture is streaming on:PORT”**.

## Step 4 — Streaming (the feeder)

The feeder lives inside the app (a worker thread), no background services:

1. reads the stream port from `/data/pscapture.port` (FTP),
2. connects to the console over TCP and receives **NV12 1920×1080** frames,
3. applies the live **gain** and **shift px** settings from
   `pscapture.ini` (re-read every ~30 frames — changes are instant),
4. publishes each frame into the shared memory ring `Local\Ps5CamShm`,
   where the driver picks it up for any consumer (Discord, OBS, ffmpeg…).

Closing the app stops the feeder; the camera freezes on the last frame and
reconnects automatically the next time you press **Start**.

---

## Files and locations touched

| Where | What |
|---|---|
| `%ProgramFiles%\PSCapture\` | `pscapture.exe`, `pscapture.dll` |
| Registry `HKCR\CLSID\{50533543-…-192080102030}` | the capture-card filter registration |
| Console `/data/pldmgr/payloads/pscapture/pscapture.elf` | payload (autoloader) |
| Console `/data/pscapture.port` | stream port announced by the payload |
| Console `/data/pscapture.log` | payload log (FTP-readable) |
| Next to the exe: `pscapture.ini` | `IP`, `GAIN`, `SHIFT` settings |

## Notes

- No Windows service, no scheduled task, no kernel driver: the capture card
  is a user-space DirectShow filter, the feeder dies with the app.
- The console side never needs configuration through the PS5 menus: the
  payload and the app handle everything.
- Uninstall = stop the app, run
  `rundll32 "%ProgramFiles%\PSCapture\pscapture.dll",DllUnregisterServer`,
  delete the folder (and optionally `/data/pldmgr/payloads/pscapture/` on the
  console). A one-click uninstaller is on the roadmap.
