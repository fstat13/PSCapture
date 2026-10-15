/*
 * PSCapture — PC control center (Win32, pure C, zero dependencies).
 * (c) 2026 fstat13 — all rights reserved. See LICENSE.
 *
 *   - "Deploy payload": manda pscapture.elf alla PS5 (elfldr :9021) e lo
 *     copia nell'autoloader (pldmgr via FTP :2121).
 *   - "Install driver" (admin, dal manifest): copia pscapture.dll ed
 *     esegue la registrazione della webcam virtuale.
 *   - "Start/Stop": avvia il feeder che porta i frame dalla PS5 alla
 *     shared memory del driver (FTP legge /data/pscapture.port).
 *   - INI accanto all'exe (pscapture.ini): ip, gain, shift — modificabili
 *     a caldo dai campi della finestra.
 *
 * Costruito con: gcc -O2 -mwindows pscapture.c pscapture.res -lws2_32
 * (windres per il resource con manifest admin).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shellapi.h>
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shm.h"

#define WM_APP_LOG (WM_APP + 1)
#define WM_APP_STATO (WM_APP + 2)
#define WM_APP_AUTOSTART (WM_APP + 3)

static HINSTANCE g_inst;
static HWND g_hwnd, g_h_ip, g_h_gain, g_h_shift, g_h_log, g_h_stato;
static int driver_installato(void);
static DWORD WINAPI lavoro_installa(LPVOID p);
static DWORD WINAPI lavoro_deploy(LPVOID p);
static volatile long g_streaming;
static HANDLE g_thread;
static char g_dir[MAX_PATH];
static HFONT g_font, g_font_b, g_font_t;
static HWND g_h_sub;
static int g_stato_on;

static BOOL CALLBACK
set_font_cb(HWND c, LPARAM f)
{
  SendMessageA(c, WM_SETFONT, (WPARAM)f, 1);
  return TRUE;
}

/* ---------------------------------------------------------------- */
/* utilita'                                                          */
/* ---------------------------------------------------------------- */

static void
exe_dir(char *out, int sz)
{
  GetModuleFileNameA(NULL, out, sz);
  {
    char *p = strrchr(out, '\\');

    if(p) *p = 0;
  }
}

static void
ini_path(char *out, int sz)
{
  snprintf(out, sz, "%s\\pscapture.ini", g_dir);
}

static void
ini_leggi(const char *chiave, const char *def, char *out, int sz)
{
  char p[MAX_PATH];

  ini_path(p, sizeof(p));
  GetPrivateProfileStringA("pscapture", chiave, def, out, sz, p);
}

static void
ini_scrivi(const char *chiave, const char *val)
{
  char p[MAX_PATH];

  ini_path(p, sizeof(p));
  WritePrivateProfileStringA("pscapture", chiave, val, p);
}

static void
logUI(const char *fmt, ...)
{
  char b[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(b, sizeof(b), fmt, ap);
  va_end(ap);
  if(g_hwnd) PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)_strdup(b));
}

static void
statoUI(const char *s)
{
  if(g_hwnd) PostMessageA(g_hwnd, WM_APP_STATO, 0, (LPARAM)_strdup(s));
}

/* ---------------------------------------------------------------- */
/* rete (dal feeder storico)                                         */
/* ---------------------------------------------------------------- */

static int
connetti(const char *ip, int port, DWORD timeout_ms)
{
  SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  DWORD tv = timeout_ms;

  if(s == INVALID_SOCKET) return -1;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((u_short)port);
  a.sin_addr.s_addr = inet_addr(ip);
  if(connect(s, (struct sockaddr *)&a, sizeof(a))) {
    closesocket(s);
    return -1;
  }
  return (int)s;
}

