/*
 * ps5cam.c — webcam virtuale DirectShow "PS5 Screen (avstream)".
 *
 * Filtro sorgente in C puro (niente C++/ATL/baseclasses, niente OBS):
 * un pin di output che consegna i frame NV12 letti dalla memoria
 * condivisa riempita da ps5feed.exe, convertiti nel formato chiesto
 * (NV12 / YUY2 / RGB32 / RGB24).
 *
 * Registrazione PER-UTENTE (HKCU\Software\Classes): niente admin.
 *   ps5cam.dll -> regsvr32 ps5cam.dll          (registra)
 *                 regsvr32 /u ps5cam.dll       (rimuove)
 *
 * Compilato con MinGW:  gcc -O2 -shared -o ps5cam.dll ps5cam.c \
 *                        -lole32 -loleaut32 -luser32 -ladvapi32
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <dshow.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "shm.h"

/* CLSID della nostra camera: unico, generato per questo progetto */
DEFINE_GUID(CLSID_Ps5Cam,
            0x50533543, 0x414d, 0x4def, 0x9b, 0x0e,
            0x19, 0x20, 0x80, 0x10, 0x20, 0x30);

/* sottotipi usati (valori standard; costanti nostre per non dipendere
 * dalle uuids.h del compilatore) */
DEFINE_GUID(SUBTYPE_NV12, 0x3231564E, 0x0000, 0x0010, 0x80, 0x00,
            0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(SUBTYPE_YUY2, 0x32595559, 0x0000, 0x0010, 0x80, 0x00,
            0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71);
DEFINE_GUID(SUBTYPE_RGB32, 0xe436eb7e, 0x524f, 0x11ce, 0x9f, 0x53,
            0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);
DEFINE_GUID(SUBTYPE_I420, 0x30323449, 0x0000, 0x0010, 0x80, 0x00,
            0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71);
DEFINE_GUID(SUBTYPE_RGB24, 0xe436eb7d, 0x524f, 0x11ce, 0x9f, 0x53,
            0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70);

DEFINE_GUID(PS5_IID_IKsPropertySet, 0x31EFAC30, 0x515C, 0x11D0, 0xa9, 0xaa,
            0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93);
DEFINE_GUID(PS5_AMPROPSETID_Pin, 0x9b00f101, 0x1567, 0x11d1, 0xb3, 0xf1,
            0x00, 0xaa, 0x00, 0x37, 0x61, 0xc5);
DEFINE_GUID(PS5_PIN_CATEGORY_CAPTURE, 0xfb6c4281, 0x0353, 0x11d1, 0x90, 0x5f,
            0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba);

static HINSTANCE g_hinst_ps5cam;
#include <stdarg.h>

static void
trace(const char *fmt, ...)
{
  static int prima = 1;
  FILE *f = fopen("pscapture_trace.txt", "a");

  if(f) {
    va_list ap;

    if(prima) {
      prima = 0;
      fprintf(f, "=== BUILD %s %s (pid %lu) ===\n", __DATE__, __TIME__,
              (unsigned long)GetCurrentProcessId());
    }
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
  }
}


#define PS5CAM_FPS 30
#define PS5CAM_NAME L"PS5 Screen (avstream)"

/* ------------------------------------------------------------------ */
/* helper COM                                                          */
/* ------------------------------------------------------------------ */

#define container_of(ptr, type, member) \
  ((type *)((char *)(ptr) - offsetof(type, member)))

static void
copia_nome(LPWSTR out, UINT cc, LPCWSTR in)
{
  if(!out) return;
  lstrcpynW(out, in, cc);
}

static AM_MEDIA_TYPE *
media_type_dup(const AM_MEDIA_TYPE *m)
{
  AM_MEDIA_TYPE *c = CoTaskMemAlloc(sizeof(*c));

  if(!c) return NULL;
  *c = *m;
  if(m->cbFormat) {
    c->pbFormat = CoTaskMemAlloc(m->cbFormat);
    if(!c->pbFormat) { CoTaskMemFree(c); return NULL; }
    memcpy(c->pbFormat, m->pbFormat, m->cbFormat);
  } else {
    c->pbFormat = NULL;
  }
  return c;
}

static void
media_type_free(AM_MEDIA_TYPE *m)
{
  if(!m) return;
  if(m->pbFormat) CoTaskMemFree(m->pbFormat);
  if(m->pUnk) m->pUnk->lpVtbl->Release(m->pUnk);
  CoTaskMemFree(m);
}

static void
mt_1080x(AM_MEDIA_TYPE *m, const GUID *sub, const GUID *fmt, int pulito, int pos)
{
  VIDEOINFOHEADER *v;

  memset(m, 0, sizeof(*m));
  m->majortype = MEDIATYPE_Video;
  m->subtype = *sub;
  m->bFixedSizeSamples = TRUE;
  m->bTemporalCompression = FALSE;
  m->formattype = *fmt;
  if(IsEqualGUID(fmt, &FORMAT_VideoInfo)) {
    v = CoTaskMemAlloc(sizeof(*v));
    memset(v, 0, sizeof(*v));
    /* "pulito" = VIDEOINFOHEADER con gli extra a zero, come lo costruisce
     * il SampleGrabber (qedit): solo cosi' il suo ReceiveConnection lo
     * accetta (e' il percorso della chiamata vocale Discord/Chromium,
     * dove l'errore 2014 nasce da 0x8004022a = tipo non accettato). */
    if(!pulito) {
      v->AvgTimePerFrame = 10000000LL / PS5CAM_FPS;
      v->rcSource.right = PS5CAM_W;
      v->rcSource.bottom = PS5CAM_H;
      v->rcTarget = v->rcSource;
    }
    v->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    v->bmiHeader.biWidth = PS5CAM_W;
    v->bmiHeader.biHeight = pos ? (LONG)PS5CAM_H : -(LONG)PS5CAM_H;
    v->bmiHeader.biPlanes = 1;
    if(IsEqualGUID(sub, &SUBTYPE_NV12)) {
      v->bmiHeader.biBitCount = 12;
      v->bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
      if(!pulito) { v->bmiHeader.biSizeImage = PS5CAM_FRAME; m->lSampleSize = PS5CAM_FRAME; }
    } else if(IsEqualGUID(sub, &SUBTYPE_YUY2)) {
      v->bmiHeader.biBitCount = 16;
      v->bmiHeader.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
      if(!pulito) { v->bmiHeader.biSizeImage = PS5CAM_W * PS5CAM_H * 2; m->lSampleSize = v->bmiHeader.biSizeImage; }
    } else if(IsEqualGUID(sub, &SUBTYPE_RGB32)) {
      v->bmiHeader.biBitCount = 32;
      v->bmiHeader.biCompression = BI_RGB;
      if(!pulito) { v->bmiHeader.biSizeImage = PS5CAM_W * PS5CAM_H * 4; m->lSampleSize = v->bmiHeader.biSizeImage; }
    } else {  /* RGB24 */
      v->bmiHeader.biBitCount = 24;
      v->bmiHeader.biCompression = BI_RGB;
      if(!pulito) { v->bmiHeader.biSizeImage = PS5CAM_W * PS5CAM_H * 3; m->lSampleSize = v->bmiHeader.biSizeImage; }
    }
    m->cbFormat = sizeof(*v);
    m->pbFormat = (BYTE *)v;
  } else {
    m->cbFormat = 0;
    m->pbFormat = NULL;
    m->lSampleSize = 0;
  }
}

static void
mt_1080(AM_MEDIA_TYPE *m, const GUID *sub, const GUID *fmt)
{
  mt_1080x(m, sub, fmt, 0, 0);
}

/* media type generico (dimensioni arbitrarie): il "profilo webcam vera".
 * pos=1 -> biHeight positivo (bottom-up), pos=0 -> negativo (top-down). */
static void
mt_gen(AM_MEDIA_TYPE *m, const GUID *sub, int w, int h, int pulito, int pos)
{
  VIDEOINFOHEADER *v;
  DWORD sz;

  memset(m, 0, sizeof(*m));
  m->majortype = MEDIATYPE_Video;
  m->subtype = *sub;
  m->bFixedSizeSamples = TRUE;
  m->bTemporalCompression = FALSE;
  m->formattype = FORMAT_VideoInfo;
  v = CoTaskMemAlloc(sizeof(*v));
  if(!v) { m->cbFormat = 0; m->pbFormat = NULL; return; }
  memset(v, 0, sizeof(*v));
  if(!pulito) {
    v->AvgTimePerFrame = 10000000LL / PS5CAM_FPS;
    v->rcSource.right = w;
    v->rcSource.bottom = h;
    v->rcTarget = v->rcSource;
  }
  v->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  v->bmiHeader.biWidth = w;
  v->bmiHeader.biHeight = pos ? h : -h;
  v->bmiHeader.biPlanes = 1;
  if(IsEqualGUID(sub, &SUBTYPE_NV12)) {
    v->bmiHeader.biBitCount = 12;
    v->bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
    sz = (DWORD)w * h * 3 / 2;
  } else if(IsEqualGUID(sub, &SUBTYPE_YUY2)) {
    v->bmiHeader.biBitCount = 16;
    v->bmiHeader.biCompression = MAKEFOURCC('Y', 'U', 'Y', '2');
    sz = (DWORD)w * h * 2;
  } else if(IsEqualGUID(sub, &SUBTYPE_I420)) {
    v->bmiHeader.biBitCount = 12;
    v->bmiHeader.biCompression = MAKEFOURCC('I', '4', '2', '0');
    sz = (DWORD)w * h * 3 / 2;
  } else if(IsEqualGUID(sub, &SUBTYPE_RGB32)) {
    v->bmiHeader.biBitCount = 32;
    v->bmiHeader.biCompression = BI_RGB;
    sz = (DWORD)w * h * 4;
  } else {
    v->bmiHeader.biBitCount = 24;
    v->bmiHeader.biCompression = BI_RGB;
    sz = (DWORD)w * h * 3;
  }
  if(!pulito) {
    v->bmiHeader.biSizeImage = sz;
    m->lSampleSize = sz;
  }
  m->cbFormat = sizeof(*v);
  m->pbFormat = (BYTE *)v;
}

/* Il "profilo webcam vera": capability come una UVC (piu' formati e
 * dimensioni) cosi' il picker del motore voce di Discord trova cio' che
 * chiede (es. 1280x720@30). Quelle virtuali povere (solo 1080p) vengono
 * scartate con "Failed to create capturer". */
typedef struct CapDef {
  const GUID *sub;
  int w, h, pos;
} CapDef;

static const CapDef g_caps[] = {
  /* NB: le varianti NEGATIVE (top-down, flip=0) PRIMA: i consumer che
   * scansionano per (formato,dimensione) prendono queste e l'immagine
   * e' dritta senza dipendere dalla convenzione DIB (ffmpeg docet). */
  { &SUBTYPE_YUY2,  1920, 1080, 0 },   /* [0] default storico */
  { &SUBTYPE_YUY2,  1280,  720, 0 },
  { &SUBTYPE_YUY2,   640,  480, 0 },
  { &SUBTYPE_YUY2,  1920, 1080, 1 },   /* fallback bottom-up per chi lo chiede */
  { &SUBTYPE_YUY2,  1280,  720, 1 },
  { &SUBTYPE_NV12,  1920, 1080, 0 },
  { &SUBTYPE_NV12,  1280,  720, 0 },
  { &SUBTYPE_I420,  1920, 1080, 0 },
  { &SUBTYPE_I420,  1280,  720, 0 },
  { &SUBTYPE_RGB24, 1920, 1080, 0 },
  { &SUBTYPE_RGB24, 1280,  720, 0 },
  { &SUBTYPE_RGB32, 1920, 1080, 0 },
};
#define N_CAPS ((int)(sizeof(g_caps) / sizeof(g_caps[0])))

/* ------------------------------------------------------------------ */
/* oggetti                                                             */
/* ------------------------------------------------------------------ */

typedef struct Ps5Filter Ps5Filter;
typedef struct Ps5Pin Ps5Pin;

/* --- allocatore + campioni --- */

typedef struct Ps5Sample {
  const IMediaSampleVtbl *lpVtbl;
  LONG ref;
  BYTE *buf;
  LONG size;
  LONG actuallen;
  REFERENCE_TIME t_start, t_stop;
  BOOL sync, preroll, disco;
} Ps5Sample;

typedef struct Ps5Alloc {
  const IMemAllocatorVtbl *lpVtbl;
  LONG ref;
  ALLOCATOR_PROPERTIES props;
  Ps5Sample samples[4];
  BOOL committed;
} Ps5Alloc;

/* --- pin --- */

typedef struct Ps5Pin {
  /* sotto-oggetti interfaccia (il primo deve essere IPin) */
  struct {
    const IPinVtbl *lpVtbl;
    Ps5Pin *self;
  } pin;
  struct {
    const IAMStreamConfigVtbl *lpVtbl;
    Ps5Pin *self;
  } amsc;
  struct {
    const IMemInputPinVtbl *lpVtbl;
    Ps5Pin *self;
  } mip;
  struct {
    const IKsPropertySetVtbl *lpVtbl;
    Ps5Pin *self;
  } kpset;

  LONG ref;
  Ps5Filter *filt;
  BOOL connesso;
  PIN_DIRECTION dir;
  AM_MEDIA_TYPE mt;              /* formato negoziato */
  DWORD dim_frame;               /* byte del formato negoziato */
  int formato;                   /* 0 NV12 1 YUY2 2 RGB32 3 RGB24 4 I420 */
  int w, h;                      /* dimensioni negoziate */
  int flip;                      /* 1 = consumer vuole altezza positiva */
  IPin *peer;                    /* pin a valle */
  IMemInputPin *peer_in;
  Ps5Alloc *alloc;
  IMemAllocator *alloc_loro;     /* allocatore notificato dal consumer */
} Ps5Pin;

/* --- filtro --- */

struct Ps5Filter {
  const IBaseFilterVtbl *lpVtbl;
  LONG ref;
  Ps5Pin *pin;
  FILTER_STATE stato;
  IReferenceClock *orologio;
  IFilterGraph *graph;
  HANDLE thread;
  HANDLE ev_stop;                /* segnale di stop */
  volatile LONG vivi;            /* thread attivo */
  unsigned long long seq_visto;
  HANDLE shm_map;
  ps5cam_shm_t *shm;
};


/* ------------------------------------------------------------------ */
/* campione (IMediaSample)                                             */
/* ------------------------------------------------------------------ */

static HRESULT WINAPI samp_QI(IMediaSample *me, REFIID iid, void **out);
static ULONG WINAPI samp_AddRef(IMediaSample *me);
static ULONG WINAPI samp_Release(IMediaSample *me);
static HRESULT WINAPI samp_GetPointer(IMediaSample *me, BYTE **p);
static LONG WINAPI samp_GetSize(IMediaSample *me);
static HRESULT WINAPI samp_GetTime(IMediaSample *me, REFERENCE_TIME *a,
                                   REFERENCE_TIME *b);
static HRESULT WINAPI samp_SetTime(IMediaSample *me, REFERENCE_TIME *a,
                                   REFERENCE_TIME *b);
static HRESULT WINAPI samp_IsSyncPoint(IMediaSample *me);
static HRESULT WINAPI samp_SetSyncPoint(IMediaSample *me, BOOL v);
static HRESULT WINAPI samp_IsPreroll(IMediaSample *me);
static HRESULT WINAPI samp_SetPreroll(IMediaSample *me, BOOL v);
static LONG WINAPI samp_GetActual(IMediaSample *me);
static HRESULT WINAPI samp_SetActual(IMediaSample *me, LONG v);
static HRESULT WINAPI samp_GetMediaType(IMediaSample *me, AM_MEDIA_TYPE **p);
static HRESULT WINAPI samp_SetMediaType(IMediaSample *me, AM_MEDIA_TYPE *m);
static HRESULT WINAPI samp_IsDisco(IMediaSample *me);
static HRESULT WINAPI samp_SetDisco(IMediaSample *me, BOOL v);
static HRESULT WINAPI samp_GetMediaTime(IMediaSample *me,
                                        REFERENCE_TIME *a, REFERENCE_TIME *b);
static HRESULT WINAPI samp_SetMediaTime(IMediaSample *me, REFERENCE_TIME *a,
                                        REFERENCE_TIME *b);

static const IMediaSampleVtbl samp_vt = {
  samp_QI, samp_AddRef, samp_Release,
  samp_GetPointer, samp_GetSize, samp_GetTime, samp_SetTime,
  samp_IsSyncPoint, samp_SetSyncPoint, samp_IsPreroll, samp_SetPreroll,
  samp_GetActual, samp_SetActual, samp_GetMediaType, samp_SetMediaType,
  samp_IsDisco, samp_SetDisco, samp_GetMediaTime, samp_SetMediaTime
};

static HRESULT WINAPI
samp_QI(IMediaSample *me, REFIID iid, void **out)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMediaSample)) {
    *out = me;
    samp_AddRef(me);
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
samp_AddRef(IMediaSample *me)
{
  return InterlockedIncrement(&((Ps5Sample *)me)->ref);
}

static ULONG WINAPI
samp_Release(IMediaSample *me)
{
  return InterlockedDecrement(&((Ps5Sample *)me)->ref);
}

static HRESULT WINAPI
samp_GetPointer(IMediaSample *me, BYTE **p)
{
  *p = ((Ps5Sample *)me)->buf;
  return NOERROR;
}

static LONG WINAPI
samp_GetSize(IMediaSample *me)
{
  return ((Ps5Sample *)me)->size;
}

static HRESULT WINAPI
samp_GetTime(IMediaSample *me, REFERENCE_TIME *a, REFERENCE_TIME *b)
{
  Ps5Sample *s = (Ps5Sample *)me;

  if(a) *a = s->t_start;
  if(b) *b = s->t_stop;
  return NOERROR;
}

static HRESULT WINAPI
samp_SetTime(IMediaSample *me, REFERENCE_TIME *a, REFERENCE_TIME *b)
{
  Ps5Sample *s = (Ps5Sample *)me;

  if(a) s->t_start = *a;
  if(b) s->t_stop = *b;
  return NOERROR;
}

static HRESULT WINAPI
samp_IsSyncPoint(IMediaSample *me)
{
  return ((Ps5Sample *)me)->sync ? S_OK : S_FALSE;
}

static HRESULT WINAPI
samp_SetSyncPoint(IMediaSample *me, BOOL v)
{
  ((Ps5Sample *)me)->sync = v;
  return NOERROR;
}

static HRESULT WINAPI
samp_IsPreroll(IMediaSample *me)
{
  return ((Ps5Sample *)me)->preroll ? S_OK : S_FALSE;
}

static HRESULT WINAPI
samp_SetPreroll(IMediaSample *me, BOOL v)
{
  ((Ps5Sample *)me)->preroll = v;
  return NOERROR;
}

static LONG WINAPI
samp_GetActual(IMediaSample *me)
{
  return ((Ps5Sample *)me)->actuallen;
}

static HRESULT WINAPI
samp_SetActual(IMediaSample *me, LONG v)
{
  ((Ps5Sample *)me)->actuallen = v;
  return NOERROR;
}

static HRESULT WINAPI
samp_GetMediaType(IMediaSample *me, AM_MEDIA_TYPE **p)
{
  (void)me;
  *p = NULL;
  return S_FALSE;
}

static HRESULT WINAPI
samp_SetMediaType(IMediaSample *me, AM_MEDIA_TYPE *m)
{
  (void)me; (void)m;
  return NOERROR;
}

static HRESULT WINAPI
samp_IsDisco(IMediaSample *me)
{
  return ((Ps5Sample *)me)->disco ? S_OK : S_FALSE;
}

static HRESULT WINAPI
samp_SetDisco(IMediaSample *me, BOOL v)
{
  ((Ps5Sample *)me)->disco = v;
  return NOERROR;
}

static HRESULT WINAPI
samp_GetMediaTime(IMediaSample *me, REFERENCE_TIME *a, REFERENCE_TIME *b)
{
  (void)me; (void)a; (void)b;
  return VFW_E_MEDIA_TIME_NOT_SET;
}

static HRESULT WINAPI
samp_SetMediaTime(IMediaSample *me, REFERENCE_TIME *a, REFERENCE_TIME *b)
{
  (void)me; (void)a; (void)b;
  return NOERROR;
}

/* ------------------------------------------------------------------ */
/* allocatore (IMemAllocator)                                          */
/* ------------------------------------------------------------------ */

static HRESULT WINAPI alloc_QI(IMemAllocator *me, REFIID iid, void **out);
static ULONG WINAPI alloc_AddRef(IMemAllocator *me);
static ULONG WINAPI alloc_Release(IMemAllocator *me);
static HRESULT WINAPI alloc_SetProps(IMemAllocator *me,
                                     ALLOCATOR_PROPERTIES *req,
                                     ALLOCATOR_PROPERTIES *act);
static HRESULT WINAPI alloc_GetProps(IMemAllocator *me,
                                     ALLOCATOR_PROPERTIES *p);
static HRESULT WINAPI alloc_Commit(IMemAllocator *me);
static HRESULT WINAPI alloc_Decommit(IMemAllocator *me);
static HRESULT WINAPI alloc_GetBuffer(IMemAllocator *me, IMediaSample **s,
                                      REFERENCE_TIME *a, REFERENCE_TIME *b,
                                      DWORD flags);
static HRESULT WINAPI alloc_ReleaseBuffer(IMemAllocator *me, IMediaSample *s);

static const IMemAllocatorVtbl alloc_vt = {
  alloc_QI, alloc_AddRef, alloc_Release,
  alloc_SetProps, alloc_GetProps, alloc_Commit, alloc_Decommit,
  alloc_GetBuffer, alloc_ReleaseBuffer
};

static HRESULT WINAPI
alloc_QI(IMemAllocator *me, REFIID iid, void **out)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMemAllocator)) {
    *out = me;
    alloc_AddRef(me);
    return S_OK;
  }
  *out = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
