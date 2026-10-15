# PSCapture — Engineering log: problems, diagnoses, work-arounds

This is the long story: every wall we hit while building PSCapture, how it
was diagnosed, and what finally fixed (or bypassed) it. Dead ends are kept on
purpose — they cost real time and shouldn't cost yours.

*(Testbed: retail PS5, firmware 13.60, jailbroken; PC on the same LAN.
All work done on personally owned hardware.)*

---

## Part A — Getting pixels out of the console

### A1. The official capture stack is a dead end
**Symptoms / approach.** First attempts used the console services that look
like capture APIs: `libSceAvcap2` / `libSceVideoRecording` etc.
**What we found.** `libSceVideoRecording` opens a session backed by a **128 MB
file** and talks to its client over internal IPC; calls from an unprivileged
payload fail with the service unavailable. The IPMI (inter-process messaging)
client table is limited, and every live instance of our payload holds a
session — with ~26 stale instances, a new payload waits **forever**
(`connect -> 0x8002000d`).
**Resolution.** Abandoned the service path entirely. The work-around that
later became permanent: (1) never rely on the recording service; (2) payloads
kill their own stale instances at boot ("v17 hygiene"), fingerprinting them by
*their* fixed cacheable mapping in the page tables (PML4 entry #128 — no
system process uses it).

### A2. The direct path: the display engine's scan-out ring
**Diagnosis.** The AV-control daemon (`SceSysAvControl.elf`) exposes the
scan-out geometry: ring at `0x200200000`, data at `+0x4200000`, four surfaces
`0x600000` apart, rewritten at 60 Hz, stored as P010 (10-bit).
**First bug — P010 decoding.** Extracting the *even* bytes gave noise: the
usable byte of a 10-bit sample is the **odd** one. Lesson recorded in
`REVERSE_ENGINEERING.md` so nobody re-makes it.

### A3. The rotating image (the pan)
**Symptom.** The picture "rotated" and slid horizontally forever; reading the
four surfaces in alternation made it jump every frame.
**Diagnosis.** The scan-out base drifts within the ring: the frame does not
start at a fixed offset. The surface content is also quasi-periodic, which
made the first metric find a **twin minimum** 1920 bytes away and oscillate
between the two.
**Dead ends (documented so you skip them):**
- a "junction" continuity metric that needed a `+960` fudge — produced
  oscillation between twins;
- chasing the drift with a per-frame tracker — the pursuit itself caused
  visible horizontal wiggling.
  **Final metric** ("exact cut"): strip `[row r, 0..maxj]` must match
  `[row r-1, c..]`, minimizing `Σ |Y[r][j*2+1] − Y[r-1][(c+j)*2+1]|` over
  sampled rows; conversion `pan = ((1920 − c) % 1920) * 2`.
**Resolution.** Field-verified fixed `pan = 2230` (the drift is ~2 B/s,
below visibility), and later a **user-tunable fine shift** on the PC side
(even pixels only, see D2).

### A4. The color saga (or: "v10 colors were right")
**Symptom.** Backgrounds looked red/amber instead of blue; users kept saying
"too red", "now too gray", "the colors of v10 were right".
**Diagnosis path.** The chroma plane does **not** start after the luma:
it begins at byte `0x3EA300` (= 4 104 960), 11 rows *before* the presumed
boundary, with dead zones before and after (the "green bar" of legend was the
tail read into the gap → replicate the last good row).
But position alone wasn't the whole story. Comparing a live frame **byte for
byte** against an old reference frame of the same screen showed the output
matched only with **U/V pairs swapped**: in this plane the 16-bit words are
stored **V-first**. Mean absolute error dropped 15.5 → 1.7 → 0.06 once the
pair order was corrected in the P010→NV12 conversion (a second shuffle mask
for the chroma region only).
**Honest note.** Before that discovery, the wrong knob was shipped: a chroma
**gain** to "lower the red" — it made the cast less visible but never right.
The gain knob survives as an optional tuning, default 1.0 (= raw).
**How the truth was found.** Three independent anchors: neutral white stays
(128,128) either way, the bluish background flips the right way only with the
swap, and the byte-level diff against the reference settles it numerically.
Rendering the *same bytes* with two different YUV formulas long ago had also
caused a wild goose chase — the rule became: **compare numerically, never
eye-render across periods** (see Lessons).

