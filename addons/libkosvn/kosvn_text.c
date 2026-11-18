/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_text.c
 * Shift-JIS Dreamcast BIOS font renderer, typewriter reveal, choices & backlog.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <kos/dbglog.h>
#include <dc/biosfont.h>
#include <dc/pvr.h>

#include "kosvn_internal.h"

#define TEXT_TEX_W      1024U
#define TEXT_TEX_H      512U
#define TEXT_CANVAS_W   640U
#define TEXT_CANVAS_H   480U
#define TEXT_LINE_STEP  28U
/* map mode name box: PS (38,167)-(283,220), 2x */
#define MAP_BOX_X       76U
#define MAP_BOX_Y       334U
#define MAP_BOX_W       490U
#define MAP_BOX_H       106U

typedef struct vn_choice_item {
    char label[128];
    char target[64];
    int  dot_x, dot_y;     /* map mode: place dot centre (640x480), -1 = none */
    int  pawn_x, pawn_y;   /* map mode: top-left of the marker */
    int  rank;             /* map mode: order in the script (lowest = start) */
} vn_choice_item_t;

typedef struct vn_backlog_entry {
    char speaker[64];
    char text[256];
} vn_backlog_entry_t;

static struct {
    bool               initialized;
    uint16_t          *canvas;
    vn_tex_t           tex;
    bool               full_dirty;
    bool               dlg_dirty;
    bool               status_dirty;
    bool               window_visible;

    char               speaker[64];
    char               dialogue[512];
    size_t             total_chars;
    size_t             visible_chars;
    uint32_t           char_interval_ms;
    uint32_t           char_accum_ms;

    uint32_t           dlg_x;
    uint32_t           dlg_y;
    size_t             dlg_byte_pos;
    size_t             dlg_drawn_chars;
    int                dlg_hang;          /* kinsoku: chars hung past the margin */

    vn_choice_item_t   choices[VN_MAX_CHOICES];
    int                choice_count;
    int                choice_selected;
    char               choice_cancel[64];  /* CANCEL target (B button), "" = none */
    int                choice_x;           /* CHOICE_LAYOUT; choice_x < 0 = default cards */
    int                choice_y;
    int                choice_step;
    bool               choice_cursor_only; /* only draw a cursor (items are in the picture) */
    bool               box_hidden;         /* TEXTBOX off: no dialogue box / speaker badge */
    bool               area_on;            /* TEXT_AREA: text in the game's own frame */
    uint32_t           area_x, area_y, area_w;
    bool               map_on;             /* CHOICE ... MAP: place selection on a map */
    vn_tex_t           map_tex, pawn_tex;
    bool               map_loaded, pawn_loaded;
    char               map_path[64], pawn_path[64];
    uint32_t           map_frames;

    vn_backlog_entry_t backlog[VN_BACKLOG_SIZE];
    int                backlog_head;
    int                backlog_count;
    bool               backlog_visible;
    int                backlog_scroll;

    char               status_line[96];
    char               site_badge[48];
} txt_st;

static bool sjis_is_wide_lead(uint8_t c) {
    uint8_t m = c & 0xF0U;
    return (m == 0x80U || m == 0x90U || m == 0xE0U);
}

/* Kinsoku: characters that must not start a line (Shift-JIS codes). When the
   line is full they hang past the margin instead (up to KINSOKU_HANG of them),
   so 「……」, 「。」 and small kana stay with the text before them. */
#define KINSOKU_HANG 2
static bool sjis_no_line_start(uint32_t code) {
    static const uint16_t tbl[] = {
        0x8141, 0x8142, 0x8143, 0x8144, 0x8145, 0x8146, 0x8147, 0x8148, 0x8149, /* 、。，．・：；？！ */
        0x814A, 0x814B, 0x8152, 0x8153, 0x8154, 0x8155, 0x8158, 0x815B,         /* ゛゜ヽヾゝゞ々ー */
        0x8160, 0x8163, 0x8164, 0x8166, 0x8168,                                 /* 〜…‥’” */
        0x816A, 0x816C, 0x816E, 0x8170, 0x8172, 0x8174, 0x8176, 0x8178, 0x817A, /* ）〕］｝〉》」』】 */
        0x829F, 0x82A1, 0x82A3, 0x82A5, 0x82A7, 0x82C1, 0x82E1, 0x82E3, 0x82E5, 0x82EC, /* ぁぃぅぇぉっゃゅょゎ */
        0x8340, 0x8342, 0x8344, 0x8346, 0x8348, 0x8362, 0x8383, 0x8385, 0x8387, 0x838E, /* ァィゥェォッャュョヮ */
        0x8395, 0x8396                                                          /* ヵヶ */
    };
    if(code < 0x80U)
        return code == '!' || code == '?' || code == ')' || code == ',' || code == '.';
    for(size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++)
        if(tbl[i] == code)
            return true;
    return false;
}

