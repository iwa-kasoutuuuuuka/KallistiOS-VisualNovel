/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_gfx.c
 * PowerVR2 hardware 2D compositor, procedural/raw textures, fades & screen shake.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kos/dbglog.h>
#include <dc/pvr.h>

#include "kosvn_internal.h"

typedef struct vn_sprite_slot {
    bool     active;
    vn_tex_t tex;
    char     name[64];
    float    alpha;
    float    alpha_target;
} vn_sprite_slot_t;

static struct {
    bool             initialized;
    bool             has_bg;
    vn_tex_t         bg_tex;
    char             bg_name[64];
    vn_sprite_slot_t sprites[VN_MAX_SPRITES];
    bool             has_cutin;
    vn_tex_t         cutin_tex;
    char             cutin_name[64];

    /* Fade overlay */
    uint32_t         fade_rgb;
    float            fade_alpha;
    float            fade_start;
    float            fade_target;
    uint32_t         fade_elapsed_ms;
    uint32_t         fade_duration_ms;

    /* Screen shake */
    float            shake_amp;
    uint32_t         shake_elapsed_ms;
    uint32_t         shake_duration_ms;
    float            shake_x;
    float            shake_y;
} gfx_st;

static inline uint16_t pack_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline uint16_t pack_argb4444(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((a >> 4) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4));
}

static inline uint8_t lerp_u8(uint8_t c0, uint8_t c1, uint32_t num, uint32_t den) {
    if(den == 0)
        return c1;
    int32_t diff = (int32_t)c1 - (int32_t)c0;
    int32_t val = (int32_t)c0 + (diff * (int32_t)num) / (int32_t)den;
    if(val < 0) val = 0;
    if(val > 255) val = 255;
    return (uint8_t)val;
}

static bool is_pow2(uint16_t v) {
    return v >= 8 && (v & (v - 1)) == 0;
}

int vn_gfx_init(void) {
    if(gfx_st.initialized)
        return 0;

    memset(&gfx_st, 0, sizeof(gfx_st));
    if(pvr_init_defaults() < 0) {
        dbglog(DBG_ERROR, "kosvn_gfx: pvr_init_defaults failed\n");
        return -1;
    }
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);
    gfx_st.initialized = true;
    return 0;
}

void vn_gfx_shutdown(void) {
    if(!gfx_st.initialized)
        return;

    vn_tex_free(&gfx_st.bg_tex);
    gfx_st.has_bg = false;
    vn_tex_free(&gfx_st.cutin_tex);
    gfx_st.has_cutin = false;
    gfx_st.cutin_name[0] = '\0';
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_tex_free(&gfx_st.sprites[i].tex);
        gfx_st.sprites[i].active = false;
    }
    pvr_shutdown();
    gfx_st.initialized = false;
}

void vn_tex_free(vn_tex_t *tex) {
    if(!tex)
        return;
    if(tex->vram_ptr) {
        pvr_mem_free(tex->vram_ptr);
        tex->vram_ptr = NULL;
    }
    tex->width = 0;
    tex->height = 0;
    tex->fmt = 0;
}

int vn_tex_load_raw16(vn_tex_t *tex, const char *path, uint16_t w, uint16_t h, uint32_t fmt) {
    if(!tex || !path || !is_pow2(w) || !is_pow2(h))
        return -1;

    FILE *fp = fopen(path, "rb");
    if(!fp) {
        dbglog(DBG_ERROR, "kosvn_gfx: failed to open raw16 '%s'\n", path);
        return -1;
    }

    size_t bytes = (size_t)w * (size_t)h * 2U;
    void *tmp = memalign(32, bytes);
    if(!tmp) {
        fclose(fp);
        return -1;
    }

    size_t nread = fread(tmp, 1, bytes, fp);
    fclose(fp);
    if(nread != bytes) {
        free(tmp);
        dbglog(DBG_ERROR, "kosvn_gfx: short read on '%s' (%zu/%zu)\n", path, nread, bytes);
        return -1;
    }

    vn_tex_free(tex);
    tex->vram_ptr = pvr_mem_malloc(bytes);
    if(!tex->vram_ptr) {
        free(tmp);
        dbglog(DBG_ERROR, "kosvn_gfx: out of PVR VRAM (%zu bytes)\n", bytes);
        return -1;
    }

    pvr_txr_load(tmp, tex->vram_ptr, bytes);
    free(tmp);

    tex->width = w;
    tex->height = h;
    tex->fmt = fmt;
    return 0;
}