### A5. Kernel reads without crashing the console (three crashes, one rule)
**Crash #1/#2.** Full console lock-ups (all TCP ports mute, ICMP half-dead)
after storms of live payload instances + IPMI maintenance traffic. Recovered
only by physical power cycle. Root cause never *proven*; the permanent
hygiene (single instance, kill-istanze at boot) removed the whole class.
**Crash #3 (diagnosed exactly).** A kernel-dump experiment read kernel VA
through `kernel_copyout` — a fault there is a **kernel-mode fault**: instant
panic. **Rule, now absolute:** kernel memory is only ever read via a page
table walk with present-bit validation + 8-byte reads through the direct map;
`kernel_copyout` on kernel addresses is banned.
**Related guard bug (fun one).** The cacheable-mapping builder refused
physical addresses above 16 GB as "suspicious" — but the display surfaces
live at `0x459200000` (≈18.7 GB). Result: silent fallback to the slow path
(45 MB/s, 6 fps). Fix: raise the guard to 32 GB; the real safety net is the
page-table validation, not the numeric limit.

### A6. Speed: from 45 MB/s to 4 GB/s
**Diagnosis.** Reading the ring through syscall copies serialized inside the
process (multi-threading didn't help — the copies bottleneck in the kernel
path).
**Fix.** Build a **cacheable mapping** of the DCE physical range inside the
payload process: allocate our own PDPT/PD pages, replicate the daemon's PDE
flags (write-back, no PCD/PWT), write **one** PML4 entry, done. Bulk frame
reads went to ~4 GB/s.
**Tearing.** The DCE rewrites a surface at 60 Hz; a read during a rewrite
yields a mixed frame (ghosts, colored tails). Fix: read, wait 2 ms, compare;
if the surface changed it was alive — move to the next one.

---

## Part B — The PC side: from frames to a "webcam"

### B1. Pipeline shape
Feeder (TCP receiver) → shared memory ring → **DirectShow source filter**
(`ps5cam.c` → `pscapture.dll`) that every consumer sees as a camera called
**PS5 Capture**.