static size_t sjis_next_char(const char *s, size_t pos, uint32_t *code_out, bool *wide_out, bool *kana_out) {
    uint8_t c0 = (uint8_t)s[pos];
    if(c0 == 0) {
        *code_out = 0;
        *wide_out = false;
        *kana_out = false;
        return 0;
    }

    if((c0 & 0x80U) != 0U) {
        if(sjis_is_wide_lead(c0) && s[pos + 1] != '\0') {
            uint8_t c1 = (uint8_t)s[pos + 1];
            *code_out = ((uint32_t)c0 << 8) | (uint32_t)c1;
            *wide_out = true;
            *kana_out = false;
            return 2;
        }
        *code_out = c0;
        *wide_out = false;
        *kana_out = true;
        return 1;
    }

    *code_out = c0;
    *wide_out = false;
    *kana_out = false;
    return 1;
}

static size_t sjis_count_chars(const char *s) {
    size_t pos = 0;
    size_t count = 0;
    while(s[pos] != '\0') {
        uint32_t code = 0;
        bool wide = false, kana = false;
        size_t adv = sjis_next_char(s, pos, &code, &wide, &kana);
        if(adv == 0)
            break;
        pos += adv;
        count++;
    }
    return count;
}

static void draw_sjis_block(uint32_t x0, uint32_t y0, uint32_t max_w, uint32_t max_y,
                            uint16_t fg_argb4444, const char *str, size_t max_chars) {
    uint32_t x = x0;
    uint32_t y = y0;
    size_t pos = 0;
    size_t drawn = 0;
    int hang = 0;

    while(str[pos] != '\0' && drawn < max_chars) {
        if(str[pos] == '\n') {
            x = x0;
            y += TEXT_LINE_STEP;
            pos++;
            drawn++;
            continue;
        }
        if(str[pos] == '\r') {
            pos++;
            continue;
        }

        uint32_t code = 0;
        bool wide = false, kana = false;
        size_t adv = sjis_next_char(str, pos, &code, &wide, &kana);
        if(adv == 0)
            break;

        uint32_t glyph_w = wide ? BFONT_WIDE_WIDTH : BFONT_THIN_WIDTH;
        if(x - x0 + glyph_w > max_w) {
            if(sjis_no_line_start(code) && hang < KINSOKU_HANG) {
                hang++;
            } else {
                x = x0;
                y += TEXT_LINE_STEP;
                hang = 0;
            }
        } else {
            hang = 0;
        }

        if(y + BFONT_HEIGHT + 1U > max_y || x + glyph_w + 1U > TEXT_TEX_W)
            break;

        uint16_t *sh_ptr = txt_st.canvas + (size_t)(y + 1U) * TEXT_TEX_W + (x + 1U);
        bfont_draw_ex(sh_ptr, TEXT_TEX_W, 0xC000U, 0x0000U, 16, false, code, wide, kana);

        uint16_t *fg_ptr = txt_st.canvas + (size_t)y * TEXT_TEX_W + x;
        bfont_draw_ex(fg_ptr, TEXT_TEX_W, fg_argb4444, 0x0000U, 16, false, code, wide, kana);

        x += glyph_w;
        pos += adv;
        drawn++;
    }
}

static void upload_canvas_rows(uint32_t y0, uint32_t y1) {
    if(!txt_st.canvas || !txt_st.tex.vram_ptr)
        return;
    if(y1 > TEXT_TEX_H)
        y1 = TEXT_TEX_H;
    if(y0 >= y1)
        return;
    size_t offset_words = (size_t)y0 * TEXT_TEX_W;
    size_t bytes = (size_t)(y1 - y0) * TEXT_TEX_W * sizeof(uint16_t);
    pvr_txr_load(txt_st.canvas + offset_words,
                 (pvr_ptr_t)((uintptr_t)txt_st.tex.vram_ptr + offset_words * sizeof(uint16_t)),
                 bytes);
}