alloc_AddRef(IMemAllocator *me)
{
  return InterlockedIncrement(&((Ps5Alloc *)me)->ref);
}

static ULONG WINAPI
alloc_Release(IMemAllocator *me)
{
  Ps5Alloc *a = (Ps5Alloc *)me;
  LONG r = InterlockedDecrement(&a->ref);

  if(r == 0) {
    int i;

    for(i = 0; i < 4; i++)
      if(a->samples[i].buf) VirtualFree(a->samples[i].buf, 0, MEM_RELEASE);
    CoTaskMemFree(a);
  }
  return r;
}

static HRESULT WINAPI
alloc_SetProps(IMemAllocator *me, ALLOCATOR_PROPERTIES *req,
               ALLOCATOR_PROPERTIES *act)
{
  Ps5Alloc *a = (Ps5Alloc *)me;
  int i;
  LONG size = req->cbBuffer > 0 ? req->cbBuffer : (LONG)PS5CAM_FRAME;
  int count = req->cBuffers > 0 ? (int)req->cBuffers : 1;

  if(count > 4) count = 4;
  if(a->committed) return VFW_E_ALREADY_COMMITTED;
  for(i = 0; i < 4; i++) {
    a->samples[i].lpVtbl = &samp_vt;
    a->samples[i].ref = 1;
    a->samples[i].size = size;
  }
  for(i = 0; i < count; i++) {
    if(!a->samples[i].buf)
      a->samples[i].buf = VirtualAlloc(NULL, size, MEM_COMMIT,
                                        PAGE_READWRITE);
    if(!a->samples[i].buf) return E_OUTOFMEMORY;
  }
  a->props.cbBuffer = size;
  a->props.cBuffers = count;
  a->props.cbAlign = 1;
  a->props.cbPrefix = 0;
  if(act) *act = a->props;
  return NOERROR;
}

static HRESULT WINAPI
alloc_GetProps(IMemAllocator *me, ALLOCATOR_PROPERTIES *p)
{
  *p = ((Ps5Alloc *)me)->props;
  return NOERROR;
}

static HRESULT WINAPI
alloc_Commit(IMemAllocator *me)
{
  ((Ps5Alloc *)me)->committed = TRUE;
  return NOERROR;
}

static HRESULT WINAPI
alloc_Decommit(IMemAllocator *me)
{
  ((Ps5Alloc *)me)->committed = FALSE;
  return NOERROR;
}

static HRESULT WINAPI
alloc_GetBuffer(IMemAllocator *me, IMediaSample **s, REFERENCE_TIME *a,
                REFERENCE_TIME *b, DWORD flags)
{
  Ps5Alloc *al = (Ps5Alloc *)me;
  int i;

  (void)flags;
  for(i = 0; i < al->props.cBuffers; i++) {
    if(al->samples[i].ref == 1) {          /* libero */
      Ps5Sample *c = &al->samples[i];

      c->actuallen = 0;
      c->sync = TRUE;
      c->preroll = FALSE;
      c->disco = FALSE;
      if(a) c->t_start = *a;
      if(b) c->t_stop = *b;
      c->ref = 2;
      *s = (IMediaSample *)c;
      return NOERROR;
    }
  }
  return VFW_E_NOT_COMMITTED;              /* nessun buffer libero */
}

