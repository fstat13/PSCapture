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
#include <uxtheme.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

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
static HFONT g_font, g_font_b, g_font_t, g_font_s, g_font_log;
static HBRUSH g_br_bg, g_br_edit, g_br_pill;
static char g_stato_text[96] = "IDLE";
static int g_stato_level;   /* 0 idle, 1 attesa, 2 streaming */

/* palette scura */
#define CL_BG RGB(16, 20, 27)
#define CL_TBAR RGB(11, 14, 19)
#define CL_EDIT RGB(12, 15, 21)
#define CL_LINE RGB(32, 39, 51)
#define CL_TXT RGB(232, 237, 244)
#define CL_DIM RGB(152, 162, 179)
#define CL_ACC RGB(41, 134, 235)
#define CL_ACC_D RGB(28, 100, 180)
#define CL_GHOST RGB(26, 32, 41)
#define CL_GHOST_D RGB(38, 46, 59)
#define CL_GBD RGB(44, 54, 70)
#define CL_GREEN RGB(34, 197, 94)
#define CL_AMBER RGB(245, 158, 11)
#define CL_DIM2 RGB(120, 130, 145)
#define CL_PILL RGB(26, 33, 48)
#define CL_EDTX RGB(203, 211, 224)

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
  {
    char p[MAX_PATH];
    FILE *f;

    snprintf(p, sizeof(p), "%s\\pscapture-app.log", g_dir);
    f = fopen(p, "a");
    if(!f) {
      char t[MAX_PATH];
      int e = errno;
      DWORD n = GetTempPathA(sizeof(t), t);

      if(n > 0 && n < sizeof(t)) {
        snprintf(p, sizeof(p), "%sps5capture-app.log", t);
        f = fopen(p, "a");
      }
      if(f) {
        char nota[300];

        snprintf(nota, sizeof(nota),
                 "log accanto all'exe non scrivibile (errno=%d), uso: %s", e, p);
        fprintf(f, "%s\n", nota);
      }
    }
    if(f) {
      SYSTEMTIME st;

      GetLocalTime(&st);
      fprintf(f, "[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute,
              st.wSecond, b);
      fclose(f);
    }
  }
  if(g_hwnd) PostMessageA(g_hwnd, WM_APP_LOG, 0, (LPARAM)_strdup(b));
}

static void
statoUI(const char *s)
{
  if(!strcmp(s, g_stato_text)) return;
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
        statoUI("IN ATTESA DELLA PS5 - accesa e col jailbreak avviato?");
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
  logUI("payload avviato - la PS5 deve mostrare la notifica PSCapture");
  return 0;
}