static uint32_t dlg_origin_x(void) { return txt_st.area_on ? txt_st.area_x : 24U; }
static uint32_t dlg_origin_y(void) {
    return txt_st.area_on ? txt_st.area_y + TEXT_LINE_STEP : 330U;
}

static void draw_dialogue_incremental(size_t target_chars, uint32_t *y_min_out, uint32_t *y_max_out) {
    const uint32_t x0 = dlg_origin_x();
    const uint32_t max_w = txt_st.area_on ? txt_st.area_w : 592U;
    const uint32_t max_y = txt_st.area_on ? txt_st.area_y + 4U * TEXT_LINE_STEP + 2U : 446U;
    uint32_t y_min = TEXT_CANVAS_H;
    uint32_t y_max = 0U;

    while(txt_st.dialogue[txt_st.dlg_byte_pos] != '\0' && txt_st.dlg_drawn_chars < target_chars) {
        char ch = txt_st.dialogue[txt_st.dlg_byte_pos];
        if(ch == '\n') {
            txt_st.dlg_x = x0;
            txt_st.dlg_y += TEXT_LINE_STEP;
            txt_st.dlg_byte_pos++;
            txt_st.dlg_drawn_chars++;
            continue;
        }
        if(ch == '\r') {
            txt_st.dlg_byte_pos++;
            continue;
        }

        uint32_t code = 0;
        bool wide = false, kana = false;
        size_t adv = sjis_next_char(txt_st.dialogue, txt_st.dlg_byte_pos, &code, &wide, &kana);
        if(adv == 0)
            break;

        uint32_t glyph_w = wide ? BFONT_WIDE_WIDTH : BFONT_THIN_WIDTH;
        if(txt_st.dlg_x - x0 + glyph_w > max_w) {
            if(sjis_no_line_start(code) && txt_st.dlg_hang < KINSOKU_HANG) {
                txt_st.dlg_hang++;          /* hang it past the margin */
            } else {
                txt_st.dlg_x = x0;
                txt_st.dlg_y += TEXT_LINE_STEP;
                txt_st.dlg_hang = 0;
            }
        } else {
            txt_st.dlg_hang = 0;
        }

        if(txt_st.dlg_y + BFONT_HEIGHT + 1U > max_y || txt_st.dlg_x + glyph_w + 1U > TEXT_TEX_W) {
            txt_st.dlg_drawn_chars = target_chars;
            break;
        }

        if(txt_st.dlg_y < y_min)
            y_min = txt_st.dlg_y;
        if(txt_st.dlg_y + BFONT_HEIGHT + 2U > y_max)
            y_max = txt_st.dlg_y + BFONT_HEIGHT + 2U;

        uint16_t *sh_ptr = txt_st.canvas + (size_t)(txt_st.dlg_y + 1U) * TEXT_TEX_W + (txt_st.dlg_x + 1U);
        bfont_draw_ex(sh_ptr, TEXT_TEX_W, 0xC000U, 0x0000U, 16, false, code, wide, kana);

        uint16_t *fg_ptr = txt_st.canvas + (size_t)txt_st.dlg_y * TEXT_TEX_W + txt_st.dlg_x;
        bfont_draw_ex(fg_ptr, TEXT_TEX_W, 0xFFFFU, 0x0000U, 16, false, code, wide, kana);

        txt_st.dlg_x += glyph_w;
        txt_st.dlg_byte_pos += adv;
        txt_st.dlg_drawn_chars++;
    }

    if(y_min_out)
        *y_min_out = y_min;
    if(y_max_out)
        *y_max_out = y_max;
}