### B2. Discord error 2014 (spinner → no camera), root cause found in their binary
**Symptoms.** Camera selectable, infinite spinner, then Discord error 2014 —
with *any* virtual camera (ours and OBS Virtual Camera alike).
**Diagnosis.** Discord's own media logs contained the smoking gun:
`Failed to create capturer (video_capture_module_source.cpp:105)`. Reading
their capture engine (WebRTC's DShow path inside the Discord binary):
the engine's device **picker requests 1280×720@30** and only accepts a device
that lists a *matching* capability. Ours offered only 1920×1080 → no match →
capturer never created. OBS Virtual Camera exposed zero caps → same fate.
**Fix — "real webcam" capability profile.** The driver now exposes 11 UVC-like
capabilities (YUY2/NV12/I420/RGB24/RGB32 at 1080p/720p/480p, negative-height
variants first), plus a `DevicePath` string in the registry (the modern
identifier consumers use instead of the friendly name). Verified in their
logs: `Capturer running at 1280x720@30`, first frame in 10–13 ms.
**Tooling discoveries that made this tractable:**
- Discord can be driven with `--remote-debugging-port`; from it the **media
  engine is scriptable** (device selection, camera on/off) without touching
  the UI — see `docs/` history and our notes.
- The binary is searchable for log strings (`(file:line)` hints) and
  RIP-relative xrefs; that's how the picker logic was pinned down.

### B3. Flips and orientations
**Symptom.** 720p output upside-down for one consumer but not another.
**Diagnosis.** DIB convention sign vs YUV conventions differ between
consumers: ffmpeg/WebRTC treat YUV as top-down regardless of `biHeight` sign,
but XRGB-style DIBs as bottom-up.
**Fix.** Flip **only** for RGB subtypes when `biHeight > 0`; all YUV paths
stay top-down. Verified with ffmpeg grabs.

### B4. Alignment ("more space on the left than on the right")
**Symptom.** The image sat a few dozen pixels off; users described margins,
not offsets.
**Diagnosis.** Measured **inside** the frame: the centered title text of the
screen had its glyph center at x≈993 (960 = screen center) → the capture was
~33 px to the right; button margins agreed (~29 px, arrow ~18 px — the title
is the cleanest anchor).
**Fix.** A **cyclic horizontal shift** in the feeder (whole luma plane and UV
plane, same byte count; **even** pixels only — an odd shift re-labels U/V
pairs and hue-inverts), exposed as the live `shift px` knob.
**Long-term.** Bake the value into the payload's pan and drop the PC-side
shift (planned).

### B5. The 1080p question (and what Discord really does)
Users compared with capture cards: "Elgatos stream at 1080p!".
**Findings.** (1) The capture path in Discord calls is capped by *Discord's
engine* (it asks every camera for 720p — capture cards included); (2) as a
generic DirectShow source, PSCapture **already delivers 1920×1080** to OBS and
ffmpeg (verified with an ffmpeg 1080p grab while Discord was live) and that's
the road for 1080p streaming; (3) 1080p *inside* a Discord call via screen
share is possible up to 1080p60 with Nitro.

---

### B6. The installer crash-course: a cryptic module error and a lying log
**Symptom.** Pressing *Install driver* showed *"the specified module could not
be found"* - the classic rundll32 message, with no other clue anywhere.
**Diagnosis.** Two faults stacked: when the app was not elevated, writing
`pscapture.dll` into `%ProgramFiles%` failed silently - the old code logged
the extraction as successful anyway - and then `rundll32` was asked to load a
file that did not exist (hence the generic, useless message). On top of that,
`rundll32` never reports what went wrong back to the caller.
**Fix.** Every filesystem step is now verified (write -> check size ->
report), the install aborts honestly when the dll cannot be placed, and
registration happens in-process (`LoadLibrary` + `DllRegisterServer`) so
failures come back as real HRESULTs. The driver check also verifies that the
registered dll still *exists* on disk before declaring victory.

### B7. The modern control center UI
The first GUI iteration was a plain Win32 dialog. It was rebuilt as a dark
custom-chrome window: painted title bar with integrated minimize/close,
rounded window region, owner-drawn buttons, a live status pill with a state
dot, and a dark monospace console pane. Everything is still pure C with zero
frameworks - the UI is drawn by hand.

## Part C — The road to 60 fps

**The math.** Raw NV12 1080p is 3.11 MB/frame → 30 fps ≈ 93 MB/s (a gigabit
LAN is already at the wall at ~110 MB/s); 60 fps ≈ 186 MB/s → needs 2.5 GbE
*or* compression.

**Raw path (2.5 GbE adapter on the console).** The user's adapter is a TP-Link
UE302C (Realtek RTL8156B). Findings: the console kernel is **FreeBSD 9.0**
based; the matching driver (`ure`) appears in FreeBSD 13+, and Sony ships only
the drivers it uses — so nothing will recognize the adapter out of the box.
Two enablers were secured anyway: the **PLD payload format** and the kernel
payload loader on port **9019** (how the scene's kernel payloads ship), plus
the kernel offsets table for 13.60 — enough to *deliver* a custom kernel
module when the port is ready. (Full port = a separate workstream; the
homework is in `REVERSE_ENGINEERING.md` §4.)

**Compressed path (stays on gigabit).** The console has a hardware JPEG
encoder (`libSceJpegEnc`). At ~250-350 KB/frame, **60 fps ≈ 15-21 MB/s** —
five times inside a gigabit. The API surface was recovered from the library
itself (exports, NIDs, argument structs, validation rules — see the notes in
`REVERSE_ENGINEERING.md`), and a measuring payload is ready to run on the
console (each call in a forked child with timeout — Sony APIs abort on wrong
arguments, so a bad call must die alone).

---

## Lessons (the rules we now follow)

1. **Never** hand a kernel address to `kernel_copyout` — page-walk + present
   check + direct-map reads, always.
2. One live payload instance. Kill stale ones at boot; never accumulate.
3. Measure with bytes, not eyes: same formula, same reference frame, signed
   deltas. Eye-comparison across renderers lies.
4. Even-pixel shifts only (U/V pairing), for the same reason.
5. Never call `sceAvControlChangeScanInMode` from a payload (it can wedge the
   display engine).
6. When an API can abort the host on bad arguments, isolate the call in a
   child process with a timeout.
7. When a suspicious "fix" targets a symptom (the chroma gain), keep looking
   for the *identity* of the problem (the U/V swap).

8. **Never log success without checking the I/O result.** A lying log (the
   installer claiming "extracted ok" on a failed write) costs far more time
   than a missing one - it sends you hunting in the wrong place.