int vn_tex_create_procedural_bg(vn_tex_t *tex, uint16_t w, uint16_t h, int style_id) {
    if(!tex || !is_pow2(w) || !is_pow2(h))
        return -1;

    size_t bytes = (size_t)w * (size_t)h * 2U;
    uint16_t *pix = (uint16_t *)memalign(32, bytes);
    if(!pix)
        return -1;

    for(uint16_t y = 0; y < h; y++) {
        for(uint16_t x = 0; x < w; x++) {
            uint8_t r = 0, g = 0, b = 0;
            if(style_id == 0) {
                /* Night sky with moon & city skyline */
                r = lerp_u8(8, 28, y, h);
                g = lerp_u8(14, 48, y, h);
                b = lerp_u8(42, 92, y, h);

                /* Twinkling stars */
                uint32_t hash = ((uint32_t)x * 73856093U) ^ ((uint32_t)y * 19349663U);
                if(y < (h * 5U) / 8U && (hash & 0x3FFU) == 0x1A5U) {
                    r = 240; g = 245; b = 255;
                }

                /* Moon */
                int32_t dx = (int32_t)x - (int32_t)((w * 3U) / 4U);
                int32_t dy = (int32_t)y - (int32_t)(h / 4U);
                int32_t d2 = dx * dx + dy * dy;
                int32_t rad = (int32_t)(h / 9U);
                if(d2 <= rad * rad) {
                    r = 250; g = 242; b = 210;
                } else if(d2 <= (rad + 14) * (rad + 14)) {
                    r = (uint8_t)(r + 25 > 255 ? 255 : r + 25);
                    g = (uint8_t)(g + 25 > 255 ? 255 : g + 25);
                    b = (uint8_t)(b + 35 > 255 ? 255 : b + 35);
                }

                /* Distant skyline blocks */
                uint32_t blk = (x * 16U) / w;
                uint32_t bld_h = h - ((h / 5U) + ((blk * 37U) % (h / 6U)));
                if(y >= bld_h) {
                    r = 12; g = 14; b = 26;
                    if(((x & 7U) == 3U || (x & 7U) == 4U) && ((y & 15U) == 6U) && ((blk + y) % 5U == 0U)) {
                        r = 235; g = 205; b = 120;
                    }
                }
            } else if(style_id == 1) {
                /* Sunset shrine overlook */
                r = lerp_u8(70, 235, y, (h * 2U) / 3U);
                g = lerp_u8(28, 118, y, (h * 2U) / 3U);
                b = lerp_u8(64, 58, y, (h * 2U) / 3U);
                if(y >= (h * 2U) / 3U) {
                    r = 32; g = 24; b = 34;
                }
                /* Stylized torii silhouette */
                uint16_t cx = w / 2U;
                int32_t adx = (int32_t)x - (int32_t)cx;
                if(adx < 0) adx = -adx;
                bool pillar = (adx >= (int32_t)(w / 8U) && adx <= (int32_t)(w / 8U + w / 36U) && y >= h / 3U);
                bool topbar = (adx <= (int32_t)(w / 5U) && y >= h / 3U && y <= h / 3U + h / 22U);
                bool midbar = (adx <= (int32_t)(w / 6U) && y >= (h * 4U) / 9U && y <= (h * 4U) / 9U + h / 32U);
                if(pillar || topbar || midbar) {
                    r = 45; g = 14; b = 18;
                }
            } else {
                /* Cyber lab / Dreamcast VM monitor aesthetic */
                r = lerp_u8(6, 18, y, h);
                g = lerp_u8(22, 48, y, h);
                b = lerp_u8(36, 78, y, h);
                if(y > (h / 2U)) {
                    /* Perspective-like horizontal scan grid */
                    uint16_t rel_y = y - (h / 2U);
                    if((rel_y % 16U) == 0U || (((x - w / 2) * 32) / (rel_y + 12)) % 24 == 0) {
                        r = 24; g = 180; b = 220;
                    }
                }
                /* Top corner diagnostic bars */
                if((y >= 16U && y <= 20U) && ((x >= 16U && x <= w - 16U))) {
                    r = 40; g = 210; b = 245;
                }
            }

            pix[(size_t)y * w + x] = pack_rgb565(r, g, b);
        }
    }

    vn_tex_free(tex);
    tex->vram_ptr = pvr_mem_malloc(bytes);
    if(!tex->vram_ptr) {
        free(pix);
        return -1;
    }

    pvr_txr_load(pix, tex->vram_ptr, bytes);
    free(pix);

    tex->width = w;
    tex->height = h;
    tex->fmt = PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED;
    return 0;
}