static DWORD WINAPI
lavoro_installa(LPVOID p)
{
  char dst[MAX_PATH], src[MAX_PATH], cmd[MAX_PATH * 2];

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
  snprintf(cmd, sizeof(cmd), "%s\\pscapture.dll", dst);
  {
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(3), RT_RCDATA);
    int scritto = 0;

    if(r) {
      HGLOBAL g = LoadResource(NULL, r);
      DWORD n = SizeofResource(NULL, r);
      void *sd = g ? LockResource(g) : NULL;

      if(sd && n) {
        HANDLE h = CreateFileA(cmd, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD wr = 0;

        if(h != INVALID_HANDLE_VALUE) {
          if(WriteFile(h, sd, n, &wr, NULL) && wr == n) scritto = 1;
          CloseHandle(h);
        }
        if(scritto) {
          logUI("driver estratto dall'eseguibile (%u byte)", (unsigned)n);
        } else {
          logUI("ERRORE: driver non scrivibile in %s (serve admin?)", dst);
        }
      }
    } else {
      const char *dlls[] = { "pscapture.dll", "ps5cam.dll", NULL };
      int i;

      for(i = 0; dlls[i]; i++) {
        snprintf(src, sizeof(src), "%s\\%s", g_dir, dlls[i]);
        if(GetFileAttributesA(src) != INVALID_FILE_ATTRIBUTES &&
           CopyFileA(src, cmd, FALSE)) {
          scritto = 1;
          logUI("driver copiato dal disco: %s", dlls[i]);
          break;
        }
      }
    }
    {
      char self[MAX_PATH];

      GetModuleFileNameA(NULL, self, sizeof(self));
      snprintf(cmd, sizeof(cmd), "%s\\pscapture.exe", dst);
      if(!CopyFileA(self, cmd, FALSE)) {
        logUI("eseguibile gia' presente in %s", dst);
      } else {
        logUI("eseguibile installato (payload e driver inclusi)");
      }
      snprintf(cmd, sizeof(cmd), "%s\\pscapture.dll", dst);
    }

    /* registra il filtro DirectShow (con codici errore veri, niente
     * rundll32: se la dll non si carica lo diciamo e basta) */
    if(GetFileAttributesA(cmd) != INVALID_FILE_ATTRIBUTES) {
      HMODULE m = LoadLibraryExA(cmd, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

      if(m) {
        typedef HRESULT (WINAPI *reg_t)(void);
        reg_t fn = (reg_t)(void *)GetProcAddress(m, "DllRegisterServer");

        if(fn) {
          HRESULT hr = fn();

          if(SUCCEEDED(hr)) {
            logUI("driver registrato - la webcam 'PS5 Capture' e' pronta");
          } else {
            logUI("ERRORE registrazione driver (hr=0x%08lx)", (unsigned long)hr);
          }
        } else {
          logUI("ERRORE: DllRegisterServer assente nella dll");
        }
        FreeLibrary(m);
      } else {
        logUI("ERRORE caricamento dll (err=%lu)", GetLastError());
      }
    } else {
      logUI("registrazione saltata: dll assente in %s", dst);
    }
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

    g_font = CreateFontA(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, "Segoe UI");
    g_font_b = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");
    g_font_t = CreateFontA(-34, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");
    g_font_s = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, "Segoe UI");
    g_font_log = CreateFontA(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH, "Consolas");

    g_br_bg = CreateSolidBrush(CL_BG);
    g_br_edit = CreateSolidBrush(CL_EDIT);
    g_br_pill = CreateSolidBrush(CL_PILL);

    /* riga 1: indirizzo + azioni */
    CreateWindowA("STATIC", "PS5 IP", WS_CHILD | WS_VISIBLE,
                  26, 148, 50, 18, h, 0, hi, 0);
    g_h_ip = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                           82, 142, 150, 30, h, (HMENU)ID_IP, hi, 0);
    ini_leggi("IP", "192.168.0.36", b, sizeof(b));
    SetWindowTextA(g_h_ip, b);

    CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  250, 142, 132, 30, h, (HMENU)ID_DEPLOY, hi, 0);
    CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  392, 142, 124, 30, h, (HMENU)ID_INSTALL, hi, 0);

    /* riga 2: start/stop + regolazioni */
    CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  26, 188, 210, 48, h, (HMENU)ID_START, hi, 0);
    CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  250, 188, 110, 48, h, (HMENU)ID_STOP, hi, 0);

    CreateWindowA("STATIC", "gain", WS_CHILD | WS_VISIBLE,
                  388, 204, 32, 18, h, 0, hi, 0);
    g_h_gain = CreateWindowA("EDIT", "1.0", WS_CHILD | WS_VISIBLE,
                             424, 199, 58, 26, h, (HMENU)ID_GAIN, hi, 0);
    ini_leggi("GAIN", "1.0", b, sizeof(b));
    SetWindowTextA(g_h_gain, b);
    CreateWindowA("STATIC", "shift px", WS_CHILD | WS_VISIBLE,
                  496, 204, 48, 18, h, 0, hi, 0);
    g_h_shift = CreateWindowA("EDIT", "0", WS_CHILD | WS_VISIBLE,
                              550, 199, 58, 26, h, (HMENU)ID_SHIFT, hi, 0);
    ini_leggi("SHIFT", "0", b, sizeof(b));
    SetWindowTextA(g_h_shift, b);
    CreateWindowA("BUTTON", "", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                  622, 197, 100, 30, h, (HMENU)ID_APPLY, hi, 0);

    /* log */
    g_h_log = CreateWindowA("EDIT", "",
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                            26, 324, 728, 312, h, (HMENU)ID_LOG, hi, 0);
    SendMessageA(g_h_log, EM_SETLIMITTEXT, 250000, 0);
    SendMessageA(g_h_log, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(12, 12));
    SetWindowTheme(g_h_log, L"DarkMode_Explorer", NULL);

    EnumChildWindows(h, set_font_cb, (LPARAM)g_font);
    SendMessageA(g_h_log, WM_SETFONT, (WPARAM)g_font_log, 1);

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
  case WM_PAINT: {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r, rr;
    HBRUSH b, ob;
    HPEN op;
    HFONT old;

    GetClientRect(h, &r);
    FillRect(dc, &r, g_br_bg);

    /* barra titolo con logo */
    rr = r;
    rr.bottom = 44;
    b = CreateSolidBrush(CL_TBAR);
    FillRect(dc, &rr, b);
    DeleteObject(b);
    b = CreateSolidBrush(CL_ACC);
    ob = (HBRUSH)SelectObject(dc, b);
    op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, 16, 9, 42, 35, 10, 10);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    {
      POINT pt[3];

      pt[0].x = 24; pt[0].y = 15;
      pt[1].x = 24; pt[1].y = 29;
      pt[2].x = 34; pt[2].y = 22;
      b = CreateSolidBrush(RGB(255, 255, 255));
      ob = (HBRUSH)SelectObject(dc, b);
      op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
      Polygon(dc, pt, 3);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(b);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CL_TXT);
    old = SelectObject(dc, g_font_b);
    {
      RECT t = {52, 12, 300, 34};

      DrawTextA(dc, "PSCapture", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
    {
      HPEN pn = CreatePen(PS_SOLID, 2, CL_DIM);
      int W = r.right;

      op = (HPEN)SelectObject(dc, pn);
      MoveToEx(dc, W - 72, 22, NULL);
      LineTo(dc, W - 58, 22);
      MoveToEx(dc, W - 34, 16, NULL);
      LineTo(dc, W - 20, 30);
      MoveToEx(dc, W - 34, 30, NULL);
      LineTo(dc, W - 20, 16);
      SelectObject(dc, op);
      DeleteObject(pn);
    }
    /* hero */
    SetTextColor(dc, CL_TXT);
    old = SelectObject(dc, g_font_t);
    {
      RECT t = {26, 50, 700, 92};

      DrawTextA(dc, "PSCapture", -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
    old = SelectObject(dc, g_font_s);
    SetTextColor(dc, CL_DIM);
    {
      RECT t = {28, 92, 750, 114};

      DrawTextA(dc, "PS5 screen to PC capture card  -  by fstat13  -  v0.1",
                -1, &t, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
    b = CreateSolidBrush(CL_LINE);
    rr.left = 0;
    rr.top = 126;
    rr.right = r.right;
    rr.bottom = 127;
    FillRect(dc, &rr, b);
    DeleteObject(b);
    /* cornici dei campi e del log */
    b = CreateSolidBrush(CL_GBD);
    {
      RECT e = {81, 141, 233, 173};

      FrameRect(dc, &e, b);
      e.left = 423; e.top = 198; e.right = 483; e.bottom = 226;
      FrameRect(dc, &e, b);
      e.left = 549; e.top = 198; e.right = 609; e.bottom = 226;
      FrameRect(dc, &e, b);
      e.left = 25; e.top = 323; e.right = 755; e.bottom = 637;
      FrameRect(dc, &e, b);
    }
    DeleteObject(b);
    /* pill di stato */
    {
      COLORREF dot = (g_stato_level == 2) ? CL_GREEN :
                     (g_stato_level == 1) ? CL_AMBER : CL_DIM2;

      b = CreateSolidBrush(CL_PILL);
      ob = (HBRUSH)SelectObject(dc, b);
      op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
      RoundRect(dc, 26, 258, 754, 302, 44, 44);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(b);
      b = CreateSolidBrush(dot);
      ob = (HBRUSH)SelectObject(dc, b);
      op = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
      Ellipse(dc, 46, 269, 58, 281);
      SelectObject(dc, ob);
      SelectObject(dc, op);
      DeleteObject(b);
      SetTextColor(dc, (g_stato_level == 2) ? CL_TXT : CL_DIM);
      old = SelectObject(dc, g_font_b);
      {
        RECT t = {68, 258, 740, 302};

        DrawTextA(dc, g_stato_text, -1, &t,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
      }
      SelectObject(dc, old);
    }
    EndPaint(h, &ps);
    return 0;
  }
  case WM_DRAWITEM: {
    DRAWITEMSTRUCT *d = (DRAWITEMSTRUCT *)l;
    RECT r = d->rcItem;
    int start = (d->CtlID == ID_START);
    int sel = (d->itemState & ODS_SELECTED) != 0;
    int dis = (d->itemState & ODS_DISABLED) != 0;
    const char *lbl = "";
    COLORREF bg, fg, bd;
    HBRUSH br, ob;
    HPEN pn, op;
    HFONT of;

    switch(d->CtlID) {
    case ID_START: lbl = "START STREAMING"; break;
    case ID_STOP: lbl = "STOP"; break;
    case ID_DEPLOY: lbl = "Deploy payload"; break;
    case ID_INSTALL: lbl = "Install driver"; break;
    case ID_APPLY: lbl = "Apply"; break;
    }
    if(start) {
      bg = sel ? CL_ACC_D : CL_ACC;
      fg = RGB(255, 255, 255);
      bd = bg;
    } else {
      bg = sel ? CL_GHOST_D : CL_GHOST;
      fg = CL_TXT;
      bd = CL_GBD;
    }
    if(dis) {
      bg = CL_GHOST;
      fg = CL_DIM2;
    }
    br = CreateSolidBrush(bg);
    pn = CreatePen(PS_SOLID, 1, bd);
    ob = (HBRUSH)SelectObject(d->hDC, br);
    op = (HPEN)SelectObject(d->hDC, pn);
    RoundRect(d->hDC, r.left + 1, r.top + 1, r.right - 1, r.bottom - 1,
              start ? 14 : 10, start ? 14 : 10);
    SelectObject(d->hDC, ob);
    SelectObject(d->hDC, op);
    DeleteObject(br);
    DeleteObject(pn);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, fg);
    of = (HFONT)SelectObject(d->hDC, g_font_b);
    DrawTextA(d->hDC, lbl, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->hDC, of);
    return TRUE;
  }
  case WM_CTLCOLORSTATIC: {
    HDC dc = (HDC)w;
    HWND ctl = (HWND)l;

    if(ctl == g_h_log) {   /* l'EDIT readonly arriva qui */
      SetBkMode(dc, OPAQUE);
      SetBkColor(dc, CL_EDIT);
      SetTextColor(dc, CL_EDTX);
      return (LRESULT)g_br_edit;
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CL_DIM);
    return (LRESULT)g_br_bg;
  }
  case WM_CTLCOLOREDIT: {
    HDC dc = (HDC)w;

    SetBkColor(dc, CL_EDIT);
    SetTextColor(dc, CL_TXT);
    return (LRESULT)g_br_edit;
  }
  case WM_APP_LOG: {
    char *s = (char *)l;

    if(s) {
      int len = (int)SendMessageA(g_h_log, WM_GETTEXTLENGTH, 0, 0);

      if(len > 150000) {
        SendMessageA(g_h_log, EM_SETSEL, 0, 60000);
        SendMessageA(g_h_log, EM_REPLACESEL, FALSE, (LPARAM)"");
        len = (int)SendMessageA(g_h_log, WM_GETTEXTLENGTH, 0, 0);
      }
      SendMessageA(g_h_log, EM_SETSEL, len, len);
      SendMessageA(g_h_log, EM_REPLACESEL, FALSE, (LPARAM)s);
      SendMessageA(g_h_log, EM_REPLACESEL, FALSE, (LPARAM)"\r\n");
      SendMessageA(g_h_log, EM_SCROLLCARET, 0, 0);
      free(s);
    }
    return 0;
  }
  case WM_APP_STATO: {
    char *s = (char *)l;

    if(s) {
      RECT pr = {26, 258, 754, 302};

      snprintf(g_stato_text, sizeof(g_stato_text), "%s", s);
      g_stato_level = strstr(s, "STREAMING") ? 2 :
                      (strstr(s, "ATTESA") || strstr(s, "AVVIO")) ? 1 : 0;
      InvalidateRect(h, &pr, FALSE);
      free(s);
    }
    return 0;
  }
  case WM_NCHITTEST: {
    POINT p = {LOWORD(l), HIWORD(l)};
    RECT wr;
    int W;

    GetWindowRect(h, &wr);
    p.x -= wr.left;
    p.y -= wr.top;
    W = wr.right - wr.left;
    if(p.y >= 0 && p.y < 44) {
      if(p.x >= W - 80 && p.x < W - 44) return HTMINBUTTON;
      if(p.x >= W - 44) return HTCLOSE;
      return HTCAPTION;
    }
    return HTCLIENT;
  }
  case WM_NCLBUTTONDOWN:
    if(w == HTMINBUTTON) {
      ShowWindow(h, SW_MINIMIZE);
      return 0;
    }
    if(w == HTCLOSE) {
      PostMessageA(h, WM_CLOSE, 0, 0);
      return 0;
    }
    break;
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
    char dll[MAX_PATH];
    DWORD sz = sizeof(dll), tipo = 0;

    if(RegQueryValueExA(k, NULL, NULL, &tipo, (LPBYTE)dll, &sz) == ERROR_SUCCESS &&
       (tipo == REG_SZ || tipo == REG_EXPAND_SZ) &&
       GetFileAttributesA(dll) != INVALID_FILE_ATTRIBUTES) {
      ok = 1;
    }
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

  g_inst = inst;
  exe_dir(g_dir, sizeof(g_dir));
  logUI("PSCapture avvio: admin=%d no_elevate=%d dir=%s", is_admin(),
        getenv("PSCAPTURE_NO_ELEVATE") != NULL, g_dir);

  if(!is_admin() && !getenv("PSCAPTURE_NO_ELEVATE")) {
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
    logUI("richiedo elevazione (prompt UAC)...");
    if(ShellExecuteExA(&sei)) return 0;   /* rilanciata come admin */
    logUI("elevazione rifiutata: continuo senza privilegi");
  }

  WSAStartup(MAKEWORD(2, 2), &(WSADATA){0});

  map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                           (DWORD)sizeof(ps5cam_shm_t), PS5CAM_SHM_NAME);
  if(map) {
    g_shm = (ps5cam_shm_t *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                          sizeof(ps5cam_shm_t));
  }
  if(!g_shm) {
    logUI("ERRORE: shared memory non creabile");
    MessageBoxA(NULL, "Impossibile creare la shared memory del driver.",
                "PSCapture", MB_ICONERROR);
    return 1;
  }
  logUI("shared memory OK");
  memset(&wc, 0, sizeof(wc));
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(NULL, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(CL_BG);
  wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
  wc.lpszClassName = "PSCaptureWnd";
  RegisterClassA(&wc);

  {
    g_hwnd = CreateWindowExA(WS_EX_APPWINDOW, "PSCaptureWnd",
                             "PSCapture by fstat13 - PS5 screen to PC",
                             WS_POPUP,
                             CW_USEDEFAULT, CW_USEDEFAULT, 780, 660,
                             NULL, NULL, inst, NULL);
    if(g_hwnd) {
      HRGN rg = CreateRoundRectRgn(0, 0, 781, 661, 20, 20);

      if(rg) SetWindowRgn(g_hwnd, rg, TRUE);
    }
  }
  ShowWindow(g_hwnd, show);
  UpdateWindow(g_hwnd);
  PostMessageA(g_hwnd, WM_APP_AUTOSTART, 0, 0);

  while(GetMessageA(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  return 0;
}
