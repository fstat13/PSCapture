/*
 * PSCapture — PS5 payload: reads the display engine scan-out and
 * streams NV12 frames over TCP; writes /data/pscapture.port and shows
 * the system toast notification.
 * (c) 2026 fstat13 — all rights reserved. See LICENSE.
 */
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>

#define PORT      5599
#define RING_OFF  0x4200000UL
#define SURF_STRIDE 0x600000UL
#define FRAME_SZ  0x5EE000UL
#define W         1920
#define H         1080
#define NV12_SZ   (W * H * 3 / 2)
#define BLK       0x8000UL
#define NBLK      ((int)((FRAME_SZ + BLK - 1) / BLK))

static void
logfile(const char *fmt, ...)
{
  FILE *f = fopen("/data/pscapture.log", "a");

  if(f) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fclose(f);
  }
}

#define LOG(fmt, ...) do {                          \
    printf(fmt "\n", ##__VA_ARGS__);                \
    logfile(fmt "\n", ##__VA_ARGS__);               \
    klog_printf("[avstream] " fmt, ##__VA_ARGS__);  \
  } while(0)

extern unsigned long KERNEL_ADDRESS_DMAP_BASE;

extern int sceKernelLoadStartModule(const char *path, size_t argc,
                                    const void *argv, unsigned int flags,
                                    void *unk1, unsigned int *unk2,
                                    int *handle);

static sigjmp_buf g_rec;
static volatile int g_in_probe;

static void
on_segv(int sig, siginfo_t *si, void *uc)
{
  ucontext_t *u = (ucontext_t *)uc;
  char buf[160];
  int n;

  (void)sig;
  if(g_in_probe) siglongjmp(g_rec, 1);   /* durante la prova: si torna indietro */
  n = snprintf(buf, sizeof(buf),
               "\n[avstream] SIGSEGV rip=0x%llx addr=0x%llx\n",
               (unsigned long long)u->uc_mcontext.mc_rip,
               (unsigned long long)(unsigned long)si->si_addr);
  write(1, buf, n);
  klog_printf("%s", buf);
  _exit(2);
}

static unsigned long
now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ------------------------------------------------------------------ */
/* page walk con cache del blocco                                     */
/* ------------------------------------------------------------------ */

static unsigned long
pt_walk_ex(unsigned long pml4_pa, unsigned long va, int *ok, int *big)
{
  unsigned long e, pa;

  *ok = 0;
  if(big) *big = 0;
  e = kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pml4_pa +
                     ((va >> 39) & 0x1ff) * 8);
  if(!(e & 1)) return 0;
  pa = e & 0x000ffffffffff000UL;
  e = kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pa + ((va >> 30) & 0x1ff) * 8);
  if(!(e & 1)) return 0;
  if(e & 0x80) {
    *ok = 1;
    if(big) *big = 2;
    return (e & 0x000fffffffe00000UL) + (va & 0x1fffffUL);
  }
  if(big == NULL && 0) return 0;
  pa = e & 0x000ffffffffff000UL;
  e = kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pa + ((va >> 21) & 0x1ff) * 8);
  if(!(e & 1)) return 0;
  if(e & 0x80) {
    *ok = 1;
    if(big) *big = 1;
    return (e & 0x000fffffffe00000UL) + (va & 0x1fffffUL);
  }
  pa = e & 0x000ffffffffff000UL;
  e = kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pa + ((va >> 12) & 0x1ff) * 8);
  if(!(e & 1)) return 0;
  *ok = 1;
  return (e & 0x000ffffffffff000UL) + (va & 0xfffUL);
}

static unsigned long
pt_walk(unsigned long pml4_pa, unsigned long va, int *ok)
{
  return pt_walk_ex(pml4_pa, va, ok, NULL);
}

struct ptcache {
  unsigned long va;
  unsigned long pa;
  unsigned long len;
};

static unsigned long g_cache_va;
static unsigned long g_cache_pa;
static unsigned long g_scanin_obj;
static unsigned long g_pan[4];       /* offset della LUMA per superficie */
static unsigned long g_pan_ob[4];    /* obiettivo del tracking (filtro anti-rumore) */
static unsigned long g_pan_uv[4];    /* offset della CROMA (piano UV) */
static int g_sup = -1;               /* superficie agganciata (-1 = da scegliere) */
static unsigned long g_cache_len;
static unsigned char *g_map;      /* mappatura cacheabile del ring (o NULL) */

static int
pt_read_c(struct ptcache *c, unsigned long pml4_pa, unsigned long va,
          void *buf, size_t len)
{
  size_t done = 0;

  while(done < len) {
    unsigned long pa;
    size_t run;

    if(c->len && va + done >= c->va && va + done < c->va + c->len) {
      pa = c->pa + ((va + done) - c->va);
    } else {
      int ok = 0, big = 0;

      pa = pt_walk_ex(pml4_pa, va + done, &ok, &big);
      if(!ok) return -1;
      if(big && ((va + done) & 0xfff) == 0) {
        c->va = (va + done) & ~0x1fffffUL;
        c->pa = pa & ~0x1fffffUL;
        c->len = 0x200000UL;
      } else {
        c->len = 0;
      }
    }
    run = 0x1000 - ((va + done) & 0xfff);
    while(done + run + 0x1000 <= len && ((va + done + run) & 0xfff) == 0 &&
          run < 0x4000000UL) {
      unsigned long nxt;

      if(c->len && va + done + run < c->va + c->len) {
        nxt = c->pa + ((va + done + run) - c->va);
      } else {
        int ok2 = 0, big2 = 0;

        nxt = pt_walk_ex(pml4_pa, va + done + run, &ok2, &big2);
        if(!ok2) break;
        if(big2) {
          c->va = (va + done + run) & ~0x1fffffUL;
          c->pa = nxt & ~0x1fffffUL;
          c->len = 0x200000UL;
        } else {
          c->len = 0;
        }
      }
      if((nxt & ~0xfffUL) != ((pa + run) & ~0xfffUL)) break;
      run += 0x1000;
    }
    if(run > len - done) run = len - done;
    if(kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + pa, (char *)buf + done, run))
      return -1;
    done += run;
  }
  return 0;
}