static void rebuild_canvas(void) {
    if(!txt_st.canvas || !txt_st.tex.vram_ptr)
        return;

    if(txt_st.full_dirty) {
        memset(txt_st.canvas, 0, (size_t)TEXT_TEX_W * TEXT_CANVAS_H * sizeof(uint16_t));

        if(txt_st.backlog_visible) {
            draw_sjis_block(32U, 26U, 576U, 56U, 0xF6EFU,
                            "=== BACKLOG (DPAD:Scroll  A/B/Y:Close) ===", 96U);

            int max_show = 7;
            int start = txt_st.backlog_scroll;
            if(start > txt_st.backlog_count - 1)
                start = txt_st.backlog_count - 1;
            if(start < 0)
                start = 0;

            uint32_t cy = 68U;
            for(int row = 0; row < max_show && (start + row) < txt_st.backlog_count; row++) {
                int offset_from_newest = start + row;
                int idx = (txt_st.backlog_head - 1 - offset_from_newest + VN_BACKLOG_SIZE * 2) % VN_BACKLOG_SIZE;
                const vn_backlog_entry_t *e = &txt_st.backlog[idx];
                if(e->speaker[0] != '\0') {
                    draw_sjis_block(32U, cy, 132U, cy + 28U, 0xFFD6U, e->speaker, 64U);
                    draw_sjis_block(172U, cy, 436U, cy + 54U, 0xFFFFU, e->text, 128U);
                } else {
                    draw_sjis_block(32U, cy, 576U, cy + 54U, 0xFEEFU, e->text, 128U);
                }
                cy += 56U;
                if(cy >= 445U)
                    break;
            }
        } else if(txt_st.window_visible) {
            if(txt_st.site_badge[0] != '\0') {
                draw_sjis_block(416U, 17U, 204U, 44U, 0xF8FFU, txt_st.site_badge, 48U);
            }

            if(txt_st.speaker[0] != '\0' && !txt_st.map_on) {
                if(txt_st.area_on)     /* first row of the frame's text window */
                    draw_sjis_block(txt_st.area_x, txt_st.area_y, txt_st.area_w,
                                    txt_st.area_y + TEXT_LINE_STEP, 0xFFD6U, txt_st.speaker, 64U);
                else
                    draw_sjis_block(36U, 289U, 216U, 316U, 0xFFD6U, txt_st.speaker, 64U);
            }

            txt_st.dlg_x = dlg_origin_x();
            txt_st.dlg_y = dlg_origin_y();
            txt_st.dlg_byte_pos = 0U;
            txt_st.dlg_drawn_chars = 0U;
            txt_st.dlg_hang = 0;
            if(txt_st.dialogue[0] != '\0' && !txt_st.map_on) {
                draw_dialogue_incremental(txt_st.visible_chars, NULL, NULL);
            }

            if(txt_st.map_on && txt_st.choice_count > 0) {
                /* map mode: the selected place name in the grey box (PS 38,167) */
                draw_sjis_block(MAP_BOX_X + 10U, MAP_BOX_Y + 10U, MAP_BOX_W - 20U, MAP_BOX_Y + 40U,
                                0xFFFFU, txt_st.choices[txt_st.choice_selected].label, 64U);
            } else if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars &&
               txt_st.choice_cursor_only) {
                uint32_t cx = (uint32_t)txt_st.choice_x;
                uint32_t cy = (uint32_t)txt_st.choice_y + (uint32_t)(txt_st.choice_selected * txt_st.choice_step);
                draw_sjis_block(cx >= 26U ? cx - 26U : 0U, cy, 24U, cy + 26U, 0xFFD4U, "\x81\x9c", 4U); /* ● */
            } else if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars) {
                uint32_t cy = (txt_st.choice_count > 4) ? 48U : 100U;
                uint32_t cy_step = (txt_st.choice_count > 4) ? 38U : 44U;
                uint32_t txt_off = (txt_st.choice_count > 4) ? 4U : 6U;
                for(int i = 0; i < txt_st.choice_count; i++) {
                    uint16_t col = (i == txt_st.choice_selected) ? 0xFFD4U : 0xFEEFU;
                    if(i == txt_st.choice_selected)
                        draw_sjis_block(74U, cy + txt_off, 24U, cy + txt_off + 26U, 0xFFD4U, ">", 4U);
                    draw_sjis_block(96U, cy + txt_off, 472U, cy + txt_off + 26U, col, txt_st.choices[i].label, 128U);
                    cy += cy_step;
                }
            }
        }

        if(txt_st.status_line[0] != '\0') {
            draw_sjis_block(24U, 452U, 592U, 478U, 0xF8EEU, txt_st.status_line, 96U);
        }

        upload_canvas_rows(0U, TEXT_CANVAS_H);
        txt_st.full_dirty = false;
        txt_st.dlg_dirty = false;
        txt_st.status_dirty = false;
        return;
    }

    if(txt_st.dlg_dirty) {
        if(!txt_st.backlog_visible && txt_st.window_visible && txt_st.dialogue[0] != '\0' && !txt_st.map_on) {
            uint32_t y0 = TEXT_CANVAS_H;
            uint32_t y1 = 0U;
            draw_dialogue_incremental(txt_st.visible_chars, &y0, &y1);
            upload_canvas_rows(y0, y1);
        }
        txt_st.dlg_dirty = false;
    }

    if(txt_st.status_dirty) {
        memset(txt_st.canvas + (size_t)450U * TEXT_TEX_W, 0,
               (size_t)(TEXT_CANVAS_H - 450U) * TEXT_TEX_W * sizeof(uint16_t));
        if(txt_st.status_line[0] != '\0') {
            draw_sjis_block(24U, 452U, 592U, 478U, 0xF8EEU, txt_st.status_line, 96U);
        }
        upload_canvas_rows(450U, TEXT_CANVAS_H);
        txt_st.status_dirty = false;
    }
}

