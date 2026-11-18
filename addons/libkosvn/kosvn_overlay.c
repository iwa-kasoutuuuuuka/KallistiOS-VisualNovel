/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_overlay.c
 * Animated overlay layers drawn on top of the background picture:
 * eye blinks and voice-driven lip-sync (PS-style "difference" parts).
 *
 * Script:
 *   OVERLAY <slot> <raw16> <texw> <texh> <fw> <fh> <stride> <frames> <x> <y> <w> <h> <blink|lip|lip<N>|still>
 *   OVERLAY_CLEAR [slot]
 *   OVERLAY_TRACK <slot> <file.TRK>   keyframed sprite (one row per 60 Hz frame)
 *   LIP_MAP <frames> <f0> .. <f4>     mouth shape -> frame for parts with that many frames
 *
 * TRK file (little endian): "TRK1", u16 frames, u16 loop start (frames from
 * there repeat; = frames: play once and stay on the last), u8 images, 3 pad,
 * images x { char path[40], u16 tex w, u16 tex h } (ARGB4444 RAW16), then per
 * frame { s8 image (-1 = nothing), u8 pad, s16 left, top, w, h (PS px x4),
 * u16 src w, src h (texels), s16 rot (deg x16, clockwise), s16 pivot x, y
 * (PS px x4) }. PS pixels are drawn 2x.
 *   SPEAKER <N|none>       owner of the next voice (for lip<N> layers)
 *
 * The RAW16 file is ARGB4444, non-twiddled, texw x texh (powers of two).
 * Frame i occupies texture rows [i*stride+1, i*stride+1+fh) and columns
 * [0, fw); the extra rows/column are edge padding against filter bleed.
 * x/y/w/h are screen coordinates (640x480). BG changes clear all layers.
 *
 * The base picture already shows the resting state (eyes open, mouth shut),
 * so a layer is only drawn while it differs from it:
 *   blink: hidden, then frames-1 (half) -> 0 (closed) -> frames-1 -> hidden
 *   lip:   frame = current mouth shape while a voice with lip data plays
 *   lip<N>: same, but only the mouth of person N: the layer moves only when
 *          the last SPEAKER was N, and only the first such layer (PS rule:
 *          one mouth per voice, first matching part wins)
 *   still: never drawn (e.g. mouths whose speaker is unknown)
 *   show:  frame 0 always drawn (a sprite on the picture)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <kos/dbglog.h>
#include <dc/fmath.h>

#include "kosvn_internal.h"

#define VN_MAX_OVERLAYS 6

typedef enum { OV_STILL = 0, OV_BLINK = 1, OV_LIP = 2, OV_SHOW = 3, OV_TRACK = 4 } ov_mode_t;

#define TRK_MAX_IMG 4
typedef struct __attribute__((packed)) {
    int8_t   img; uint8_t pad;
    int16_t  left, top, w, h;
    uint16_t src_w, src_h;
    int16_t  rot, pvx, pvy;
} trk_frame_t;

typedef struct {
    bool      active;
    vn_tex_t  tex;
    uint16_t  fw, fh, stride, frames;
    float     x, y, w, h;
    ov_mode_t mode;
    int       frame;        /* -1 = hidden (base picture shows through) */
    uint32_t  timer_ms;     /* blink: time until the next blink step */
    int       blink_step;   /* 0 idle, 1 half, 2 closed, 3 half */
    int       owner;        /* lip<N>: person N; -1 = any speaker */
    trk_frame_t *trk;       /* OVERLAY_TRACK: frames (malloc) */
    uint16_t  trk_n, trk_loop;
    int       trk_nimg;
    vn_tex_t  trk_img[TRK_MAX_IMG];
    uint32_t  trk_ms;       /* time since the start */
} vn_overlay_t;

static const uint32_t blink_step_ms[4] = { 0U, 40U, 70U, 40U };

/* Mouth shape (0..4, .LIP) -> frame, per frame count of the part. Set by the
   game script (LIP_MAP <frames> <f0> <f1> <f2> <f3> <f4>); without one the
   frame is the shape itself (clamped to the last frame). */
#define LIP_MAP_MAX_FRAMES 8
static int8_t lip_map[LIP_MAP_MAX_FRAMES + 1][5];
static bool   lip_map_set[LIP_MAP_MAX_FRAMES + 1];

void vn_overlay_lip_map(const char *arg) {
    int n = 0, f[5] = { 0, 0, 0, 0, 0 };
    if(sscanf(arg, "%d %d %d %d %d %d", &n, &f[0], &f[1], &f[2], &f[3], &f[4]) != 6 ||
       n < 1 || n > LIP_MAP_MAX_FRAMES) {
        dbglog(DBG_WARNING, "kosvn_overlay: bad LIP_MAP '%s'\n", arg);
        return;
    }
    for(int k = 0; k < 5; k++)
        lip_map[n][k] = (int8_t)((f[k] >= 0 && f[k] < n) ? f[k] : n - 1);
    lip_map_set[n] = true;
}