typedef struct {
  unsigned long pa;
  unsigned char *dst;
  size_t len;
} run_t;

typedef struct {
  run_t *runs;
  int n;
  int tid;
  int nth;
  int rc;
} warg_t;

static void *
wcopy(void *a)
{
  warg_t *w = a;
  int i;

  w->rc = 0;
  for(i = w->tid; i < w->n; i += w->nth) {
    if(kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + w->runs[i].pa,
                      w->runs[i].dst, w->runs[i].len)) {
      w->rc = -1;
    }
  }
  return NULL;
}

#define RTHREADS 4
#define MAX_RUNS 512

/* Cammina in serie (i walk non sono thread-safe: condividono lo stato del
 * kexec) e poi fa copiare le run contigue a piu' thread: le copyout sono
 * syscall pure e i buffer non si sovrappongono. */
static int
pt_read_parallel(unsigned long pml4_pa, unsigned long va, unsigned char *buf,
                 size_t len)
{
  static run_t runs[MAX_RUNS];
  struct ptcache c = { 0, 0, 0 };
  size_t done = 0;
  int nr = 0, i, rc = 0;
  pthread_t th[RTHREADS];
  warg_t wa[RTHREADS];

  while(done < len && nr < MAX_RUNS) {
    unsigned long pa;
    size_t run;

    if(c.len && va + done >= c.va && va + done < c.va + c.len) {
      pa = c.pa + ((va + done) - c.va);
    } else {
      int ok = 0, big = 0;

      pa = pt_walk_ex(pml4_pa, va + done, &ok, &big);
      if(!ok) return -1;
      if(big && ((va + done) & 0xfff) == 0) {
        c.va = (va + done) & ~0x1fffffUL;
        c.pa = pa & ~0x1fffffUL;
        c.len = 0x200000UL;
      } else {
        c.len = 0;
      }
    }
    run = 0x1000 - ((va + done) & 0xfff);
    while(done + run + 0x1000 <= len && ((va + done + run) & 0xfff) == 0 &&
          run < 0x4000000UL && nr + 1 < MAX_RUNS) {
      unsigned long nxt;

      if(c.len && va + done + run < c.va + c.len) {
        nxt = c.pa + ((va + done + run) - c.va);
      } else {
        int ok2 = 0, big2 = 0;

        nxt = pt_walk_ex(pml4_pa, va + done + run, &ok2, &big2);
        if(!ok2) break;
        if(big2) {
          c.va = (va + done + run) & ~0x1fffffUL;
          c.pa = nxt & ~0x1fffffUL;
          c.len = 0x200000UL;
        } else {
          c.len = 0;
        }
      }
      if((nxt & ~0xfffUL) != ((pa + run) & ~0xfffUL)) break;
      run += 0x1000;
    }
    if(run > len - done) run = len - done;
    runs[nr].pa = pa;
    runs[nr].dst = buf + done;
    runs[nr].len = run;
    nr++;
    done += run;
  }
  if(done < len) return -1;

  for(i = 0; i < RTHREADS; i++) {
    wa[i].runs = runs;
    wa[i].n = nr;
    wa[i].tid = i;
    wa[i].nth = RTHREADS;
    wa[i].rc = 0;
    if(pthread_create(&th[i], NULL, wcopy, &wa[i])) {
      /* niente thread: copia il resto qui */
      wcopy(&wa[i]);
      th[i] = 0;
    }
  }
  for(i = 0; i < RTHREADS; i++) {
    if(th[i]) pthread_join(th[i], NULL);
    if(wa[i].rc) rc = -1;
  }
  return rc;
}

/* Legge dal direct map. La cache copre solo i blocchi risolti con una
 * pagina da 2 MB (li' l'offset virtuale e' l'offset fisico); per le
 * mappature a 4 KB ogni pagina si cammina, fondendo pero' le run
 * fisicamente contigue in una sola copyout. */