int vn_text_init(void) {
    if(txt_st.initialized)
        return 0;

    memset(&txt_st, 0, sizeof(txt_st));
    bfont_set_encoding(BFONT_CODE_SJIS);

    size_t bytes = (size_t)TEXT_TEX_W * TEXT_TEX_H * sizeof(uint16_t);
    txt_st.canvas = (uint16_t *)memalign(32, bytes);
    if(!txt_st.canvas)
        return -1;
    memset(txt_st.canvas, 0, bytes);

    txt_st.tex.vram_ptr = pvr_mem_malloc(bytes);
    if(!txt_st.tex.vram_ptr) {
        free(txt_st.canvas);
        txt_st.canvas = NULL;
        return -1;
    }

    txt_st.tex.width = TEXT_TEX_W;
    txt_st.tex.height = TEXT_TEX_H;
    txt_st.tex.fmt = PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED;
    txt_st.window_visible = true;
    txt_st.char_interval_ms = 28U;
    txt_st.full_dirty = true;
    txt_st.initialized = true;
    return 0;
}

void vn_text_shutdown(void) {
    if(!txt_st.initialized)
        return;

    if(txt_st.canvas) {
        free(txt_st.canvas);
        txt_st.canvas = NULL;
    }
    vn_tex_free(&txt_st.tex);
    txt_st.initialized = false;
}

void vn_text_set_speed(uint32_t ms_per_char) {
    txt_st.char_interval_ms = (ms_per_char == 0U) ? 1U : ms_per_char;
}

void vn_text_set_window_visible(bool visible) {
    if(txt_st.window_visible != visible) {
        txt_st.window_visible = visible;
        txt_st.full_dirty = true;
    }
}

bool vn_text_is_window_visible(void) {
    return txt_st.window_visible;
}

void vn_text_set_speaker(const char *speaker_sjis) {
    const char *spk = speaker_sjis ? speaker_sjis : "";
    if(strcmp(txt_st.speaker, spk) == 0)
        return;
    vn_copy_str(txt_st.speaker, sizeof(txt_st.speaker), spk);
    txt_st.full_dirty = true;
}

void vn_text_set_dialogue(const char *text_sjis) {
    vn_copy_str(txt_st.dialogue, sizeof(txt_st.dialogue), text_sjis);
    txt_st.total_chars = sjis_count_chars(txt_st.dialogue);
    txt_st.visible_chars = 0;
    txt_st.char_accum_ms = 0;
    txt_st.dlg_x = dlg_origin_x();
    txt_st.dlg_y = dlg_origin_y();
    txt_st.dlg_byte_pos = 0U;
    txt_st.dlg_drawn_chars = 0U;
    txt_st.full_dirty = true;
}

const char *vn_text_get_speaker(void) { return txt_st.speaker; }
const char *vn_text_get_dialogue(void) { return txt_st.dialogue; }

void vn_text_update(uint32_t dt_ms, bool fast_forward) {
    if(txt_st.visible_chars >= txt_st.total_chars)
        return;

    if(fast_forward) {
        txt_st.visible_chars = txt_st.total_chars;
        txt_st.dlg_dirty = true;
        if(txt_st.choice_count > 0)
            txt_st.full_dirty = true;
        return;
    }

    txt_st.char_accum_ms += dt_ms;
    while(txt_st.char_accum_ms >= txt_st.char_interval_ms &&
          txt_st.visible_chars < txt_st.total_chars) {
        txt_st.char_accum_ms -= txt_st.char_interval_ms;
        txt_st.visible_chars++;
        txt_st.dlg_dirty = true;
    }
    if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars)
        txt_st.full_dirty = true;
}

