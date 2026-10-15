# PSCapture — build from source, step by step

🇮🇹 Versione italiana: [BUILD.it.md](BUILD.it.md)

Everything needed to go from these sources to a working `PSCapture.exe` and a
`pscapture.elf` for the console. Every command is written exactly as it is
run, in order. Nothing here is optional magic — the order matters and each
step is explained.

There are two halves, built in different places:

| Half | Output | Where it builds | Toolchain |
|---|---|---|---|
| Console payload | `payload/pscapture.elf` | Docker (Linux container) | PS5 payload SDK (prospero-clang) |
| PC side | `pc/pscapture.exe` + `pc/pscapture.dll` | Windows | MinGW-w64 (`gcc` + `windres`) |

---

## Part 1 — The console payload

### 1.1 What you need

- **Docker** (Docker Desktop on Windows, or plain Docker on Linux/macOS).
- **The PS5 payload SDK container image.** This is the community PS5 payload
  SDK (`prospero-clang`, `make`, `PS5_PAYLOAD_SDK` set in the environment)
  baked into an image. On the development machine the image is tagged
  `ps5sdk:18` — any name works, adjust the commands below to match yours.

Check the image is there:

```bash
docker images | grep ps5sdk
```

### 1.2 Build

From the repository root:

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload ps5sdk:18 make
```

On Windows PowerShell/cmd, give the full path instead of `"$PWD/payload"`,
e.g. `-v "C:\Users\you\PSCapture\payload:/payload"`.

Result: **`payload/pscapture.elf`** (~175 KB).

The `Makefile` does the only thing that matters: compile `main.c` with the
Prospero clang from the SDK and link the final ELF. No other inputs.

### 1.3 Optional — send it to the console

With the console jailbroken (payload loader on port 9021):

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload ps5sdk:18 \
    make deploy PS5_HOST=192.168.0.36
```

(or just push `pscapture.elf` with whatever loader you normally use).

What a healthy run looks like on the console:

- `/data/pscapture.log` appears and logs the startup,
- `/data/pscapture.port` appears containing the TCP port of the stream,
- the console pops the notification *PSCapture is streaming on:PORT*.

---

## Part 2 — The PC side

### 2.1 What you need (Windows)

**MinGW-w64 64-bit** with `gcc` and `windres`. On the development machine:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

(or any MSYS2 `mingw-w64-x86_64-gcc` install — `pacman -S mingw-w64-x86_64-gcc`).

Check both tools are on PATH:

```bash
gcc --version
windres --version
```

### 2.2 Build — the order is mandatory

`pscapture.res` is a bundle that **embeds files as they are on disk**: the
icon, the console payload and the capture-card driver. So the DLL must be
built *before* the resource step, and the payload must be sitting in `pc/`
when `windres` runs. Build the EXE last.

```bash
cd pc

# 0) bring the freshly built payload next to the .rc (it gets embedded as-is)
cp ../payload/pscapture.elf .

# 1) the capture-card driver (DirectShow source filter)
gcc -O2 -shared -static -static-libgcc -o pscapture.dll ps5cam.c \
    -lole32 -loleaut32 -luser32 -ladvapi32 -lws2_32

# 2) bundle icon + payload + driver into one resource object
#    (pscapture.rc => 1 ICON pscapture.ico / 2 RCDATA pscapture.elf
#                     / 3 RCDATA pscapture.dll)
windres pscapture.rc -O coff -o pscapture.res

# 3) the app — one self-contained executable
gcc -O2 -mwindows -o pscapture.exe pscapture.c pscapture.res \
    -lws2_32 -lshell32 -ladvapi32 -luxtheme
```

### 2.3 Verify the build

```bash
# the driver must depend on system DLLs ONLY (no libgcc/libwinpthread):
objdump -p pscapture.dll | grep "DLL Name"

# sizes look right: exe ~500 KB, dll ~220 KB
ls -la pscapture.exe pscapture.dll

# both the payload and the driver are really inside the exe:
strings pscapture.exe | grep -c PSCapture
```

Run `pscapture.exe`: the window opens (UAC prompt → Yes), the log pane walks
through driver check → payload extraction → deploy attempt. That is the same
sequence described in `docs/INSTALLER.md`.

### 2.4 Gotchas (learned the hard way)

- **`-static -static-libgcc` on the DLL is not cosmetic.** Without it the
  driver can end up needing mingw runtime DLLs that are *not* on the system
  PATH when Windows (or another app) loads it — the classic
  *"the specified module could not be found"*.
- **Build order.** `windres` reads `pscapture.dll` and `pscapture.elf` from
  `pc/` at compile time. Rebuild the DLL → re-run `windres` → rebuild the
  EXE, every time. Forgetting the middle step ships an old driver inside the
  new exe.
- **ASCII only** in strings passed to the `*A` Win32 APIs (window titles,
  buttons, logs). UTF-8 bytes in an ANSI string render as mojibake.
- The **manifest** (`pc/pscapture.manifest`) is kept for reference only;
  the current build does **not** embed it — self-elevation is done in code
  (`ShellExecuteEx … "runas"`), which avoids mingw's `.rsrc merge failure`
  with multiple manifests.

### 2.5 Package a release

```bash
mkdir -p dist
cp pc/pscapture.exe pc/pscapture.dll dist/
```

`pscapture.exe` alone is enough for users (the DLL and the payload travel
inside it). The bare `pscapture.dll` is attached for manual installs:

```bash
rundll32 "pscapture.dll",DllRegisterServer
```