static HRESULT WINAPI
alloc_ReleaseBuffer(IMemAllocator *me, IMediaSample *s)
{
  (void)me;
  ((Ps5Sample *)s)->ref = 1;
  return NOERROR;
}

/* ------------------------------------------------------------------ */
/* conversione NV12 -> vari                                            */
/* ------------------------------------------------------------------ */

static int
clip255(int v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : v);
}

/* NV12 (1920x1080) -> RGB bottom-up, bpp 3 o 4 */
static void
nv12_to_rgb(BYTE *dst, const BYTE *y, const BYTE *uv, int bpp, int flip)
{
  int r, c;
  int stride = PS5CAM_W * bpp;
  int lut_y[256], lut_u_r[256], lut_u_g[256], lut_v_g[256], lut_v_r[256];

  for(c = 0; c < 256; c++) {
    lut_y[c] = (298 * (c - 16) + 128) >> 8;
    lut_u_r[c] = (409 * (c - 128) + 128) >> 8;
    lut_u_g[c] = (-100 * (c - 128) + 128) >> 8;
    lut_v_g[c] = (-208 * (c - 128) + 128) >> 8;
    lut_v_r[c] = (516 * (c - 128) + 128) >> 8;
  }
  for(r = 0; r < PS5CAM_H; r++) {
    BYTE *o = dst + (size_t)(flip ? PS5CAM_H - 1 - r : r) * stride;
    const BYTE *ly = y + (size_t)r * PS5CAM_W;
    const BYTE *luv = uv + (size_t)(r >> 1) * PS5CAM_W;

    for(c = 0; c < PS5CAM_W; c++) {
      int Y = lut_y[ly[c]];
      /* NB: NV12 del payload = Cb per primo (byte PARI), Cr secondo (DISP).
       * La via YUY2 (che Discord usa) mette il pari nello slot Cb: giusto.
       * Qui in RGB il canale R va col Cr = byte DISPARI (prima era
       * invertito: bug latente mai visto perche' Discord negozia YUY2). */
      int U = luv[((c >> 1) << 1) + 1];
      int V = luv[(c >> 1) << 1];

      if(bpp == 4) {
        o[0] = (BYTE)clip255(Y + lut_u_r[U]);
        o[1] = (BYTE)clip255(Y + lut_u_g[U] + lut_v_g[V]);
        o[2] = (BYTE)clip255(Y + lut_v_r[V]);
        o[3] = 255;
      } else {
        o[0] = (BYTE)clip255(Y + lut_u_r[U]);
        o[1] = (BYTE)clip255(Y + lut_u_g[U] + lut_v_g[V]);
        o[2] = (BYTE)clip255(Y + lut_v_r[V]);
      }
      o += bpp;
    }
  }
}

/* NV12 -> YUY2 */
static void
nv12_to_yuy2(BYTE *dst, const BYTE *y, const BYTE *uv, int flip)
{
  int r, c;
  int stride = PS5CAM_W * 2;

  for(r = 0; r < PS5CAM_H; r++) {
    BYTE *o = dst + (size_t)(flip ? PS5CAM_H - 1 - r : r) * stride;
    const BYTE *ly = y + (size_t)r * PS5CAM_W;
    const BYTE *luv = uv + (size_t)(r >> 1) * PS5CAM_W;

    for(c = 0; c < PS5CAM_W; c += 2) {
      o[0] = ly[c];
      o[1] = luv[(c >> 1) << 1];
      o[2] = ly[c + 1];
      o[3] = luv[((c >> 1) << 1) + 1];
      o += 4;
    }
  }
}

/* pattern "NESSUN SEGNALE": 8 barre verticali classiche, nel formato
 * chiesto. Sostituisce il nero quando il feed e' assente: il nero puro
 * faceva restare Discord in "caricamento" per sempre. */
static const unsigned char g_bar_y[8] = { 180, 168, 145, 122, 90, 54, 26, 16 };
static const unsigned char g_bar_u[8] = { 128, 44, 147, 80, 184, 100, 210, 128 };
static const unsigned char g_bar_v[8] = { 128, 136, 44, 60, 190, 210, 110, 128 };
static int g_barre_on = -1;

static void
riempi_barre(Ps5Pin *p, BYTE *dst)
{
  DWORD x, y;
  DWORD w = (DWORD)p->w, h = (DWORD)p->h;

  switch(p->formato) {
  case 0:                                 /* NV12 */
    for(y = 0; y < h; y++) {
      BYTE *o = dst + (size_t)y * w;
      for(x = 0; x < w; x++)
        o[x] = g_bar_y[(x * 8) / w];
    }
    for(y = 0; y < h / 2; y++) {
      BYTE *o = dst + (size_t)w * h + (size_t)y * w;
      for(x = 0; x < w; x += 2) {
        DWORD b = (x * 8) / w;
        o[x] = g_bar_u[b];
        o[x + 1] = g_bar_v[b];
      }
    }
    break;
  case 4: {                               /* I420: piani U e V separati */
    BYTE *pu = dst + (size_t)w * h;
    BYTE *pv = pu + (size_t)(w / 2) * (h / 2);

    for(y = 0; y < h; y++) {
      BYTE *o = dst + (size_t)y * w;
      for(x = 0; x < w; x++)
        o[x] = g_bar_y[(x * 8) / w];
    }
    for(y = 0; y < h / 2; y++) {
      for(x = 0; x < w / 2; x++) {
        DWORD b = (x * 16) / w;
        pu[(size_t)y * (w / 2) + x] = g_bar_u[b];
        pv[(size_t)y * (w / 2) + x] = g_bar_v[b];
      }
    }
    break;
  }
  case 1:                                 /* YUY2 */
    for(y = 0; y < h; y++) {
      BYTE *o = dst + (size_t)y * w * 2;
      for(x = 0; x < w; x += 2) {
        DWORD b = (x * 8) / w;
        o[0] = g_bar_y[b];
        o[1] = g_bar_u[b];
        o[2] = g_bar_y[((x + 1) * 8) / w];
        o[3] = g_bar_v[b];
        o += 4;
      }
    }
    break;
  default: {                              /* RGB24/RGB32 */
    static const BYTE rgb[8][3] = {
      {180, 180, 180}, {180, 180, 0}, {0, 180, 180}, {0, 180, 0},
      {180, 0, 180}, {180, 0, 0}, {0, 0, 180}, {8, 8, 8}
    };
    DWORD bpp = (p->formato == 2) ? 4 : 3;
    for(y = 0; y < h; y++) {
      BYTE *o = dst + (size_t)y * w * bpp;
      for(x = 0; x < w; x++) {
        const BYTE *c = rgb[(x * 8) / w];
        o[0] = c[0]; o[1] = c[1]; o[2] = c[2];
        if(bpp == 4) o[3] = 255;
        o += bpp;
      }
    }
    break;
  }
  }
}

/* copia il frame corrente dalla shm nel campione, nel formato giusto */

static int formato_da_subtype(const GUID *s);

/* scala nearest-neighbor: indice sorgente per l'indice destinazione d */
static int
sca(int d, int s, int ds)
{
  int r = (int)((long long)d * s / ds);

  return r >= s ? s - 1 : r;
}

/* YUY2 di dimensioni arbitrarie (sorgente NV12 1920x1080) */
static void
yu2_gen(BYTE *dst, const BYTE *y, const BYTE *uv, int w, int h, int flip)
{
  int r, c;

  for(r = 0; r < h; r++) {
    BYTE *o = dst + (size_t)(flip ? h - 1 - r : r) * (size_t)w * 2;
    int sy = sca(r, PS5CAM_H, h);
    int su = sca(r >> 1, PS5CAM_H / 2, h >> 1);
    const BYTE *ly = y + (size_t)sy * PS5CAM_W;
    const BYTE *luv = uv + (size_t)su * PS5CAM_W;

    for(c = 0; c < w; c += 2) {
      int xu = sca(c >> 1, PS5CAM_W / 2, w >> 1);

      o[c * 2 + 0] = ly[sca(c, PS5CAM_W, w)];
      o[c * 2 + 1] = luv[xu * 2];
      o[c * 2 + 2] = ly[sca(c + 1, PS5CAM_W, w)];
      o[c * 2 + 3] = luv[xu * 2 + 1];
    }
  }
}

/* NV12 (is_i420=0) o I420 (is_i420=1) di dimensioni arbitrarie */
static void
nvplanes_gen(BYTE *dst, const BYTE *y, const BYTE *uv, int w, int h, int flip,
             int is_i420)
{
  int r, c;
  BYTE *d2 = dst + (size_t)w * h;

  for(r = 0; r < h; r++) {
    BYTE *o = dst + (size_t)(flip ? h - 1 - r : r) * (size_t)w;
    int sy = sca(r, PS5CAM_H, h);

    if(w == PS5CAM_W) memcpy(o, y + (size_t)sy * PS5CAM_W, (size_t)w);
    else for(c = 0; c < w; c++) o[c] = y[(size_t)sy * PS5CAM_W + sca(c, PS5CAM_W, w)];
  }
  for(r = 0; r < h / 2; r++) {
    int su = sca(r, PS5CAM_H / 2, h / 2);
    const BYTE *srow = uv + (size_t)su * PS5CAM_W;

    if(!is_i420) {
      BYTE *o = d2 + (size_t)r * w;

      if(w == PS5CAM_W) memcpy(o, srow, (size_t)w);
      else for(c = 0; c < w; c += 2) {
        int xu = sca(c >> 1, PS5CAM_W / 2, w >> 1);

        o[c] = srow[xu * 2];
        o[c + 1] = srow[xu * 2 + 1];
      }
    } else {
      BYTE *pu = d2 + (size_t)r * (w / 2);
      BYTE *pv = d2 + (size_t)(w / 2) * (h / 2) + (size_t)r * (w / 2);

      for(c = 0; c < w / 2; c++) {
        int xu = sca(c, PS5CAM_W / 2, w / 2);

        pu[c] = srow[xu * 2];
        pv[c] = srow[xu * 2 + 1];
      }
    }
  }
}

/* RGB24/32 di dimensioni arbitrarie. NB: come nv12_to_rgb, R prende il
 * byte DISPARI (Cr), B il pari (Cb). */
static void
rgb_gen(BYTE *dst, const BYTE *y, const BYTE *uv, int w, int h, int bpp,
        int flip)
{
  int r, c;
  int lut_y[256], lut_u_r[256], lut_u_g[256], lut_v_g[256], lut_v_r[256];
  int stride = w * bpp;

  for(c = 0; c < 256; c++) {
    lut_y[c] = (298 * (c - 16) + 128) >> 8;
    lut_u_r[c] = (409 * (c - 128) + 128) >> 8;
    lut_u_g[c] = (-100 * (c - 128) + 128) >> 8;
    lut_v_g[c] = (-208 * (c - 128) + 128) >> 8;
    lut_v_r[c] = (516 * (c - 128) + 128) >> 8;
  }
  for(r = 0; r < h; r++) {
    BYTE *o = dst + (size_t)(flip ? h - 1 - r : r) * stride;
    int sy = sca(r, PS5CAM_H, h);
    int su = sca(r >> 1, PS5CAM_H / 2, h >> 1);
    const BYTE *ly = y + (size_t)sy * PS5CAM_W;
    const BYTE *luv = uv + (size_t)su * PS5CAM_W;

    for(c = 0; c < w; c++) {
      int Y = lut_y[ly[sca(c, PS5CAM_W, w)]];
      int xu = sca(c >> 1, PS5CAM_W / 2, w >> 1);
      int cr = luv[xu * 2 + 1];
      int cb = luv[xu * 2];

      o[0] = (BYTE)clip255(Y + lut_u_r[cr]);
      o[1] = (BYTE)clip255(Y + lut_u_g[cr] + lut_v_g[cb]);
      o[2] = (BYTE)clip255(Y + lut_v_r[cb]);
      if(bpp == 4) o[3] = 255;
      o += bpp;
    }
  }
}