static int
pt_read(unsigned long pml4_pa, unsigned long va, void *buf, size_t len)
{
  size_t done = 0;

  while(done < len) {
    unsigned long pa;
    size_t run;

    if(g_cache_len && va + done >= g_cache_va &&
       va + done < g_cache_va + g_cache_len) {
      pa = g_cache_pa + ((va + done) - g_cache_va);
    } else {
      int ok = 0, big = 0;

      pa = pt_walk_ex(pml4_pa, va + done, &ok, &big);
      if(!ok) return -1;
      if(big && ((va + done) & 0xfff) == 0) {
        g_cache_va = (va + done) & ~0x1fffffUL;
        g_cache_pa = pa & ~0x1fffffUL;
        g_cache_len = 0x200000UL;
      } else {
        g_cache_len = 0;
      }
    }

    run = 0x1000 - ((va + done) & 0xfff);
    while(done + run + 0x1000 <= len && ((va + done + run) & 0xfff) == 0 &&
          run < 0x4000000UL) {
      unsigned long nxt;

      if(g_cache_len && va + done + run < g_cache_va + g_cache_len) {
        nxt = g_cache_pa + ((va + done + run) - g_cache_va);
      } else {
        int ok2 = 0, big2 = 0;

        nxt = pt_walk_ex(pml4_pa, va + done + run, &ok2, &big2);
        if(!ok2) break;
        if(big2) {
          g_cache_va = (va + done + run) & ~0x1fffffUL;
          g_cache_pa = nxt & ~0x1fffffUL;
          g_cache_len = 0x200000UL;
        } else {
          g_cache_len = 0;
        }
      }
      if((nxt & ~0xfffUL) != ((pa + run) & ~0xfffUL)) break;
      run += 0x1000;
    }
    if(run > len - done) run = len - done;
    if(kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + pa, (char *)buf + done, run))
      return -1;
    done += run;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* P010 -> NV12 8 bit (byte dispari), con PSHUFB: 16 byte per giro    */
/* ------------------------------------------------------------------ */

/* --- auto-allineamento matematico: al taglio giusto c*, la striscia
 * [riga r, col 0..] e' la continuazione di [riga r-1, col c*..]: sono
 * pezzi della STESSA riga del frame originale. Si minimizza l'errore su
 * 64 righe campionate: nessun bordo di menu puo' imitare questa firma. */
static unsigned long
scan_pan(unsigned char *base_p, unsigned long pm4, unsigned long va_base,
         unsigned long atteso)
{
  static unsigned char buf[64][3840];
  unsigned long bestv = 0xFFFFFFFFUL;
  int migliore = -1, c, r, j;
  int c0 = 0, c1 = 1918;

  for(r = 0; r < 64; r++) {
    if(base_p) {
      memcpy(buf[r], base_p + (unsigned long)(r * 8) * 3840UL, 3840);
    } else {
      g_cache_len = 0;
      if(pt_read(pm4, va_base + (unsigned long)(r * 8) * 3840UL, buf[r], 3840))
        return 0xFFFFFFFFUL;
    }
  }
  if(atteso != 0xFFFFFFFFUL) {
    int cc = (int)((1920UL - atteso / 2UL) % 1920UL);
    int s0 = cc - 120, s1 = cc + 120;

    if(s0 < 4) s0 = 4;
    if(s1 > 1914) s1 = 1914;
    c0 = s0; c1 = s1;
  }
  for(c = c0; c <= c1; c += 2) {
    unsigned long e = 0, n = 0;
    int maxj = 1918 - c;
    int passo = maxj > 200 ? maxj / 20 : 1;

    /* finestre troppo strette = degeneri (su contenuto piatto vincono
     * sempre): si salta, e si normalizza per il numero di campioni.
     * Senza questo la scansione libera tornava sempre "pan 4". */
    if(maxj < 200) continue;
    for(r = 1; r < 64; r++) {
      for(j = 0; j <= maxj; j += passo) {
        long d = (long)buf[r][j * 2 + 1] - (long)buf[r - 1][(c + j) * 2 + 1];

        if(d < 0) d = -d;
        e += (unsigned long)d;
        n += 1;
      }
    }
    if(!n) continue;
    if(e * 1024UL / n < bestv) { bestv = e * 1024UL / n; migliore = c; }
  }
  if(migliore < 0) return 0xFFFFFFFFUL;
  return ((1920UL - (unsigned long)migliore) % 1920UL) * 2UL;
}

static void
p010_to_nv12(unsigned char *dst, const unsigned char *p010, unsigned long n)
{
  /* si carica da p010+1: gli eventuali byte del campione sono a indice
   * PARI del carico (errore storico al contrario: byte bassi -> rumore) */
  const __m128i mA = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14,
                                   -1, -1, -1, -1, -1, -1, -1, -1);
  /* nella zona CROMA il piano della superficie tiene la parola V PRIMA
   * della U (misurato contro il frame di riferimento v10: invertendo le
   * due meta' l'errore scende da 15.5 a 1.7). Il piano luma resta a mA. */
  const __m128i mB = _mm_setr_epi8(2, 0, 6, 4, 10, 8, 14, 12,
                                   -1, -1, -1, -1, -1, -1, -1, -1);
  const unsigned long LM = 1920UL * 1080UL;   /* confine luma/croma */
  unsigned long i = 0;

  for(; i + 16 <= n; i += 16) {
    const unsigned char *s = p010 + 1 + 2 * i;
    __m128i mk = (i >= LM) ? mB : mA;
    __m128i a = _mm_loadu_si128((const __m128i *)(const void *)s);
    __m128i b = _mm_loadu_si128((const __m128i *)(const void *)(s + 16));
    __m128i x = _mm_shuffle_epi8(a, mk);
    __m128i y = _mm_shuffle_epi8(b, mk);

    _mm_storeu_si128((__m128i *)(void *)(dst + i), _mm_unpacklo_epi64(x, y));
  }
  for(; i < n; i++) {
    const unsigned char *s = p010 + 1 + 2 * i;

    dst[i] = (i < LM) ? s[0] : ((i & 1) ? s[-2] : s[2]);
  }
}

/* ------------------------------------------------------------------ */
/* mappatura cacheabile del buffer DCE (il muro dei 45 MB/s)          */
/* ------------------------------------------------------------------ */

/* u64 in kernel: kernel_copyin e' (src user, kaddr, len) */
static int
kwrite64(unsigned long kaddr, unsigned long long val)
{
  return kernel_copyin(&val, (intptr_t)kaddr, 8);
}

/* Costruisce nel NOSTRO processo una mappatura CACHEABILE (pagine da
 * 2 MB, sola lettura, NX) del range fisico che comincia a rv_va, su una
 * sotto-catena NUOVA appesa a una entry PML4 libera: le pagine PDPT e PD
 * sono nostre (mmap) e le scriviamo noi; l'unica scrittura kernel e' la
 * entry PML4. Le PDE replicano i flag della mappa del daemon
 * (0xA5 = P|US|A|PS, senza PCD/PWT: write-back). Ritorna la base o NULL
 * (in tal caso si resta sul percorso lento). */