static vn_overlay_t ov[VN_MAX_OVERLAYS];
static uint32_t ov_rng = 0x2545F491U;
static int ov_speaker = -1;     /* SPEAKER: person of the current voice, -1 none */

void vn_overlay_set_speaker(const char *arg) {
    char *end = NULL;
    long v = strtol(arg, &end, 10);
    ov_speaker = (end != arg && v >= 0 && v < 1000) ? (int)v : -1;
}

static uint32_t ov_rand(void) {
    ov_rng ^= ov_rng << 13;
    ov_rng ^= ov_rng >> 17;
    ov_rng ^= ov_rng << 5;
    return ov_rng;
}

static uint32_t next_blink_ms(void) {
    return 2000U + (ov_rand() % 3500U);
}

static void ov_free(vn_overlay_t *o) {
    if(o->active && o->mode != OV_TRACK)
        vn_tex_free_later(&o->tex);
    for(int k = 0; k < o->trk_nimg; k++)
        vn_tex_free_later(&o->trk_img[k]);
    free(o->trk);
    memset(o, 0, sizeof(*o));
}

void vn_overlay_clear(int slot) {
    if(slot < 0 || slot >= VN_MAX_OVERLAYS)
        return;
    ov_free(&ov[slot]);
}

void vn_overlay_clear_all(void) {
    for(int i = 0; i < VN_MAX_OVERLAYS; i++)
        ov_free(&ov[i]);
}

int vn_overlay_command(const char *arg) {
    int slot = 0;
    char path[64] = {0};
    char mode[16] = {0};
    unsigned tw = 0, th = 0, fw = 0, fh = 0, stride = 0, frames = 0;
    float x = 0, y = 0, w = 0, h = 0;

    if(sscanf(arg, "%d %63s %u %u %u %u %u %u %f %f %f %f %15s",
              &slot, path, &tw, &th, &fw, &fh, &stride, &frames, &x, &y, &w, &h, mode) != 13) {
        dbglog(DBG_WARNING, "kosvn_overlay: bad OVERLAY '%s'\n", arg);
        return -1;
    }
    if(slot < 0 || slot >= VN_MAX_OVERLAYS || frames == 0U || fh == 0U ||
       stride * frames > th || fw > tw) {
        dbglog(DBG_WARNING, "kosvn_overlay: OVERLAY out of range '%s'\n", arg);
        return -1;
    }

    vn_overlay_t *o = &ov[slot];
    ov_free(o);
    if(vn_tex_load_raw16(&o->tex, path, (uint16_t)tw, (uint16_t)th,
                         PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED) < 0)
        return -1;

    o->active = true;
    o->fw = (uint16_t)fw;
    o->fh = (uint16_t)fh;
    o->stride = (uint16_t)stride;
    o->frames = (uint16_t)frames;
    o->x = x; o->y = y; o->w = w; o->h = h;
    o->owner = -1;
    if(strcmp(mode, "show") == 0) {
        o->mode = OV_SHOW;
    } else if(strcmp(mode, "blink") == 0) {
        o->mode = OV_BLINK;
    } else if(strncmp(mode, "lip", 3) == 0) {
        o->mode = OV_LIP;
        if(mode[3] != '\0')
            o->owner = atoi(mode + 3);
    } else {
        o->mode = OV_STILL;
    }
    o->frame = -1;
    o->timer_ms = next_blink_ms();
    return 0;
}

int vn_overlay_track(const char *arg) {
    int slot = -1;
    char path[64] = {0};
    if(sscanf(arg, "%d %63s", &slot, path) != 2 || slot < 0 || slot >= VN_MAX_OVERLAYS) {
        dbglog(DBG_WARNING, "kosvn_overlay: bad OVERLAY_TRACK '%s'\n", arg);
        return -1;
    }
    vn_overlay_t *o = &ov[slot];
    ov_free(o);
    FILE *fp = vn_fopen(path, "rb");
    if(!fp) {
        dbglog(DBG_WARNING, "kosvn_overlay: no track '%s'\n", path);
        return -1;
    }
    uint8_t hdr[12];
    if(fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "TRK1", 4) != 0) {
        fclose(fp);
        return -1;
    }
    uint16_t n = (uint16_t)(hdr[4] | (hdr[5] << 8));
    uint16_t loop = (uint16_t)(hdr[6] | (hdr[7] << 8));
    int nimg = hdr[8];
    if(n == 0U || nimg > TRK_MAX_IMG) {
        fclose(fp);
        return -1;
    }
    for(int k = 0; k < nimg; k++) {
        uint8_t ent[44];
        if(fread(ent, 1, 44, fp) != 44) {
            fclose(fp);
            ov_free(o);
            return -1;
        }
        char ipath[41];
        memcpy(ipath, ent, 40);
        ipath[40] = '\0';
        uint16_t tw = (uint16_t)(ent[40] | (ent[41] << 8)), th = (uint16_t)(ent[42] | (ent[43] << 8));
        if(vn_tex_load_raw16(&o->trk_img[k], ipath, tw, th,
                             PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED) < 0) {
            fclose(fp);
            ov_free(o);
            return -1;
        }
        o->trk_nimg = k + 1;
    }
    o->trk = (trk_frame_t *)malloc(sizeof(trk_frame_t) * n);
    if(!o->trk || fread(o->trk, sizeof(trk_frame_t), n, fp) != n) {
        fclose(fp);
        ov_free(o);
        return -1;
    }
    fclose(fp);
    o->active = true;
    o->mode = OV_TRACK;
    o->trk_n = n;
    o->trk_loop = loop;
    o->frame = 0;
    return 0;
}