int vn_tex_create_procedural_portrait(vn_tex_t *tex, uint16_t w, uint16_t h, int char_id, int expr_id) {
    if(!tex || !is_pow2(w) || !is_pow2(h))
        return -1;

    size_t bytes = (size_t)w * (size_t)h * 2U;
    uint16_t *pix = (uint16_t *)memalign(32, bytes);
    if(!pix)
        return -1;

    /* Palette per character */
    uint8_t hair_r = (char_id == 0) ? 215 : 95;
    uint8_t hair_g = (char_id == 0) ? 95  : 145;
    uint8_t hair_b = (char_id == 0) ? 130 : 210;

    uint8_t coat_r = (char_id == 0) ? 54  : 38;
    uint8_t coat_g = (char_id == 0) ? 62  : 50;
    uint8_t coat_b = (char_id == 0) ? 102 : 74;

    uint8_t tie_r  = (char_id == 0) ? 230 : 60;
    uint8_t tie_g  = (char_id == 0) ? 80  : 205;
    uint8_t tie_b  = (char_id == 0) ? 115 : 235;

    int32_t cx = (int32_t)w / 2;
    int32_t head_cy = (int32_t)((h * 28U) / 100U);
    int32_t head_rx = (int32_t)((w * 24U) / 100U);
    int32_t head_ry = (int32_t)((h * 14U) / 100U);

    for(uint16_t y = 0; y < h; y++) {
        for(uint16_t x = 0; x < w; x++) {
            uint16_t col = 0x0000;
            int32_t dx = (int32_t)x - cx;
            int32_t dy = (int32_t)y - head_cy;

            /* Long back hair silhouette */
            int32_t hdx = dx;
            int32_t hdy = (int32_t)y - (head_cy + head_ry / 2);
            if((hdx * hdx) * 100 / ((head_rx + 12) * (head_rx + 12)) +
               (hdy * hdy) * 100 / ((head_ry * 3) * (head_ry * 3)) <= 100 &&
               (int32_t)y >= head_cy - head_ry - 8) {
                col = pack_argb4444(255, hair_r, hair_g, hair_b);
            }

            /* Outfit / Torso trapezoid */
            if((int32_t)y >= head_cy + head_ry + 8) {
                int32_t torso_half = (int32_t)((w * 20U) / 100U) +
                                     (((int32_t)y - (head_cy + head_ry)) * (int32_t)w) / ((int32_t)h * 4);
                int32_t max_half = (int32_t)((w * 44U) / 100U);
                if(torso_half > max_half)
                    torso_half = max_half;
                if(dx >= -torso_half && dx <= torso_half) {
                    if(dx >= -(int32_t)(w / 10U) && dx <= (int32_t)(w / 10U) &&
                       (int32_t)y < head_cy + head_ry + (int32_t)(h / 5U)) {
                        col = pack_argb4444(255, 240, 242, 250); /* Collar shirt */
                        if(dx >= -5 && dx <= 5) {
                            col = pack_argb4444(255, tie_r, tie_g, tie_b); /* Ribbon/tie */
                        }
                    } else {
                        col = pack_argb4444(255, coat_r, coat_g, coat_b);
                    }
                }
            }

            /* Neck */
            if((int32_t)y >= head_cy + head_ry - 4 && (int32_t)y <= head_cy + head_ry + 14 &&
               dx >= -(head_rx / 3) && dx <= (head_rx / 3)) {
                col = pack_argb4444(255, 238, 205, 185);
            }

            /* Face oval */
            int32_t face_eq = (dx * dx) * 100 / (head_rx * head_rx) + (dy * dy) * 100 / (head_ry * head_ry);
            if(face_eq <= 100) {
                col = pack_argb4444(255, 255, 224, 204);

                /* Eyes */
                int32_t eye_y = head_cy + 1;
                int32_t eye_lx = cx - head_rx / 2 + 2;
                int32_t eye_rx = cx + head_rx / 2 - 2;
                int32_t eldx = (int32_t)x - eye_lx;
                int32_t erdx = (int32_t)x - eye_rx;
                int32_t edy = (int32_t)y - eye_y;

                if(expr_id == 1) {
                    /* Smile happy arc eyes */
                    if((eldx >= -5 && eldx <= 5 && edy >= -2 && edy <= 0) ||
                       (erdx >= -5 && erdx <= 5 && edy >= -2 && edy <= 0)) {
                        col = pack_argb4444(255, 45, 32, 48);
                    }
                } else {
                    int32_t eye_h = (expr_id == 2) ? 6 : 4;
                    if((eldx >= -4 && eldx <= 4 && edy >= -eye_h && edy <= eye_h) ||
                       (erdx >= -4 && erdx <= 4 && edy >= -eye_h && edy <= eye_h)) {
                        col = pack_argb4444(255, tie_r, tie_g, tie_b);
                        if(edy <= -1 && (eldx == -1 || erdx == -1)) {
                            col = pack_argb4444(255, 255, 255, 255); /* Highlight */
                        }
                    }
                }

                /* Blush cheeks on smile */
                if(expr_id == 1 && ((int32_t)y >= eye_y + 7 && (int32_t)y <= eye_y + 10) &&
                   ((eldx >= -5 && eldx <= 3) || (erdx >= -3 && erdx <= 5))) {
                    col = pack_argb4444(255, 252, 165, 172);
                }

                /* Mouth */
                int32_t my = head_cy + (head_ry * 55) / 100;
                if(expr_id == 2) {
                    /* Surprised 'O' mouth */
                    int32_t md2 = dx * dx + ((int32_t)y - my) * ((int32_t)y - my);
                    if(md2 <= 12)
                        col = pack_argb4444(255, 210, 95, 105);
                } else if(expr_id == 1) {
                    /* Smile mouth */
                    if(dx >= -5 && dx <= 5 && (int32_t)y >= my && (int32_t)y <= my + 3)
                        col = pack_argb4444(255, 215, 95, 110);
                } else {
                    if(dx >= -4 && dx <= 4 && (int32_t)y >= my && (int32_t)y <= my + 1)
                        col = pack_argb4444(255, 195, 105, 110);
                }
            }

            /* Front bangs over forehead */
            if(face_eq <= 115 && (int32_t)y < head_cy - 3) {
                int32_t fringe = ((x * 5U + (uint32_t)char_id * 3U) % 7U);
                if((int32_t)y < head_cy - 7 + (int32_t)fringe) {
                    col = pack_argb4444(255, hair_r, hair_g, hair_b);
                }
            }

            pix[(size_t)y * w + x] = col;
        }
    }

    vn_tex_free(tex);
    tex->vram_ptr = pvr_mem_malloc(bytes);
    if(!tex->vram_ptr) {
        free(pix);
        return -1;
    }

    pvr_txr_load(pix, tex->vram_ptr, bytes);
    free(pix);

    tex->width = w;
    tex->height = h;
    tex->fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    return 0;
}

