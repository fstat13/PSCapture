/* ps5-dumplib — enumera ed estrae le librerie di sistema (.sprx) dalla memoria
 * dei processi che le hanno caricate.
 *
 * Perche' funziona: con kstuff il payload gira con accesso kernel. Le librerie di
 * sistema stanno mappate nella memoria dei processi che le usano, e il filesystem
 * non le espone (vivono dentro le immagini PFS cifrate). Quindi non le scarichiamo:
 * le leggiamo direttamente da chi le ha in RAM, gia' decifrate dalla console.
 *
 * La catena:
 *   kernel_get_proc(pid)                        -> kproc
 *   read(kproc + 0x3e8)                         -> testa della lista dynlib
 *   walk .next, leggi .path / .mapbase          -> cosa e' caricato e dove
 *   proc_read(pid, base, buf, len)    -> copia la memoria di quel processo
 *
 * La libreria e' un ELF, quindi si autodescrive: leggiamo header e program header,
 * poi per ogni PT_LOAD copiamo p_filesz byte da base+p_vaddr scrivendoli nel file
 * all'offset p_offset. Cosi' si ricostruisce il file.
 *
 * Nota: in memoria le rilocazioni possono essere gia' applicate, quindi il file
 * ricostruito e' ottimo per l'analisi statica ma non identico byte-per-byte
 * all'originale. Per farci reverse engineering va benissimo.
 *
 * uso:  dumplib.elf                      lista tutto (processi + librerie caricate)
 *       dumplib.elf list                 idem
 *       dumplib.elf targets              estrae la lista di default (encoder e affini)
 *       dumplib.elf <lib> [<lib> ...]    estrae quelle librerie in /data/dumps
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/proc.h>
#include <sys/user.h>    /* struct kinfo_proc */
#include <sys/sysctl.h>

#include <ps5/kernel.h>
#include <ps5/klog.h>


#define OUTDIR     "/data/dumps"
#define MAX_SEG    (16)
#define MAX_LIBS   (400)   /* tetto per non far esplodere l'output */
#define MAX_PATH   (256)

/* Offset della testa della lista dynlib dentro struct proc.
 * Vale per i firmware gestiti da kernel.c (13.40, 13.60, ...). */
#define KPROC_DYNLIB_HEAD (0x3e8)