/* da un media type negoziato ricava formato/dimensioni/flip/dim_frame */
static void
estrai_dime(const AM_MEDIA_TYPE *mt, Ps5Pin *p)
{
  DWORD npx;

  p->w = PS5CAM_W;
  p->h = PS5CAM_H;
  p->flip = 0;
  p->formato = formato_da_subtype(&mt->subtype);
  if(p->formato < 0) p->formato = 0;
  if(mt->pbFormat && IsEqualGUID(&mt->formattype, &FORMAT_VideoInfo)) {
    VIDEOINFOHEADER *vh = (VIDEOINFOHEADER *)mt->pbFormat;
    int ww = (int)vh->bmiHeader.biWidth;
    int hh = (int)vh->bmiHeader.biHeight;

    /* flip solo per RGB (convenzione DIB solida e Blink la rispetta:
     * flip_y_ per RGB32). Per i formati YUV la prassi reale (ffmpeg,
     * sink webrtc) e' top-down a prescindere dal segno. */
    if(hh > 0 && (p->formato == 2 || p->formato == 3)) p->flip = 1;
    if(hh < 0) hh = -hh;
    if(ww >= 16 && ww <= 4096 && hh >= 16 && hh <= 4096 &&
       !(ww & 1) && !(hh & 1)) {
      p->w = ww;
      p->h = hh;
    }
  }
  npx = (DWORD)p->w * (DWORD)p->h;
  p->dim_frame = (p->formato == 0 || p->formato == 4) ? npx * 3 / 2 :
                 p->formato == 1 ? npx * 2 :
                 p->formato == 2 ? npx * 4 : npx * 3;
}

static void
riempi_campione(Ps5Filter *f, Ps5Pin *p, BYTE *dst, DWORD dim)
{
  const BYTE *src = NULL;
  DWORD stale = 0;

  if(f->shm && f->shm->magic == PS5CAM_MAGIC) {
    src = f->shm->buf[f->shm->cur];
    stale = (DWORD)(GetTickCount() - (DWORD)f->shm->tick);
  }
  if(!src || stale > 3000) {
    /* niente feed (o fermo): barre "nessun segnale" (vedi sopra) */
    if(g_barre_on != 1) {
      trace("feed assente: barre (stale %u ms)", stale);
      g_barre_on = 1;
    }
    riempi_barre(p, dst);
    return;
  }
  if(g_barre_on != 0) {
    trace("feed tornato");
    g_barre_on = 0;
  }

  switch(p->formato) {
  case 0: {                                 /* NV12 (fast path 1080p) */
    if(p->w == PS5CAM_W && p->h == PS5CAM_H) {
      DWORD rr;

      for(rr = 0; rr < (DWORD)PS5CAM_H; rr++) {
        DWORD dr = p->flip ? (DWORD)PS5CAM_H - 1 - rr : rr;

        memcpy(dst + (size_t)dr * PS5CAM_W, src + (size_t)rr * PS5CAM_W,
               PS5CAM_W);
      }
      for(rr = 0; rr < (DWORD)PS5CAM_H / 2; rr++) {
        DWORD dr = p->flip ? (DWORD)PS5CAM_H / 2 - 1 - rr : rr;

        memcpy(dst + (size_t)PS5CAM_W * PS5CAM_H + (size_t)dr * PS5CAM_W,
               src + (size_t)PS5CAM_W * PS5CAM_H + (size_t)rr * PS5CAM_W,
               PS5CAM_W);
      }
    } else {
      nvplanes_gen(dst, src, src + PS5CAM_W * PS5CAM_H, p->w, p->h, p->flip,
                   0);
    }
    break;
  }
  case 4:                                   /* I420 */
    nvplanes_gen(dst, src, src + PS5CAM_W * PS5CAM_H, p->w, p->h, p->flip,
                 1);
    break;
  case 1:                                   /* YUY2 */
    if(p->w == PS5CAM_W && p->h == PS5CAM_H)
      nv12_to_yuy2(dst, src, src + PS5CAM_W * PS5CAM_H, p->flip);
    else
      yu2_gen(dst, src, src + PS5CAM_W * PS5CAM_H, p->w, p->h, p->flip);
    break;
  case 2:                                   /* RGB32 */
    if(p->w == PS5CAM_W && p->h == PS5CAM_H)
      nv12_to_rgb(dst, src, src + PS5CAM_W * PS5CAM_H, 4, p->flip);
    else
      rgb_gen(dst, src, src + PS5CAM_W * PS5CAM_H, p->w, p->h, 4, p->flip);
    break;
  default:                                  /* RGB24 */
    if(p->w == PS5CAM_W && p->h == PS5CAM_H)
      nv12_to_rgb(dst, src, src + PS5CAM_W * PS5CAM_H, 3, p->flip);
    else
      rgb_gen(dst, src, src + PS5CAM_W * PS5CAM_H, p->w, p->h, 3, p->flip);
    break;
  }
}

/* ------------------------------------------------------------------ */
/* thread di streaming                                                 */
/* ------------------------------------------------------------------ */

static IMemAllocator *
alloc_attivo(Ps5Pin *p)
{
  /* se il consumer ha notificato il SUO allocatore si usa quello (il
   * nostro a 4 buffer si esaurisce con le pipeline che trattengono i
   * campioni: era la fame da "consegna 1" e poi silenzio) */
  return p->alloc_loro ? p->alloc_loro : (IMemAllocator *)p->alloc;
}

static DWORD WINAPI
thread_stream(LPVOID arg)
{
  Ps5Filter *f = arg;
  Ps5Pin *p = f->pin;
  LONGLONG periodo = 10000000LL / PS5CAM_FPS;
  LONGLONG t = 0;
  DWORD inizio = GetTickCount();

  unsigned int nconsegna = 0;
  unsigned int ngiro = 0, ngb_ko = 0, nrecv_ko = 0;
  int gb_ko = 0;
  DWORD t_ko = 0;

  trace("Thread: alloc=%s", p->alloc_loro ? "consumer" : "nostro");
  while(WaitForSingleObject(f->ev_stop, 0) == WAIT_TIMEOUT) {
    IMediaSample *s = NULL;
    DWORD atteso;

    if(f->stato == State_Paused) {
      WaitForSingleObject(f->ev_stop, 20);
      continue;
    }
    /* cadenza: attesa fino al prossimo periodo */
    atteso = (DWORD)(inizio + (DWORD)((t / periodo) * (periodo / 10000)));
    {
      DWORD ora = GetTickCount();
      DWORD delta = atteso > ora ? atteso - ora : 0;

      if(delta > 0 && delta < 1000) Sleep(delta);
    }

    /* battito: prova che il giro gira anche quando non consegna (se i
     * battiti si fermano senza un "Grafo: Stop/Pause" = bloccati dentro
     * GetBuffer) */
    ngiro++;
    if((ngiro % 300) == 1)
      trace("giro %u (sup %d)", ngiro, (int)p->formato);

    if(alloc_attivo(p) &&
       alloc_attivo(p)->lpVtbl->GetBuffer(alloc_attivo(p), &s,
                                          NULL, NULL, 0) == NOERROR) {
      BYTE *ptr;

      if(gb_ko) {
        trace("GetBuffer ok dopo %u KO (%lu ms)", ngb_ko,
              (unsigned long)(GetTickCount() - t_ko));
        gb_ko = 0;
        ngb_ko = 0;
      }

      if(s->lpVtbl->GetPointer(s, &ptr) == NOERROR) {
        nconsegna++;
        if((nconsegna % 120) == 1)
          trace("consegna %u (sup %d stale %lu)", nconsegna, (int)(p->formato),
                (unsigned long)(f->shm
                  ? (DWORD)(GetTickCount() - (DWORD)f->shm->tick) : 999999UL));
        /* timestamp = dall'orologio del grafo (sorgente "live"): con la
         * base a 0 la catena della chiamata (Chromium) non considera
         * mai "partita" la cattura (20 s e chiude) */
        REFERENCE_TIME a, b;
        if(f->orologio &&
           f->orologio->lpVtbl->GetTime(f->orologio, &a) == S_OK) {
          b = a + periodo;
        } else {
          a = t;
          b = t + periodo;
        }

        riempi_campione(f, p, ptr, (DWORD)p->dim_frame);
        s->lpVtbl->SetActualDataLength(s, (LONG)p->dim_frame);
        s->lpVtbl->SetTime(s, &a, &b);
        s->lpVtbl->SetSyncPoint(s, TRUE);
        s->lpVtbl->SetPreroll(s, FALSE);
        s->lpVtbl->SetDiscontinuity(s, FALSE);
        if(p->peer_in) {
          HRESULT r = p->peer_in->lpVtbl->Receive(p->peer_in, s);

          if(r != S_OK) {
            nrecv_ko++;
            if(nrecv_ko == 1 || (nrecv_ko % 300) == 0)
              trace("Receive -> 0x%lx (%u volte)", (unsigned long)r, nrecv_ko);
          }
        }
      }
      alloc_attivo(p)->lpVtbl->ReleaseBuffer(alloc_attivo(p), s);
    } else {
      if(!gb_ko) {
        gb_ko = 1;
        t_ko = GetTickCount();
        ngb_ko = 1;
        trace("GetBuffer KO (alloc=%s)", p->alloc_loro ? "consumer" : "nostro");
      } else {
        ngb_ko++;
        if((ngb_ko % 30) == 0)
          trace("GetBuffer ancora KO da %lu ms",
                (unsigned long)(GetTickCount() - t_ko));
      }
    }
    t += periodo;
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* pin: IPin                                                           */
/* ------------------------------------------------------------------ */

static HRESULT WINAPI pin_QI(IPin *me, REFIID iid, void **out);
static ULONG WINAPI pin_AddRef(IPin *me);
static ULONG WINAPI pin_Release(IPin *me);
static HRESULT WINAPI pin_Connect(IPin *me, IPin *peer,
                                  const AM_MEDIA_TYPE *mt);
static HRESULT WINAPI pin_ReceiveConnection(IPin *me, IPin *c,
                                            const AM_MEDIA_TYPE *mt);
static HRESULT WINAPI pin_Disconnect(IPin *me);
static HRESULT WINAPI pin_ConnectedTo(IPin *me, IPin **p);
static HRESULT WINAPI pin_ConnectionMediaType(IPin *me, AM_MEDIA_TYPE *m);
static HRESULT WINAPI pin_QueryPinInfo(IPin *me, PIN_INFO *i);
static HRESULT WINAPI pin_QueryDirection(IPin *me, PIN_DIRECTION *d);
static HRESULT WINAPI pin_QueryId(IPin *me, LPWSTR *id);
static HRESULT WINAPI pin_QueryAccept(IPin *me, const AM_MEDIA_TYPE *m);
static HRESULT WINAPI pin_EnumMediaTypes(IPin *me, IEnumMediaTypes **e);
static HRESULT WINAPI pin_QueryInternal(IPin *me, IPin **p, ULONG *n);
static HRESULT WINAPI pin_EndOfStream(IPin *me);
static HRESULT WINAPI pin_BeginFlush(IPin *me);
static HRESULT WINAPI pin_EndFlush(IPin *me);
static HRESULT WINAPI pin_NewSegment(IPin *me, REFERENCE_TIME a,
                                     REFERENCE_TIME b, double r);

static const IPinVtbl pin_vt = {
  pin_QI, pin_AddRef, pin_Release,
  pin_Connect, pin_ReceiveConnection, pin_Disconnect, pin_ConnectedTo,
  pin_ConnectionMediaType, pin_QueryPinInfo, pin_QueryDirection,
  pin_QueryId, pin_QueryAccept, pin_EnumMediaTypes, pin_QueryInternal,
  pin_EndOfStream, pin_BeginFlush, pin_EndFlush, pin_NewSegment
};

static Ps5Pin *pin_self(IPin *me)
{
  return ((Ps5Pin *)((char *)me - offsetof(Ps5Pin, pin)));
}

static HRESULT WINAPI
pin_QI(IPin *me, REFIID iid, void **out)
{
  Ps5Pin *p = pin_self(me);


  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPin))
    *out = &p->pin;
  else if(IsEqualIID(iid, &IID_IAMStreamConfig))
    *out = &p->amsc;
  else if(IsEqualIID(iid, &IID_IMemInputPin))
    *out = &p->mip;
  else if(IsEqualIID(iid, &PS5_IID_IKsPropertySet))
    *out = &p->kpset;
  else {
    *out = NULL;
    return E_NOINTERFACE;
  }
  pin_AddRef(me);
  return S_OK;
}

static ULONG WINAPI
pin_AddRef(IPin *me)
{
  return InterlockedIncrement(&pin_self(me)->ref);
}

static ULONG WINAPI
pin_Release(IPin *me)
{
  Ps5Pin *p = pin_self(me);
  LONG r = InterlockedDecrement(&p->ref);

  if(r == 0) {
    if(p->peer) p->peer->lpVtbl->Release(p->peer);
    if(p->peer_in) p->peer_in->lpVtbl->Release(p->peer_in);
    if(p->alloc) p->alloc->lpVtbl->Release((IMemAllocator *)p->alloc);
    if(p->mt.pbFormat) CoTaskMemFree(p->mt.pbFormat);
    CoTaskMemFree(p);
  }
  return r;
}


