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

typedef struct vn_choice_item {
    char label[128];
    char target[64];
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

    vn_choice_item_t   choices[VN_MAX_CHOICES];
    int                choice_count;
    int                choice_selected;

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
            x = x0;
            y += TEXT_LINE_STEP;
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

static void draw_dialogue_incremental(size_t target_chars, uint32_t *y_min_out, uint32_t *y_max_out) {
    const uint32_t x0 = 24U;
    const uint32_t max_w = 592U;
    const uint32_t max_y = 446U;
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
            txt_st.dlg_x = x0;
            txt_st.dlg_y += TEXT_LINE_STEP;
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

            if(txt_st.speaker[0] != '\0') {
                draw_sjis_block(36U, 289U, 216U, 316U, 0xFFD6U, txt_st.speaker, 64U);
            }

            txt_st.dlg_x = 24U;
            txt_st.dlg_y = 330U;
            txt_st.dlg_byte_pos = 0U;
            txt_st.dlg_drawn_chars = 0U;
            if(txt_st.dialogue[0] != '\0') {
                draw_dialogue_incremental(txt_st.visible_chars, NULL, NULL);
            }

            if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars) {
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
        if(!txt_st.backlog_visible && txt_st.window_visible && txt_st.dialogue[0] != '\0') {
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
    txt_st.dlg_x = 24U;
    txt_st.dlg_y = 330U;
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

void vn_text_clear_choices(void) {
    if(txt_st.choice_count > 0) {
        txt_st.choice_count = 0;
        txt_st.choice_selected = 0;
        txt_st.full_dirty = true;
    }
}

int vn_text_add_choice(const char *label_sjis, const char *target_label) {
    if(txt_st.choice_count >= VN_MAX_CHOICES || !label_sjis || !target_label)
        return -1;
    vn_choice_item_t *c = &txt_st.choices[txt_st.choice_count];
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
    } else if(txt_st.window_visible) {
        /* Multi-Sight HUD badge */
        if(txt_st.site_badge[0] != '\0') {
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 404.0f + sx, 12.0f + sy, 224.0f, 34.0f, 20.0f, 0xE048B8E8U);
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 406.0f + sx, 14.0f + sy, 220.0f, 30.0f, 20.5f, 0xD8102038U);
        }
        /* Speaker badge */
        if(txt_st.speaker[0] != '\0') {
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 24.0f + sx, 284.0f + sy, 240.0f, 34.0f, 20.0f, 0xE03A70B8U);
            vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 26.0f + sx, 286.0f + sy, 236.0f, 30.0f, 20.5f, 0xD8152340U);
        }
        /* Main dialogue box */
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 12.0f + sx, 320.0f + sy, 616.0f, 130.0f, 20.0f, 0xDF68B0F8U);
        vn_gfx_draw_col_quad(PVR_LIST_TR_POLY, 15.0f + sx, 323.0f + sy, 610.0f, 124.0f, 20.5f, 0xCE0D1528U);

        /* Choice cards */
        if(txt_st.choice_count > 0 && txt_st.visible_chars >= txt_st.total_chars) {
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