bool vn_text_is_complete(void) {
    return txt_st.visible_chars >= txt_st.total_chars;
}

void vn_text_reveal_all(void) {
    if(txt_st.visible_chars < txt_st.total_chars) {
        txt_st.visible_chars = txt_st.total_chars;
        txt_st.dlg_dirty = true;
        if(txt_st.choice_count > 0)
            txt_st.full_dirty = true;
    }
}

void vn_text_set_choice_cursor(int x, int y, int step) {
    txt_st.choice_cursor_only = (x >= 0);
    txt_st.choice_x = x;
    txt_st.choice_y = y;
    txt_st.choice_step = step;
    txt_st.full_dirty = true;
}

/* TEXT_AREA x y w: draw the speaker on the first row and the dialogue on
   the next three rows of a text window that is part of the background (the
   PS dialogue frame), with no box of our own. w = 0 turns it off. */
void vn_text_set_area(uint32_t x, uint32_t y, uint32_t w) {
    txt_st.area_on = w > 0U;
    txt_st.area_x = x;
    txt_st.area_y = y;
    txt_st.area_w = w;
    txt_st.full_dirty = true;
}

void vn_text_set_box_visible(bool visible) {
    txt_st.box_hidden = !visible;
    txt_st.full_dirty = true;
}

void vn_text_set_choice_cancel(const char *target) {
    vn_copy_str(txt_st.choice_cancel, sizeof(txt_st.choice_cancel), target);
}

const char *vn_text_get_choice_cancel(void) { return txt_st.choice_cancel; }

static void map_free(void) {
    if(txt_st.map_loaded)
        vn_tex_free(&txt_st.map_tex);
    if(txt_st.pawn_loaded)
        vn_tex_free(&txt_st.pawn_tex);
    txt_st.map_loaded = txt_st.pawn_loaded = false;
    txt_st.map_path[0] = txt_st.pawn_path[0] = '\0';
}

/* MAP <map raw16 512x256 RGB565> <marker raw16 32x64 ARGB4444> inside a
   CHOICE: the options are places on that map (OPTION @dx,dy,px,py,rank ...).
   The textures stay loaded while the same map is offered again. */
int vn_text_set_choice_map(const char *map_raw, const char *pawn_raw) {
    if(strcmp(map_raw, txt_st.map_path) != 0 || strcmp(pawn_raw, txt_st.pawn_path) != 0) {
        map_free();
        if(vn_tex_load_raw16(&txt_st.map_tex, map_raw, 512U, 256U,
                             PVR_TXRFMT_RGB565 | PVR_TXRFMT_NONTWIDDLED) < 0)
            return -1;
        txt_st.map_loaded = true;
        txt_st.pawn_loaded = vn_tex_load_raw16(&txt_st.pawn_tex, pawn_raw, 32U, 64U,
                                               PVR_TXRFMT_ARGB4444 | PVR_TXRFMT_NONTWIDDLED) == 0;
        vn_copy_str(txt_st.map_path, sizeof(txt_st.map_path), map_raw);
        vn_copy_str(txt_st.pawn_path, sizeof(txt_st.pawn_path), pawn_raw);
    }
    txt_st.map_on = true;
    txt_st.full_dirty = true;
    return 0;
}

bool vn_text_is_map_choice(void) { return txt_st.map_on && txt_st.choice_count > 0; }

/* Map mode: start on the option that comes first in the script. */
void vn_text_choice_start(void) {
    if(!txt_st.map_on)
        return;
    int best = 0;
    for(int i = 1; i < txt_st.choice_count; i++)
        if(txt_st.choices[i].rank < txt_st.choices[best].rank)
            best = i;
    txt_st.choice_selected = best;
    txt_st.full_dirty = true;
}

void vn_text_clear_choices(void) {
    txt_st.choice_cancel[0] = '\0';
    if(txt_st.map_on) {
        txt_st.map_on = false;     /* textures kept for the next map choice */
        txt_st.full_dirty = true;
    }
    if(txt_st.choice_count > 0) {
        txt_st.choice_count = 0;
        txt_st.choice_selected = 0;
        txt_st.full_dirty = true;
    }
}