/* --- IKsPropertySet sul pin: serve a ffmpeg/crome per la categoria --- */

static HRESULT WINAPI ks_QI(IKsPropertySet *me, REFIID iid, void **o);
static ULONG WINAPI ks_AddRef(IKsPropertySet *me);
static ULONG WINAPI ks_Release(IKsPropertySet *me);
static HRESULT WINAPI ks_Get(IKsPropertySet *me, REFGUID set, DWORD id,
                             void *inst, DWORD cbinst, void *data,
                             DWORD cbdata, DWORD *ret);
static HRESULT WINAPI ks_Set(IKsPropertySet *me, REFGUID set, DWORD id,
                             void *inst, DWORD cbinst, void *data,
                             DWORD cbdata);
static HRESULT WINAPI ks_Supported(IKsPropertySet *me, REFGUID set, DWORD id,
                                   DWORD *sup);

static const IKsPropertySetVtbl ks_vt = {
  .QueryInterface = ks_QI,
  .AddRef = ks_AddRef,
  .Release = ks_Release,
  .Get = ks_Get,
  .Set = ks_Set,
  .QuerySupported = ks_Supported
};

static Ps5Pin *ks_self(IKsPropertySet *me)
{
  return ((Ps5Pin *)((char *)me - offsetof(Ps5Pin, kpset)));
}

static HRESULT WINAPI
ks_QI(IKsPropertySet *me, REFIID iid, void **o)
{
  return pin_QI((IPin *)&ks_self(me)->pin, iid, o);
}

static ULONG WINAPI
ks_AddRef(IKsPropertySet *me)
{
  return InterlockedIncrement(&ks_self(me)->ref);
}

static ULONG WINAPI
ks_Release(IKsPropertySet *me)
{
  return pin_Release((IPin *)&ks_self(me)->pin);
}

static HRESULT WINAPI
ks_Get(IKsPropertySet *me, REFGUID set, DWORD id, void *inst, DWORD cbinst,
       void *data, DWORD cbdata, DWORD *ret)
{
  (void)me; (void)inst; (void)cbinst;
  if(IsEqualGUID(set, &PS5_AMPROPSETID_Pin) && id == 0) {
    /* id 0 = AMPROPERTY_PIN_CATEGORY: si restituisce il GUID della
     * categoria (PIN_CATEGORY_CAPTURE) */
    if(!data || cbdata < sizeof(GUID)) return E_UNEXPECTED;
    *(GUID *)data = PS5_PIN_CATEGORY_CAPTURE;
    if(ret) *ret = sizeof(GUID);
    return S_OK;
  }
  return 0x80070490;   /* E_PROP_ID_UNSUPPORTED */
}

static HRESULT WINAPI
ks_Set(IKsPropertySet *me, REFGUID set, DWORD id, void *inst, DWORD cbinst,
       void *data, DWORD cbdata)
{
  (void)me; (void)set; (void)id; (void)inst; (void)cbinst; (void)data;
  (void)cbdata;
  return 0x80070490;
}

static HRESULT WINAPI
ks_Supported(IKsPropertySet *me, REFGUID set, DWORD id, DWORD *sup)
{
  (void)me;
  if(IsEqualGUID(set, &PS5_AMPROPSETID_Pin) && id == 0) {
    if(sup) *sup = 1;                      /* KSPROPERTY_SUPPORT_GET */
    return S_OK;
  }
  return 0x80070490;
}

static int
formato_da_subtype(const GUID *s)
{
  if(IsEqualGUID(s, &SUBTYPE_NV12)) return 0;
  if(IsEqualGUID(s, &SUBTYPE_YUY2)) return 1;
  if(IsEqualGUID(s, &SUBTYPE_RGB32)) return 2;
  if(IsEqualGUID(s, &SUBTYPE_RGB24)) return 3;
  if(IsEqualGUID(s, &SUBTYPE_I420)) return 4;
  return -1;
}

static HRESULT WINAPI
pin_QueryAccept(IPin *me, const AM_MEDIA_TYPE *m)
{
  (void)me;
  if(IsEqualGUID(&m->majortype, &MEDIATYPE_Video) &&
     IsEqualGUID(&m->formattype, &FORMAT_VideoInfo) &&
     formato_da_subtype(&m->subtype) >= 0) {
    trace("QueryAccept %08lx -> si", (unsigned long)m->subtype.Data1);
    return S_OK;
  }
  trace("QueryAccept %08lx -> no", (unsigned long)(m ? m->subtype.Data1 : 0));
  return S_FALSE;
}

/* nome (basename) del modulo che contiene un indirizzo: ci dice CHI ci
 * sta parlando (Discord.exe? un helper? quale DLL?) */
static const char *
modulo_di(void *indirizzo)
{
  static char buf[MAX_PATH];
  MEMORY_BASIC_INFORMATION mbi;

  buf[0] = 0;
  if(!indirizzo) return buf;
  if(VirtualQuery(indirizzo, &mbi, sizeof(mbi)) && mbi.AllocationBase) {
    if(!GetModuleFileNameA((HMODULE)mbi.AllocationBase, buf, sizeof(buf)))
      buf[0] = 0;
  }
  return buf;
}

static HRESULT WINAPI
pin_Connect(IPin *me, IPin *peer, const AM_MEDIA_TYPE *mt)
{
  Ps5Pin *p = pin_self(me);

  trace("Connect mt=%d peer=%p modulo=%s", mt != NULL, (void *)peer,
        peer ? modulo_di(*(void **)peer) : "?");

  AM_MEDIA_TYPE *scelto = NULL;
  IMemInputPin *min = NULL;
  HRESULT r;

  if(p->connesso) { trace("  gia connesso"); return VFW_E_ALREADY_CONNECTED; }
  if(!peer) return E_POINTER;

  if(mt) {
    if(pin_QueryAccept(me, mt) != S_OK) { trace("  nostro QueryAccept no"); return VFW_E_TYPE_NOT_ACCEPTED; }
    /* il vero handshake DirectShow e' ReceiveConnection: QueryAccept e'
     * solo un sondaggio; senza ReceiveConnection il pin a valle resta
     * "non connesso" e la costruzione del grafo fallisce */
    trace("  ReceiveConnection(%08lx)...", (unsigned long)mt->subtype.Data1);
    r = peer->lpVtbl->ReceiveConnection(peer, (IPin *)&p->pin, mt);
    trace("  ReceiveConnection -> 0x%lx", (unsigned long)r);
    if(r != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
    scelto = media_type_dup(mt);
    if(!scelto) return E_OUTOFMEMORY;
  } else {
    int i;

    /* primo candidato: il formato chiesto con SetFormat (Discord chiede
     * YUY2; se gli si connette altro, il suo grabber legge il buffer con
     * la convenzione sbagliata -> mezza risoluzione e barra verde).
     * Poi 4 formati x 4 varianti: pieno/ripulito x altezza neg/pos.
     * Le varianti "ripulite" servono al SampleGrabber della chiamata
     * (qedit), che accetta solo un VIDEOINFOHEADER coi campi extra a
     * zero; l'altezza positiva = buffer scritto ribaltato. */
    p->flip = 0;
    for(i = -1; i < 12; i++) {
      AM_MEDIA_TYPE t;

      if(i < 0) {
        if(!p->mt.pbFormat) continue;
        if(IsEqualGUID(&p->mt.subtype, &SUBTYPE_NV12)) continue;
        t = p->mt;
        t.pbFormat = NULL;
        if(p->mt.cbFormat) {
          t.pbFormat = CoTaskMemAlloc(p->mt.cbFormat);
          if(!t.pbFormat) continue;
          memcpy(t.pbFormat, p->mt.pbFormat, p->mt.cbFormat);
        }
      } else {
        /* NV12 ESCLUSO: il SampleGrabber (qedit) della chiamata vocale
         * non sa copiare NV12 (E_OUTOFMEMORY, 0 frame) -> spinner e
         * errore 2014. Restano i formati che il grabber digerisce. */
        static const GUID *subs[3] = {
          &SUBTYPE_YUY2, &SUBTYPE_RGB24, &SUBTYPE_RGB32
        };

        mt_1080x(&t, subs[i % 3], &FORMAT_VideoInfo, (i / 6) & 1, (i / 3) & 1);
        if(!((i / 6) & 1))
          t.lSampleSize = 0;
      }
      /* percorso canonico: SOLO ReceiveConnection. La QueryAccept
       * preventiva si pianta sui pin che dentro RenderStream
       * (Chromium/Discord) rientrano nel grafo */
      trace("  cand %08lx v%d ReceiveConnection...", (unsigned long)t.subtype.Data1, i);
      r = peer->lpVtbl->ReceiveConnection(peer, (IPin *)&p->pin, &t);
      trace("  cand %08lx v%d ReceiveConnection -> 0x%lx",
            (unsigned long)t.subtype.Data1, i, (unsigned long)r);
      if(r == S_OK) {
        scelto = media_type_dup(&t);
        if(t.pbFormat) CoTaskMemFree(t.pbFormat);
        break;
      }
      if(t.pbFormat) CoTaskMemFree(t.pbFormat);
    }
    if(!scelto) { trace("  NESSUN tipo accettato"); return VFW_E_NO_ACCEPTABLE_TYPES; }
  }

  trace("  QI IMemInputPin...");
  r = peer->lpVtbl->QueryInterface(peer, &IID_IMemInputPin, (void **)&min);
  trace("  QI IMemInputPin -> 0x%lx", (unsigned long)r);
  if(r != S_OK || !min) {
    media_type_free(scelto);
    return VFW_E_NO_TRANSPORT;
  }

  /* allocatore: si prova il loro, altrimenti il nostro */
  {
    IMemAllocator *alloc = NULL;
    ALLOCATOR_PROPERTIES req, act;
    DWORD dim;

    /* orientamento e dimensioni dal tipo negoziato (altezza positiva =
     * consumer vuole bottom-up; ora supportiamo anche 720p/480p ecc.) */
    estrai_dime(scelto, p);
    dim = p->dim_frame;
    req.cBuffers = 4;
    req.cbBuffer = (LONG)dim;
    req.cbAlign = 1;
    req.cbPrefix = 0;

    /* SEMPRE il nostro allocatore (4 buffer con refcount): quello del
     * consumer (qedit) con lettura diretta fa tearing su Discord, che
     * legge i buffer mentre il thread li scrive */
    alloc = NULL;
    if(1) {
      Ps5Alloc *a = CoTaskMemAlloc(sizeof(*a));

      if(!a) {
        min->lpVtbl->Release(min);
        media_type_free(scelto);
        return E_OUTOFMEMORY;
      }
      memset(a, 0, sizeof(*a));
      a->lpVtbl = &alloc_vt;
      a->ref = 1;
      alloc = (IMemAllocator *)a;
    }
    trace("  SetProperties...");
    r = alloc->lpVtbl->SetProperties(alloc, &req, &act);
    trace("  SetProperties -> 0x%lx", (unsigned long)r);
    if(r != S_OK) {
      /* ripiego: allocatore nostro */
      Ps5Alloc *a = CoTaskMemAlloc(sizeof(*a));

      if(alloc) alloc->lpVtbl->Release(alloc);
      if(!a) {
        min->lpVtbl->Release(min);
        media_type_free(scelto);
        return E_OUTOFMEMORY;
      }
      memset(a, 0, sizeof(*a));
      a->lpVtbl = &alloc_vt;
      a->ref = 1;
      alloc = (IMemAllocator *)a;
      r = alloc->lpVtbl->SetProperties(alloc, &req, &act);
      if(r != S_OK) {
        alloc->lpVtbl->Release(alloc);
        min->lpVtbl->Release(min);
        media_type_free(scelto);
        return VFW_E_BADALIGN;
      }
    }
    trace("  NotifyAllocator...");
    r = min->lpVtbl->NotifyAllocator(min, alloc, TRUE);
    trace("  NotifyAllocator -> 0x%lx", (unsigned long)r);

    /* tiene l'allocatore per il pin (se e' il nostro lo creiamo qui) */
    alloc->lpVtbl->AddRef(alloc);
    p->alloc = (Ps5Alloc *)alloc;
    alloc->lpVtbl->Release(alloc);
    (void)act;
  }

  p->peer = peer;
  p->peer->lpVtbl->AddRef(peer);
  p->peer_in = min;
  p->mt = *scelto;
  CoTaskMemFree(scelto);            /* il pbFormat resta di p->mt */
  p->connesso = TRUE;
  trace("Connect OK tipo=%08lx dim=%lu w=%d h=%d flip=%d",
        (unsigned long)p->mt.subtype.Data1, (unsigned long)p->dim_frame,
        p->w, p->h, p->flip);
  return S_OK;
}

static HRESULT WINAPI
pin_ReceiveConnection(IPin *me, IPin *c, const AM_MEDIA_TYPE *mt)
{
  (void)me; (void)c; (void)mt;
  return E_UNEXPECTED;               /* siamo un pin di output */
}

static HRESULT WINAPI
pin_Disconnect(IPin *me)
{
  Ps5Pin *p = pin_self(me);

  if(!p->connesso) return S_FALSE;
  trace("Pin: Disconnect");
  if(p->peer) { p->peer->lpVtbl->Release(p->peer); p->peer = NULL; }
  if(p->peer_in) { p->peer_in->lpVtbl->Release(p->peer_in); p->peer_in = NULL; }
  if(p->alloc) {
    p->alloc->lpVtbl->Release((IMemAllocator *)p->alloc);
    p->alloc = NULL;
  }
  if(p->mt.pbFormat) { CoTaskMemFree(p->mt.pbFormat); p->mt.pbFormat = NULL; }
  p->connesso = FALSE;
  return S_OK;
}

static HRESULT WINAPI
pin_ConnectedTo(IPin *me, IPin **out)
{
  Ps5Pin *p = pin_self(me);

  if(!p->peer) { *out = NULL; return VFW_E_NOT_CONNECTED; }
  *out = p->peer;
  p->peer->lpVtbl->AddRef(p->peer);
  return S_OK;
}

static HRESULT WINAPI
pin_ConnectionMediaType(IPin *me, AM_MEDIA_TYPE *m)
{
  Ps5Pin *p = pin_self(me);

  if(!p->connesso) return VFW_E_NOT_CONNECTED;
  /* copia PROFONDA: il chiamante libera pbFormat con CoTaskMemFree */
  memset(m, 0, sizeof(*m));
  m->majortype = p->mt.majortype;
  m->subtype = p->mt.subtype;
  m->bFixedSizeSamples = p->mt.bFixedSizeSamples;
  m->bTemporalCompression = p->mt.bTemporalCompression;
  m->lSampleSize = p->mt.lSampleSize;
  m->formattype = p->mt.formattype;
  m->pUnk = NULL;
  m->cbFormat = p->mt.cbFormat;
  if(p->mt.cbFormat) {
    m->pbFormat = CoTaskMemAlloc(p->mt.cbFormat);
    if(!m->pbFormat) return E_OUTOFMEMORY;
    memcpy(m->pbFormat, p->mt.pbFormat, p->mt.cbFormat);
  }
  return S_OK;
}

static HRESULT WINAPI
pin_QueryPinInfo(IPin *me, PIN_INFO *i)
{
  Ps5Pin *p = pin_self(me);

  i->pFilter = (IBaseFilter *)p->filt;
  ((IBaseFilter *)p->filt)->lpVtbl->AddRef((IBaseFilter *)p->filt);
  i->dir = PINDIR_OUTPUT;
  copia_nome(i->achName, 128, L"Capture");
  return S_OK;
}

static HRESULT WINAPI
pin_QueryDirection(IPin *me, PIN_DIRECTION *d)
{
  (void)me;
  *d = PINDIR_OUTPUT;
  return S_OK;
}

static HRESULT WINAPI
pin_QueryId(IPin *me, LPWSTR *id)
{
  static const WCHAR nome[] = L"Capture";
  size_t n = (sizeof(nome) + 1) * sizeof(WCHAR);

  (void)me;
  *id = CoTaskMemAlloc(n);
  if(!*id) return E_OUTOFMEMORY;
  memcpy(*id, nome, sizeof(nome));
  return S_OK;
}

/* --- IEnumMediaTypes minimale --- */

typedef struct EnumMT {
  const IEnumMediaTypesVtbl *lpVtbl;
  LONG ref;
  int pos;
} EnumMT;

static HRESULT WINAPI emt_QI(IEnumMediaTypes *me, REFIID iid, void **o);
static ULONG WINAPI emt_AddRef(IEnumMediaTypes *me);
static ULONG WINAPI emt_Release(IEnumMediaTypes *me);
static HRESULT WINAPI emt_Next(IEnumMediaTypes *me, ULONG n,
                               AM_MEDIA_TYPE **m, ULONG *f);
static HRESULT WINAPI emt_Skip(IEnumMediaTypes *me, ULONG n);
static HRESULT WINAPI emt_Reset(IEnumMediaTypes *me);
static HRESULT WINAPI emt_Clone(IEnumMediaTypes *me, IEnumMediaTypes **o);

static const IEnumMediaTypesVtbl emt_vt = {
  emt_QI, emt_AddRef, emt_Release, emt_Next, emt_Skip, emt_Reset, emt_Clone
};

static HRESULT WINAPI
emt_QI(IEnumMediaTypes *me, REFIID iid, void **o)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumMediaTypes)) {
    *o = me;
    emt_AddRef(me);
    return S_OK;
  }
  *o = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