void vn_overlay_update(uint32_t dt_ms) {
    int lip = vn_audio_lip_shape();   /* -1 when no voice / no lip data */
    bool owner_done = false;          /* first lip<N> layer of the speaker only */
    for(int i = 0; i < VN_MAX_OVERLAYS; i++) {
        vn_overlay_t *o = &ov[i];
        if(!o->active)
            continue;
        if(o->mode == OV_BLINK) {
            if(o->timer_ms > dt_ms) {
                o->timer_ms -= dt_ms;
                continue;
            }
            o->blink_step = (o->blink_step + 1) & 3;
            if(o->blink_step == 0) {
                o->frame = -1;
                o->timer_ms = next_blink_ms();
            } else {
                /* frame 0 = closed, last frame = half-closed (1-frame parts: closed only) */
                o->frame = (o->blink_step == 2) ? 0 : (int)o->frames - 1;
                o->timer_ms = blink_step_ms[o->blink_step];
            }
        } else if(o->mode == OV_LIP) {
            int f = lip;
            if(o->owner >= 0) {
                if(owner_done || o->owner != ov_speaker)
                    f = -1;
                else
                    owner_done = true;
            }
            if(f >= 0) {
                if(o->frames <= LIP_MAP_MAX_FRAMES && lip_map_set[o->frames])
                    f = lip_map[o->frames][f > 4 ? 4 : f];
                else if(f >= (int)o->frames)
                    f = (int)o->frames - 1;
            }
            o->frame = f;           /* -1 while silent: the base picture shows */
        } else if(o->mode == OV_SHOW) {
            o->frame = 0;
        } else if(o->mode == OV_TRACK) {
            o->trk_ms += dt_ms;
            uint32_t f = (o->trk_ms * 60U) / 1000U;
            if(f >= o->trk_n) {
                if(o->trk_loop < o->trk_n)
                    f = o->trk_loop + (f - o->trk_loop) % (uint32_t)(o->trk_n - o->trk_loop);
                else
                    f = o->trk_n - 1U;
            }
            o->frame = (int)f;
        } else {
            o->frame = -1;
        }
    }
}

void vn_overlay_render(float sx, float sy) {
    for(int i = 0; i < VN_MAX_OVERLAYS; i++) {
        vn_overlay_t *o = &ov[i];
        if(!o->active || o->frame < 0)
            continue;
        if(o->mode == OV_TRACK) {
            const trk_frame_t *t = &o->trk[o->frame];
            if(t->img < 0 || t->img >= o->trk_nimg || t->w == 0 || t->h == 0)
                continue;
            const vn_tex_t *tx = &o->trk_img[(int)t->img];
            /* box corners (TL, BL, TR, BR) rotated about the pivot, PS px x4 -> screen x2 */
            float l = t->left / 4.0f, tp = t->top / 4.0f, w = t->w / 4.0f, h = t->h / 4.0f;
            float px = t->pvx / 4.0f, py = t->pvy / 4.0f;
            float cx[4] = { l, l, l + w, l + w }, cy[4] = { tp, tp + h, tp, tp + h };
            float a = (float)t->rot / 16.0f * (3.14159265f / 180.0f);
            float c = fcos(a), sn = fsin(a);
            float xs[4], ys[4];
            for(int k = 0; k < 4; k++) {
                float dx = cx[k] - px, dy = cy[k] - py;
                xs[k] = (px + dx * c - dy * sn) * 2.0f + sx;
                ys[k] = (py + dx * sn + dy * c) * 2.0f + sy;
            }
            vn_gfx_draw_txr_quad4(PVR_LIST_TR_POLY, tx, xs, ys, 5.0f + (float)i * 0.1f,
                                  0.0f, 0.0f, (float)t->src_w / (float)tx->width,
                                  (float)t->src_h / (float)tx->height, 0xFFFFFFFFU);
            continue;
        }
        float tw = (float)o->tex.width;
        float th = (float)o->tex.height;
        float v0 = (float)(o->frame * o->stride + 1U) / th;
        float v1 = (float)(o->frame * o->stride + 1U + o->fh) / th;
        vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &o->tex, o->x + sx, o->y + sy, o->w, o->h,
                             5.0f + (float)i * 0.1f, 0.0f, v0, (float)o->fw / tw, v1, 0xFFFFFFFFU);
    }
}
