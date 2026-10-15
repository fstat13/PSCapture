# PSCapture — la guida a prova di scemo

Da zero alla tua PS5 dentro Discord. Ogni passo dice esattamente cosa devi
vedere sullo schermo. Se qualcosa non torna, salta alla tabella dei problemi
in fondo.

🇬🇧 English version: [GUIDE.md](GUIDE.md)

---

## 0. Cosa serve

**Sulla console** — tutto standard nei setup jailbreak di oggi:

| Cosa | Porta | A che serve |
|---|---|---|
| Payload loader (tipo `elfldr`) | TCP **9021** | esegue il payload che gli mandiamo |
| FTP anonimo | TCP **2121** | l'app legge/scrive file sulla console |
| `pldmgr` autoloader (opzionale) | — | fa partire il payload a ogni boot |

**Sul PC**: Windows 10/11 64-bit, rete cablata consigliata.

**L'app**: `PSCapture.exe` — **un solo file**. Scaricalo dalla pagina Releases
della repo. È il pacchetto completo: il payload per la console e il driver
per Windows sono dentro di lui.

---

## 1. Primo avvio sul PC

1. Metti `PSCapture.exe` dove vuoi (va benissimo il Desktop) e **fai doppio click**.

2. Windows chiede *"Consenti a questa app di apportare modifiche al
   dispositivo?"* → clicca **Sì**.
   *Perché:* registrare la webcam virtuale per tutti i programmi del PC
   richiede i privilegi di amministratore, una volta sola. L'app si eleva da
   sola — niente tasto destro.

3. Si apre la finestra (scura, con il bottone blu grande **START STREAMING**).
   Se la tua PS5 non è a `192.168.0.36`: scrivi il suo indirizzo nel campo
   **PS5 IP** e premi **Apply**.

4. Da qui in poi l'app fa tutto da sola. Guarda il pannello log scuro:

   | Dovresti leggere | Significa |
   |---|---|
   | `driver gia' installato` | la camera è già registrata — ok |
   | `driver estratto dall'eseguibile (… byte)` + `driver registrato - la webcam 'PS5 Capture' e' pronta` | primo avvio: driver installato |
   | `payload dall'eseguibile (… byte)` + `payload avviato - la PS5 deve mostrare la notifica PSCapture` | il payload sta girando sulla console |
   | `porta stream: 9xxx` poi `collegato a 192.168.0.36:9xxx` | stream connesso |
   | la pillola diventa **verde**: `STREAMING - scegli 'PS5 Capture' in Discord` | sei live |

5. Sulla PS5 compare la notifica di sistema: **PSCapture is streaming on:PORT**.
   (Quel toast è la console che ti dice che il payload è vivo.)

> Se la pillola resta su *"IN ATTESA DELLA PS5 — accesa e col jailbreak
> avviato?"* la console è spenta, non è jailbroken, o l'IP è sbagliato.
> Vedi la sezione 5.

## 2. Usarlo

**Discord (in chiamata)** — *Impostazioni → Voce e video → Telecamera* →
**PS5 Capture**. In chiamata l'immagine è 720p: è il motore di Discord, chiede
1280x720 a *tutte* le camere (capture card incluse).

**OBS** — *Sorgenti → **+** → Video Capture Device* → **PS5 Capture**.
Qui va alla piena risoluzione **1920x1080**.

**ffmpeg** — `ffmpeg -f dshow -i video="PS5 Capture" registrazione.mkv`

## 3. Uso quotidiano

- Apri `PSCapture.exe` → **Sì** all'UAC. L'app si ricollega da sola: se il
  payload è già sulla console (autoloader) lo stream parte subito; se la
  console è spenta la pillola dice *"IN ATTESA DELLA PS5"* e si connette nel
  momento in cui la console è pronta.
- Premi **Stop** (o chiudi l'app) per fermare lo stream. La camera si congela
  sull'ultimo frame; gli altri programmi continuano a vedere il dispositivo.
- **Deploy payload** rimanda il payload a mano (utile dopo un riavvio della
  console senza autoloader). **Install driver** riregistra la camera a mano
  (utile se il log mostra un errore del driver).

## 4. Le due manopole

| Manopola | Cosa fa | Regole |
|---|---|---|
| `gain` | forza del colore (croma) | `1.0` = grezzo, è il valore giusto. Toccala solo se i colori sembrano sbagliati. |
| `shift px` | sposta l'immagine a destra/sinistra | **solo numeri PARI** (un numero dispari inverte i colori). Regola finché l'immagine non è centrata: il testo del titolo della console è un buon riferimento. |

Entrambe si applicano in diretta (circa un secondo) e sono salvate in
`pscapture.ini` accanto all'exe, quindi sopravvivono ai riavvii.

## 5. Problemi — sintomo per sintomo

| Sintomo | Causa | Rimedio |
|---|---|---|
| La pillola resta su `AVVIO...` / `IN ATTESA DELLA PS5` | console spenta, **non jailbroken**, o IP sbagliato | accendi la console, avvia il jailbreak (le porte 9021/2121 devono essere aperte), controlla il campo IP |
| Il log ripete `PS5 non raggiungibile (...), riprovo` | come sopra | come sopra |
| La camera `PS5 Capture` non appare in Discord/OBS | driver mai registrato (install fallito) | premi **Install driver**; leggi la riga di log che segue — ora riporta l'errore *vero* |
| Log: `ERRORE: driver non scrivibile in C:\Program Files\PSCapture (serve admin?)` | app non elevata | chiudi e riavvia `PSCapture.exe` accettando il prompt UAC |
| Log: `ERRORE caricando la dll (err=126)` | l'antivirus ha messo in quarantena la dll estratta | controlla la quarantena dell'antivirus per `pscapture.dll`, sbloccala, premi di nuovo **Install driver** |
| La camera appare ma l'immagine è nera | stream non partito | controlla la pillola; premi **Deploy payload** e/o **START STREAMING** |
| I colori sembrano sbagliati (pelle rossastra, cielo arancione) | qualcuno ha toccato `gain` | rimetti `gain` a `1.0` → Apply |
| Immagine decentrata / barra nera | allineamento | regola `shift px` con numeri pari finché non è centrata |
| Discord continua a mostrare 720p | il motore di Discord limita ogni camera a 720p | usa OBS per il 1080p, o la condivisione schermo di Discord (1080p60 con Nitro) |
| OBS vede la camera, Discord no | Discord era già aperto quando hai installato il driver | chiudi Discord COMPLETAMENTE (anche dalla tray) e riaprilo |
| Tutto il resto | — | leggi `pscapture-app.log` (accanto all'exe, o in `%TEMP%`) e il log della console `/data/pscapture.log` (via FTP) |

## 6. Disinstallazione

1. Chiudi l'app.
2. `rundll32 "%ProgramFiles%\PSCapture\pscapture.dll",DllUnregisterServer`
   (rimuove la registrazione della camera).
3. Cancella `C:\Program Files\PSCapture`.
4. Opzionale, sulla console: cancella `/data/pldmgr/payloads/pscapture/` così
   l'autoloader smette di avviarlo.