int vn_bg_set(const char *asset_spec) {
    if(!asset_spec || asset_spec[0] == '\0' || strcmp(asset_spec, "none") == 0) {
        vn_tex_free(&gfx_st.bg_tex);
        gfx_st.has_bg = false;
        gfx_st.bg_name[0] = '\0';
        return 0;
    }

    int rc = -1;
    if(strncmp(asset_spec, "proc:", 5) == 0) {
        const char *tag = asset_spec + 5;
        int style = 0;
        if(strcmp(tag, "shrine_sunset") == 0) style = 1;
        else if(strcmp(tag, "cyber_lab") == 0) style = 2;
        rc = vn_tex_create_procedural_bg(&gfx_st.bg_tex, 256, 256, style);
    } else {
        rc = vn_tex_load_raw16(&gfx_st.bg_tex, asset_spec, 512, 512,
                               PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED);
    }

    if(rc == 0) {
        gfx_st.has_bg = true;
        vn_copy_str(gfx_st.bg_name, sizeof(gfx_st.bg_name), asset_spec);
    }
    return rc;
}

int vn_char_set(vn_sprite_pos_t pos, const char *asset_spec) {
    if((int)pos < 0 || (int)pos >= VN_MAX_SPRITES)
        return -1;

    if(!asset_spec || asset_spec[0] == '\0' || strcmp(asset_spec, "none") == 0) {
        vn_char_clear(pos);
        return 0;
    }

    vn_sprite_slot_t *sp = &gfx_st.sprites[pos];
    int rc = -1;
    if(strncmp(asset_spec, "proc:", 5) == 0) {
        const char *tag = asset_spec + 5;
        int char_id = 0;
        int expr_id = 0;
        if(strstr(tag, "rei") != NULL) char_id = 1;
        if(strstr(tag, "smile") != NULL) expr_id = 1;
        else if(strstr(tag, "surprised") != NULL) expr_id = 2;
        rc = vn_tex_create_procedural_portrait(&sp->tex, 128, 256, char_id, expr_id);
    } else {
        rc = vn_tex_load_raw16(&sp->tex, asset_spec, 128, 256,
                               PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED);
    }

    if(rc == 0) {
        sp->active = true;
        sp->alpha = 0.0f;
        sp->alpha_target = 1.0f;
        vn_copy_str(sp->name, sizeof(sp->name), asset_spec);
    }
    return rc;
}

