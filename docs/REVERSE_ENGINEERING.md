# PSCapture — Reverse Engineering Notes

Everything in this document was obtained on a **personally owned, jailbroken
PS5** for interoperability purposes (reading the console's own frame buffer to
use it as a capture device). **No Sony binaries are distributed in this
repository** — the extracted libraries below stay on the researcher's machine;
the `tools/` folder contains only our own code, and `tools/dumplib/` is the
payload that extracts the libraries **from your own console** so you can
reproduce every finding yourself.

---

## 1. Extracting the system libraries (`tools/dumplib/`)

Method: a payload running under the jailbreak asks the kernel for the module
list of a live process and dumps each module to `/data` (FTP out).

Getting there took two false walls:

1. **`0xCC` fill** — reading a module image through the process' user mappings
   yields trap padding for unloaded segments. The fix: read the *file image*
   (the module as loaded from its source file), not the mapped view.
2. **`kernel_dynlib_handle` semantics** — the handle index counts modules per
   process; querying the wrong process silently fails. The reliable recipe:
   `kernel_dynlib_handle(pid, "libXxx.sprx") → handle → dump via the kernel's
   own copy primitives`.

What a full pass pulls from the console (kept locally, catalogued here):

| File | Why it mattered |
|---|---|
| `libSceAvcap2.sprx`, `libSceAvcap2mvr.sprx` | the "capture" service family |
| `libSceVideoOut.sprx`, `libSceVideoOutSecondary.sprx` | video output |
| `libSceVideoRecording.sprx` | recording service (128 MB session file) |
| `libSceJpegEnc.sprx` | the hardware JPEG encoder |
| `libSceScreenShot.sprx` | system screenshot API |
| `libSceAvSetting.sprx`, `libSceAgc.sprx`, `libSceVenc.sprx`, `libSceVdecCore.native.sprx` | AV pipeline |
| `libSceIpmi.sprx`, `libSceMbus.sprx`, `libSceLncUtil.sprx` | IPC plumbing used by the services |
| `libSceSysmodule.sprx`, `libSceSystemService.sprx` | module loading / services |
| `libkernel.sprx`, `libkernel_sys.sprx` | syscall surface |
| `daemon/SceSysAvControl.elf` | **the key**: the AV control daemon |
| `daemon/AgcCompositor.elf`, `daemon/SceSysCore.elf` | companions used for cross-checks |

Loaded-library inventory (which process has what) is produced by the same
payload (`recon` style listing).

---

## 2. Where the pixels are

The picture never travels through any "capture" API that apps can call
(that was the *other* project — the one with 128 MB session files and the IPMI
wall). Instead, the display controller (**DCE**) continuously writes the
scan-out frame into a ring buffer in memory, and a system daemon exposes the
geometry of that buffer.

### The scan-out ring

- Ring virtual address: `0x200200000`, ring data at `+0x4200000`.
- Four surfaces, `SURF_STRIDE = 0x600000` (6 MB) apart, written at 60 Hz.
- Each surface holds the frame as **P010** (10-bit), row stride `3840` bytes
  (1920 px × 2 bytes).
- The daemon structure (`ScaninControl` @ `0x4f1958` inside
  `SceSysAvControl.elf`, vtable `0x4e5078`) exposes:
  - `+0x528` / `+0x530`: physical addresses of the two tap buffers,
  - `+0x550` / `+0x554`: dimensions,
  - `+0x55c`: flag, `+0x560`: slot table `{u64 w; u64 h; u32 pid; u32 tapPoint} × 3`,
  - `getScaninBufferOffset` (`0x435a30`) returns the buffer paddr and the VA.

### P010 decoding

10-bit samples store the useful byte at **odd** indices; extracting the odd
bytes yields a clean 8-bit plane (the "even = low bits = noise" mistake was
made once and is documented so it is not repeated).

### The pan ("dimension")

The scan-out base inside the ring **drifts**: the frame's first pixel does not
sit at a fixed offset. Reading at a fixed offset yields the classic
"image rotates and slides" artifact.

- Exact metric (v10): continuity across a cut. For a candidate cut `c*`, the
  strip `[row r, col 0..]` must match `[row r-1, col c*..]`; minimize
  `Σ |Y[r][j*2+1] − Y[r-1][(c+j)*2+1]|` over ~64 sampled rows.