int vn_text_add_choice_at(const char *label_sjis, const char *target_label,
                          int dot_x, int dot_y, int pawn_x, int pawn_y, int rank) {
    if(vn_text_add_choice(label_sjis, target_label) < 0)
        return -1;
    vn_choice_item_t *c = &txt_st.choices[txt_st.choice_count - 1];
    c->dot_x = dot_x; c->dot_y = dot_y;
    c->pawn_x = pawn_x; c->pawn_y = pawn_y;
    c->rank = rank;
    return 0;
}

int vn_text_add_choice(const char *label_sjis, const char *target_label) {
    if(txt_st.choice_count >= VN_MAX_CHOICES || !label_sjis || !target_label)
        return -1;
    vn_choice_item_t *c = &txt_st.choices[txt_st.choice_count];
    c->dot_x = c->dot_y = c->pawn_x = c->pawn_y = -1;
    c->rank = txt_st.choice_count;
    vn_copy_str(c->label, sizeof(c->label), label_sjis);
    vn_copy_str(c->target, sizeof(c->target), target_label);
    txt_st.choice_count++;
    txt_st.full_dirty = true;
    return 0;
}

int vn_text_get_choice_count(void) { return txt_st.choice_count; }
int vn_text_get_selected_choice(void) { return txt_st.choice_selected; }

void vn_text_move_choice(int delta) {
    if(txt_st.choice_count <= 0)
        return;
    int next = (txt_st.choice_selected + delta) % txt_st.choice_count;
    if(next < 0)
        next += txt_st.choice_count;
    if(next != txt_st.choice_selected) {
        txt_st.choice_selected = next;
        txt_st.full_dirty = true;
    }
}

const char *vn_text_get_choice_target(int index) {
    if(index < 0 || index >= txt_st.choice_count)
        return NULL;
    return txt_st.choices[index].target;
}

void vn_backlog_push(const char *speaker_sjis, const char *text_sjis) {
    if(!text_sjis || text_sjis[0] == '\0')
        return;
    const char *spk = speaker_sjis ? speaker_sjis : "";
    if(txt_st.backlog_count > 0) {
        int latest = (txt_st.backlog_head - 1 + VN_BACKLOG_SIZE) % VN_BACKLOG_SIZE;
        if(strcmp(txt_st.backlog[latest].speaker, spk) == 0 &&
           strcmp(txt_st.backlog[latest].text, text_sjis) == 0)
            return;
    }
    vn_backlog_entry_t *e = &txt_st.backlog[txt_st.backlog_head];
    vn_copy_str(e->speaker, sizeof(e->speaker), spk);
    vn_copy_str(e->text, sizeof(e->text), text_sjis);
    txt_st.backlog_head = (txt_st.backlog_head + 1) % VN_BACKLOG_SIZE;
    if(txt_st.backlog_count < VN_BACKLOG_SIZE)
        txt_st.backlog_count++;
}

void vn_backlog_set_visible(bool visible) {
    if(txt_st.backlog_visible != visible) {
        txt_st.backlog_visible = visible;
        txt_st.backlog_scroll = 0;
        txt_st.full_dirty = true;
    }
}

bool vn_backlog_is_visible(void) { return txt_st.backlog_visible; }

void vn_backlog_scroll(int delta) {
    int max_scroll = (txt_st.backlog_count > 1) ? (txt_st.backlog_count - 1) : 0;
    int next = txt_st.backlog_scroll + delta;
    if(next < 0) next = 0;
    if(next > max_scroll) next = max_scroll;
    if(next != txt_st.backlog_scroll) {
        txt_st.backlog_scroll = next;
        txt_st.full_dirty = true;
    }
}

void vn_text_set_status_overlay(const char *status_line) {
    char prev[96];
    vn_copy_str(prev, sizeof(prev), txt_st.status_line);
    vn_copy_str(txt_st.status_line, sizeof(txt_st.status_line), status_line);
    if(strcmp(prev, txt_st.status_line) != 0)
        txt_st.status_dirty = true;
}

void vn_text_set_site_badge(const char *badge_sjis) {
    char prev[48];
    vn_copy_str(prev, sizeof(prev), txt_st.site_badge);
    vn_copy_str(txt_st.site_badge, sizeof(txt_st.site_badge), badge_sjis);
    if(strcmp(prev, txt_st.site_badge) != 0)
        txt_st.full_dirty = true;
}

