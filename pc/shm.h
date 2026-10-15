/*
 * shm.h — memoria condivisa tra il feeder (ps5feed.exe) e la webcam
 * virtuale (ps5cam.dll). Niente dipendenze: solo Win32.
 *
 * 3 buffer NV12 a rotazione: il feeder scrive SEMPRE nel buffer
 * successivo a "cur" e poi sposta "cur": il lettore che copia buf[cur]
 * non viene mai disturbato (una copia dura ~1 ms contro ~25 ms/frame).
 */
#ifndef PS5CAM_SHM_H
#define PS5CAM_SHM_H

#define PS5CAM_SHM_NAME "Local\\Ps5CamShm"
#define PS5CAM_MAGIC    0x50533543UL        /* "PS5C" */
#define PS5CAM_W        1920
#define PS5CAM_H        1080
#define PS5CAM_FRAME    (PS5CAM_W * PS5CAM_H * 3 / 2)   /* NV12 */
#define PS5CAM_BUFS     3

typedef struct {
  volatile unsigned int magic;      /* PS5CAM_MAGIC quando pronto */
  volatile unsigned int w;
  volatile unsigned int h;
  volatile unsigned int seq;        /* frame pubblicati */
  volatile unsigned int cur;        /* 0..PS5CAM_BUFS-1 */
  volatile unsigned int feed_ok;    /* 1 = collegato alla console */
  volatile unsigned int reticolo[2];
  volatile unsigned long long tick; /* GetTickCount64 dell'ultimo frame */
  unsigned char buf[PS5CAM_BUFS][PS5CAM_FRAME];
} ps5cam_shm_t;

#endif