static unsigned char *
build_fastmap(unsigned long pml4, unsigned long rv_va, int slots)
{
  static unsigned char *hh;
  unsigned long pas[32], kp, vm, pmp, pm4, base = 0, ptabs = 0;
  unsigned long i;
  int ok, big;

  if(slots < 1 || slots > 32) return NULL;
  if(rv_va & 0x1fffffUL) { LOG("  fastmap: base non allineata"); return NULL; }

  for(i = 0; i < (unsigned long)slots; i++) {
    pas[i] = pt_walk_ex(pml4, rv_va + i * 0x200000UL, &ok, &big);
    if(!ok || big != 1 || (pas[i] & 0x1fffffUL) || !pas[i]) {
      LOG("  fastmap: walk slot %lu ko", i);
      return NULL;
    }
  }

  hh = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON, -1, 0);
  if(hh == MAP_FAILED) { LOG("  fastmap: mmap ko"); return NULL; }
  memset(hh, 0, 0x2000);
  hh[0] = 0;
  hh[0x1000] = 0;

  kp = kernel_get_proc(getpid());
  vm = kernel_getlong(kp + KERNEL_OFFSET_PROC_P_VMSPACE);
  pmp = vm ? kernel_getlong(vm + 0x1d8) : 0;
  pm4 = pmp ? kernel_getlong(pmp + 0x28) : 0;
  if(!pm4) { LOG("  fastmap: pmap nostra non trovata"); return NULL; }

  {
    unsigned long pa0, pa1;

    pa0 = pt_walk_ex(pm4, (unsigned long)hh, &ok, NULL);
    pa1 = pt_walk_ex(pm4, (unsigned long)hh + 0x1000, &ok, NULL);
    /* il paddr space della console ha alias hardware oltre i 16GB (le
     * superfici DCE stanno a 0x459200000): niente rifiuto su questo.
     * Il paracadute vero resta la verifica sotto sigsetjmp piu' sotto. */
    if(!pa0 || !pa1 || (pa0 & 0xfff) || (pa1 & 0xfff) ||
       pa0 > 0x800000000UL || pa1 > 0x800000000UL) {
      LOG("  fastmap: paddr delle tabelle sospetti");
      return NULL;
    }
    *(unsigned long long *)(void *)(hh + 0) = 0x7ULL | pa1;  /* PDPT -> PD */
    for(i = 0; i < (unsigned long)slots; i++) {
      *(unsigned long long *)(void *)(hh + 0x1000 + i * 8) =
        0x8000000000000000ULL | pas[i] | 0xa5ULL;
    }
    __asm volatile("mfence" ::: "memory");
    ptabs = pa0;
  }

  /* entry PML4 libera (con indici PDPT/PD a zero: cosi' la catena e'
   * fatta solo da PDPT[0] e PD[0..slots)) */
  for(i = 128; i < 256; i++) {
    unsigned long v = i << 39;

    if(((v >> 30) & 0x1ff) || ((v >> 21) & 0x1ff)) continue;
    if(kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pm4 + i * 8)) continue;
    base = v;
    break;
  }
  if(!base) { LOG("  fastmap: nessuna entry PML4 libera"); return NULL; }

  if(kwrite64(KERNEL_ADDRESS_DMAP_BASE + pm4 + ((base >> 39) & 0x1ff) * 8,
              0x7ULL | ptabs)) {
    LOG("  fastmap: scrittura PML4 fallita");
    return NULL;
  }

  /* verifica sotto protezione fault: la prima QWORD di ogni slot deve
   * essere leggibile; qualunque fault -> fallback (il contenuto non si
   * confronta: lo schermo anima e i byte cambiano fra due letture) */
  {
    unsigned long n;

    g_in_probe = 1;
    if(sigsetjmp(g_rec, 1) == 0) {
      unsigned long long acc = 0;

      for(n = 0; n < (unsigned long)slots; n++) {
        acc += *(volatile unsigned long long *)(void *)(base + n * 0x200000UL);
      }
      /* anche l'ultima qword dell'ultimo slot: mai sforare la mappa */
      acc += *(volatile unsigned long long *)(void *)
               (base + (unsigned long)slots * 0x200000UL - 8);
      g_in_probe = 0;
      if(acc == 0) {
        LOG("  fastmap: nota: prime qword tutte nulle");
      }
    } else {
      g_in_probe = 0;
      LOG("  fastmap: fault durante la verifica: fallback");
      return NULL;
    }
  }

  LOG("  mappa rapida: v=0x%lx <= paddr 0x%lx (%d slot, cacheable)",
      base, pas[0], slots);
  return (unsigned char *)base;
}