void vn_text_render_to_pvr(void) {
    if(!txt_st.initialized)
        return;

    if(txt_st.full_dirty || txt_st.dlg_dirty || txt_st.status_dirty)
        rebuild_canvas();

    float sx = vn_gfx_get_shake_x();
    float sy = vn_gfx_get_shake_y();

    if(txt_st.backlog_visible) {
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 20.0f, 20.0f, 600.0f, 426.0f, 20.0f, 0xDF0B1222U);
    } else if(txt_st.map_on && txt_st.choice_count > 0 && txt_st.map_loaded) {
        /* Map: picture, a pulsing dot per place, the marker on the selected
           one, its name in a grey box (PS layout, 2x). */
        txt_st.map_frames++;
        vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &txt_st.map_tex, 0.0f, 0.0f, 640.0f, 480.0f, 18.0f,
                             0.0f, 0.0f, 320.0f / 512.0f, 240.0f / 256.0f, 0xFFFFFFFFU);
        uint32_t a = 0x80U + ((txt_st.map_frames * 4U) & 0x7FU);
        for(int i = 0; i < txt_st.choice_count; i++) {
            const vn_choice_item_t *c = &txt_st.choices[i];
            if(c->dot_x < 0)
                continue;
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, (float)c->dot_x - 8.0f, (float)c->dot_y - 8.0f,
                                 16.0f, 16.0f, 18.5f, (a << 24) | 0xFFFFFFU);
        }
        const vn_choice_item_t *s = &txt_st.choices[txt_st.choice_selected];
        if(s->pawn_x >= 0 && txt_st.pawn_loaded)
            vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &txt_st.pawn_tex, (float)s->pawn_x, (float)s->pawn_y,
                                 48.0f, 96.0f, 19.0f, 0.0f, 0.0f, 24.0f / 32.0f, 48.0f / 64.0f, 0xFFFFFFFFU);
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, (float)MAP_BOX_X, (float)MAP_BOX_Y,
                             (float)MAP_BOX_W, (float)MAP_BOX_H, 19.5f, 0xC0505050U);
    } else if(txt_st.window_visible) {
        /* Multi-Sight HUD badge */
        if(txt_st.site_badge[0] != '\0') {
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 404.0f + sx, 12.0f + sy, 224.0f, 34.0f, 20.0f, 0xE048B8E8U);
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 406.0f + sx, 14.0f + sy, 220.0f, 30.0f, 20.5f, 0xD8102038U);
        }
        /* Speaker badge */
        if(txt_st.speaker[0] != '\0' && !txt_st.box_hidden && !txt_st.area_on) {
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 24.0f + sx, 284.0f + sy, 240.0f, 34.0f, 20.0f, 0xE03A70B8U);
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 26.0f + sx, 286.0f + sy, 236.0f, 30.0f, 20.5f, 0xD8152340U);
        }
        /* Main dialogue box */
        if(!txt_st.box_hidden && !txt_st.area_on) {
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 12.0f + sx, 320.0f + sy, 616.0f, 130.0f, 20.0f, 0xDF68B0F8U);
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 15.0f + sx, 323.0f + sy, 610.0f, 124.0f, 20.5f, 0xCE0D1528U);
        }

        /* Choice cards */
        if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars &&
           !txt_st.choice_cursor_only) {
            float cy = (txt_st.choice_count > 4) ? 48.0f : 100.0f;
            float cy_step = (txt_st.choice_count > 4) ? 38.0f : 44.0f;
            float card_h = (txt_st.choice_count > 4) ? 32.0f : 36.0f;
            for(int i = 0; i < txt_st.choice_count; i++) {
                uint32_t border = (i == txt_st.choice_selected) ? 0xFFF2C94CU : 0xD05888C8U;
                uint32_t fill   = (i == txt_st.choice_selected) ? 0xE0253A68U : 0xCC121E38U;
                vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 60.0f, cy, 520.0f, card_h, 21.0f, border);
                vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 62.0f, cy + 2.0f, 516.0f, card_h - 4.0f, 21.5f, fill);
                cy += cy_step;
            }
        }
    }

    vn_gfx_draw_txr_quad(PVR_LIST_TR_POLY, &txt_st.tex,
                         0.0f + sx, 0.0f + sy, (float)TEXT_CANVAS_W, (float)TEXT_CANVAS_H, 25.0f,
                         0.0f, 0.0f,
                         (float)TEXT_CANVAS_W / (float)TEXT_TEX_W,
                         (float)TEXT_CANVAS_H / (float)TEXT_TEX_H,
                         0xFFFFFFFFU);
}