static int create_procedural_cutin(vn_tex_t *tex, const char *tag) {
    const uint16_t w = 128U;
    const uint16_t h = 128U;
    size_t bytes = (size_t)w * (size_t)h * 2U;
    uint16_t *pix = (uint16_t *)memalign(32, bytes);
    if(!pix)
        return -1;

    int kind = 2; /* 0 = keycard, 1 = microfilm/disc, 2 = classified file */
    if(tag && strstr(tag, "keycard") != NULL)
        kind = 0;
    else if(tag && strstr(tag, "microfilm") != NULL)
        kind = 1;

    for(uint16_t y = 0; y < h; y++) {
        for(uint16_t x = 0; x < w; x++) {
            uint16_t col = pack_argb4444(210, 12, 24, 48);

            /* Outer tactical analyzer grid & frame */
            if(x < 2 || x >= w - 2 || y < 2 || y >= h - 2) {
                col = pack_argb4444(240, 80, 210, 248);
            } else if((x % 16 == 0) || (y % 16 == 0)) {
                col = pack_argb4444(210, 20, 44, 78);
            }

            if(kind == 0) {
                /* Security keycard with IC chip & magnetic stripe */
                if(x >= 16 && x <= 111 && y >= 34 && y <= 93) {
                    col = pack_argb4444(255, 225, 232, 244);
                    if(x == 16 || x == 111 || y == 34 || y == 93)
                        col = pack_argb4444(255, 70, 190, 240);
                    else if(y >= 42 && y <= 52)
                        col = pack_argb4444(255, 24, 34, 56);
                    else if(x >= 28 && x <= 48 && y >= 62 && y <= 78)
                        col = pack_argb4444(255, 235, 185, 55);
                    else if(x >= 60 && x <= 98 && (y == 66 || y == 74))
                        col = pack_argb4444(255, 45, 130, 210);
                }
            } else if(kind == 1) {
                /* Optical microfilm disc */
                int32_t dx = (int32_t)x - 64;
                int32_t dy = (int32_t)y - 64;
                int32_t r2 = dx * dx + dy * dy;
                if(r2 <= 42 * 42 && r2 >= 10 * 10) {
                    col = ((x + y) & 8) ? pack_argb4444(255, 150, 220, 245)
                                        : pack_argb4444(255, 210, 230, 248);
                    if(r2 >= 39 * 39)
                        col = pack_argb4444(255, 70, 200, 245);
                } else if(r2 < 10 * 10 && r2 >= 4 * 4) {
                    col = pack_argb4444(255, 240, 195, 70);
                }
            } else {
                /* Classified dossier folder with red seal */
                if(x >= 22 && x <= 105 && y >= 24 && y <= 103) {
                    col = pack_argb4444(255, 236, 224, 188);
                    if(y >= 24 && y <= 34 && x <= 62)
                        col = pack_argb4444(255, 218, 192, 132);
                    else if(x >= 34 && x <= 90 && (y == 48 || y == 58 || y == 68))
                        col = pack_argb4444(255, 65, 75, 95);
                    else if(x >= 68 && x <= 96 && y >= 78 && y <= 94)
                        col = pack_argb4444(255, 210, 50, 55);
                }
            }

            pix[(size_t)y * w + x] = col;
        }
    }

    vn_tex_free(tex);
    tex->vram_ptr = pvr_mem_malloc(bytes);
    if(!tex->vram_ptr) {
        free(pix);
        return -1;
    }

    pvr_txr_load(pix, tex->vram_ptr, bytes);
    free(pix);

    tex->width = w;
    tex->height = h;
    tex->fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    return 0;
}