emt_AddRef(IEnumMediaTypes *me)
{
  return InterlockedIncrement(&((EnumMT *)me)->ref);
}

static ULONG WINAPI
emt_Release(IEnumMediaTypes *me)
{
  EnumMT *e = (EnumMT *)me;
  LONG r = InterlockedDecrement(&e->ref);

  if(r == 0) CoTaskMemFree(e);
  return r;
}

static HRESULT WINAPI
emt_Next(IEnumMediaTypes *me, ULONG n, AM_MEDIA_TYPE **m, ULONG *f)
{
  EnumMT *e = (EnumMT *)me;
  ULONG fatti = 0;


  while(fatti < n && e->pos < N_CAPS) {
    AM_MEDIA_TYPE t;
    AM_MEDIA_TYPE *c;

    mt_gen(&t, g_caps[e->pos].sub, g_caps[e->pos].w, g_caps[e->pos].h, 0,
           g_caps[e->pos].pos);
    c = media_type_dup(&t);
    if(t.pbFormat) CoTaskMemFree(t.pbFormat);
    if(!c) break;
    m[fatti++] = c;
    e->pos++;
  }
  if(f) *f = fatti;
  return fatti == n ? S_OK : S_FALSE;
}

static HRESULT WINAPI
emt_Skip(IEnumMediaTypes *me, ULONG n)
{
  EnumMT *e = (EnumMT *)me;

  e->pos += (int)n;
  if(e->pos > N_CAPS) e->pos = N_CAPS;
  return e->pos < N_CAPS ? S_OK : S_FALSE;
}

static HRESULT WINAPI
emt_Reset(IEnumMediaTypes *me)
{
  ((EnumMT *)me)->pos = 0;
  return S_OK;
}

static HRESULT WINAPI
emt_Clone(IEnumMediaTypes *me, IEnumMediaTypes **o)
{
  EnumMT *e = (EnumMT *)me;
  EnumMT *c = CoTaskMemAlloc(sizeof(*c));

  if(!c) return E_OUTOFMEMORY;
  c->lpVtbl = &emt_vt;
  c->ref = 1;
  c->pos = e->pos;
  *o = (IEnumMediaTypes *)c;
  return S_OK;
}

static HRESULT WINAPI
pin_EnumMediaTypes(IPin *me, IEnumMediaTypes **e)
{
  EnumMT *c = CoTaskMemAlloc(sizeof(*c));

  (void)me;
  if(!c) return E_OUTOFMEMORY;
  c->lpVtbl = &emt_vt;
  c->ref = 1;
  c->pos = 0;
  *e = (IEnumMediaTypes *)c;
  return S_OK;
}

static HRESULT WINAPI
pin_QueryInternal(IPin *me, IPin **p, ULONG *n)
{
  (void)me;
  *p = NULL;
  *n = 0;
  return E_NOTIMPL;
}

static HRESULT WINAPI
pin_EndOfStream(IPin *me)
{
  (void)me;
  return E_UNEXPECTED;
}

static HRESULT WINAPI
pin_BeginFlush(IPin *me)
{
  (void)me;
  return S_OK;
}

static HRESULT WINAPI
pin_EndFlush(IPin *me)
{
  (void)me;
  return S_OK;
}

static HRESULT WINAPI
pin_NewSegment(IPin *me, REFERENCE_TIME a, REFERENCE_TIME b, double r)
{
  (void)me; (void)a; (void)b; (void)r;
  return S_OK;
}

/* --- IAMStreamConfig --- */

static HRESULT WINAPI amsc_QI(IAMStreamConfig *me, REFIID iid, void **o);
static ULONG WINAPI amsc_AddRef(IAMStreamConfig *me);
static ULONG WINAPI amsc_Release(IAMStreamConfig *me);
static HRESULT WINAPI amsc_SetFormat(IAMStreamConfig *me, AM_MEDIA_TYPE *m);
static HRESULT WINAPI amsc_GetFormat(IAMStreamConfig *me, AM_MEDIA_TYPE **m);
static HRESULT WINAPI amsc_GetNum(IAMStreamConfig *me, int *n, int *s);
static HRESULT WINAPI amsc_GetCaps(IAMStreamConfig *me, int i,
                                   AM_MEDIA_TYPE **m, BYTE *caps);

static const IAMStreamConfigVtbl amsc_vt = {
  amsc_QI, amsc_AddRef, amsc_Release,
  amsc_SetFormat, amsc_GetFormat, amsc_GetNum, amsc_GetCaps
};

static Ps5Pin *amsc_self(IAMStreamConfig *me)
{
  return ((Ps5Pin *)((char *)me - offsetof(Ps5Pin, amsc)));
}

static HRESULT WINAPI
amsc_QI(IAMStreamConfig *me, REFIID iid, void **o)
{
  return pin_QI((IPin *)&amsc_self(me)->pin, iid, o);
}

static ULONG WINAPI
amsc_AddRef(IAMStreamConfig *me)
{
  return InterlockedIncrement(&amsc_self(me)->ref);
}

static ULONG WINAPI
amsc_Release(IAMStreamConfig *me)
{
  return pin_Release((IPin *)&amsc_self(me)->pin);
}

static HRESULT WINAPI
amsc_SetFormat(IAMStreamConfig *me, AM_MEDIA_TYPE *m)
{
  Ps5Pin *p = amsc_self(me);

  trace("SetFormat %08lx w=%ld h=%ld",
        (unsigned long)(m ? m->subtype.Data1 : 0),
        (m && m->pbFormat) ? (long)((VIDEOINFOHEADER *)m->pbFormat)->bmiHeader.biWidth : -1L,
        (m && m->pbFormat) ? (long)((VIDEOINFOHEADER *)m->pbFormat)->bmiHeader.biHeight : -1L);

  if(!m) return E_POINTER;
  if(pin_QueryAccept((IPin *)&p->pin, m) != S_OK) {
    return VFW_E_INVALIDMEDIATYPE;
  }
  if(p->mt.pbFormat) CoTaskMemFree(p->mt.pbFormat);
  p->mt = *m;
  if(m->cbFormat) {
    p->mt.pbFormat = CoTaskMemAlloc(m->cbFormat);
    if(p->mt.pbFormat) memcpy(p->mt.pbFormat, m->pbFormat, m->cbFormat);
  }
  estrai_dime(&p->mt, p);
  return S_OK;
}

static HRESULT WINAPI
amsc_GetFormat(IAMStreamConfig *me, AM_MEDIA_TYPE **m)
{
  Ps5Pin *p = amsc_self(me);

  AM_MEDIA_TYPE t;

  mt_gen(&t, p->formato == 0 ? &SUBTYPE_NV12 :
         p->formato == 1 ? &SUBTYPE_YUY2 :
         p->formato == 2 ? &SUBTYPE_RGB32 :
         p->formato == 4 ? &SUBTYPE_I420 : &SUBTYPE_RGB24,
         p->w, p->h, 0, p->flip);
  *m = media_type_dup(&t);
  if(t.pbFormat) CoTaskMemFree(t.pbFormat);
  return *m ? S_OK : E_OUTOFMEMORY;
}

static HRESULT WINAPI
amsc_GetNum(IAMStreamConfig *me, int *n, int *s)
{
  (void)me;
  if(n) *n = N_CAPS;
  if(s) *s = sizeof(VIDEO_STREAM_CONFIG_CAPS);
  return S_OK;
}