/* ELF64 definiti a mano: <elf.h> qui non e' garantito. */
typedef struct {
  uint8_t  e_ident[16];
  uint16_t e_type, e_machine;
  uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff;
  uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_ehdr_t;

typedef struct {
  uint32_t p_type, p_flags;
  uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
} elf64_phdr_t;

#define PT_LOAD (1)


/* dynlib_obj_t (crt/kernel.h). Il layout va rispettato esattamente, padding compreso.
 * offset 0   next
 * offset 8   path          puntatore alla stringa del path completo
 * offset 40  handle
 * offset 48  mapbase       base della MAPPATURA (range 0x8_0000_0000 = GPU/direct)
 * offset 56  mapsize
 * offset 64  textsize
 * offset 120 vaddrbase     base dell'indirizzo VIRTUALE utente  <- candidato per l'ELF
 */
typedef struct {
  uint64_t next;
  uint64_t path;
  uint64_t unknown0[2];
  uint32_t refcount;
  uint32_t pad;
  uint64_t handle;
  uint64_t mapbase;
  uint64_t mapsize;
  uint64_t textsize;
  uint64_t database;
  uint64_t datasize;
  uint64_t unknown1;
  uint64_t unknown1size;
  uint64_t entry;
  uint64_t unknown2;
  uint64_t vaddrbase;
} dynlib_head_t;

#define OFF_VADDRBASE (120)


/* Nomi confermati dalla scansione: sono quelli che la console ha davvero caricati. */
static const char *targets[] = {
  "libSceVenc.sprx",                    /* encoder video hardware */
  "libSceVdecCore.native.sprx",         /* decoder */
  "libSceVcodec.sprx",                  /* codec generico */
  "libSceJpegEnc.sprx",                 /* encoder JPEG hardware */
  "libSceShareInternal.native.sprx",    /* pipeline share/registrazione */
  "libSceSharePlay.sprx",
  "libSceShareFactoryUtil.sprx",
  "libSceRemoteplay.sprx",              /* server Remote Play */
  "libSceRemotePlayClientIpc.sprx",
  "libSceAvcap2.sprx",                  /* cattura audio/video */
  "libSceVideoOut.sprx",
  "libSceVideoRecording.native.sprx",   /* registrazione video */
  "libSceVideoStreamingEngine_sys.sprx",/* motore di streaming */
  "webrtc_daemon.self",                 /* il daemon WebRTC */
  "libwebrtc.sprx",                     /* stack WebRTC completo */
  NULL
};

/* Le directory in cui la console tiene le librerie di sistema, provate in ordine. */
static const char *lib_dirs[] = {
  "/system/common/lib/",
  "/system/priv/lib/",
  "/system/common_ex/lib/",
  NULL
};


#define LOG(fmt, ...) do {                          \
    printf(fmt "\n", ##__VA_ARGS__);                \
    klog_printf("[dumplib] " fmt, ##__VA_ARGS__);   \
  } while(0)


/* Nodo dynlib e pid correnti: walk_dynlibs li aggiorna prima di ogni callback,
 * cosi' le callback che devono leggere altri campi del nodo (enumerazione degli
 * export) sanno dove guardare. */
static unsigned long g_node;
static pid_t         g_pid;


static const char *basename_of(const char *path);


/* --- lettura della memoria di un altro processo, saltando mdbg ---
 *
 * kernel_proc_copyout() dell'SDK prova mdbg_copyout() come PRIMA cosa e si fida
 * del suo esito:
 *
 *     if(!mdbg_copyout(pid, addr, buf, len)) return 0;    // "successo"
 *
 * Su questo firmware mdbg risponde successo senza copiare niente, quindi il
 * buffer resta col contenuto che aveva prima (0xCC, riempimento di stack) e il
 * percorso buono — page-table walk + direct map — non viene mai raggiunto.
 * Risultato: ogni lettura sembra riuscire e restituisce spazzatura.
 *
 * Qui rifacciamo la stessa cosa senza mdbg: traduciamo il virtuale in fisico
 * camminando le page table del processo, poi leggiamo dal direct map.
 *
 * I due simboli esistono nel crt linkato ma non sono nell'header pubblico
 * <ps5/kernel.h>, quindi li dichiariamo qui. */
extern unsigned long KERNEL_ADDRESS_DMAP_BASE;
extern int kernel_proc_getpaddr(pid_t pid, unsigned long vaddr,
                                unsigned long *paddr, unsigned long *plen);

static int
proc_read(pid_t pid, intptr_t addr, void *buf, size_t len)
{
  size_t done = 0;

  while(done < len) {
    unsigned long paddr = 0;
    unsigned long plen = 0;

    if(kernel_proc_getpaddr(pid, (unsigned long)addr + done, &paddr, &plen)) {
      return -1;
    }
    if(!plen) {
      return -1;
    }
    if((size_t)plen > len - done) {
      plen = len - done;
    }
    if(kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + paddr,
                      (char*)buf + done, plen)) {
      return -1;
    }
    done += plen;
  }

  return 0;
}


/* Elenca i processi vivi e ritorna il buffer sysctl; il chiamante libera. */
static void *
list_procs(size_t *out_size)
{
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
  size_t sz = 0;
  void *buf;

  if(sysctl(mib, 4, NULL, &sz, NULL, 0)) {
    LOG("sysctl(size) fallita: %s", strerror(errno));
    return NULL;
  }
  if(!(buf = malloc(sz))) {
    LOG("malloc(%zu) fallita", sz);
    return NULL;
  }
  if(sysctl(mib, 4, buf, &sz, NULL, 0)) {
    LOG("sysctl(proc) fallita: %s", strerror(errno));
    free(buf);
    return NULL;
  }

  *out_size = sz;
  return buf;
}


/* Cammina la lista dynlib di un processo e chiama cb() per ogni voce.
 * Ritorna il numero di voci viste. Se la testa non e' leggibile ritorna -1:
 * e' il segnale che l'offset KPROC_DYNLIB_HEAD non vale per questo firmware. */
static int
walk_dynlibs(pid_t pid, void (*cb)(pid_t, dynlib_head_t*, const char*, void*),
             void *ud)
{
  unsigned long kproc;
  uint64_t node;
  char path[MAX_PATH];
  int n = 0;

  if(!(kproc = kernel_get_proc(pid))) {
    return -1;
  }
  if(kernel_copyout(kproc + KPROC_DYNLIB_HEAD, &node, sizeof(node))) {
    return -1;
  }

  while(node && n < MAX_LIBS) {
    dynlib_head_t d;

    if(kernel_copyout(node, &d, sizeof(d))) {
      break;
    }

    /* le callback che ne hanno bisogno (enumerazione export) leggono il nodo */
    g_node = node;
    g_pid  = pid;

    path[0] = '\0';
    if(d.path) {
      if(kernel_copyout(d.path, path, sizeof(path) - 1)) {
        strcpy(path, "<path illeggibile>");
      }
      path[MAX_PATH - 1] = '\0';
    }

    cb(pid, &d, path, ud);
    n++;

    if(d.next == node) {   /* guardia contro un ciclo */
      break;
    }
    node = d.next;
  }

  return n;
}


/* ---- callback: stampa ---- */
static void
cb_print(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  (void)ud;
  LOG("  pid %-6d handle=%-4llu mapbase=0x%012llx size=%-9llu %s",
      (int)pid, (unsigned long long)d->handle,
      (unsigned long long)d->mapbase,
      (unsigned long long)d->mapsize, path);
}


/* ---- dump ---- */

/* Carica la libreria nel processo del payload.
 * sceKernelLoadStartModule(path, argc, argv, flags, unk, unk, &handle) */
extern int sceKernelLoadStartModule(const char *path, size_t argc,
                                    const void *argv, unsigned int flags,
                                    void *unk1, unsigned int *unk2,
                                    int *handle);


/* Cerca una mappatura per basename fra le dynlib di UN processo. */
typedef struct {
  const char *want;
  intptr_t    base;
  unsigned long size;
} find_ctx_t;

static void
cb_find(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  find_ctx_t *c = ud;
  (void)pid;

  if(c->base || !path[0]) {
    return;
  }
  if(!strcmp(basename_of(path), c->want)) {
    c->base = d->mapbase;
    c->size = d->mapsize;
  }
}


static intptr_t
find_in_process(pid_t pid, const char *name, unsigned long *out_size)
{
  find_ctx_t c = { name, 0, 0 };

  walk_dynlibs(pid, cb_find, &c);
  if(out_size) {
    *out_size = c.size;
  }
  return c.base;
}


/* Ricostruisce il file leggendo dalla PROPRIA memoria: nessuna traduzione
 * virtuale->fisico, quindi nessun 0xCC. */
static int
dump_local(intptr_t base, const char *name, const char *outdir)
{
  elf64_ehdr_t eh;
  elf64_phdr_t ph[MAX_SEG];
  char path[512];
  int fd;

  memcpy(&eh, (void*)base, sizeof(eh));

  if(memcmp(eh.e_ident, "\x7f" "ELF", 4)) {
    LOG("%s: non e' un ELF (base 0x%lx)", name, (unsigned long)base);
    return -1;
  }
  if(eh.e_phnum > MAX_SEG) {
    LOG("%s: %u program header, oltre il limite %d", name, eh.e_phnum, MAX_SEG);
    return -1;
  }

  memcpy(ph, (void*)(base + eh.e_phoff), (size_t)eh.e_phnum * sizeof(elf64_phdr_t));

  snprintf(path, sizeof(path), "%s/%s", outdir, name);

  if((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    LOG("%s: open(%s) fallita: %s", name, path, strerror(errno));
    return -1;
  }

  size_t written = 0;
  for(int i = 0; i < eh.e_phnum; i++) {
    if(ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) {
      continue;
    }
    if(ph[i].p_filesz > (64u << 20)) {
      LOG("%s: segmento %d da %llu byte, sospetto — salto", name, i,
          (unsigned long long)ph[i].p_filesz);
      continue;
    }
    if(pwrite(fd, (void*)(base + ph[i].p_vaddr), ph[i].p_filesz,
              ph[i].p_offset) != (ssize_t)ph[i].p_filesz) {
      LOG("%s: scrittura segmento %d fallita", name, i);
      close(fd);
      return -1;
    }
    written += ph[i].p_filesz;
  }

  close(fd);
  chmod(path, 0644);
  LOG("%s -> %s (%zu byte, base 0x%lx)", name, path, written,
      (unsigned long)base);
  return 0;
}


/* Carica la libreria nel proprio processo e la estrae.
 *
 * Leggere la memoria di un ALTRO processo richiede di tradurre virtuale->fisico,
 * ed e' li' che la lettura torna 0xCC. Se invece la libreria sta nel NOSTRO
 * address space si legge con un memcpy normale: nessuna traduzione, nessun
 * direct map, nessun mdbg. Quindi la carichiamo e poi la copiamo da noi stessi. */
__attribute__((unused))
static int
load_and_dump(const char *name, const char *outdir)
{
  pid_t me = getpid();
  intptr_t base;
  char full[512];
  int err;
  int loaded = 0;

  /* gia' in casa? (es. se l'abbiamo caricata in un giro precedente) */
  base = find_in_process(me, name, NULL);
  if(base) {
    LOG("%s: gia' presente nel processo del payload", name);
    return dump_local(base, name, outdir);
  }

  for(int i = 0; lib_dirs[i]; i++) {
    int handle = 0;

    snprintf(full, sizeof(full), "%s%s", lib_dirs[i], name);
    err = sceKernelLoadStartModule(full, 0, NULL, 0, NULL, NULL, &handle);

    if(!err) {
      LOG("%s: caricata da %s (handle %d)", name, full, handle);
      loaded = 1;
      break;
    }
  }

  if(!loaded) {
    LOG("%s: caricamento fallito (ultimo errore 0x%08x)", name, err);
    return -1;
  }

  base = find_in_process(me, name, NULL);
  if(!base) {
    LOG("%s: caricata ma non trovata nel nostro spazio", name);
    return -1;
  }

  return dump_local(base, name, outdir);
}

/* Controlla se all'indirizzo indicato c'e' la magic di un ELF. */
static int
looks_like_elf(pid_t pid, intptr_t base)
{
  unsigned char m[4];

  if(!base) {
    return 0;
  }
  if(proc_read(pid, base, m, sizeof(m))) {
    return 0;
  }
  return !memcmp(m, "\x7f" "ELF", 4);
}


/* Stampa i primi byte a un indirizzo usando DUE meccanismi diversi, per capire
 * quale dei due legge davvero:
 *   - kernel_proc_copyout: mdbg oppure page-table walk + direct map
 *   - kernel_copyout:      pipe trick su indirizzo kernel diretto
 * Se danno risultati diversi, sappiamo quale strada e' quella giusta. */
static void
probe(pid_t pid, const char *tag, intptr_t addr)
{
  unsigned char a[16], b[16];
  char ha[16 * 3 + 1], hb[16 * 3 + 1];
  int ra, rb, i;

  if(!addr) {
    LOG("    %-10s = 0x0", tag);
    return;
  }

  ra = proc_read(pid, addr, a, sizeof(a));
  rb = kernel_copyout(addr, b, sizeof(b));

  for(i = 0; i < 16; i++) {
    snprintf(ha + i * 3, 4, "%02x ", a[i]);
    snprintf(hb + i * 3, 4, "%02x ", b[i]);
  }

  LOG("    %-10s = 0x%012lx", tag, (unsigned long)addr);
  LOG("        proc  [%s] %s", ra ? "fail" : " ok ", ra ? "-" : ha);
  LOG("        kern  [%s] %s", rb ? "fail" : " ok ", rb ? "-" : hb);
}


/* Cerca la magic ELF dentro una mappatura, a passi di pagina.
 * Ritorna l'offset rispetto a base, oppure -1. */
static long
scan_for_elf(pid_t pid, intptr_t base, unsigned long size)
{
  unsigned char page[4096];
  unsigned long off;
  long limit = (long)(size > (4u << 20) ? (4u << 20) : size);  /* tetto: 4 MiB */

  for(off = 0; off + sizeof(page) <= (unsigned long)limit; off += sizeof(page)) {
    if(proc_read(pid, base + off, page, sizeof(page))) {
      continue;
    }
    for(size_t i = 0; i + 4 <= sizeof(page); i++) {
      if(!memcmp(page + i, "\x7f" "ELF", 4)) {
        return (long)(off + i);
      }
    }
  }
  return -1;
}
static int
dump_elf(pid_t pid, intptr_t base, const char *name, const char *outdir)
{
  elf64_ehdr_t eh;
  elf64_phdr_t ph[MAX_SEG];
  char path[512];
  int fd;

  if(proc_read(pid, base, &eh, sizeof(eh))) {
    LOG("%s: header illeggibile (pid %d)", name, (int)pid);
    return -1;
  }
  if(memcmp(eh.e_ident, "\x7f" "ELF", 4)) {
    LOG("%s: non e' un ELF (pid %d)", name, (int)pid);
    return -1;
  }
  if(eh.e_phnum > MAX_SEG) {
    LOG("%s: %u program header, oltre il limite %d", name, eh.e_phnum, MAX_SEG);
    return -1;
  }
  if(proc_read(pid, base + eh.e_phoff, ph,
                         (size_t)eh.e_phnum * sizeof(elf64_phdr_t))) {
    LOG("%s: program header illeggibili", name);
    return -1;
  }

  snprintf(path, sizeof(path), "%s/%s", outdir, name);

  if((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    LOG("%s: open(%s) fallita: %s", name, path, strerror(errno));
    return -1;
  }

  size_t written = 0;
  for(int i = 0; i < eh.e_phnum; i++) {
    void *seg;

    if(ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0) {
      continue;
    }
    /* Guardia: un p_filesz assurdo significa che stiamo leggendo spazzatura. */
    if(ph[i].p_filesz > (64u << 20)) {
      LOG("%s: segmento %d da %llu byte, sospetto — salto",
          name, i, (unsigned long long)ph[i].p_filesz);
      continue;
    }
    if(!(seg = malloc(ph[i].p_filesz))) {
      LOG("%s: malloc(%llu) fallita", name,
          (unsigned long long)ph[i].p_filesz);
      close(fd);
      return -1;
    }
    if(proc_read(pid, base + ph[i].p_vaddr, seg, ph[i].p_filesz)) {
      LOG("%s: lettura segmento %d fallita", name, i);
      free(seg);
      close(fd);
      return -1;
    }
    if(pwrite(fd, seg, ph[i].p_filesz, ph[i].p_offset) != (ssize_t)ph[i].p_filesz) {
      LOG("%s: scrittura segmento %d fallita", name, i);
      free(seg);
      close(fd);
      return -1;
    }
    written += ph[i].p_filesz;
    free(seg);
  }

  close(fd);
  chmod(path, 0644);
  LOG("%s -> %s (%zu byte da pid %d)", name, path, written, (int)pid);
  return 0;
}


/* Elenca /dev.
 *
 * libSceVideoOut cita /dev/dce (Display Controller Engine) e /dev/dipsw: sono i
 * device della display controller, cioe' l'hardware che scandisce il frame
 * finale verso l'HDMI. E' la pista piu' concreta per la cattura: se un payload
 * puo' aprirli, da li' si puo' arrivare al buffer di scanout senza hookare la
 * GPU di un altro processo.
 *
 * Il server FTP non elenca le sottodirectory della root, quindi lo facciamo
 * da dentro il payload, che gira sulla console. */
__attribute__((unused))
static void
list_dev(void)
{
  DIR *d;
  struct dirent *e;
  int n = 0;

  if(!(d = opendir("/dev"))) {
    LOG("/dev: opendir fallita: %s", strerror(errno));
    return;
  }

  LOG("=== /dev ===");
  while((e = readdir(d)) != NULL && n < 80) {
    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) {
      continue;
    }
    LOG("  %s", e->d_name);
    n++;
  }
  closedir(d);
  LOG("(/dev: %d voci)", n);
}


/* Prova ad aprire i device legati a video/display e vedere cosa rispondono.
 *
 * La cattura e' il pezzo incerto di tutto il progetto. Nella /dev non c'e' un
 * device che dia il frame composto, ma ci sono hdmi e dipsw (che libSceVideoOut
 * cita) e i deci_mp4_*. Se uno di questi espone riferimenti al buffer di
 * scanout, la strada "framebuffer via kernel" diventa praticabile. Se
 * rispondono solo con mode/timing o negano l'accesso, la strada e' chiusa e
 * lo sappiamo adesso invece che dopo una settimana di RE.
 *
 * Tutto in sola lettura: apriamo, proviamo a leggere, chiudiamo. */
__attribute__((unused))
static void
probe_devices(void)
{
  static const char *devs[] = {
    "/dev/hdmi", "/dev/dipsw", "/dev/dce",
    "/dev/mbus_av", "/dev/s3da",
    NULL
    /* deci_mp4_* esclusi: aprono ma la read() blocca anche con O_NONBLOCK,
     * e appendono il payload. */
  };

  LOG("=== device video/display ===");

  for(int i = 0; devs[i]; i++) {
    unsigned char buf[32];
    int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
    ssize_t n;

    /* O_NONBLOCK e' obbligatorio: un device senza dati pronti bloccherebbe la
     * read() e appenderebbe il payload, esattamente come e' successo al primo
     * tentativo senza questa flag. */
    if(fd < 0) {
      int rd_err = errno;
      fd = open(devs[i], O_RDWR | O_NONBLOCK);
      if(fd < 0) {
        LOG("  %-20s  R:%-18s RW:%s", devs[i], strerror(rd_err),
            strerror(errno));
        continue;
      }
      LOG("  %-20s  RW ok (fd %d)  [sola lettura negata: %s]",
          devs[i], fd, strerror(rd_err));
    } else {
      LOG("  %-20s  R ok (fd %d)", devs[i], fd);
    }

    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf));
    if(n < 0) {
      LOG("      read -> %s", strerror(errno));
    } else if(n == 0) {
      LOG("      read -> 0 byte (EOF/nessun dato)");
    } else {
      LOG("      read -> %zd byte: %02x %02x %02x %02x %02x %02x %02x %02x",
          n, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
    }

    close(fd);
  }
}


/* Enumerazione dei sysmodule: quali moduli di sistema sono raggiungibili dal
 * payload, e con quale ID.
 *
 * sceSysmoduleIsLoaded() e' una QUERY: non carica niente, non prende lock, non
 * puo' bloccare. E' il modo sicuro di sondare se l'encoder (o la libreria
 * Remote Play) e' alla portata di un payload — che e' il lucchetto vero di
 * tutta la strada "stream nostro". */
/* ---- risolvere e chiamare funzioni di sistema SENZA linkare stub ----
 *
 * Gli stub dell'SDK non sono linkati (solo libc, pthread, ufs, dl) e
 * sceKernelLoadStartModule si blocca, quindi sembrava un vicolo cieco.
 *
 * Ma c'e' una via che non passa da nessuno dei due:
 *   1. la libreria di sistema e' GIA' mappata nel nostro processo
 *      (libSceSysmodule lo e' — l'abbiamo visto nella lista delle nostre dynlib)
 *   2. kernel_dynlib_resolve() risolve per NID, e restituisce l'indirizzo
 *      DENTRO IL NOSTRO spazio di indirizzamento
 *   3. nid_encode() (include/ps5/nid.h) converte il nome umano nel NID
 *
 * Quindi: nessun caricamento, nessun blocco, e l'indirizzo e' chiamabile
 * direttamente perche' sta nel nostro spazio. Vale per QUALUNQUE funzione di
 * QUALUNQUE libreria che il processo ospite abbia gia' caricato. */

#include <ps5/nid.h>

extern intptr_t kernel_dynlib_resolve(pid_t pid, unsigned int handle,
                                      const char *nid);


typedef struct {
  const char *want;
  unsigned int handle;
} hctx_t;

static void
cb_h(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  hctx_t *c = ud;
  (void)pid;

  if(c->handle || !path[0]) {
    return;
  }
  if(!strcmp(basename_of(path), c->want)) {
    c->handle = (unsigned int)d->handle;
  }
}


static unsigned int
handle_of(const char *lib)
{
  hctx_t c = { lib, 0 };

  walk_dynlibs(getpid(), cb_h, &c);
  return c.handle;
}


/* Risolve un simbolo di una libreria caricata nel nostro processo.
 * Ritorna l'indirizzo, oppure 0. */
static intptr_t
resolve_self(const char *lib, const char *sym)
{
  unsigned int h = handle_of(lib);
  char nid[12] = {0};
  intptr_t fn;

  if(!h) {
    LOG("  %s: non caricata nel nostro processo", lib);
    return 0;
  }

  nid_encode(sym, nid);
  fn = kernel_dynlib_resolve(getpid(), h, nid);

  LOG("  %s!%s  NID=%s  ->  0x%lx", lib, sym, nid, (unsigned long)fn);
  return fn;
}


/* Sonda i sysmodule via la funzione risolta a runtime.
 * sceSysmoduleIsLoaded e' una query: non carica, non prende lock, non blocca. */
__attribute__((unused))
static void
probe_sysmodules(void)
{
  intptr_t fn = resolve_self("libSceSysmodule.sprx", "sceSysmoduleIsLoaded");
  int (*is_loaded)(unsigned int);
  int n = 0;

  if(!fn) {
    LOG("sceSysmoduleIsLoaded non risolta: niente enumerazione");
    return;
  }

  is_loaded = (int (*)(unsigned int))fn;

  LOG("=== sysmodule caricati (via NID, nessun link) ===");
  for(unsigned int id = 0; id < 0x100; id++) {
    if(is_loaded(id) == 0) {
      LOG("  id 0x%03x", id);
      n++;
    }
  }
  LOG("(%d caricati su 0x100)", n);
}


/* ---- esperimento controllato ----
 *
 * Tutto il problema delle letture sta nella catena
 *     VA -> (page-walk) -> paddr -> (direct map) -> byte
 * che finora ha sempre prodotto 0xCC. Per capire QUALE anello e' rotto serve
 * un caso di cui conosciamo la risposta esatta.
 *
 * Ce l'abbiamo: un buffer del nostro stesso processo. Ne conosciamo il VA,
 * ci scriviamo un pattern noto, e poi facciamo il giro completo:
 *   1. traduciamo il VA in fisico col page-walk
 *   2. leggiamo dal direct map
 *   3. confrontiamo col pattern che abbiamo scritto
 *
 * Se combacia, la catena e' sana e il problema e' negli indirizzi delle librerie.
 * Se non combacia, sappiamo che e' il page-walk o il dmap base, e possiamo
 * restringere ulteriormente. */
__attribute__((unused))
static void
self_test(void)
{
  static char marker[4096] __attribute__((aligned(4096)));
  char buf[64];
  unsigned long paddr = 0;
  unsigned long plen = 0;
  pid_t me = getpid();
  const char *witness = "CONTROLLO-1234567890-abcdefghij";

  memset(marker, 0, sizeof(marker));
  memcpy(marker, witness, 30);

  LOG("self-test: pid=%d  marker VA=0x%lx", (int)me, (unsigned long)marker);

  if(kernel_proc_getpaddr(me, (unsigned long)marker, &paddr, &plen)) {
    LOG("  page-walk FALLITO sul nostro stesso processo");
    return;
  }
  LOG("  paddr=0x%012lx  plen=0x%lx  dmap=0x%012lx", paddr, plen,
      (unsigned long)(KERNEL_ADDRESS_DMAP_BASE + paddr));

  memset(buf, 0, sizeof(buf));
  if(kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + paddr, buf, sizeof(buf))) {
    LOG("  lettura dal dmap FALLITA");
    return;
  }

  LOG("  atteso : %.30s", witness);
  LOG("  letto  : %.30s", buf);
  LOG("  ESITO  : %s", memcmp(buf, witness, 30) ? "DIVERSO -> catena rotta"
                                                 : "UGUALE -> catena sana");
}