static int
recv_esatto(int s, void *buf, int n)
{
  char *p = buf;

  while(n > 0) {
    int c = recv((SOCKET)s, p, n, 0);

    if(c <= 0) return -1;
    p += c;
    n -= c;
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* FTP minimale                                                      */
/* ---------------------------------------------------------------- */

static int
ftp_apri(const char *ip)
{
  int c = connetti(ip, 2121, 6000);
  char buf[512];
  int n;

  if(c < 0) return -1;
  n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  if(n <= 0) goto ko;
  send((SOCKET)c, "USER anonymous\r\n", 16, 0);
  n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  if(n <= 0) goto ko;
  send((SOCKET)c, "PASS x\r\n", 8, 0);
  n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  if(n <= 0) goto ko;
  send((SOCKET)c, "TYPE I\r\n", 8, 0);
  n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  if(n <= 0) goto ko;
  return c;
ko:
  closesocket((SOCKET)c);
  return -1;
}

static int
ftp_cmd(int c, const char *cmd, char *risp, int sz)
{
  char buf[1024];
  int n;

  send((SOCKET)c, cmd, (int)strlen(cmd), 0);
  n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  if(n <= 0) return -1;
  buf[n] = 0;
  if(risp) snprintf(risp, sz, "%s", buf);
  return buf[0] == '2' || buf[0] == '3' || buf[0] == '1' ? 0 : -1;
}

static int
ftp_leggi_file(const char *ip, const char *file, char *out, int outsz)
{
  int c = ftp_apri(ip), d = -1, i = 0;
  char buf[4096];
  int n;

  if(c < 0) return -1;
  ftp_cmd(c, "PASV\r\n", buf, sizeof(buf));
  {
    char *p = strchr(buf, '(');
    unsigned h1, h2, h3, h4, p1, p2;

    if(!p || sscanf(p, "(%u,%u,%u,%u,%u,%u)", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
      goto ko;
    d = connetti(ip, (int)(p1 * 256 + p2), 6000);
    if(d < 0) goto ko;
  }
  {
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "RETR %s\r\n", file);
    send((SOCKET)c, cmd, (int)strlen(cmd), 0);
    n = recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
    if(n <= 0 || buf[0] != '1') goto ko;
  }
  for(;;) {
    n = recv((SOCKET)d, buf, sizeof(buf) - 1, 0);
    if(n <= 0) break;
    if(i + n < outsz - 1) {
      memcpy(out + i, buf, n);
      i += n;
    }
  }
  out[i] = 0;
ko:
  if(d >= 0) closesocket((SOCKET)d);
  closesocket((SOCKET)c);
  return i > 0 ? 0 : -1;
}

static int
ftp_manda_file(const char *ip, const char *remoto, const unsigned char *d,
               int len)
{
  int c = ftp_apri(ip), ds = -1;
  char buf[4096];

  if(c < 0) return -1;
  ftp_cmd(c, "PASV\r\n", buf, sizeof(buf));
  {
    char *p = strchr(buf, '(');
    unsigned h1, h2, h3, h4, p1, p2;

    if(!p || sscanf(p, "(%u,%u,%u,%u,%u,%u)", &h1, &h2, &h3, &h4, &p1, &p2) != 6)
      goto ko;
    ds = connetti(ip, (int)(p1 * 256 + p2), 6000);
    if(ds < 0) goto ko;
  }
  {
    char cmd[512];

    snprintf(cmd, sizeof(cmd), "STOR %s\r\n", remoto);
    send((SOCKET)c, cmd, (int)strlen(cmd), 0);
    if(recv((SOCKET)c, buf, sizeof(buf) - 1, 0) <= 0 || buf[0] != '1')
      goto ko;
  }
  if(send((SOCKET)ds, (const char *)d, len, 0) != len) goto ko;
  closesocket((SOCKET)ds);
  ds = -1;
  recv((SOCKET)c, buf, sizeof(buf) - 1, 0);
  closesocket((SOCKET)c);
  return 0;
ko:
  if(ds >= 0) closesocket((SOCKET)ds);
  closesocket((SOCKET)c);
  return -1;
}

/* ---------------------------------------------------------------- */
/* feeder: frame PS5 -> shared memory del driver                     */
/* ---------------------------------------------------------------- */

static ps5cam_shm_t *g_shm;
static float g_gain = 1.0f;
static int g_shift;
static char g_ip[64] = "192.168.0.36";

static void
cv_righe(unsigned char *buf)   /* conversione piani NV12 identica al driver */
{
  int r, k;
  static unsigned char tmp[1920];
  unsigned char *y = buf, *uv;

  if(!g_shift) return;
  k = g_shift;
  for(r = 0; r < PS5CAM_H; r++) {
    unsigned char *row = y + (size_t)r * PS5CAM_W;

    memcpy(tmp, row, k);
    memmove(row, row + k, PS5CAM_W - k);
    memcpy(row + PS5CAM_W - k, tmp, k);
  }
  uv = buf + (size_t)PS5CAM_W * PS5CAM_H;
  for(r = 0; r < PS5CAM_H / 2; r++) {
    unsigned char *row = uv + (size_t)r * PS5CAM_W;

    memcpy(tmp, row, k);
    memmove(row, row + k, PS5CAM_W - k);
    memcpy(row + PS5CAM_W - k, tmp, k);
  }
}

static void
cv_gain(unsigned char *buf, unsigned int len)
{
  unsigned char *uv = buf + PS5CAM_W * PS5CAM_H;
  unsigned int nuv = len > PS5CAM_W * PS5CAM_H ? len - PS5CAM_W * PS5CAM_H : 0;
  unsigned int i;
  int g;

  if(g_gain == 1.0f || !nuv) return;
  g = (int)(g_gain * 256.0f + 0.5f);
  for(i = 0; i < nuv; i++) {
    int v = 128 + ((((int)uv[i] - 128) * g) >> 8);

    uv[i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
  }
}

static void
aggiorna_knob(unsigned int *giro)
{
  char b[64];

  if(!(*giro % 30)) {
    ini_leggi("GAIN", "1.0", b, sizeof(b));
    g_gain = (float)atof(b);
    if(g_gain < 0.0f || g_gain > 2.0f) g_gain = 1.0f;
    ini_leggi("SHIFT", "0", b, sizeof(b));
    g_shift = atoi(b) % 1920;
    if(g_shift < 0) g_shift += 1920;
    if(g_shift & 1) g_shift++;
    ini_leggi("IP", "192.168.0.36", b, sizeof(b));
    if(strcmp(b, g_ip)) {
      snprintf(g_ip, sizeof(g_ip), "%s", b);
    }
  }
  (*giro)++;
}

static DWORD WINAPI
feeder(LPVOID p)
{
  unsigned int n = 0, giro = 0;
  unsigned long long ultimo = 0;
  int port = -1, s = -1;

  (void)p;
  memset(g_shm, 0, sizeof(*g_shm));
  g_shm->magic = PS5CAM_MAGIC;
  g_shm->w = PS5CAM_W;
  g_shm->h = PS5CAM_H;

  while(g_streaming) {
    unsigned char hdr[12];
    char b[64];

    aggiorna_knob(&giro);

    if(s < 0 || port < 0) {
      if(port < 0) {
        if(ftp_leggi_file(g_ip, "/data/pscapture.port", b, sizeof(b))) {
          Sleep(2000);
          continue;
        }
        port = atoi(b);
        if(port <= 0 || port > 65535) { port = -1; Sleep(2000); continue; }
        logUI("porta stream: %d", port);
      }
      s = connetti(g_ip, port, 8000);
      if(s < 0) {
        logUI("PS5 non raggiungibile (%s), riprovo", g_ip);
        Sleep(2000);
        port = -1;
        continue;
      }
      g_shm->feed_ok = 1;
      statoUI("STREAMING - scegli 'PS5 Capture' in Discord");
      logUI("collegato a %s:%d", g_ip, port);
    }

    if(recv_esatto(s, hdr, 12)) goto persa;
    if(memcmp(hdr, "AVS2", 4)) {
      logUI("header inatteso dallo stream");
      goto persa;
    }
    {
      unsigned len = (unsigned)hdr[8] | ((unsigned)hdr[9] << 8) |
                     ((unsigned)hdr[10] << 16) | ((unsigned)hdr[11] << 24);
      unsigned next;

      if(len != PS5CAM_FRAME) goto persa;
      next = (g_shm->cur + 1) % PS5CAM_BUFS;
      if(recv_esatto(s, g_shm->buf[next], (int)len)) goto persa;
      cv_gain(g_shm->buf[next], len);
      cv_righe(g_shm->buf[next]);
      g_shm->cur = next;
      g_shm->seq++;
      g_shm->tick = GetTickCount64();
      n++;
    }
    {
      unsigned long long ora = GetTickCount64();

      if(ora - ultimo >= 3000 && ultimo) {
        char s2[64];

        snprintf(s2, sizeof(s2), "STREAMING  %.1f fps", n * 1000.0 / (ora - ultimo));
        statoUI(s2);
        n = 0;
      }
      ultimo = ora;
    }
    continue;

persa:
    if(s >= 0) closesocket((SOCKET)s);
    s = -1;
    port = -1;
    g_shm->feed_ok = 0;
    logUI("stream caduto, riconnetto");
    Sleep(1000);
  }
  if(s >= 0) closesocket((SOCKET)s);
  g_shm->feed_ok = 0;
  statoUI("FERMO");
  return 0;
}

/* ---------------------------------------------------------------- */
/* azioni bottoni (thread di lavoro)                                 */
/* ---------------------------------------------------------------- */

static unsigned char *
leggi_file(const char *nome, int *len)
{
  char p[MAX_PATH];
  FILE *f;
  unsigned char *d;
  long n;

  snprintf(p, sizeof(p), "%s\\%s", g_dir, nome);
  f = fopen(p, "rb");
  if(!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  d = malloc(n);
  if(d && fread(d, 1, n, f) != (size_t)n) { free(d); d = NULL; }
  fclose(f);
  if(d) *len = (int)n;
  return d;
}

/* il payload viaggia DENTRO l'eseguibile (risorsa RCDATA n.2);
 * come riserva si accetta anche il file accanto all'exe */
static unsigned char *
carica_payload(int *len)
{
  HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(2), RT_RCDATA);

  if(r) {
    HGLOBAL g = LoadResource(NULL, r);
    DWORD n = SizeofResource(NULL, r);
    void *src = g ? LockResource(g) : NULL;

    if(src && n) {
      unsigned char *d = malloc(n);

      if(d) {
        memcpy(d, src, n);
        *len = (int)n;
        logUI("payload dall'eseguibile (%u byte)", (unsigned)n);
        return d;
      }
    }
  }
  logUI("risorsa payload assente: provo il file accanto all'exe");
  return leggi_file("pscapture.elf", len);
}

static DWORD WINAPI
lavoro_deploy(LPVOID p)
{
  int len = 0, s;
  unsigned char *elf;
  char ip[64];
  char buf[4096];

  (void)p;
  ini_leggi("IP", "192.168.0.36", ip, sizeof(ip));
  elf = carica_payload(&len);
  if(!elf) {
    logUI("ERRORE: payload non disponibile (ne' risorsa ne' file)");
    return 1;
  }
  logUI("payload %d byte, invio a %s", len, ip);

  /* 1) autoloader (pldmgr): crea cartella e copia */
  {
    int c = ftp_apri(ip);

    if(c >= 0) {
      ftp_cmd(c, "MKD /data/pldmgr/payloads/pscapture\r\n", buf, sizeof(buf));
      closesocket((SOCKET)c);
    }
  }
  if(ftp_manda_file(ip, "/data/pldmgr/payloads/pscapture/pscapture.elf", elf, len))
    logUI("autoloader: copia FALLITA (pldmgr c'e'?)");
  else
    logUI("autoloader aggiornato (pldmgr)");

  /* 2) avvio immediato via elfldr 9021 */
  s = connetti(ip, 9021, 6000);
  if(s < 0) {
    logUI("elfldr :9021 non raggiungibile (payload non avviato adesso)");
    free(elf);
    return 1;
  }
  send((SOCKET)s, (const char *)elf, len, 0);
  /* 3) leggi lo stream di log per qualche secondo */
  {
    DWORD t0 = GetTickCount();
    int riga = 0, tot = 0;

    while(GetTickCount() - t0 < 4000 && riga < 30) {
      int n = recv((SOCKET)s, buf, sizeof(buf) - 1, 0);

      if(n <= 0) break;
      buf[n] = 0;
      logUI("%s", buf);
      tot += n;
      if(buf[n - 1] == '\n') riga++;
    }
    (void)tot;
  }
  closesocket((SOCKET)s);
  free(elf);
  logUI("payload avviato — la PS5 deve mostrare la notifica PSCapture");
  return 0;
}

static DWORD WINAPI
lavoro_installa(LPVOID p)
{
  char dst[MAX_PATH], src[MAX_PATH], cmd[MAX_PATH * 2];
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  char win[MAX_PATH];

  (void)p;
  {
    char *pf = getenv("ProgramFiles");

    if(pf) snprintf(dst, sizeof(dst), "%s\\PSCapture", pf);
    else snprintf(dst, sizeof(dst), "C:\\Program Files\\PSCapture");
  }
  CreateDirectoryA(dst, NULL);
  logUI("installazione in %s", dst);

  /* il driver viaggia DENTRO l'eseguibile (risorsa RCDATA n.3): lo si
   * estrae e lo si scrive in Program Files; fallback: copia dal disco */
  {
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(3), RT_RCDATA);

    snprintf(cmd, sizeof(cmd), "%s\\pscapture.dll", dst);
    if(r) {
      HGLOBAL g = LoadResource(NULL, r);
      DWORD n = SizeofResource(NULL, r);
      void *sd = g ? LockResource(g) : NULL;

      if(sd && n) {
        HANDLE h = CreateFileA(cmd, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr = 0;

        if(h != INVALID_HANDLE_VALUE) {
          WriteFile(h, sd, n, &wr, NULL);
          CloseHandle(h);
        }
        logUI("driver estratto dall'eseguibile (%u byte)", (unsigned)n);
      }
    } else {
      const char *dlls[] = { "pscapture.dll", "ps5cam.dll", NULL };
      int i;

      for(i = 0; dlls[i]; i++) {
        snprintf(src, sizeof(src), "%s\\%s", g_dir, dlls[i]);
        if(GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES) {
          CopyFileA(src, cmd, FALSE);
          logUI("driver copiato dal disco: %s", dlls[i]);
          break;
        }
      }
    }
    {
      char self[MAX_PATH];

      GetModuleFileNameA(NULL, self, sizeof(self));
      snprintf(cmd, sizeof(cmd), "%s\\pscapture.exe", dst);
      CopyFileA(self, cmd, FALSE);
      logUI("eseguibile installato (payload e driver inclusi)");
    }
  }

  /* registra il filtro DirectShow */
  snprintf(cmd, sizeof(cmd),
           "rundll32.exe \"%s\\pscapture.dll\",DllRegisterServer", dst);
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if(CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    WaitForSingleObject(pi.hProcess, 20000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    logUI("driver registrato — la webcam 'PS5 Capture' e' pronta");
  } else {
    logUI("ERRORE registrazione driver (serve admin?)");
  }
  return 0;
}

/* ---------------------------------------------------------------- */
/* GUI                                                               */
/* ---------------------------------------------------------------- */

#define ID_IP 101
#define ID_GAIN 102
#define ID_SHIFT 103
#define ID_DEPLOY 110
#define ID_INSTALL 111
#define ID_START 112
#define ID_STOP 113
#define ID_APPLY 114
#define ID_LOG 120
#define ID_STATO 121

static void
leggi_campi(void)
{
  char b[64];

  GetWindowTextA(g_h_ip, b, sizeof(b));
  ini_scrivi("IP", b);
  GetWindowTextA(g_h_gain, b, sizeof(b));
  ini_scrivi("GAIN", b);
  GetWindowTextA(g_h_shift, b, sizeof(b));
  ini_scrivi("SHIFT", b);
  logUI("impostazioni salvate (effetto immediato)");
}

static DWORD WINAPI
lavoro_avvio(LPVOID p)
{
  (void)p;
  logUI("--- AVVIO AUTOMATICO ---");
  if(!driver_installato()) {
    logUI("driver non installato: lo installo ora (primo avvio)");
    lavoro_installa(NULL);
  } else {
    logUI("driver gia' installato");
  }
  lavoro_deploy(NULL);
  if(!g_streaming) {
    g_streaming = 1;
    g_thread = CreateThread(NULL, 0, feeder, NULL, 0, NULL);
    statoUI("AVVIO...");
  }
  return 0;
}

static LRESULT CALLBACK
WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch(m) {
  case WM_CREATE: {
    char b[64];
    HINSTANCE hi = g_inst;
    HWND c;

    g_font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, "Segoe UI");
    g_font_b = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");
    g_font_t = CreateFontA(-30, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");

    c = CreateWindowA("STATIC", "PSCapture", WS_CHILD | WS_VISIBLE,
                      20, 14, 400, 42, h, 0, hi, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font_t, 1);
    g_h_sub = CreateWindowA("STATIC", "PS5 screen to PC capture card  -  by fstat13  -  v0.1",
                            WS_CHILD | WS_VISIBLE, 22, 58, 600, 20, h, 0, hi, 0);

    CreateWindowA("STATIC", "PS5 IP:", WS_CHILD | WS_VISIBLE,
                  22, 98, 55, 22, h, 0, hi, 0);
    g_h_ip = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                           78, 96, 140, 24, h, (HMENU)ID_IP, hi, 0);
    ini_leggi("IP", "192.168.0.36", b, sizeof(b));
    SetWindowTextA(g_h_ip, b);

    CreateWindowA("BUTTON", "Deploy payload", WS_CHILD | WS_VISIBLE,
                  238, 95, 130, 26, h, (HMENU)ID_DEPLOY, hi, 0);
    CreateWindowA("BUTTON", "Install driver", WS_CHILD | WS_VISIBLE,
                  376, 95, 120, 26, h, (HMENU)ID_INSTALL, hi, 0);
    c = CreateWindowA("BUTTON", "Start", WS_CHILD | WS_VISIBLE,
                      504, 95, 92, 26, h, (HMENU)ID_START, hi, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font_b, 1);
    c = CreateWindowA("BUTTON", "Stop", WS_CHILD | WS_VISIBLE,
                      604, 95, 92, 26, h, (HMENU)ID_STOP, hi, 0);
    SendMessageA(c, WM_SETFONT, (WPARAM)g_font_b, 1);

    CreateWindowA("STATIC", "gain:", WS_CHILD | WS_VISIBLE,
                  22, 138, 38, 22, h, 0, hi, 0);
    g_h_gain = CreateWindowA("EDIT", "1.0", WS_CHILD | WS_VISIBLE | WS_BORDER,
                             62, 136, 55, 24, h, (HMENU)ID_GAIN, hi, 0);
    ini_leggi("GAIN", "1.0", b, sizeof(b));
    SetWindowTextA(g_h_gain, b);
    CreateWindowA("STATIC", "shift px:", WS_CHILD | WS_VISIBLE,
                  130, 138, 58, 22, h, 0, hi, 0);
    g_h_shift = CreateWindowA("EDIT", "0", WS_CHILD | WS_VISIBLE | WS_BORDER,
                              190, 136, 55, 24, h, (HMENU)ID_SHIFT, hi, 0);
    ini_leggi("SHIFT", "0", b, sizeof(b));
    SetWindowTextA(g_h_shift, b);
    CreateWindowA("BUTTON", "Apply", WS_CHILD | WS_VISIBLE,
                  258, 135, 78, 26, h, (HMENU)ID_APPLY, hi, 0);

    g_h_stato = CreateWindowA("STATIC", "IDLE", WS_CHILD | WS_VISIBLE,
                              24, 176, 660, 24, h, (HMENU)ID_STATO, hi, 0);
    g_h_log = CreateWindowA("LISTBOX", "",
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
                            LBS_NOINTEGRALHEIGHT,
                            22, 210, 664, 320, h, (HMENU)ID_LOG, hi, 0);

    EnumChildWindows(h, set_font_cb, (LPARAM)g_font);
    SendMessageA(g_h_sub, WM_SETFONT, (WPARAM)g_font, 1);
    SendMessageA(g_h_stato, WM_SETFONT, (WPARAM)g_font_b, 1);

    logUI("PSCapture pronto: premo Start e faccio tutto da solo.");
    return 0;
  }
  case WM_APP_AUTOSTART: {
    HANDLE t = CreateThread(NULL, 0, lavoro_avvio, NULL, 0, NULL);

    if(t) CloseHandle(t);
    return 0;
  }
  case WM_COMMAND:
    switch(LOWORD(w)) {
    case ID_DEPLOY: {
      HANDLE t = CreateThread(NULL, 0, lavoro_deploy, NULL, 0, NULL);

      if(t) CloseHandle(t);
      return 0;
    }
    case ID_INSTALL: {
      HANDLE t = CreateThread(NULL, 0, lavoro_installa, NULL, 0, NULL);

      if(t) CloseHandle(t);
      return 0;
    }
    case ID_START:
      if(!g_streaming) {
        g_streaming = 1;
        g_thread = CreateThread(NULL, 0, feeder, NULL, 0, NULL);
        statoUI("AVVIO...");
      }
      return 0;
    case ID_STOP:
      if(g_streaming) {
        g_streaming = 0;
        if(g_thread) { WaitForSingleObject(g_thread, 4000); CloseHandle(g_thread); g_thread = 0; }
        statoUI("STOP");
      }
      return 0;
    case ID_APPLY:
      leggi_campi();
      return 0;
    }
    return 0;
  case WM_CTLCOLORSTATIC: {
    HDC dc = (HDC)w;
    HWND ctl = (HWND)l;

    SetBkMode(dc, TRANSPARENT);
    if(ctl == g_h_stato) {
      SetTextColor(dc, g_stato_on ? RGB(14, 150, 80) : RGB(120, 120, 120));
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    if(ctl == g_h_sub) {
      SetTextColor(dc, RGB(110, 110, 110));
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
  }
  case WM_APP_LOG: {
    char *s = (char *)l;

    if(s) {
      int i = (int)SendMessageA(g_h_log, LB_ADDSTRING, 0, (LPARAM)s);
      int cnt = (int)SendMessageA(g_h_log, LB_GETCOUNT, 0, 0);

      if(cnt > 500) SendMessageA(g_h_log, LB_DELETESTRING, 0, 0);
      SendMessageA(g_h_log, LB_SETTOPINDEX, i, 0);
      free(s);
    }
    return 0;
  }
  case WM_APP_STATO: {
    char *s = (char *)l;

    if(s) {
      SetWindowTextA(g_h_stato, s);
      g_stato_on = (strstr(s, "STREAMING") || strstr(s, "AVVIO")) ? 1 : 0;
      InvalidateRect(g_h_stato, NULL, TRUE);
      free(s);
    }
    return 0;
  }
  case WM_CLOSE:
    if(g_streaming) {
      g_streaming = 0;
      if(g_thread) { WaitForSingleObject(g_thread, 4000); CloseHandle(g_thread); }
    }
    DestroyWindow(h);
    return 0;
  case WM_DESTROY:
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

static int
is_admin(void)
{
  SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
  PSID g = NULL;
  BOOL r = FALSE;

  if(AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                              DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &g)) {
    CheckTokenMembership(NULL, g, &r);
    FreeSid(g);
  }
  return r;
}

static int
driver_installato(void)
{
  HKEY k;
  int ok = 0;

  if(RegOpenKeyA(HKEY_CLASSES_ROOT,
                 "CLSID\\{50533543-414d-4def-9b0e-192080102030}\\InprocServer32",
                 &k) == ERROR_SUCCESS) {
    ok = 1;
    RegCloseKey(k);
  }
  return ok;
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show)
{
  WNDCLASSA wc;
  MSG msg;
  HANDLE map;

  (void)prev;
  (void)cmd;
  if(!is_admin()) {
    SHELLEXECUTEINFOA sei;
    char self[MAX_PATH];

    memset(&sei, 0, sizeof(sei));
    GetModuleFileNameA(NULL, self, sizeof(self));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.lpFile = self;
    sei.lpDirectory = NULL;
    sei.nShow = SW_SHOWNORMAL;
    if(ShellExecuteExA(&sei)) return 0;   /* rilanciata come admin */
  }
  g_inst = inst;
  exe_dir(g_dir, sizeof(g_dir));

  WSAStartup(MAKEWORD(2, 2), &(WSADATA){0});

  map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           (DWORD)sizeof(ps5cam_shm_t), PS5CAM_SHM_NAME);
  if(map) {
    g_shm = (ps5cam_shm_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                          sizeof(ps5cam_shm_t));
  }
  if(!g_shm) {
    MessageBoxA(NULL, "Impossibile creare la shared memory del driver.",
                "PSCapture", MB_ICONERROR);
    return 1;
  }
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
  wc.lpszClassName = "PSCaptureWnd";
  RegisterClassA(&wc);

  g_hwnd = CreateWindowA("PSCaptureWnd", "PSCapture by fstat13 — PS5 screen to PC",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, 728, 580,
                         NULL, NULL, inst, NULL);
  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);
  PostMessageA(g_hwnd, WM_APP_AUTOSTART, 0, 0);

  while(GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return 0;
}