int vn_cutin_set(const char *asset_spec) {
    if(!asset_spec || asset_spec[0] == '\0' || strcmp(asset_spec, "none") == 0) {
        vn_cutin_clear();
        return 0;
    }

    int rc = -1;
    if(strncmp(asset_spec, "proc:", 5) == 0) {
        rc = create_procedural_cutin(&gfx_st.cutin_tex, asset_spec + 5);
    } else {
        rc = vn_tex_load_raw16(&gfx_st.cutin_tex, asset_spec, 128, 128,
                               PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED);
    }

    if(rc == 0) {
        gfx_st.has_cutin = true;
        vn_copy_str(gfx_st.cutin_name, sizeof(gfx_st.cutin_name), asset_spec);
    }
    return rc;
}

void vn_cutin_clear(void) {
    vn_tex_free(&gfx_st.cutin_tex);
    gfx_st.has_cutin = false;
    gfx_st.cutin_name[0] = '\0';
}

const char *vn_cutin_get_name(void) {
    return gfx_st.cutin_name;
}

void vn_char_clear(vn_sprite_pos_t pos) {
    if((int)pos < 0 || (int)pos >= VN_MAX_SPRITES)
        return;
    vn_tex_free(&gfx_st.sprites[pos].tex);
    gfx_st.sprites[pos].active = false;
    gfx_st.sprites[pos].name[0] = '\0';
    gfx_st.sprites[pos].alpha = 0.0f;
}

void vn_char_clear_all(void) {
    for(int i = 0; i < VN_MAX_SPRITES; i++)
        vn_char_clear((vn_sprite_pos_t)i);
}

void vn_fx_fade(uint32_t target_argb, uint32_t duration_ms) {
    gfx_st.fade_rgb = target_argb & 0x00FFFFFFU;
    gfx_st.fade_start = gfx_st.fade_alpha;
    gfx_st.fade_target = (float)((target_argb >> 24) & 0xFFU) / 255.0f;
    gfx_st.fade_elapsed_ms = 0;
    gfx_st.fade_duration_ms = duration_ms;
    if(duration_ms == 0) {
        gfx_st.fade_alpha = gfx_st.fade_target;
    }
}