/* ---- test cross-process ----
 *
 * Il self-test prova la catena VA->paddr->dmap sul NOSTRO processo. Ma il
 * problema riguarda gli ALTRI processi, quindi serve un caso di controllo la'.
 *
 * Ce n'e' uno pulito: ogni processo ha il proprio eboot.bin mappato a 0x400000,
 * e un eseguibile ELF comincia per \x7f E L F. Se leggendo li' troviamo la magic,
 * il walk funziona anche cross-process e il colpevole e' specifico degli
 * indirizzi delle librerie. Se troviamo 0xCC, il walk cross-process e' rotto. */
typedef struct {
  pid_t    pid;
  intptr_t base;
} xctx_t;

static void
cb_x(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  xctx_t *c = ud;

  if(c->base) {
    return;
  }
  if(!strcmp(basename_of(path), "eboot.bin")) {
    c->pid = pid;
    c->base = d->mapbase;
  }
}

__attribute__((unused))
static void
cross_test(void)
{
  size_t sz;
  void *buf = list_procs(&sz);
  xctx_t c = { 0, 0 };

  if(!buf) {
    return;
  }

  for(void *ptr = buf; ptr < (buf + sz);) {
    struct kinfo_proc *ki = (struct kinfo_proc*)ptr;

    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;

    if(ki->ki_pid > 0) {
      walk_dynlibs(ki->ki_pid, cb_x, &c);
      if(c.base) {
        break;
      }
    }
  }
  free(buf);

  if(!c.base) {
    LOG("cross-test: nessun eboot.bin trovato");
    return;
  }

  {
    unsigned char b[16] = {0};
    unsigned long paddr = 0, plen = 0;

    LOG("cross-test: eboot.bin di pid %d a 0x%lx", (int)c.pid,
        (unsigned long)c.base);

    if(proc_read(c.pid, c.base, b, sizeof(b))) {
      LOG("  lettura FALLITA");
      return;
    }

    LOG("  primi byte: %02x %02x %02x %02x %02x %02x %02x %02x",
        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    LOG("  e' un ELF? %s",
        memcmp(b, "\x7f" "ELF", 4) ? "NO -> walk cross-process rotto"
                                   : "SI -> walk cross-process sano");

    if(!kernel_proc_getpaddr(c.pid, (unsigned long)c.base, &paddr, &plen)) {
      LOG("  paddr=0x%012lx plen=0x%lx", paddr, plen);
    }
  }
}


/* ---- scandaglio di una libreria ----
 *
 * Se una mappatura fosse tutta 0xCC sarebbe sospetto: del codice che gira non
 * puo' essere riempimento. Se invece solo alcune pagine lo sono, allora la
 * mappatura e' parzialmente residente e stiamo leggendo quella parte.
 * Leggiamo punti sparsi invece del solo inizio, che ci ha ingannati finora. */
typedef struct {
  const char *want;
  pid_t       pid;
  intptr_t    base;
  unsigned long size;
} lctx_t;

static void
cb_l(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  lctx_t *c = ud;

  if(c->base) {
    return;
  }
  if(!strcmp(basename_of(path), c->want)) {
    c->pid = pid;
    c->base = d->mapbase;
    c->size = d->mapsize;
  }
}

__attribute__((unused))
static void
scan_library(const char *name)
{
  size_t sz;
  void *buf = list_procs(&sz);
  lctx_t c = { name, 0, 0, 0 };

  if(!buf) {
    return;
  }

  for(void *ptr = buf; ptr < (buf + sz);) {
    struct kinfo_proc *ki = (struct kinfo_proc*)ptr;

    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;

    if(ki->ki_pid > 0) {
      walk_dynlibs(ki->ki_pid, cb_l, &c);
      if(c.base) {
        break;
      }
    }
  }
  free(buf);

  if(!c.base) {
    LOG("scan %s: non trovata", name);
    return;
  }

  LOG("scan %s: pid %d base=0x%lx size=%lu",
      name, (int)c.pid, (unsigned long)c.base, c.size);

  {
    static const unsigned long offs[] = {
      0x0, 0x1000, 0x2000, 0x10000, 0x40000, 0x80000
    };

    for(unsigned i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
      unsigned char b[16] = {0};
      unsigned long paddr = 0, plen = 0;

      if(offs[i] >= c.size) {
        break;
      }
      if(proc_read(c.pid, c.base + offs[i], b, sizeof(b))) {
        LOG("  +0x%-7lx  <lettura fallita>", offs[i]);
        continue;
      }
      if(!kernel_proc_getpaddr(c.pid, (unsigned long)c.base + offs[i],
                               &paddr, &plen)) {
        LOG("  +0x%-7lx  paddr=0x%012lx  %02x %02x %02x %02x %02x %02x %02x %02x",
            offs[i], paddr, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
      }
    }
  }
}


/* ---- dump di una libreria: mappatura + ELF sintetico ----
 *
 * Il loader PS5 riempie di 0xCC la prima pagina di un .sprx caricato: l'header
 * ELF originale non sopravvive, quindi ricostruire il file com'era e' impossibile.
 * Ma al reverse engineering serve il CODICE, e quello e' intatto da +0x1000 in poi.
 *
 * Quindi non inseguiamo il file originale: dumpiamo la mappatura e le costruiamo
 * attorno un header ELF64 valido, con un PT_LOAD che copre tutto a p_vaddr = base.
 * Il risultato si apre direttamente in Ghidra, objdump o nel container rea, con
 * gli indirizzi corretti.
 *
 * Layout del file prodotto:
 *   0x0000            Elf64_Ehdr + 1 Elf64_Phdr (dentro la prima pagina)
 *   0x1000  in poi    la mappatura, identica byte per byte
 */
/* Ritorna il numero di byte LEGGIBILI, non solo 0/-1: alcune mappature sono
 * parzialmente non residenti (pagine mai toccate), e fra piu' processi che
 * mappano la stessa libreria quello col maggior numero di pagine residenti e'
 * il candidato migliore. */
static long
dump_library(pid_t pid, intptr_t base, unsigned long size,
             const char *name, const char *outdir)
{
  const unsigned long PAGE = 0x1000;
  elf64_ehdr_t eh;
  elf64_phdr_t ph;
  unsigned char preamble[PAGE];
  char path[512];
  unsigned char *chunk;
  int fd;

  if(size < PAGE || size > (64u << 20)) {
    LOG("%s: dimensione mappatura sospetta (%lu)", name, size);
    return -1;
  }

  memset(&eh, 0, sizeof(eh));
  memset(&ph, 0, sizeof(ph));
  memset(preamble, 0, sizeof(preamble));

  /* --- header ELF64 --- */
  eh.e_ident[0] = 0x7f;
  eh.e_ident[1] = 'E';
  eh.e_ident[2] = 'L';
  eh.e_ident[3] = 'F';
  eh.e_ident[4] = 2;              /* ELFCLASS64 */
  eh.e_ident[5] = 1;              /* ELFDATA2LSB */
  eh.e_ident[6] = 1;              /* EV_CURRENT */
  eh.e_ident[7] = 0;              /* ELFOSABI_SYSV */
  eh.e_type      = 3;             /* ET_DYN */
  eh.e_machine   = 62;            /* EM_X86_64 */
  eh.e_version   = 1;
  eh.e_entry     = (uint64_t)base + PAGE;
  eh.e_phoff     = sizeof(elf64_ehdr_t);
  eh.e_shoff     = 0;
  eh.e_ehsize    = sizeof(elf64_ehdr_t);
  eh.e_phentsize = sizeof(elf64_phdr_t);
  eh.e_phnum     = 1;

  /* --- un PT_LOAD che copre la mappatura --- */
  ph.p_type   = PT_LOAD;
  ph.p_flags  = 7;                /* R|W|X */
  ph.p_offset = PAGE;             /* i dati iniziano dopo la prima pagina */
  ph.p_vaddr  = (uint64_t)base;
  ph.p_paddr  = (uint64_t)base;
  ph.p_filesz = (uint64_t)size;
  ph.p_memsz  = (uint64_t)size;
  ph.p_align  = PAGE;

  memcpy(preamble, &eh, sizeof(eh));
  memcpy(preamble + sizeof(eh), &ph, sizeof(ph));

  snprintf(path, sizeof(path), "%s/%s", outdir, name);

  if((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    LOG("%s: open(%s) fallita: %s", name, path, strerror(errno));
    return -1;
  }

  if(pwrite(fd, preamble, PAGE, 0) != (ssize_t)PAGE) {
    LOG("%s: scrittura preambolo fallita", name);
    close(fd);
    return -1;
  }

  /* --- la mappatura, a blocchi --- */
  if(!(chunk = malloc(64 * 1024))) {
    LOG("%s: malloc fallita", name);
    close(fd);
    return -1;
  }

  unsigned long off = 0;
  unsigned long good = 0;

  /* Lettura PAGINA PER PAGINA, non a blocchi da 64 KB.
   *
   * proc_read fallisce l'intero blocco se anche una sola pagina al suo interno
   * non e' residente. Con blocchi da 64 KB bastava una pagina mancante per
   * perdere 15 pagine buone — ed e' quello che faceva sembrare illeggibili
   * libwebrtc (0%) e libSceVideoRecording (6%), quando un campione a 4 KB
   * mostrava 8 pagine su 8 leggibili. */
  const unsigned long CH = 4096;

  while(off < size) {
    unsigned long n = size - off;

    if(n > CH) {
      n = CH;
    }

    if(proc_read(pid, base + off, chunk, n)) {
      /* pagina non residente: la riempiamo di zero e proseguiamo.
       * Meglio un buco che perdere tutto il resto. */
      memset(chunk, 0, n);
    } else {
      good += n;
    }

    if(pwrite(fd, chunk, n, (off_t)(PAGE + off)) != (ssize_t)n) {
      LOG("%s: scrittura a offset 0x%lx fallita", name, off);
      free(chunk);
      close(fd);
      return -1;
    }
    off += n;
  }

  free(chunk);
  close(fd);
  chmod(path, 0644);

  LOG("%s <- pid %d (0x%lx): %lu byte su %lu leggibili (%lu%%)",
      name, (int)pid, (unsigned long)base, good, size,
      size ? (good * 100 / size) : 0);
  return (long)good;
}


/* La stessa libreria e' mappata in PIU' processi, e non tutti sono leggibili:
 * alcuni danno 0 byte su tutta la mappatura pur avendo i metadati accessibili.
 * Quindi raccogliamo tutti i candidati e li proviamo in ordine, finche' uno
 * si lascia leggere, invece di fermarci al primo trovato. */
typedef struct {
  const char *want;
  int          n;
  pid_t        pid[8];
  intptr_t     base[8];
  unsigned long size[8];
} mcand_t;

static void
cb_m(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  mcand_t *c = ud;

  if(c->n >= 8) {
    return;
  }
  if(!path[0] || strcmp(basename_of(path), c->want)) {
    return;
  }

  c->pid[c->n]  = pid;
  c->base[c->n] = d->mapbase;
  c->size[c->n] = d->mapsize;
  c->n++;
}


static int
dump_library_by_name(const char *name, const char *outdir)
{
  size_t sz;
  void *buf = list_procs(&sz);
  mcand_t c;

  memset(&c, 0, sizeof(c));
  c.want = name;

  if(!buf) {
    return -1;
  }

  for(void *ptr = buf; ptr < (buf + sz);) {
    struct kinfo_proc *ki = (struct kinfo_proc*)ptr;

    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;

    if(ki->ki_pid > 0) {
      walk_dynlibs(ki->ki_pid, cb_m, &c);
    }
  }
  free(buf);

  if(!c.n) {
    LOG("%s: non trovata in nessun processo", name);
    return -1;
  }

  LOG("%s: %d processi la mappano, provo tutti e tengo il migliore", name, c.n);

  long best = 0;

  for(int i = 0; i < c.n; i++) {
    long g = dump_library(c.pid[i], c.base[i], c.size[i], name, outdir);

    if(g > best) {
      best = g;
    }
    /* mappatura completamente leggibile: non c'e' di meglio da cercare */
    if(g > 0 && (unsigned long)g == c.size[i]) {
      return 0;
    }
  }

  if(!best) {
    char path[512];

    snprintf(path, sizeof(path), "%s/%s", outdir, name);
    unlink(path);
    LOG("%s: nessun processo con pagine leggibili", name);
    return -1;
  }

  LOG("%s: migliore parziale, %ld byte", name, best);
  return 0;
}


/* Chiede al sistema di caricare l'encoder nel NOSTRO processo.
 *
 * Gli ID vengono dalla tabella dei nomi dentro libSceSysmodule (dumpata prima):
 * le voci sono in ordine di ID, e contando solo i nomi di modulo — scartando le
 * stringhe d'errore interlacciate — libSceVenc risulta la voce 117.
 *
 * Differenza cruciale rispetto al primo tentativo: quello spazzava ID da 0 a 0xFF
 * alla cieca, e le API Sony fanno abort() su argomenti fuori range — ha ucciso il
 * processo ospite. Qui si prova UN ID plausibile, e solo se fallisce il vicino.
 *
 * Il caricamento lo facciamo con la funzione risolta per NID, non linkata: e'
 * l'unica via visto che gli stub non sono linkati e sceKernelLoadStartModule
 * si blocca. */
/* Calibra la corrispondenza indice-in-tabella -> module ID.
 *
 * La tabella dei nomi in libSceSysmodule e' in ordine di ID, ma non sappiamo a
 * quale ID corrisponda la prima voce. Per scoprirlo usiamo due riferimenti che
 * conosciamo per certo: libSceLibcInternal (indice 33) e libSceNet (indice 89)
 * sono ENTRAMBE caricate nel nostro processo — le abbiamo viste nella lista
 * delle nostre dynlib.
 *
 * Chiedendo al sistema quali ID risultano caricati, l'ID che combacia con 33 e
 * quello che combacia con 89 ci danno la base. Con quella, l'ID dell'encoder
 * smette di essere una stima.
 *
 * Sondiamo 1..149: la tabella ha 411 voci, quindi qualunque sia la base questo
 * intervallo e' dentro il range valido — e gli ID fuori range sono quelli che
 * fanno abortire, non quelli validi. */
__attribute__((unused))
static void
calibrate_ids(void)
{
  intptr_t fn = resolve_self("libSceSysmodule.sprx", "sceSysmoduleIsLoaded");
  int (*is_loaded)(unsigned int);
  int n = 0;

  if(!fn) {
    LOG("sceSysmoduleIsLoaded non risolta: niente calibrazione");
    return;
  }
  is_loaded = (int (*)(unsigned int))fn;

  LOG("=== calibrazione ID (query, nessun caricamento) ===");

  for(unsigned int id = 1; id < 150; id++) {
    int r = is_loaded(id);
    if(r == 0) {
      LOG("  id %u -> caricato", id);
      n++;
    }
  }
  LOG("(%d caricati su 1..149)", n);
}


/* Sonda LoadModule su un intervallo di ID e si ferma appena libSceVenc compare
 * fra le NOSTRE librerie.
 *
 * E' auto-verificante e sicuro:
 *  - verificato due volte che un ID non valido restituisce 0x805a1000 e NON aborta
 *    (l'abort di prima era di sceSysmoduleIsLoaded, non di LoadModule)
 *  - l'esito si legge dal risultato, non si deduce: se libSceVenc entra nel
 *    nostro spazio, la funzione di ricerca la vede
 *
 * Tetto basso di proposito: caricare centinaia di moduli in un'app VSH che ha
 * ~20 MB liberi la farebbe esaurire. Se dopo MAX_TRIES non l'ha trovata, ci
 * fermiamo e lo diciamo. */
#define SWEEP_MAX (140)

__attribute__((unused))
static void
load_encoder(void)
{
  intptr_t fn = resolve_self("libSceSysmodule.sprx", "sceSysmoduleLoadModule");
  int (*load)(unsigned int);
  int tried = 0, ok = 0;

  if(!fn) {
    LOG("sceSysmoduleLoadModule non risolta");
    return;
  }
  load = (int (*)(unsigned int))fn;

  LOG("=== sonda LoadModule, cerco libSceVenc ===");

  for(unsigned int id = 1; id <= SWEEP_MAX && !tried; id++) {
    int r = load(id);

    if(r != 0) {
      continue;    /* non valido o non caricabile: normale, si prosegue */
    }

    ok++;
    LOG("  id %u: caricato", id);

    if(find_in_process(getpid(), "libSceVenc.sprx", NULL)) {
      LOG("  *** libSceVenc CARICATA con id %u ***", id);
      tried = id;
    }
  }

  if(tried) {
    intptr_t base = find_in_process(getpid(), "libSceVenc.sprx", NULL);
    LOG("libSceVenc nel mio processo a 0x%lx", (unsigned long)base);
    dump_library(getpid(), base, 704512, "libSceVenc.sprx", OUTDIR);
    return;
  }

  LOG("libSceVenc non comparsa (id provati 1..%d, caricati %d)", SWEEP_MAX, ok);
}


__attribute__((unused))
static void
load_encoder_old(void)
{
  intptr_t fn = resolve_self("libSceSysmodule.sprx", "sceSysmoduleLoadModule");
  int (*load)(unsigned int);
  unsigned int ids[2] = { 117, 118 };
  int done = 0;

  if(!fn) {
    LOG("sceSysmoduleLoadModule non risolta: non posso caricare");
    return;
  }

  load = (int (*)(unsigned int))fn;

  for(int i = 0; i < 2 && !done; i++) {
    int r = load(ids[i]);
    LOG("  load(%u) -> %d (0x%08x)", ids[i], r, (unsigned)r);
    if(r == 0) {
      done = 1;
      LOG("  ^^ CARICATO con id %u", ids[i]);
    }
  }

  if(!done) {
    LOG("nessuno dei due ID ha caricato l'encoder");
    return;
  }

  /* ora dovrebbe essere nel nostro processo: dumpiamola */
  {
    intptr_t base = find_in_process(getpid(), "libSceVenc.sprx", NULL);

    if(!base) {
      LOG("caricato ma non lo vedo nel mio spazio");
      return;
    }
    LOG("libSceVenc ora a 0x%lx nel mio processo", (unsigned long)base);
    dump_library(getpid(), base, 704512, "libSceVenc.sprx", OUTDIR);
  }
}


/* Legge /dev/klog e ne stampa la coda.
 *
 * Il loader dei sysmodule scrive nel kernel log il MOTIVO del fallimento,
 * compreso il nome del PRX e il riferimento all'errore nel Kernel Reference.
 * Invece di indovinare dal codice 0x805a1000, ci facciamo dire cosa e' andato
 * storto davvero. */
__attribute__((unused))
static void
read_klog(void)
{
  char buf[2048];
  ssize_t n;
  int fd = open("/dev/klog", O_RDONLY | O_NONBLOCK);
  int got = 0;

  if(fd < 0) {
    LOG("/dev/klog: %s", strerror(errno));
    return;
  }

  LOG("=== /dev/klog ===");
  while(got < 40 && (n = read(fd, buf, sizeof(buf) - 1)) > 0) {
    buf[n] = '\0';
    printf("%s", buf);
    got++;
  }
  if(got == 0) {
    LOG("  (nessun dato leggibile)");
  }
  close(fd);
  LOG("=== fine /dev/klog ===");
}


/* ---- enumerazione degli export di una libreria caricata ----
 *
 * Il problema di fondo era: per chiamare una funzione servono NID e indirizzo,
 * e per averli serviva la libreria... che non riuscivamo a leggere.
 *
 * Ma dynlib_obj_t ha un campo dynsec che punta alla dynamic section della
 * libreria CARICATA. Da li' si arriva a DT_SYMTAB / DT_STRTAB, e quindi
 * all'elenco completo degli export: per ognuno il NID (il nome del simbolo) e
 * l'indirizzo.
 *
 * Questo non richiede di leggere il codice: legge solo le strutture di
 * collegamento, che sono piccole e sempre residenti. E vale per qualunque
 * libreria mappata, in qualunque processo. */
#define OFF_DYNLIB_DYNSEC (328)

typedef struct { uint64_t tag, val; } dyn_ent_t;

#define DT_NULL      0
#define DT_HASH      4
#define DT_STRTAB    5
#define DT_SYMTAB    6
#define DT_STRSZ     10
#define DT_SYMENT    11

typedef struct {
  uint32_t st_name;
  uint8_t  st_info;
  uint8_t  st_other;
  uint16_t st_shndx;
  uint64_t st_value;
  uint64_t st_size;
} elf64_sym_t;

/* Prova a leggere un processo che finora ci e' risultato opaco (pid 75,
 * webrtc_daemon). Ritorna il numero di pagine leggibili su un campione. */
static int
probe_opaque(pid_t target, intptr_t base, unsigned long size)
{
  unsigned char buf[64];
  int ok = 0;

  for(unsigned long off = 0; off + sizeof(buf) <= size && off < 0x8000;
      off += 4096) {
    if(!kernel_proc_copyout(target, base + off, buf, sizeof(buf))) {
      ok++;
    }
  }
  return ok;
}


/* Alza le capability del NOSTRO processo e vede se i processi opachi diventano
 * leggibili.
 *
 * Ipotesi da verificare: webrtc_daemon (pid 75) e' trasparente alle nostre
 * letture non perche' le sue pagine siano assenti, ma perche' non abbiamo la
 * capability di attraversarlo. Con kstuff la cosa e' reversibile e il confronto
 * prima/dopo dice se l'ipotesi regge. */
static void
try_elevate(void)
{
  unsigned char caps[16];
  unsigned char all[16];
  pid_t me = getpid();
  int before = 0, after = 0;
  char hex[16 * 3 + 1];
  int i;

  /* pid 75 e la sua mappatura, dalla scansione precedente */
  const pid_t target = 75;
  const intptr_t tbase = 0x82c880000;   /* libwebrtc.sprx */

  LOG("=== lettura di un campione da pid %d PRIMA ===", (int)target);
  before = probe_opaque(target, tbase, 0x8000);
  LOG("  pagine leggibili: %d", before);

  if(!kernel_get_ucred_caps(me, caps)) {
    for(i = 0; i < 16; i++) {
      snprintf(hex + i * 3, 4, "%02x ", caps[i]);
    }
    LOG("  nostre caps: %s", hex);
  } else {
    LOG("  kernel_get_ucred_caps fallita");
  }

  LOG("  nostro authid: 0x%lx", (unsigned long)kernel_get_ucred_authid(me));

  memset(all, 0xff, sizeof(all));
  LOG("  set caps a ff*16 -> %d", kernel_set_ucred_caps(me, all));

  if(!kernel_get_ucred_caps(me, caps)) {
    for(i = 0; i < 16; i++) {
      snprintf(hex + i * 3, 4, "%02x ", caps[i]);
    }
    LOG("  caps dopo: %s", hex);
  }

  LOG("=== stesso campione DOPO ===");
  after = probe_opaque(target, tbase, 0x8000);
  LOG("  pagine leggibili: %d", after);

  LOG("  esito: %s", after > before ? "ELEVAZIONE HA FUNZIONATO"
                                      : "nessun cambiamento");
}


/* Legge da un indirizzo che puo' essere indifferentemente kernel o utente.
 *
 * Nel nodo dynlib i puntatori sono un misto: mapbase e' un VA utente del
 * processo, dynsec e' un indirizzo del direct map kernel, e symtab/strtab
 * dipendono da come il loader li ha rilocati. Invece di indovinare ogni volta
 * quale lettore usare, li proviamo entrambi. */
static int
read_any(pid_t pid, uint64_t addr, void *buf, size_t len)
{
  if(!addr) {
    return -1;
  }
  if(!kernel_copyout(addr, buf, len)) {
    return 0;
  }
  if(!kernel_proc_copyout(pid, addr, buf, len)) {
    return 0;
  }
  return -1;
}


/* g_node / g_pid sono dichiarati in cima al file: walk_dynlibs li aggiorna. */
static void
enum_exports(pid_t pid, unsigned long node, const char *libname)
{
  uint64_t dynsec = 0;
  uint64_t mapbase = 0, mapsize = 0;

  /* Prima di tutto: mappa empirica del nodo. Il layout di dynlib_obj_t che
   * avevo ricavato a mano deriva a meta', quindi invece di contare i campi
   * cerco, dentro il nodo, i valori che puntano DENTRO la mappatura della
   * libreria. dynsec e' per forza uno di quelli. */
  /* Il nodo dynlib sta nell'heap KERNEL: si legge con kernel_copyout, non con
   * kernel_proc_copyout (che traduce indirizzi UTENTE del processo target).
   * E' l'errore che faceva tornare mapbase=0 e dynsec illeggibile. */
  if(kernel_copyout(node + 48, &mapbase, sizeof(mapbase)) ||
     kernel_copyout(node + 56, &mapsize, sizeof(mapsize))) {
    LOG("%s: mapbase illeggibile", libname);
    return;
  }

  LOG("%s: mapbase=0x%lx mapsize=%lu", libname,
      (unsigned long)mapbase, (unsigned long)mapsize);

  {
    unsigned char raw[640];
    int n = 0;

    if(!kernel_copyout(node, raw, sizeof(raw))) {
      for(int off = 0; off + 8 <= (int)sizeof(raw); off += 8) {
        uint64_t v;

        memcpy(&v, raw + off, 8);
        if(mapbase && v >= mapbase && v < mapbase + mapsize) {
          LOG("    campo @%3d -> 0x%lx", off, (unsigned long)v);
          n++;

          /* Prova: questo indirizzo e' una dynamic section?
           * Non mi fido dell'offset calcolato a mano — un campo che credevo
           * dynsec non lo era. Lascio che sia il contenuto a dirlo. */
          {
            dyn_ent_t t;
            uint64_t st = 0, sr = 0;

            for(int k = 0; k < 48; k++) {
              if(read_any(pid, v + (unsigned long)k * 16, &t, sizeof(t))) {
                break;
              }
              if(t.tag == DT_NULL) {
                break;
              }
              if(t.tag == DT_SYMTAB) st = t.val;
              if(t.tag == DT_STRTAB) sr = t.val;
            }
            if(st && sr) {
              LOG("      ^^ E' LA DYNAMIC SECTION: symtab=0x%lx strtab=0x%lx",
                  (unsigned long)st, (unsigned long)sr);
            }
          }
        }
      }
    }
    LOG("  (%d puntatori interni trovati)", n);
  }

  uint64_t symtab = 0, strtab = 0, hash = 0, strsz = 0;
  dyn_ent_t e;
  int shown = 0;

  if(kernel_copyout(node + OFF_DYNLIB_DYNSEC, &dynsec, sizeof(dynsec))
     || !dynsec) {
    LOG("%s: dynsec non leggibile", libname);
    return;
  }

  LOG("%s: dynsec=0x%lx  cerco symtab/strtab", libname, (unsigned long)dynsec);

  for(int i = 0; i < 64; i++) {
    if(read_any(pid, dynsec + (unsigned long)i * sizeof(e), &e, sizeof(e))) {
      break;
    }
    if(e.tag == DT_NULL) {
      break;
    }
    if(e.tag == DT_SYMTAB) symtab = e.val;
    if(e.tag == DT_STRTAB) strtab = e.val;
    if(e.tag == DT_HASH)   hash = e.val;
    if(e.tag == DT_STRSZ)  strsz = e.val;
  }

  LOG("  symtab=0x%lx strtab=0x%lx hash=0x%lx strsz=%lu",
      (unsigned long)symtab, (unsigned long)strtab,
      (unsigned long)hash, (unsigned long)strsz);

  if(!symtab || !strtab) {
    LOG("  niente symbol table");
    return;
  }

  /* nchain dalla hash table: seconda parola */
  unsigned long nsym = 0;
  if(hash) {
    uint32_t w[2] = {0, 0};
    if(!read_any(pid, hash, w, sizeof(w))) {
      nsym = w[1];
    }
  }
  if(!nsym || nsym > 20000) {
    nsym = 512;   /* stima prudente */
  }

  LOG("  %lu simboli, ecco gli export:", nsym);

  for(unsigned long i = 0; i < nsym && shown < 60; i++) {
    elf64_sym_t s;
    char name[64];

    if(read_any(pid, symtab + i * sizeof(s), &s, sizeof(s))) {
      break;
    }
    if(!s.st_name || !s.st_value || s.st_shndx == 0) {
      continue;   /* vuoto o non definito: non e' un export */
    }
    if(s.st_name >= (strsz ? strsz : 0x10000)) {
      continue;
    }
    if(read_any(pid, strtab + s.st_name, name, sizeof(name) - 1)) {
      continue;
    }
    name[sizeof(name) - 1] = '\0';

    if(!name[0]) {
      continue;
    }

    LOG("    %-14s  0x%lx", name, (unsigned long)s.st_value);
    shown++;
  }

  LOG("  (%d export mostrati)", shown);
}


typedef struct { const char *want; int done; } expctx_t;

static void
cb_exp(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  expctx_t *c = ud;
  const char *name = basename_of(path);

  (void)d;

  if(c->done || !name[0]) {
    return;
  }
  if(strcmp(name, c->want)) {
    return;
  }

  c->done = 1;
  enum_exports(pid, g_node, name);
}


__attribute__((unused))
static void
show_exports(const char *libname)
{
  expctx_t c = { libname, 0 };

  /* cerca in tutti i processi: la libreria puo' stare in un altro */
  {
    size_t sz;
    void *buf = list_procs(&sz);

    if(!buf) {
      return;
    }
    for(void *ptr = buf; ptr < (buf + sz);) {
      struct kinfo_proc *ki = (struct kinfo_proc*)ptr;

      if(ki->ki_structsize <= 0) {
        break;
      }
      ptr += ki->ki_structsize;

      if(ki->ki_pid > 0) {
        walk_dynlibs(ki->ki_pid, cb_exp, &c);
        if(c.done) {
          break;
        }
      }
    }
    free(buf);
  }

  if(!c.done) {
    LOG("%s: non trovata", libname);
  }
}


/* Mappa dell'infrastruttura di streaming/cattura.
 *
 * Il kernel log ha rivelato processi che non conoscevamo: SceGameLiveStreaming,
 * webrtc_daemon.self, SceDiscordDaemon, SceAvCapture, AgcCompositor. Sono
 * infrastruttura di streaming nativa, distinta da Remote Play.
 *
 * Qui cerco quali processi hanno caricate le librerie che contano, per capire
 * dove sta la pipeline — e quindi in quale host avrebbe senso far girare un
 * payload che la usi. */
static int
is_media_lib(const char *n)
{
  static const char *pat[] = {
    "Stream", "WebRTC", "webrtc", "Streaming", "Capture", "Avcap",
    "Venc", "Vdec", "VideoCore", "Share", "Video", NULL
  };

  for(int i = 0; pat[i]; i++) {
    if(strstr(n, pat[i])) {
      return 1;
    }
  }
  return 0;
}


static void
cb_scan(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  int *n = ud;
  const char *name = basename_of(path);

  if(!name[0] || !is_media_lib(name)) {
    return;
  }
  LOG("  pid %-6d  base=0x%012lx  %8lu  %s",
      (int)pid, (unsigned long)d->mapbase, d->mapsize, name);
  (*n)++;
}


__attribute__((unused))
static void
scan_media(void)
{
  size_t sz;
  void *buf = list_procs(&sz);
  int n = 0;

  if(!buf) {
    return;
  }

  LOG("=== librerie video/streaming in TUTTI i processi ===");

  for(void *ptr = buf; ptr < (buf + sz);) {
    struct kinfo_proc *ki = (struct kinfo_proc*)ptr;

    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;

    if(ki->ki_pid > 0) {
      walk_dynlibs(ki->ki_pid, cb_scan, &n);
    }
  }
  free(buf);
  LOG("(%d voci)", n);
}


/* ---- dump delle librerie del NOSTRO processo (solo memcpy) ----
 *
 * Questa e' la strada piu' sicura di tutto il payload: le librerie caricate nel
 * processo che ci ospita stanno nel NOSTRO spazio di indirizzamento, quindi si
 * leggono con un memcpy. Nessuna kernel read, nessun page walk, nessun device,
 * nessuna chiamata di sistema. Non puo' far male a niente.
 *
 * E ci serve esattamente questo: libSceSysmodule e' fra queste, e contiene la
 * corrispondenza fra ID numerico e nome del modulo — cioe' quello che manca
 * per chiedere il caricamento dell'encoder con un ID VALIDO, invece di sondare
 * a tentoni come ho fatto prima (e che ha fatto abortire il processo ospite). */
__attribute__((unused))
static void
dump_local_raw(intptr_t base, unsigned long size, const char *name,
               const char *outdir)
{
  const unsigned long PAGE = 0x1000;
  elf64_ehdr_t eh;
  elf64_phdr_t ph;
  unsigned char preamble[PAGE];
  char path[512];
  int fd;

  if(size < PAGE || size > (64u << 20)) {
    LOG("  %s: dimensione sospetta (%lu), salto", name, size);
    return;
  }

  memset(&eh, 0, sizeof(eh));
  memset(&ph, 0, sizeof(ph));
  memset(preamble, 0, sizeof(preamble));

  eh.e_ident[0] = 0x7f;
  eh.e_ident[1] = 'E';
  eh.e_ident[2] = 'L';
  eh.e_ident[3] = 'F';
  eh.e_ident[4] = 2;
  eh.e_ident[5] = 1;
  eh.e_ident[6] = 1;
  eh.e_type      = 3;
  eh.e_machine   = 62;
  eh.e_version   = 1;
  eh.e_entry     = (uint64_t)base + PAGE;
  eh.e_phoff     = sizeof(elf64_ehdr_t);
  eh.e_ehsize    = sizeof(elf64_ehdr_t);
  eh.e_phentsize = sizeof(elf64_phdr_t);
  eh.e_phnum     = 1;

  ph.p_type   = PT_LOAD;
  ph.p_flags  = 7;
  ph.p_offset = PAGE;
  ph.p_vaddr  = (uint64_t)base;
  ph.p_paddr  = (uint64_t)base;
  ph.p_filesz = (uint64_t)size;
  ph.p_memsz  = (uint64_t)size;
  ph.p_align  = PAGE;

  memcpy(preamble, &eh, sizeof(eh));
  memcpy(preamble + sizeof(eh), &ph, sizeof(ph));

  snprintf(path, sizeof(path), "%s/self_%s", outdir, name);

  if((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0) {
    LOG("  %s: open fallita: %s", name, strerror(errno));
    return;
  }

  if(pwrite(fd, preamble, PAGE, 0) != (ssize_t)PAGE) {
    close(fd);
    return;
  }

  /* Scrittura a blocchi da 64 KB. Una pwrite unica da qualche MB fallisce:
   * i dump che funzionano scrivono cosi', ed e' la differenza che conta. */
  {
    const size_t CH = 64 * 1024;
    unsigned long off = 0;

    while(off < size) {
      size_t n = size - off;

      if(n > CH) {
        n = CH;
      }
      if(pwrite(fd, (void*)(base + off), n, (off_t)(PAGE + off))
         != (ssize_t)n) {
        LOG("  %s: scrittura fallita a offset 0x%lx (%s)",
            name, off, strerror(errno));
        close(fd);
        return;
      }
      off += n;
    }
  }

  close(fd);
  chmod(path, 0644);
  LOG("  %s -> %s (%lu byte, vaddr 0x%lx)", name, path, size,
      (unsigned long)base);
}


static void
cb_selfdump(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  int *n = ud;
  const char *name = basename_of(path);

  (void)pid;

  if(!name[0] || !d->mapbase || d->mapsize < 4096) {
    return;
  }
  /* l'eseguibile dell'host: grande e non ci serve */
  if(strstr(name, "eboot.bin")) {
    return;
  }

  /* NON usiamo il memcpy: il mapbase della libreria non e' mappato nello spazio
   * del payload (il walk lo trova perche' legge via kernel). Passiamo dalla
   * stessa lettura kernel che usiamo per gli altri processi — funziona anche
   * sul proprio pid, ed e' gia' collaudata. */
  if(dump_library(pid, d->mapbase, d->mapsize, name, OUTDIR) > 0) {
    (*n)++;
  }
}


__attribute__((unused))
static void
dump_self_all(void)
{
  int n = 0;

  LOG("=== dump delle librerie del MIO processo (memcpy) ===");
  walk_dynlibs(getpid(), cb_selfdump, &n);
  LOG("(%d librerie)", n);
}


/* ---- modalita' dump ---- */

typedef struct {
  const char *want[32];   /* NULL-terminata; vuota = niente */
  int         nwant;
  char        done[MAX_LIBS][MAX_PATH / 2];  /* nomi gia' estratti */
  int         ndone;
  int         dumped;
} dump_filter_t;

static const char *
basename_of(const char *path)
{
  const char *slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}


static void
cb_dump(pid_t pid, dynlib_head_t *d, const char *path, void *ud)
{
  dump_filter_t *f = ud;
  const char *name = basename_of(path);
  intptr_t base = 0;

  if(!name[0] || d->mapsize < 64) {
    return;
  }

  /* filtra per nome, se e' stata passata una lista */
  if(f->nwant) {
    int hit = 0;
    for(int i = 0; i < f->nwant; i++) {
      if(!strcmp(f->want[i], name)) {
        hit = 1;
        break;
      }
    }
    if(!hit) {
      return;
    }
  }

  /* la stessa libreria compare in piu' processi: la estraiamo una volta sola */
  for(int i = 0; i < f->ndone; i++) {
    if(!strcmp(f->done[i], name)) {
      return;
    }
  }

  /* L'header ELF non e' detto che stia esattamente a mapbase: proviamo i candidati
   * diretti e poi una scansione della mappatura. */
  if(looks_like_elf(pid, d->mapbase)) {
    base = d->mapbase;
  } else if(d->vaddrbase && d->vaddrbase != d->mapbase &&
            looks_like_elf(pid, d->vaddrbase)) {
    base = d->vaddrbase;
  } else {
    long off = scan_for_elf(pid, d->mapbase, d->mapsize);

    if(off > 0) {
      LOG("%s (pid %d): ELF trovato a mapbase+0x%lx", name, (int)pid, off);
      base = d->mapbase + off;
    }
  }

  if(!base) {
    /* Niente da fare: mostriamo cosa c'e' davvero, invece di tirare a indovinare. */
    LOG("%s (pid %d) mapbase=0x%lx vaddrbase=0x%lx size=%lu -- nessun ELF",
        name, (int)pid, (unsigned long)d->mapbase,
        (unsigned long)d->vaddrbase, (unsigned long)d->mapsize);
    probe(pid, "mapbase", d->mapbase);
    probe(pid, "vaddrbase", d->vaddrbase);
    probe(pid, "entry", d->entry);
    return;
  }

  if(!dump_elf(pid, base, name, OUTDIR)) {
    if(f->ndone < MAX_LIBS) {
      snprintf(f->done[f->ndone], sizeof(f->done[0]), "%s", name);
      f->ndone++;
    }
    f->dumped++;
  }
}


/* Cammina le dynlib di tutti i processi ed estrae cio' che passa il filtro.
 *
 * Non usiamo kernel_dynlib_handle() qui, e il motivo e' importante: quella
 * funzione ritorna -1 al PRIMO kernel_copyout fallito, e la testa della lista
 * dynlib contiene una voce spazzatura illeggibile. Risultato: abortisce prima
 * di raggiungere le librerie vere e riporta "non trovata" per tutto.
 * Camminando a mano la voce guasta si salta e si continua. */
static int
dump_matching(const char **want)
{
  size_t sz;
  void *buf = list_procs(&sz);
  static dump_filter_t f;

  memset(&f, 0, sizeof(f));
  if(want) {
    for(int i = 0; want[i] && f.nwant < 31; i++) {
      f.want[f.nwant++] = want[i];
    }
  }

  if(!buf) {
    return -1;
  }

  for(void *ptr = buf; ptr < (buf + sz);) {
    struct kinfo_proc *ki = (struct kinfo_proc*)ptr;
    pid_t pid = ki->ki_pid;

    if(ki->ki_structsize <= 0) {
      break;
    }
    ptr += ki->ki_structsize;

    if(pid <= 0) {
      continue;
    }

    walk_dynlibs(pid, cb_dump, &f);
  }

  free(buf);
  LOG("%d librerie estratte in %s", f.dumped, OUTDIR);
  return f.dumped ? 0 : -1;
}


int
main(int argc, char *argv[])
{
  /* Senza argomenti NON e' "list": deve cadere nel dump, che e' il default. */
  const char *mode = (argc > 1) ? argv[1] : "";

  /* stdout non bufferizzato: se il payload si blocca a meta', vogliamo comunque
   * vedere quello che ha gia' stampato. Con il buffer pieno l'output resta
   * invisibile fino all'esecuzione completa, ed e' quello che ci ha ingannato. */
  setvbuf(stdout, NULL, _IONBF, 0);

  LOG("firmware 0x%08x", kernel_get_fw_version());
  mkdir(OUTDIR, 0755);

  /* Diagnostica sempre attiva: se il direct map non e' inizializzato, ogni
   * lettura dal physical address finisce nel posto sbagliato e torna riempimento.
   * Proviamo la traduzione su un indirizzo noto (l'entry point degli executable). */
  LOG("KERNEL_ADDRESS_DMAP_BASE = 0x%012lx",
      (unsigned long)KERNEL_ADDRESS_DMAP_BASE);

  /* Il payload viene INIETTATO da elfldr dentro un processo gia' esistente.
   * Se quel processo ha gia' caricate le librerie video, le loro funzioni sono
   * mappate nel NOSTRO spazio e possiamo chiamarle direttamente: il muro del
   * loader (sceKernelLoadStartModule che si blocca) sparirebbe, perche' non
   * c'e' niente da caricare.
   *
   * Non l'abbiamo mai verificato. E' la cosa piu' importante da sapere. */
  LOG("=== librerie del MIO processo (pid %d) ===", (int)getpid());
  walk_dynlibs(getpid(), cb_print, NULL);

  try_elevate();
  {
    size_t sz;
    void *buf = list_procs(&sz);

    if(buf) {
      for(void *ptr = buf; ptr < (buf + sz);) {
        struct kinfo_proc *ki = (struct kinfo_proc*)ptr;
        pid_t pid = ki->ki_pid;

        if(ki->ki_structsize <= 0) {
          break;
        }
        ptr += ki->ki_structsize;

        if(pid > 0) {
          unsigned long paddr = 0, plen = 0;

          if(!kernel_proc_getpaddr(pid, 0x400000, &paddr, &plen)) {
            unsigned char b[8] = {0};
            int r = kernel_copyout(KERNEL_ADDRESS_DMAP_BASE + paddr,
                                   b, sizeof(b));

            LOG("probe pid %d: 0x400000 -> paddr=0x%012lx plen=0x%lx dmap=0x%012lx"
                " read=%s  %02x %02x %02x %02x",
                (int)pid, paddr, plen,
                (unsigned long)(KERNEL_ADDRESS_DMAP_BASE + paddr),
                r ? "FAIL" : "ok", b[0], b[1], b[2], b[3]);
            break;
          }
        }
      }
      free(buf);
    }
  }

  /* --- modalita' lista: fa parlare la console --- */
  if(!strcmp(mode, "list")) {
    size_t sz;
    void *buf = list_procs(&sz);
    int procs = 0, libs = 0;

    if(!buf) {
      return 1;
    }

    for(void *ptr = buf; ptr < (buf + sz);) {
      struct kinfo_proc *ki = (struct kinfo_proc*)ptr;
      pid_t pid = ki->ki_pid;
      int n;

      if(ki->ki_structsize <= 0) {
        break;
      }
      ptr += ki->ki_structsize;

      if(pid <= 0) {
        continue;
      }

      n = walk_dynlibs(pid, cb_print, NULL);
      if(n > 0) {
        LOG("pid %d: %d librerie", (int)pid, n);
        procs++;
        libs += n;
      }
    }

    free(buf);
    LOG("totale: %d processi con librerie, %d voci", procs, libs);
    if(!libs) {
      LOG("nessuna libreria letta: probabile offset KPROC_DYNLIB_HEAD (0x%x) "
          "sbagliato per questo firmware", KPROC_DYNLIB_HEAD);
    }
    return 0;
  }

  /* --- modalita' dump ---
   * prospero-deploy manda solo l'ELF su TCP, senza argomenti: quindi il dump
   * dev'essere il comportamento di default. "list" resta raggiungibile per chi
   * carica il payload con un loader che passa argv. */
  if(!strcmp(mode, "targets")) {
    return dump_matching(targets) ? 1 : 0;   /* cross-process, come fallback */
  }

  if(argc > 1) {
    return dump_matching((const char**)&argv[1]) ? 1 : 0;
  }

  /* Default: dumpa le librerie.
   *
   * NOTA: la variante "carica la libreria nel proprio processo" (load_and_dump)
   * e' disattivata perche' sceKernelLoadStartModule si BLOCCA quando un payload
   * homebrew prova a caricare una libreria di sistema, e il deploy resta appeso.
   *
   * E NON usiamo dump_matching(): quella cercava l'header ELF a inizio mappatura,
   * che il loader riempie di 0xCC. dump_library_by_name() dumpa la mappatura
   * intera e ci costruisce attorno un ELF valido. */
  {
    int ok = 0;

    for(int i = 0; targets[i]; i++) {
      if(!dump_library_by_name(targets[i], OUTDIR)) {
        ok++;
      }
    }
    LOG("%d librerie estratte in %s", ok, OUTDIR);
    return ok ? 0 : 1;
  }
}