static HRESULT WINAPI
amsc_GetCaps(IAMStreamConfig *me, int i, AM_MEDIA_TYPE **m, BYTE *caps)
{
  VIDEO_STREAM_CONFIG_CAPS *vc = (VIDEO_STREAM_CONFIG_CAPS *)caps;
  AM_MEDIA_TYPE t;

  (void)me;
  if(i < 0 || i >= N_CAPS || !caps || !m) return E_INVALIDARG;
  memset(vc, 0, sizeof(*vc));
  vc->guid = FORMAT_VideoInfo;
  vc->VideoStandard = AnalogVideo_None;
  vc->InputSize.cx = g_caps[i].w;
  vc->InputSize.cy = g_caps[i].h;
  vc->MinCroppingSize.cx = g_caps[i].w;
  vc->MinCroppingSize.cy = g_caps[i].h;
  vc->MaxCroppingSize.cx = g_caps[i].w;
  vc->MaxCroppingSize.cy = g_caps[i].h;
  vc->CropGranularityX = 2;
  vc->CropGranularityY = 2;
  vc->MinOutputSize.cx = 160;
  vc->MinOutputSize.cy = 120;
  vc->MaxOutputSize.cx = PS5CAM_W;
  vc->MaxOutputSize.cy = PS5CAM_H;
  vc->OutputGranularityX = 2;
  vc->OutputGranularityY = 2;
  vc->MinFrameInterval = 10000000LL / PS5CAM_FPS;   /* 30 fps */
  vc->MaxFrameInterval = 10000000LL;                /* 1 fps */
  mt_gen(&t, g_caps[i].sub, g_caps[i].w, g_caps[i].h, 0, g_caps[i].pos);
  *m = media_type_dup(&t);
  if(t.pbFormat) CoTaskMemFree(t.pbFormat);
  return *m ? S_OK : E_OUTOFMEMORY;
}

/* --- IMemInputPin sul nostro pin: inoltra a valle --- */

static HRESULT WINAPI mip_QI(IMemInputPin *me, REFIID iid, void **o);
static ULONG WINAPI mip_AddRef(IMemInputPin *me);
static ULONG WINAPI mip_Release(IMemInputPin *me);
static HRESULT WINAPI mip_GetAllocator(IMemInputPin *me, IMemAllocator **a);
static HRESULT WINAPI mip_NotifyAllocator(IMemInputPin *me, IMemAllocator *a,
                                          BOOL ro);
static HRESULT WINAPI mip_GetAllocReq(IMemInputPin *me,
                                      ALLOCATOR_PROPERTIES *p);
static HRESULT WINAPI mip_Receive(IMemInputPin *me, IMediaSample *s);
static HRESULT WINAPI mip_ReceiveMulti(IMemInputPin *me, IMediaSample **s,
                                       LONG n, LONG *fatti);
static HRESULT WINAPI mip_ReceiveCanBlock(IMemInputPin *me);

static const IMemInputPinVtbl mip_vt = {
  mip_QI, mip_AddRef, mip_Release,
  mip_GetAllocator, mip_NotifyAllocator, mip_GetAllocReq,
  mip_Receive, mip_ReceiveMulti, mip_ReceiveCanBlock
};

static Ps5Pin *mip_self(IMemInputPin *me)
{
  return ((Ps5Pin *)((char *)me - offsetof(Ps5Pin, mip)));
}

static HRESULT WINAPI
mip_QI(IMemInputPin *me, REFIID iid, void **o)
{
  return pin_QI((IPin *)&mip_self(me)->pin, iid, o);
}

static ULONG WINAPI
mip_AddRef(IMemInputPin *me)
{
  return InterlockedIncrement(&mip_self(me)->ref);
}

static ULONG WINAPI
mip_Release(IMemInputPin *me)
{
  return pin_Release((IPin *)&mip_self(me)->pin);
}

static HRESULT WINAPI
mip_GetAllocator(IMemInputPin *me, IMemAllocator **a)
{
  *a = NULL;
  (void)me;
  return VFW_E_NO_ALLOCATOR;         /* non ne offriamo: il grafo ne crea una */
}

static HRESULT WINAPI
mip_NotifyAllocator(IMemInputPin *me, IMemAllocator *a, BOOL ro)
{
  Ps5Pin *p = mip_self(me);

  (void)ro;
  trace("NotifyAllocator(verso di noi): %d", a ? 1 : 0);
  if(p->alloc_loro) {
    p->alloc_loro->lpVtbl->Release(p->alloc_loro);
    p->alloc_loro = NULL;
  }
  if(a) {
    a->lpVtbl->AddRef(a);
    p->alloc_loro = a;
  }
  return S_OK;
}

static HRESULT WINAPI
mip_GetAllocReq(IMemInputPin *me, ALLOCATOR_PROPERTIES *p)
{
  (void)me;
  memset(p, 0, sizeof(*p));
  return E_NOTIMPL;
}

static HRESULT WINAPI
mip_Receive(IMemInputPin *me, IMediaSample *s)
{
  (void)me; (void)s;
  return E_UNEXPECTED;               /* non riceviamo sul pin di uscita */
}

static HRESULT WINAPI
mip_ReceiveMulti(IMemInputPin *me, IMediaSample **s, LONG n, LONG *fatti)
{
  (void)me; (void)s; (void)n;
  *fatti = 0;
  return E_UNEXPECTED;
}

static HRESULT WINAPI
mip_ReceiveCanBlock(IMemInputPin *me)
{
  (void)me;
  return S_OK;
}

/* ------------------------------------------------------------------ */
/* IEnumPins                                                           */
/* ------------------------------------------------------------------ */

typedef struct EnumPins {
  const IEnumPinsVtbl *lpVtbl;
  LONG ref;
  Ps5Filter *filt;
  int pos;
} EnumPins;

static HRESULT WINAPI ep_QI(IEnumPins *me, REFIID iid, void **o);
static ULONG WINAPI ep_AddRef(IEnumPins *me);
static ULONG WINAPI ep_Release(IEnumPins *me);
static HRESULT WINAPI ep_Next(IEnumPins *me, ULONG n, IPin **p, ULONG *f);
static HRESULT WINAPI ep_Skip(IEnumPins *me, ULONG n);
static HRESULT WINAPI ep_Reset(IEnumPins *me);
static HRESULT WINAPI ep_Clone(IEnumPins *me, IEnumPins **o);

static const IEnumPinsVtbl ep_vt = {
  ep_QI, ep_AddRef, ep_Release, ep_Next, ep_Skip, ep_Reset, ep_Clone
};

static HRESULT WINAPI
ep_QI(IEnumPins *me, REFIID iid, void **o)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IEnumPins)) {
    *o = me;
    ep_AddRef(me);
    return S_OK;
  }
  *o = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
ep_AddRef(IEnumPins *me)
{
  return InterlockedIncrement(&((EnumPins *)me)->ref);
}

static ULONG WINAPI
ep_Release(IEnumPins *me)
{
  EnumPins *e = (EnumPins *)me;
  LONG r = InterlockedDecrement(&e->ref);

  if(r == 0) CoTaskMemFree(e);
  return r;
}

static HRESULT WINAPI
ep_Next(IEnumPins *me, ULONG n, IPin **p, ULONG *f)
{
  EnumPins *e = (EnumPins *)me;

  ULONG fatti = 0;

  while(fatti < n && e->pos == 0) {
    p[fatti++] = (IPin *)&e->filt->pin->pin;
    pin_AddRef(p[fatti - 1]);
    e->pos++;
  }
  if(f) *f = fatti;
  return fatti == n ? S_OK : S_FALSE;
}

static HRESULT WINAPI
ep_Skip(IEnumPins *me, ULONG n)
{
  EnumPins *e = (EnumPins *)me;

  e->pos += (int)n;
  if(e->pos > 1) e->pos = 1;
  return e->pos < 1 ? S_OK : S_FALSE;
}

static HRESULT WINAPI
ep_Reset(IEnumPins *me)
{
  ((EnumPins *)me)->pos = 0;
  return S_OK;
}

static HRESULT WINAPI
ep_Clone(IEnumPins *me, IEnumPins **o)
{
  EnumPins *e = (EnumPins *)me;
  EnumPins *c = CoTaskMemAlloc(sizeof(*c));

  if(!c) return E_OUTOFMEMORY;
  c->lpVtbl = &ep_vt;
  c->ref = 1;
  c->filt = e->filt;
  c->pos = e->pos;
  *o = (IEnumPins *)c;
  return S_OK;
}

/* ------------------------------------------------------------------ */
/* filtro (IBaseFilter)                                                */
/* ------------------------------------------------------------------ */

static HRESULT WINAPI f_QI(IBaseFilter *me, REFIID iid, void **o);
static ULONG WINAPI f_AddRef(IBaseFilter *me);
static ULONG WINAPI f_Release(IBaseFilter *me);
static HRESULT WINAPI f_GetClassID(IBaseFilter *me, CLSID *c);
static HRESULT WINAPI f_Stop(IBaseFilter *me);
static HRESULT WINAPI f_Pause(IBaseFilter *me);
static HRESULT WINAPI f_Run(IBaseFilter *me, REFERENCE_TIME t);
static HRESULT WINAPI f_GetState(IBaseFilter *me, DWORD ms, FILTER_STATE *s);
static HRESULT WINAPI f_SetClock(IBaseFilter *me, IReferenceClock *c);
static HRESULT WINAPI f_GetClock(IBaseFilter *me, IReferenceClock **c);
static HRESULT WINAPI f_EnumPins(IBaseFilter *me, IEnumPins **e);
static HRESULT WINAPI f_FindPin(IBaseFilter *me, LPCWSTR id, IPin **p);
static HRESULT WINAPI f_QueryInfo(IBaseFilter *me, FILTER_INFO *i);
static HRESULT WINAPI f_Join(IBaseFilter *me, IFilterGraph *g, LPCWSTR n);
static HRESULT WINAPI f_Vendor(IBaseFilter *me, LPWSTR *s);

static const IBaseFilterVtbl filt_vt = {
  f_QI, f_AddRef, f_Release, f_GetClassID,
  f_Stop, f_Pause, f_Run, f_GetState, f_SetClock, f_GetClock,
  f_EnumPins, f_FindPin, f_QueryInfo, f_Join, f_Vendor
};

static Ps5Pin *
pin_nuovo(Ps5Filter *f)
{
  Ps5Pin *p = CoTaskMemAlloc(sizeof(*p));

  if(!p) return NULL;
  memset(p, 0, sizeof(*p));
  p->pin.lpVtbl = &pin_vt;
  p->pin.self = p;
  p->amsc.lpVtbl = &amsc_vt;
  p->amsc.self = p;
  p->mip.lpVtbl = &mip_vt;
  p->mip.self = p;
  p->kpset.lpVtbl = &ks_vt;
  p->kpset.self = p;
  p->ref = 1;
  p->filt = f;
  p->formato = 0;
  p->dim_frame = PS5CAM_FRAME;
  return p;
}

static Ps5Filter *
filtro_nuovo(void)
{
  Ps5Filter *f = CoTaskMemAlloc(sizeof(*f));

  if(!f) return NULL;
  memset(f, 0, sizeof(*f));
  f->lpVtbl = &filt_vt;
  f->ref = 1;
  f->stato = State_Stopped;
  f->ev_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  f->pin = pin_nuovo(f);
  if(!f->pin) { CoTaskMemFree(f); return NULL; }
  return f;
}

static HRESULT WINAPI
f_QI(IBaseFilter *me, REFIID iid, void **o)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IPersist) ||
     IsEqualIID(iid, &IID_IMediaFilter) || IsEqualIID(iid, &IID_IBaseFilter)) {
    *o = me;
    f_AddRef(me);
    return S_OK;
  }
  *o = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
f_AddRef(IBaseFilter *me)
{
  return InterlockedIncrement(&((Ps5Filter *)me)->ref);
}

static ULONG WINAPI
f_Release(IBaseFilter *me)
{
  Ps5Filter *f = (Ps5Filter *)me;
  LONG r = InterlockedDecrement(&f->ref);

  if(r == 0) {
    if(f->pin) pin_Release((IPin *)&f->pin->pin);
    if(f->orologio) f->orologio->lpVtbl->Release(f->orologio);
    if(f->graph) f->graph->lpVtbl->Release(f->graph);
    if(f->ev_stop) CloseHandle(f->ev_stop);
    if(f->shm) UnmapViewOfFile(f->shm);
    if(f->shm_map) CloseHandle(f->shm_map);
    CoTaskMemFree(f);
  }
  return r;
}