void vn_fx_shake(float amplitude, uint32_t duration_ms) {
    gfx_st.shake_amp = amplitude;
    gfx_st.shake_elapsed_ms = 0;
    gfx_st.shake_duration_ms = duration_ms;
    if(duration_ms == 0 || amplitude <= 0.0f) {
        gfx_st.shake_x = 0.0f;
        gfx_st.shake_y = 0.0f;
    }
}

bool vn_fx_is_busy(void) {
    return (gfx_st.fade_duration_ms > 0 && gfx_st.fade_elapsed_ms < gfx_st.fade_duration_ms) ||
           (gfx_st.shake_duration_ms > 0 && gfx_st.shake_elapsed_ms < gfx_st.shake_duration_ms);
}

void vn_gfx_update(uint32_t dt_ms) {
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_sprite_slot_t *sp = &gfx_st.sprites[i];
        if(sp->active && sp->alpha < sp->alpha_target) {
            sp->alpha += (float)dt_ms / 200.0f;
            if(sp->alpha > sp->alpha_target)
                sp->alpha = sp->alpha_target;
        }
    }

    if(gfx_st.fade_duration_ms > 0 && gfx_st.fade_elapsed_ms < gfx_st.fade_duration_ms) {
        gfx_st.fade_elapsed_ms += dt_ms;
        if(gfx_st.fade_elapsed_ms >= gfx_st.fade_duration_ms) {
            gfx_st.fade_elapsed_ms = gfx_st.fade_duration_ms;
            gfx_st.fade_alpha = gfx_st.fade_target;
        } else {
            float t = (float)gfx_st.fade_elapsed_ms / (float)gfx_st.fade_duration_ms;
            gfx_st.fade_alpha = gfx_st.fade_start + (gfx_st.fade_target - gfx_st.fade_start) * t;
        }
    }

    if(gfx_st.shake_duration_ms > 0 && gfx_st.shake_elapsed_ms < gfx_st.shake_duration_ms) {
        gfx_st.shake_elapsed_ms += dt_ms;
        if(gfx_st.shake_elapsed_ms >= gfx_st.shake_duration_ms) {
            gfx_st.shake_elapsed_ms = gfx_st.shake_duration_ms;
            gfx_st.shake_x = 0.0f;
            gfx_st.shake_y = 0.0f;
        } else {
            float decay = 1.0f - (float)gfx_st.shake_elapsed_ms / (float)gfx_st.shake_duration_ms;
            uint32_t phase = (gfx_st.shake_elapsed_ms / 24U) & 3U;
            static const float pat[4] = { 1.0f, -0.7f, -1.0f, 0.7f };
            gfx_st.shake_x = gfx_st.shake_amp * decay * pat[phase];
            gfx_st.shake_y = (gfx_st.shake_amp * 0.45f) * decay * pat[(phase + 1U) & 3U];
        }
    } else {
        gfx_st.shake_x = 0.0f;
        gfx_st.shake_y = 0.0f;
    }
}

float vn_gfx_get_shake_x(void) { return gfx_st.shake_x; }
float vn_gfx_get_shake_y(void) { return gfx_st.shake_y; }
const char *vn_gfx_get_bg_name(void) { return gfx_st.bg_name; }
const char *vn_gfx_get_char_name(vn_sprite_pos_t pos) {
    if((int)pos < 0 || (int)pos >= VN_MAX_SPRITES)
        return "";
    return gfx_st.sprites[pos].name;
}

void vn_gfx_draw_col_quad(pvr_list_t list, float x, float y, float w, float h, float z, uint32_t argb) {
    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr __attribute__((aligned(32)));
    pvr_vertex_t vert;

    pvr_poly_cxt_col(&cxt, list);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = x; vert.y = y; vert.z = z;
    vert.u = 0.0f; vert.v = 0.0f;
    vert.argb = argb; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.x = x; vert.y = y + h;
    pvr_prim(&vert, sizeof(vert));

    vert.x = x + w; vert.y = y;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x + w; vert.y = y + h;
    pvr_prim(&vert, sizeof(vert));
}

