# PSCapture — build dai sorgenti, passo per passo

Tutto il necessario per passare da questi sorgenti a un `PSCapture.exe`
funzionante e a un `pscapture.elf` per la console. Ogni comando è scritto
esattamente come va eseguito, in ordine. Qui dentro non c'è magia: l'ordine
conta e ogni passo è spiegato.

Ci sono due metà, che si compilano in posti diversi:

| Metà | Output | Dove si compila | Toolchain |
|---|---|---|---|
| Payload console | `payload/pscapture.elf` | Docker (container Linux) | PS5 payload SDK (prospero-clang) |
| Lato PC | `pc/pscapture.exe` + `pc/pscapture.dll` | Windows | MinGW-w64 (`gcc` + `windres`) |

🇬🇧 English version: [BUILD.md](BUILD.md)

---

## Parte 1 — Il payload per la console

### 1.1 Cosa serve

- **Docker** (Docker Desktop su Windows, o Docker normale su Linux/macOS).
- **L'immagine del PS5 payload SDK.** È il payload SDK della community per
  PS5 (`prospero-clang`, `make`, `PS5_PAYLOAD_SDK` nell'ambiente) impacchettato
  in una immagine. Sulla macchina di sviluppo l'immagine si chiama `ps5sdk:18`
  — qualsiasi nome va bene, basta adattare i comandi.

Controlla che l'immagine ci sia:

```bash
docker images | grep ps5sdk
```

### 1.2 Build

Dalla radice della repo:

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload ps5sdk:18 make
```

Su Windows (PowerShell/cmd) metti il percorso completo al posto di
`"$PWD/payload"`, es.: `-v "C:\Users\tuonome\PSCapture\payload:/payload"`.

Risultato: **`payload/pscapture.elf`** (~175 KB).

Il `Makefile` fa l'unica cosa che conta: compila `main.c` con il clang
Prospero dell'SDK e linka l'ELF finale. Nessun altro input.

### 1.3 Opzionale — mandarlo alla console

Con la console jailbroken (payload loader sulla porta 9021):

```bash
docker run --rm -v "$PWD/payload:/payload" -w /payload ps5sdk:18 \
    make deploy PS5_HOST=192.168.0.36
```

(oppure carica `pscapture.elf` con il loader che usi di solito).

Come si presenta un avvio sano sulla console:

- appare `/data/pscapture.log` con il log di partenza,
- appare `/data/pscapture.port` con la porta TCP dello stream,
- la console mostra la notifica *PSCapture is streaming on:PORT*.

---

## Parte 2 — Il lato PC

### 2.1 Cosa serve (Windows)

**MinGW-w64 64-bit** con `gcc` e `windres`. Sulla macchina di sviluppo:

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT
```

(oppure qualsiasi MSYS2 con `mingw-w64-x86_64-gcc` — `pacman -S mingw-w64-x86_64-gcc`).

Controlla che entrambi siano nel PATH:

```bash
gcc --version
windres --version
```

### 2.2 Build — l'ordine è obbligatorio

`pscapture.res` è un bundle che **incastra i file così come sono su disco**:
l'icona, il payload della console e il driver della capture card. Quindi la
DLL va compilata *prima* del passo risorse, e il payload deve già trovarsi in
`pc/` quando gira `windres`. L'EXE si compila per ultimo.

```bash
cd pc

# 0) porta il payload appena compilato accanto al .rc (viene incastrato così com'è)
cp ../payload/pscapture.elf .

# 1) il driver della capture card (filtro sorgente DirectShow)
gcc -O2 -shared -static -static-libgcc -o pscapture.dll ps5cam.c \
    -lole32 -loleaut32 -luser32 -ladvapi32 -lws2_32

# 2) bundle: icona + payload + driver in un unico oggetto risorse
#    (pscapture.rc => 1 ICON pscapture.ico / 2 RCDATA pscapture.elf
#                     / 3 RCDATA pscapture.dll)
windres pscapture.rc -O coff -o pscapture.res

# 3) l'app — un unico eseguibile autocontenuto
gcc -O2 -mwindows -o pscapture.exe pscapture.c pscapture.res \
    -lws2_32 -lshell32 -ladvapi32 -luxtheme
```

### 2.3 Verifica della build

```bash
# il driver deve dipendere SOLO da DLL di sistema (niente libgcc/libwinpthread):
objdump -p pscapture.dll | grep "DLL Name"

# le dimensioni giuste: exe ~500 KB, dll ~220 KB
ls -la pscapture.exe pscapture.dll

# payload e driver sono davvero dentro l'eseguibile:
strings pscapture.exe | grep -c PSCapture
```

Avvia `pscapture.exe`: si apre la finestra (prompt UAC → Sì) e il pannello log
mostra la sequenza controllo driver → estrazione payload → tentativo di
deploy. È la stessa sequenza descritta in `docs/INSTALLER.md`.

### 2.4 Trappole (imparate sul campo)

- **`-static -static-libgcc` sulla DLL non è cosmetico.** Senza, il driver
  può ritrovarsi a dipendere dalle DLL runtime di mingw che *non* sono nel
  PATH di sistema quando Windows (o un'altra app) lo carica — il classico
  *"impossibile trovare il modulo specificato"*.
- **Ordine di build.** `windres` legge `pscapture.dll` e `pscapture.elf` da
  `pc/` al momento della compilazione. Ricompili la DLL → rifai `windres` →
  ricompili l'EXE, ogni volta. Dimenticare il passo centrale spedisce il
  driver vecchio dentro l'exe nuovo.
- **Solo ASCII** nelle stringhe passate alle API Win32 `*A` (titoli, bottoni,
  log). Byte UTF-8 in una stringa ANSI si vedono come caratteri rotti.
- Il **manifest** (`pc/pscapture.manifest`) è tenuto solo per riferimento;
  la build attuale **non** lo incastra — l'auto-elevazione è fatta nel codice
  (`ShellExecuteEx … "runas"`), cosa che evita il `.rsrc merge failure` di
  mingw con più manifest.

### 2.5 Preparare una release

```bash
mkdir -p dist
cp pc/pscapture.exe pc/pscapture.dll dist/
```

Per gli utenti basta `pscapture.exe` da solo (la DLL e il payload viaggiano
dentro). La `pscapture.dll` sciolta si allega per le installazioni manuali:

```bash
rundll32 "pscapture.dll",DllRegisterServer
```