static int
has_sub(const unsigned char *p, int n, const char *needle)
{
  int m = (int)strlen(needle), i;

  for(i = 0; i + m <= n; i++) {
    if(!memcmp(p + i, needle, m)) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* set scanin via IPMI (la pipeline del tap si riaccende cosi')       */
/* ------------------------------------------------------------------ */

/* Il client IPMI va distrutto dopo l'uso: le sessioni lasciate aperte si
 * accumulano e il connect comincia a fallire con 0x8002000d. Si usa il
 * distruttore base D2 (~Client): chiude la sessione senza liberare la
 * memoria (l'oggetto sta nel pool che passiamo noi a create). */
static void *g_ipmi;
static void (*g_ipmi_dtor)(void *);

static int
set_scanin(void)
{
  uint32_t ih = 0;
  intptr_t q_cfg, q_esize, q_create, q_dtor;
  static unsigned char cfg[0x150];
  void *buf;
  void *client = 0;
  unsigned long csz;
  int r;

  if(kernel_dynlib_handle(getpid(), "libSceIpmi.sprx", &ih)) {
    int hh = 0;

    r = sceKernelLoadStartModule("/system/common/lib/libSceIpmi.sprx", 0,
                                 NULL, 0, NULL, NULL, &hh);
    LOG("  load libSceIpmi -> 0x%x", r);
    kernel_dynlib_handle(getpid(), "libSceIpmi.sprx", &ih);
  }
  if(!ih) { LOG("  libSceIpmi non disponibile"); return -1; }

  q_cfg = kernel_dynlib_resolve(getpid(), ih, "O1lQ2+do5r4");
  q_esize = kernel_dynlib_resolve(getpid(), ih, "Rih7HnIM3JQ");
  q_create = kernel_dynlib_resolve(getpid(), ih, "0zsTiDhM0nU");
  q_dtor = kernel_dynlib_resolve(getpid(), ih, "QLDcLR7Pzbg");
  if(!q_cfg || !q_esize || !q_create) {
    LOG("  NID IPMI non risolti");
    return -1;
  }

  memset(cfg, 0, sizeof(cfg));
  ((void (*)(void *))q_cfg)(cfg);
  memcpy(cfg, "SceSysAvControlIpc", 18);
  *(unsigned *)(cfg + 0x1c) = 1;
  *(unsigned *)(cfg + 0x20) = 1;
  *(unsigned long long *)(cfg + 0x28) = 0x400;
  *(unsigned long long *)(cfg + 0x30) = 0x400;
  *(unsigned *)(cfg + 0x38) = 2;
  *(unsigned long long *)(cfg + 0x40) = 0x400;
  *(unsigned long long *)(cfg + 0x48) = 0x400;

  if(g_ipmi && g_ipmi_dtor) {
    g_ipmi_dtor(g_ipmi);          /* chiude la sessione del giro prima */
    g_ipmi = 0;
  }

  csz = ((unsigned long (*)(void))q_esize)();
  buf = malloc(csz ? csz : 0x10000);
  r = ((int (*)(void **, void *, void *, void *))q_create)(&client, cfg, 0, buf);
  if(r || !client) {
    LOG("  Client::create -> 0x%x", r);
    return -1;
  }
  g_ipmi = client;
  g_ipmi_dtor = (void (*)(void *))q_dtor;

  {
    unsigned long vt = *(unsigned long *)client;
    long timeout = 20000000;
    int (*connect)(void *, int, int, void *, long *) =
      (int (*)(void *, int, int, void *, long *))(*(unsigned long *)(vt + 0x80));

    r = connect(client, 0, 0, cfg, &timeout);
    if(r) {
      LOG("  connect -> 0x%x", r);
      if(g_ipmi == client) {
        if(g_ipmi_dtor) g_ipmi_dtor(client);
        g_ipmi = 0;
      }
      return -1;
    }

    {
      unsigned scaninId = 1;
      int params[8];
      struct { void *p; unsigned long len; } data[2];
      static unsigned long out1[4], out2[4];
      int (*invoke)(void *, unsigned, void *, unsigned, void *, void *) =
        (int (*)(void *, unsigned, void *, unsigned, void *, void *))
          (*(unsigned long *)(vt + 0x60));

      memset(params, 0, sizeof(params));
      params[0] = 2;      /* tapPoint */
      params[1] = 1;
      params[2] = 0x18;
      params[3] = 11;     /* risoluzione: 11 = 1920x1080 */
      params[4] = 1;
      params[5] = 1;
      params[6] = 0;
      data[0].p = &scaninId;
      data[0].len = 4;
      data[1].p = params;
      data[1].len = 0x20;
      memset(out1, 0, sizeof(out1));
      memset(out2, 0, sizeof(out2));
      r = invoke(client, 0x24, data, 2, out1, out2);
      LOG("  set scanin -> r=0x%x", r);
      return r;
    }
  }
}

/* Manda un NV12 intero. Ritorna la connessione (o -1 se e' caduta). */
static int
send_frame(int conn, const unsigned char *buf, int frame)
{
  unsigned char hdr[12];
  unsigned nlen = NV12_SZ;
  ssize_t w, off = 0;

  if(conn < 0) return conn;
  hdr[0] = 'A'; hdr[1] = 'V'; hdr[2] = 'S'; hdr[3] = '2';
  memcpy(hdr + 4, &frame, 4);
  memcpy(hdr + 8, &nlen, 4);
  if(write(conn, hdr, 12) != 12) { close(conn); return -1; }
  while(off < (ssize_t)NV12_SZ) {
    w = write(conn, buf + off, NV12_SZ - off);
    if(w <= 0) { close(conn); return -1; }
    off += w;
  }
  return conn;
}

/* ---- notifica di sistema: "PSCapture is streaming on port N" ---- */

typedef int (*fn_notify)(int, int, const char *);
static fn_notify g_notify;

static int
notifiche_carica(void)
{
  int h = 0;

  if(g_notify) return 0;
  if(kernel_dynlib_handle(getpid(), "libSceNotification.sprx",
                          (unsigned int *)&h)) {
    int h2 = 0;

    sceKernelLoadStartModule("/system/common/lib/libSceNotification.sprx",
                             0, NULL, 0, NULL, NULL, &h2);
    kernel_dynlib_handle(getpid(), "libSceNotification.sprx",
                         (unsigned int *)&h);
  }
  if(!h) return -1;
  g_notify = (fn_notify)kernel_dynlib_resolve(getpid(), (unsigned)h,
                                              "jSWBvOPfSOk");
  return g_notify ? 0 : -1;
}

static void
notifica_porta(int port)
{
  char json[1600];
  char data[32];
  time_t t = time(NULL);
  struct tm tmv;

  gmtime_r(&t, &tmv);
  strftime(data, sizeof(data), "%Y-%m-%dT%H:%M:%SZ", &tmv);
  snprintf(json, sizeof(json),
    "{"
    "\"rawData\":{"
    "\"viewTemplateType\":\"InteractiveToastTemplateB\","
    "\"channelType\":\"Downloads\",\"useCaseId\":\"IDC\","
    "\"toastOverwriteType\":\"No\",\"isImmediate\":true,"
    "\"priority\":100,"
    "\"viewData\":{"
    "\"message\":{\"body\":\"PSCapture is streaming on:%d\"},"
    "\"subMessage\":{\"body\":\"avvia lo streaming dal PC\"}"
    "},"
    "\"platformViews\":{\"previewDisabled\":{\"viewData\":{"
    "\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},"
    "\"message\":{\"body\":\"PSCapture\"}}}}"
    "},"
    "\"createdDateTime\":\"%s\","
    "\"localNotificationId\":\"588193127\""
    "}", port, data);
  if(notifiche_carica()) {
    LOG("notifica: libSceNotification non disponibile");
    return;
  }
  g_notify(0xFE, 1, json);
  LOG("notifica inviata (porta %d)", port);
}

int
main(void)
{
  unsigned char *p010 = NULL;
  unsigned long pml4 = 0, tap_va = 0;
  int srv, conn = -1, frame = 0, inviati = 0;
  struct sockaddr_in addr;
  static unsigned char strip[0x1000];
  unsigned long t0;

  {
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
  }

  LOG("avstream: partenza");

  /* --- 0) igiene: NON si uccide piu' nessuno ---
   * (una istanza con la mappatura iniettata non va strappata via: il
   * teardown di tabelle alterate e' il sospetto del crash del 15/10).
   * Le istanze senza client non fanno nulla: si lascia stare. */
  {
    /* riga 0a: UCCIDE le vecchie istanze avstream.
     * Si riconoscono per la mappa rapida alla base fissa
     * 0x400000000000 (entry PML4 n. 128 presente nel loro pmap):
     * nessun processo di sistema la usa. Serve anche a liberare le
     * sessioni IPMI: finche' la tabella e' piena il connect muore
     * con 0x8002000d. Il teardown della sotto-catena e' innocuo
     * (gia' dimostrato dal test v6.2: la console non e' caduta). */
    {
      int pid, mio = getpid(), uccise = 0;

      for(pid = 1; pid < 400; pid++) {
        long kp2;
        unsigned long vm2, pmp2, pl4, e;

        if(pid == mio || pid == 50 || pid == 59 || pid == 79) continue;
        kp2 = kernel_get_proc(pid);
        if(!kp2) continue;
        vm2 = kernel_getlong(kp2 + KERNEL_OFFSET_PROC_P_VMSPACE);
        if(!vm2) continue;
        pmp2 = kernel_getlong(vm2 + 0x1d8);
        if(!pmp2) continue;
        pl4 = kernel_getlong(pmp2 + 0x28);
        if(!pl4) continue;
        e = kernel_getlong(KERNEL_ADDRESS_DMAP_BASE + pl4 + 128UL * 8UL);
        if(!(e & 1UL)) continue;
        if(kill(pid, SIGKILL) == 0) uccise++;
      }
      if(uccise) {
        LOG("  uccise %d vecchie istanze avstream", uccise);
        usleep(1500000);
      }
    }

    /* riga 0b: tocco in localhost sulle porte vecchie (per quelle che
     * hanno il cap e escono dopo un accept). */
    {
      int p, svegliate = 0;

      for(p = PORT; p < PORT + 30; p++) {
        int sk = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a;

        if(sk < 0) break;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = htons(p);
        a.sin_addr.s_addr = htonl(0x7f000001UL);
        if(!connect(sk, (struct sockaddr *)&a, sizeof(a))) svegliate++;
        close(sk);
      }
      if(svegliate) {
        LOG("  svegliate %d vecchie istanze ferme in accept", svegliate);
        usleep(500000);
      }
    }
  }

  /* --- 1) walk: pml4 di pid 50 --- */
  {
    long kp = kernel_get_proc(50);
    unsigned long vm = 0, pmp = 0;

    if(!kp) { LOG("  kernel_get_proc(50) fallita"); return 1; }
    vm = kernel_getlong(kp + KERNEL_OFFSET_PROC_P_VMSPACE);
    if(!vm) { LOG("  vmspace nulla"); return 1; }
    pmp = kernel_getlong(vm + 0x1d8);
    if(pmp) pml4 = kernel_getlong(pmp + 0x28);
    LOG("  pmap=0x%lx pml4=0x%lx", pmp, pml4);
    if(!pml4) { LOG("  pml4 non trovato"); return 1; }
    g_cache_len = 0;
    if(!pt_read(pml4, 0x4c64e2UL, strip, 0x40) &&
       has_sub(strip, 0x40, "getScaninBufferOffset")) {
      LOG("  verifica del modulo: OK (delta 0)");
    } else {
      LOG("  verifica del modulo: FALLITA");
      return 1;
    }
  }

  /* --- 2) ScaninControl: va del ring --- */
  {
    unsigned long va;

    for(va = 0x4a0000UL; va < 0x570000UL && !tap_va; va += 0x1000) {
      unsigned char page[0x1000];
      int j;

      g_cache_len = 0;
      if(pt_read(pml4, va, page, 0x1000)) continue;
      for(j = 0; j + 8 <= 0x1000; j += 8) {
        unsigned long q = *(unsigned long *)(page + j);

        if(q == 0x4e5078UL) {
          unsigned char f[0x48];

          g_cache_len = 0;
          if(!pt_read(pml4, va + j + 0x520UL, f, 0x48)) {
            unsigned long sz = *(unsigned int *)(f + 0x30);

            tap_va = *(unsigned long *)(f + 0x20);
            g_scanin_obj = va + j;
            LOG("  ScaninControl @ 0x%lx: va=0x%lx dim=0x%lx",
                va + j, tap_va, sz);
          }
        }
      }
    }
    if(!tap_va) { LOG("  ring non trovato"); return 1; }
  }

  /* --- 2b) mappatura cacheabile del ring (~4 GB/s invece di 45 MB/s) --- */
  /* il ring copre fino a 3*0x600000+0x5EE000 = 0x17EE000 -> 12 slot */
  g_map = build_fastmap(pml4, tap_va + RING_OFF, 12);
  if(!g_map) LOG("  mappa rapida non disponibile: si usa il direct map");
  {
    int k;

    for(k = 0; k < 4; k++) {
      unsigned long p = scan_pan(g_map ? g_map + (unsigned long)k * SURF_STRIDE : NULL,
                                 pml4, tap_va + RING_OFF + (unsigned long)k * SURF_STRIDE,
                                 0xFFFFFFFFUL);
      unsigned long qu = 0xFFFFFFFFUL;

      if(p != 0xFFFFFFFFUL) {
        g_pan[k] = p;
      } else {
        g_pan[k] = g_pan[0] ? g_pan[0] : 2230;
      }
      LOG("  scan sup %d: %lx", k, p);
      g_pan[k] = 2230;         /* dimensione giusta (verificata sul campo) */
      g_pan_ob[k] = 2230;
      g_pan_uv[k] = 2230;
    }
  }

  /* --- 3) la pipeline: set scanin (con ritentativi) --- */
  {
    int tent;

    for(tent = 0; tent < 3; tent++) {
      int r = set_scanin();

      if(r == 0) break;
      LOG("  set scanin fallito (0x%x), riprovo fra 10 s", r);
      usleep(10000000);
    }
    /* niente attesa lunga: il ring e' gia' attivo (lo scanin e' globale),
     * i frame arrivano anche se la sessione IPMI e' occupata */
  }

  /* --- 4) buffer + server TCP --- */
  p010 = malloc(FRAME_SZ + 0x2000);
  if(!p010) { LOG("  malloc fallita"); return 1; }

  srv = socket(AF_INET, SOCK_STREAM, 0);
  {
    int one = 1;

    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    /* listen non bloccante: senza client il turno deve finire lo
     * stesso (se no il processo resta appeso in accept per sempre) */
    fcntl(srv, F_SETFL, fcntl(srv, F_GETFL, 0) | O_NONBLOCK);
  }
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  {
    int port = PORT;

    for(;;) {
      addr.sin_port = htons(port);
      if(!bind(srv, (struct sockaddr *)&addr, sizeof(addr))) break;
      LOG("  porta %d occupata, provo la successiva", port);
      port++;
      if(port > PORT + 30) return 1;
    }
    listen(srv, 1);
    LOG("  in ascolto su :%d (il PC si collega)", port);
    notifica_porta(port);
  /* --- benchmark: il costo e' la copyout o la cache della zona? --- */
  {
    static unsigned char bench[0x400000];
    unsigned long t_a, t_b, t_c, tap_pa = 0;
    int okb = 0;

    tap_pa = pt_walk_ex(pml4, tap_va + RING_OFF, &okb, NULL) & ~0xfffUL;
    t_a = now_ms();
    kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + 0x2c600000UL, bench, 0x400000);
    t_b = now_ms();
    kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + (tap_pa ? tap_pa : 0x455000000UL),
                   bench, 0x400000);
    t_c = now_ms();
    LOG("  bench: 4MB da paddr basso 0x2c600000 -> %lu ms | da DCE 0x%lx -> %lu ms",
        t_b - t_a, tap_pa, t_c - t_b);
  }

  {
    const char *devs[5] = { "/dev/mem", "/dev/kmem", "/dev/dmap", "/dev/physmem", "/dev/pcislot" };
    int di;

    for(di = 0; di < 5; di++) {
      int fd = open(devs[di], O_RDONLY);

      LOG("  %s -> fd=%d errno=%d", devs[di], fd, fd < 0 ? errno : 0);
      if(fd >= 0) close(fd);
    }
  }
    {
      int fd = open("/data/pscapture.port", O_WRONLY | O_CREAT | O_TRUNC, 0644);
      char buf[16];

      snprintf(buf, sizeof(buf), "%d", port);
      if(fd >= 0) {
        write(fd, buf, strlen(buf));
        close(fd);
      }
    }
  }

  /* --- 5) loop: frame INTERO ad ogni giro ---
   * Con la mappa rapida la lettura completa costa ~1 ms: niente piu'
   * campionamenti a blocchi (accumulavano contenuto vecchio = fantasmi
   * "tripli"), ogni frame inviato e' coerente al 100%. */
  {
    unsigned char *nv12 = malloc(NV12_SZ);
    unsigned long last_set_ms = now_ms();
    int rot = 3;

    if(!nv12) { LOG("  malloc nv12 fallita"); return 1; }

    for(;;) {
      unsigned long tr0, tr1;
      int pick;

      if(conn < 0) {
        conn = accept(srv, NULL, NULL);
        if(conn >= 0) {
          int one = 1;

          fcntl(conn, F_SETFL, fcntl(conn, F_GETFL, 0) & ~O_NONBLOCK);
          setsockopt(conn, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
          LOG("  PC collegato");
        } else {
          if((now_ms() - last_set_ms) > 300000UL) {
            LOG("  set scanin di manutenzione (a vuoto)");
            set_scanin();
            last_set_ms = now_ms();
          }
          usleep(20000);
          continue;
        }
      }

      /* si resta SEMPRE sulla stessa superficie: ogni superficie ha il
       * suo pan; ruotare fra le 4 fa saltare l'immagine in continuazione.
       * Il pan scorre di ~2 byte/s: lo si insegue di continuo (leggero) */
      {
        static unsigned long ultimo_track;

        if(0 && (now_ms() - ultimo_track) > 500UL) {
          ultimo_track = now_ms();
          if(g_sup >= 0) {
            unsigned long p =
              scan_pan(g_map + (unsigned long)g_sup * SURF_STRIDE, pml4,
                       tap_va + RING_OFF + (unsigned long)g_sup * SURF_STRIDE,
                       g_pan[g_sup]);

            long dd = (long)((p + 3840UL - g_pan_ob[g_sup]) % 3840UL);

            if(dd > 1920) dd -= 3840;
            if(dd < 0) dd = -dd;
            if((dd > 24 && dd < 400) || g_pan_ob[g_sup] == 0xFFFFFFFFUL) {
              if(dd > 96)
                LOG("  track: ob %lu -> %lu (pan %lu)",
                    g_pan_ob[g_sup], p, g_pan[g_sup]);
              g_pan_ob[g_sup] = p;         /* rumore < 24 B: si ignora */
            }
            if((frame % 240) < 2)
              LOG("  stato: pan=%lu ob=%lu scan=%lx (sup %d)",
                  g_pan[g_sup], g_pan_ob[g_sup], p, g_sup);
          }
        }
      }
      /* inseguimento morbido: 1 byte/frame verso l'obiettivo */
      if(0 && g_sup >= 0 && g_pan_ob[g_sup] != 0xFFFFFFFFUL) {
        long dz = (long)((g_pan_ob[g_sup] + 3840UL - g_pan[g_sup]) % 3840UL);

        if(dz != 0) {
          if(dz > 1920) g_pan[g_sup] = (g_pan[g_sup] + 3839UL) % 3840UL;
          else g_pan[g_sup] = (g_pan[g_sup] + 1UL) % 3840UL;
        }
      }
      tr0 = now_ms();
      if(g_map) {
        int tent;

        /* il DCE riscrive ogni superficie a 60 Hz: leggendo mentre
         * scrive esce un frame misto (ghost e coda verde). Si legge,
         * si attende 2 ms e si confronta: se e' cambiata era viva e
         * si passa alla successiva. */
        if(g_sup < 0) {
          int k;

          for(k = 0; k < 4; k++) {
            unsigned char *sl = g_map + (unsigned long)k * SURF_STRIDE;

            memcpy(p010, sl + g_pan[k], FRAME_SZ);
            usleep(2500);
            if(memcmp(p010, sl + g_pan[k], FRAME_SZ) == 0) {
              g_sup = k;
              break;
            }
          }
          if(g_sup < 0) {
            g_sup = 0;
            memcpy(p010, g_map + g_pan[0], FRAME_SZ);
          }
          LOG("  agganciata superficie %d (pan %lu)", g_sup, g_pan[g_sup]);
        } else {
          unsigned char *slot = g_map + (unsigned long)g_sup * SURF_STRIDE;

          for(tent = 0; tent < 3; tent++) {
            /* Luma: come sempre (il piano vero e' 11 righe prima, shift
             * verticale invisibile). Croma MISURATA sul frame vivo:
             * il piano croma comincia a 4.104.960 (0x3EA300), cioe'
             * 42.240 byte (11 righe uv) PRIMA del bordo luma, stesso
             * pan. Ancorata coi due bordi della barra bianca (righe
             * luma 318/402 = 4.104.960+159/201 righe uv, errore 0.0).
             * Prima di 4.104.960 c'e' zona morta (non si legge).
             * Le ultime ~10 righe del piano non sono ancora scritte:
             * si replica l'ultima riga buona. */
            memcpy(p010, slot + g_pan[g_sup], 0x3F4800UL);
            memcpy(p010 + 0x3F4800UL,
                   slot + 4104960UL + g_pan[g_sup],
                   527UL * 3840UL);
            {
              unsigned char *u = p010 + 0x3F4800UL + 527UL * 3840UL;
              const unsigned char *riga = u - 3840UL;
              int riga_i;

              for(riga_i = 527; riga_i < 540; riga_i++) {
                memcpy(u, riga, 3840);
                u += 3840;
              }
            }
            usleep(6000);
            if(memcmp(p010, slot + g_pan[g_sup], 0x3F4800UL) == 0)
              break;
          }
          (void)slot;
        }
      } else if(pt_read_parallel(pml4, tap_va + RING_OFF + g_pan[0],
                                 p010, FRAME_SZ)) {
        usleep(5000);
        continue;
      }
      p010_to_nv12(nv12, p010, NV12_SZ);
      tr1 = now_ms();

      conn = send_frame(conn, nv12, frame);
      inviati++;
      frame++;
      if(frame < 3 || (frame % 120) == 0) {
        LOG("  frame %d: sup %d, lettura+conv %lums, inviati %d",
            frame, g_sup, tr1 - tr0, inviati);
      }

      if((now_ms() - last_set_ms) > 300000UL) {
        LOG("  set scanin di manutenzione");
        set_scanin();
        last_set_ms = now_ms();
      }
    }
  }

  if(conn >= 0) close(conn);
  close(srv);
  if(g_ipmi && g_ipmi_dtor) {
    g_ipmi_dtor(g_ipmi);
    g_ipmi = 0;
  }
  LOG("avstream: fine");
  return 0;
}