void vn_gfx_draw_txr_quad(pvr_list_t list, const vn_tex_t *tex, float x, float y, float w, float h, float z,
                          float u0, float v0, float u1, float v1, uint32_t argb) {
    if(!tex || !tex->vram_ptr)
        return;

    pvr_poly_cxt_t cxt;
    pvr_poly_hdr_t hdr __attribute__((aligned(32)));
    pvr_vertex_t vert;

    pvr_poly_cxt_txr(&cxt, list, tex->fmt, tex->width, tex->height, tex->vram_ptr, PVR_FILTER_BILINEAR);
    cxt.gen.culling = PVR_CULLING_NONE;
    pvr_poly_compile(&hdr, &cxt);
    pvr_prim(&hdr, sizeof(hdr));

    vert.flags = PVR_CMD_VERTEX;
    vert.x = x; vert.y = y; vert.z = z;
    vert.u = u0; vert.v = v0;
    vert.argb = argb; vert.oargb = 0;
    pvr_prim(&vert, sizeof(vert));

    vert.x = x; vert.y = y + h;
    vert.u = u0; vert.v = v1;
    pvr_prim(&vert, sizeof(vert));

    vert.x = x + w; vert.y = y;
    vert.u = u1; vert.v = v0;
    pvr_prim(&vert, sizeof(vert));

    vert.flags = PVR_CMD_VERTEX_EOL;
    vert.x = x + w; vert.y = y + h;
    vert.u = u1; vert.v = v1;
    pvr_prim(&vert, sizeof(vert));
}

void vn_gfx_render_frame(void) {
    static const float sprite_cx[VN_MAX_SPRITES] = { 145.0f, 320.0f, 495.0f };
    float sx = gfx_st.shake_x;
    float sy = gfx_st.shake_y;

    pvr_wait_ready();
    pvr_scene_begin();

    pvr_list_begin(PVR_LIST_OP_POLY);
    if(gfx_st.has_bg) {
        vn_gfx_draw_txr_quad(PVR_LIST_OP_POLY, &gfx_st.bg_tex,
                             sx, sy, 640.0f, 480.0f, 1.0f,
                             0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFU);
    } else {
        vn_gfx_draw_col_quad(PVR_LIST_OP_POLY, 0.0f, 0.0f, 640.0f, 480.0f, 1.0f, 0xFF0C1020U);
    }
    pvr_list_finish();

    pvr_list_begin(PVR_LIST_TR_POLY);
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_sprite_slot_t *sp = &gfx_st.sprites[i];
        if(!sp->active)
            continue;
        uint8_t a = (uint8_t)(sp->alpha * 255.0f);
        uint32_t tint = ((uint32_t)a << 24) | 0x00FFFFFFU;
        float sw = 190.0f;
        float sh = 380.0f;
        float px = sprite_cx[i] - sw * 0.5f + sx;
        float py = 480.0f - sh + sy;
        vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &sp->tex,
                             px, py, sw, sh, 10.0f + (float)i * 0.1f,
                             0.0f, 0.0f, 1.0f, 1.0f, tint);
    }

    if(gfx_st.has_cutin) {
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 236.0f + sx, 52.0f + sy, 168.0f, 168.0f, 15.0f, 0xE058D0F8U);
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 238.0f + sx, 54.0f + sy, 164.0f, 164.0f, 15.2f, 0xD80A1428U);
        vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &gfx_st.cutin_tex,
                             244.0f + sx, 60.0f + sy, 152.0f, 152.0f, 15.5f,
                             0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFU);
    }

    vn_text_render_to_pvr();

    if(gfx_st.fade_alpha > 0.004f) {
        uint8_t fa = (uint8_t)(gfx_st.fade_alpha * 255.0f);
        uint32_t fargb = ((uint32_t)fa << 24) | gfx_st.fade_rgb;
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 0.0f, 0.0f, 640.0f, 480.0f, 50.0f, fargb);
    }
    pvr_list_finish();

    pvr_scene_finish();
}