static HRESULT WINAPI
f_GetClassID(IBaseFilter *me, CLSID *c)
{
  (void)me;
  *c = CLSID_Ps5Cam;
  return S_OK;
}

static void
chiudi_shm(Ps5Filter *f)
{
  if(f->shm) { UnmapViewOfFile(f->shm); f->shm = NULL; }
  if(f->shm_map) { CloseHandle(f->shm_map); f->shm_map = NULL; }
}

static HRESULT WINAPI
f_Stop(IBaseFilter *me)
{
  Ps5Filter *f = (Ps5Filter *)me;

  trace("Grafo: Stop (stato era %d)", (int)f->stato);
  if(f->thread) {
    DWORD inizio = GetTickCount();

    SetEvent(f->ev_stop);
    while(f->vivi && GetTickCount() - inizio < 2000) Sleep(10);
    if(f->vivi) {
      TerminateThread(f->thread, 0);
    }
    CloseHandle(f->thread);
    f->thread = NULL;
    ResetEvent(f->ev_stop);
  }
  if(f->pin && f->pin->alloc)
    f->pin->alloc->lpVtbl->Decommit((IMemAllocator *)f->pin->alloc);
  f->stato = State_Stopped;
  chiudi_shm(f);
  trace("Grafo: Stop fine");
  return S_OK;
}

static HRESULT WINAPI
f_Pause(IBaseFilter *me)
{
  trace("Grafo: Pause (stato era %d)", (int)((Ps5Filter *)me)->stato);
  ((Ps5Filter *)me)->stato = State_Paused;
  return S_OK;
}

static HRESULT WINAPI
f_Run(IBaseFilter *me, REFERENCE_TIME t)
{
  Ps5Filter *f = (Ps5Filter *)me;

  (void)t;
  trace("Grafo: Run (thread %s)", f->thread ? "esisteva" : "da creare");
  if(!f->thread) {
    /* l'allocatore va COMMITTATO prima di usarlo (GetBuffer fallirebbe
     * con VFW_E_NOT_COMMITTED e ffmpeg resterebbe in attesa) */
    if(f->pin->alloc) {
      HRESULT rr = f->pin->alloc->lpVtbl->Commit((IMemAllocator *)
                                                 f->pin->alloc);

    } else {
    }
    f->shm_map = OpenFileMappingA(FILE_MAP_READ, FALSE, PS5CAM_SHM_NAME);
    if(f->shm_map)
      f->shm = (ps5cam_shm_t *)MapViewOfFile(f->shm_map, FILE_MAP_READ, 0, 0,
                                             sizeof(ps5cam_shm_t));
    trace("Run: thread avviato");
    f->vivi = 1;
    f->thread = CreateThread(NULL, 0, thread_stream, f, 0, NULL);
    if(!f->thread) { f->vivi = 0; return E_FAIL; }
  }
  f->stato = State_Running;
  return S_OK;
}

static HRESULT WINAPI
f_GetState(IBaseFilter *me, DWORD ms, FILTER_STATE *s)
{
  (void)ms;
  *s = ((Ps5Filter *)me)->stato;
  return S_OK;
}

static HRESULT WINAPI
f_SetClock(IBaseFilter *me, IReferenceClock *c)
{
  Ps5Filter *f = (Ps5Filter *)me;

  if(f->orologio) f->orologio->lpVtbl->Release(f->orologio);
  f->orologio = c;
  if(c) c->lpVtbl->AddRef(c);
  return S_OK;
}

static HRESULT WINAPI
f_GetClock(IBaseFilter *me, IReferenceClock **c)
{
  Ps5Filter *f = (Ps5Filter *)me;

  *c = f->orologio;
  if(f->orologio) f->orologio->lpVtbl->AddRef(f->orologio);
  return S_OK;
}

static HRESULT WINAPI
f_EnumPins(IBaseFilter *me, IEnumPins **e)
{
  Ps5Filter *f = (Ps5Filter *)me;


  EnumPins *c = CoTaskMemAlloc(sizeof(*c));

  if(!c) return E_OUTOFMEMORY;
  c->lpVtbl = &ep_vt;
  c->ref = 1;
  c->filt = f;
  c->pos = 0;
  *e = (IEnumPins *)c;
  return S_OK;
}

static HRESULT WINAPI
f_FindPin(IBaseFilter *me, LPCWSTR id, IPin **p)
{
  Ps5Filter *f = (Ps5Filter *)me;

  (void)id;

  if(id && !lstrcmpW(id, L"Capture")) {
    *p = (IPin *)&f->pin->pin;
    pin_AddRef(*p);
    return S_OK;
  }
  *p = NULL;
  return VFW_E_NOT_FOUND;
}

static HRESULT WINAPI
f_QueryInfo(IBaseFilter *me, FILTER_INFO *i)
{
  Ps5Filter *f = (Ps5Filter *)me;

  copia_nome(i->achName, 128, PS5CAM_NAME);
  i->pGraph = f->graph;
  if(f->graph) f->graph->lpVtbl->AddRef(f->graph);
  return S_OK;
}

static HRESULT WINAPI
f_Join(IBaseFilter *me, IFilterGraph *g, LPCWSTR n)
{
  Ps5Filter *f = (Ps5Filter *)me;

  (void)n;
  if(f->graph) f->graph->lpVtbl->Release(f->graph);
  f->graph = g;
  if(g) g->lpVtbl->AddRef(g);
  return S_OK;
}

static HRESULT WINAPI
f_Vendor(IBaseFilter *me, LPWSTR *s)
{
  (void)me;
  *s = NULL;
  return E_NOTIMPL;
}

/* ------------------------------------------------------------------ */
/* class factory + export                                              */
/* ------------------------------------------------------------------ */

static LONG g_ref = 0;

typedef struct Factory {
  const IClassFactoryVtbl *lpVtbl;
  LONG ref;
} Factory;

static HRESULT WINAPI cf_QI(IClassFactory *me, REFIID iid, void **o);
static ULONG WINAPI cf_AddRef(IClassFactory *me);
static ULONG WINAPI cf_Release(IClassFactory *me);
static HRESULT WINAPI cf_Create(IClassFactory *me, IUnknown *out,
                                REFIID iid, void **obj);
static HRESULT WINAPI cf_Lock(IClassFactory *me, BOOL lock);

static const IClassFactoryVtbl cf_vt = {
  cf_QI, cf_AddRef, cf_Release, cf_Create, cf_Lock
};

static Factory g_factory = { &cf_vt, 1 };

static HRESULT WINAPI
cf_QI(IClassFactory *me, REFIID iid, void **o)
{
  if(IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IClassFactory)) {
    *o = me;
    cf_AddRef(me);
    return S_OK;
  }
  *o = NULL;
  return E_NOINTERFACE;
}

static ULONG WINAPI
cf_AddRef(IClassFactory *me)
{
  (void)me;
  return 2;                          /* singleton */
}

static ULONG WINAPI
cf_Release(IClassFactory *me)
{
  (void)me;
  return 1;
}

static HRESULT WINAPI
cf_Create(IClassFactory *me, IUnknown *out, REFIID iid, void **obj)
{
  Ps5Filter *f;

  (void)me;
  if(out) return CLASS_E_NOAGGREGATION;
  f = filtro_nuovo();
  if(!f) return E_OUTOFMEMORY;
  f->ref = 0;
  f_AddRef((IBaseFilter *)f);
  {
    HRESULT r = f_QI((IBaseFilter *)f, iid, obj);

    f_Release((IBaseFilter *)f);
    return r;
  }
}

static HRESULT WINAPI
cf_Lock(IClassFactory *me, BOOL lock)
{
  (void)me;
  if(lock) InterlockedIncrement(&g_ref);
  else InterlockedDecrement(&g_ref);
  return S_OK;
}

__declspec(dllexport) HRESULT WINAPI
DllGetClassObject(REFCLSID clsid, REFIID iid, void **obj)
{
  if(IsEqualCLSID(clsid, &CLSID_Ps5Cam))
    return cf_QI((IClassFactory *)&g_factory, iid, obj);
  return CLASS_E_CLASSNOTAVAILABLE;
}

__declspec(dllexport) HRESULT WINAPI
DllCanUnloadNow(void)
{
  return g_ref == 0 ? S_OK : S_FALSE;
}

/* --- registrazione per-utente (HKCU\Software\Classes) --- */

static LSTATUS
reg_set_str(HKEY root, LPCWSTR path, LPCWSTR nome, LPCWSTR val)
{
  HKEY k;
  LSTATUS r = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &k,
                              NULL);

  if(r) return r;
  r = RegSetValueExW(k, nome, 0, REG_SZ, (const BYTE *)val,
                     (DWORD)((lstrlenW(val) + 1) * sizeof(WCHAR)));
  RegCloseKey(k);
  return r;
}

static LSTATUS
reg_set_dword(HKEY root, LPCWSTR path, LPCWSTR nome, DWORD v)
{
  HKEY k;
  LSTATUS r = RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE, NULL, &k,
                              NULL);

  if(r) return r;
  r = RegSetValueExW(k, nome, 0, REG_DWORD, (const BYTE *)&v, sizeof(v));
  RegCloseKey(k);
  return r;
}

static void
registra(HKEY root, const WCHAR *dll)
{
  WCHAR p[512];
  WCHAR guid[64];

  StringFromGUID2(&CLSID_Ps5Cam, guid, 64);

  wsprintfW(p, L"Software\\Classes\\CLSID\\%s", guid);
  reg_set_str(root, p, NULL, PS5CAM_NAME);
  wsprintfW(p, L"Software\\Classes\\CLSID\\%s\\InprocServer32", guid);
  reg_set_str(root, p, NULL, dll);
  reg_set_str(root, p, L"ThreadingModel", L"Both");

  /* categoria "Video Input Device" */
  wsprintfW(p, L"Software\\Classes\\CLSID\\"
               L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\%s",
            guid);
  reg_set_str(root, p, L"FriendlyName", PS5CAM_NAME);
  reg_set_str(root, p, L"CLSID", guid);
  /* DevicePath finto in stile UVC: alcune pipeline leggono il "product
   * id" dal property bag del moniker (le webcam vere ce l'hanno) */
  reg_set_str(root, p, L"DevicePath",
              L"\\\\?\\usb#vid_054c&pid_0ce6#7&ps5avstream&0&0000"
              L"#{65e8773d-8f56-11d0-a3b9-00a0c9223196}\\global");
  reg_set_dword(root, p, L"Merit", 0x00400000);
}

static void
rimuovi(HKEY root)
{
  WCHAR p[512];
  WCHAR guid[64];

  StringFromGUID2(&CLSID_Ps5Cam, guid, 64);
  wsprintfW(p, L"Software\\Classes\\CLSID\\"
               L"{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\%s",
            guid);
  RegDeleteKeyW(root, p);
  wsprintfW(p, L"Software\\Classes\\CLSID\\%s\\InprocServer32", guid);
  RegDeleteKeyW(root, p);
  wsprintfW(p, L"Software\\Classes\\CLSID\\%s", guid);
  RegDeleteKeyW(root, p);
}

__declspec(dllexport) HRESULT WINAPI
DllRegisterServer(void)
{
  WCHAR dll[MAX_PATH];
  WCHAR p[512];
  WCHAR guid[64];
  HKEY k;

  if(!GetModuleFileNameW(g_hinst_ps5cam, dll, MAX_PATH)) return E_FAIL;
  registra(HKEY_CURRENT_USER, dll);          /* niente admin */
  registra(HKEY_LOCAL_MACHINE, dll);         /* se siamo admin, meglio */
  /* il risultato si verifica davvero: se almeno la chiave per-utente e'
   * leggibile la registrazione e' utilizzabile (HKCR unisce HKCU+HKLM) */
  StringFromGUID2(&CLSID_Ps5Cam, guid, 64);
  wsprintfW(p, L"Software\\Classes\\CLSID\\%s\\InprocServer32", guid);
  if(RegOpenKeyExW(HKEY_CURRENT_USER, p, 0, KEY_READ, &k) == ERROR_SUCCESS) {
    RegCloseKey(k);
    return S_OK;
  }
  return E_FAIL;
}

__declspec(dllexport) HRESULT WINAPI
DllUnregisterServer(void)
{
  rimuovi(HKEY_CURRENT_USER);
  rimuovi(HKEY_LOCAL_MACHINE);
  return S_OK;
}

BOOL WINAPI
DllMain(HANDLE h, DWORD motivo, LPVOID r)
{
  (void)r;
  if(motivo == DLL_PROCESS_ATTACH) g_hinst_ps5cam = h;
  return TRUE;
}