- Pan conversion: `pan = ((1920 − c) % 1920) * 2` — no `+960` offset.
- Twin ambiguity: two minima 1920 bytes apart; the usable pair on this unit is
  `scan + 1920`. **Field-verified constant: pan 2230** (kept fixed; drift is
  ~2 B/s and below visibility). A user-controlled fine shift (PC-side) handles
  the last few pixels.

### The chroma plane (the hard one)

- The chroma data starts at byte **`0x3EA300` (4 104 960)** — *not* after the
  luma plane. The region before it is a dead zone.
- **In this plane the 16-bit words are V-first**: the pair order must be
  swapped when building NV12 (verified byte-to-byte: error 15.5 → 1.7 →
  0.06 against a reference frame with the same content).
- Valid rows: 527 of 540; the tail is not yet written on read — replicate the
  last good row (the famous "green bar" was the tail being read into the gap).
- The historical v10 build read 11 rows later (`0x3F4800 + pan`): same
  palette, wrong position — that is why "v10 colors looked right but red-ish".

### Color & geometry verification loop

A reference frame (NV12 dump) plus tiny offline tools:

- `tools/p010png.py` / `tools/nv12png.py` — deterministic renderers (same
  math as the driver's YUY2 path, so screenshots match what Discord shows);
- byte-level diffs against a stored reference frame (mean |Δ| per plane);
- anchors for alignment: the centered title text of the setup screen
  (993 px → 959 px after correction, on a 1920-wide frame);
- the correction itself: a cyclic horizontal shift, **even** values only
  (an odd shift re-labels U/V pairs and hue-inverts the image).

---

## 3. Kernel-side notes (why the payload is careful)

- **Reads of kernel memory** go through a page-table walk (validation of every
  translation step) and 8-byte reads via the direct-map alias. **Never**
  `kernel_copyout` on a kernel address: a fault there is a kernel panic
  (learned the hard way, once).
- **Ring mapping trick**: a cacheable user mapping of the DCE physical range
  is built by writing one PML4 entry pointing at our own fresh page-table
  subtree (the daemon's own PDE flags replicated: write-back, no PCD/PWT).
  This is what makes bulk frame reads fast (4 GB/s instead of 45 MB/s through
  the syscall path).
- **Quiescence**: each surface is being rewritten at 60 Hz; the reader copies,
  waits 2 ms, compares, and moves on if the surface changed — avoids tearing.
- **Service setter**: `sceAvControlChangeScanInMode` is never called (it can
  wedge the display engine and, in the worst case, the host app).

## 4. Running code with kernel privileges (delivery)

- **PLD format**: `eb 0b 'P' 'L' 'D'` + u64 `code_size`, then code, then data.
  A loader on port **9019** maps `code_size` bytes executable and the rest RW
  behind it, then calls the entry. This is how the ecosystem's kernel payloads
  ship — and how further experiments (USB NIC driver) are delivered.
- Kernel modules compile against a vendored **FreeBSD 9.0** header set (the
  Orbis kernel base, `__FreeBSD_version 900044`); per-firmware offset tables
  (e.g. `allproc`, IDT, sysentvec for 13.60) exist in the scene and were
  cross-checked.

## 5. Tools in this repo

| Tool | Purpose |
|---|---|
| `tools/dumplib/` | payload that extracts system libraries from your own console |
| `tools/sprx.py` | sprx ELF lister/extractor (exports, imports, sections) |
| `tools/callsites.py`, `tools/scan_refs.py` | xref hunt in disassembly (RIP-relative call sites) |
| `tools/whoimports.py`, `tools/stubmap.py` | import graph across the dumped libraries |
| `tools/p010png.py`, `tools/nv12png.py` | deterministic frame renderers (P010 / NV12) |
| `tools/flat.py`, `tools/asmwin.py` | binary utilities used during the hunt |

## 6. Timeline (condensed)

1. Capture through the official recording stack → dead end (session files,
   IPMI client wall).
2. Direct read of the DCE ring → correct pixels, but the image "rotated":
   the pan was discovered.
3. Pan metric, twin resolution, fixed 2230 — geometry solved.
4. Chroma plane hunt: base, dead zones, V-first words, tail replication —
   colors solved (byte-verified).
5. Ring mapped cacheable → 1080p at ~19-35 fps over plain TCP to the PC.
6. Kernel-side delivery studied (PLD, 9019) → foundation for the next step:
   a real USB network driver for full-rate/60 fps streaming.

*Reported, reproduced, and maintained by **fstat13**.*
