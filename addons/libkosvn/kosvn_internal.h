/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_internal.h
 * Internal module declarations and shared helpers.
 */

#ifndef __KOSVN_INTERNAL_H
#define __KOSVN_INTERNAL_H

#include <stddef.h>
#include "kosvn/kosvn.h"

static inline void vn_copy_str(char *dst, size_t dst_sz, const char *src) {
    size_t i = 0;
    if(!dst || dst_sz == 0)
        return;
    if(!src) {
        dst[0] = '\0';
        return;
    }
    while(i + 1 < dst_sz && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Internal graphics hooks (kosvn_gfx.c) */
void        vn_gfx_update(uint32_t dt_ms);
void        vn_gfx_draw_col_quad(pvr_list_t list, float x, float y, float w, float h, float z, uint32_t argb);
void        vn_gfx_draw_txr_quad(pvr_list_t list, const vn_tex_t *tex, float x, float y, float w, float h, float z, float u0, float v0, float u1, float v1, uint32_t argb);
float       vn_gfx_get_shake_x(void);
float       vn_gfx_get_shake_y(void);
const char *vn_gfx_get_bg_name(void);
const char *vn_gfx_get_char_name(vn_sprite_pos_t pos);

/* Internal text hooks (kosvn_text.c) */
void        vn_text_set_speed(uint32_t ms_per_char);
void        vn_text_set_window_visible(bool visible);
bool        vn_text_is_window_visible(void);
const char *vn_text_get_speaker(void);
const char *vn_text_get_dialogue(void);
void        vn_text_set_site_badge(const char *badge_sjis);

/* Internal script hooks (kosvn_script.c) */
int         vn_script_jump_line(uint32_t line);
bool        vn_script_is_waiting_fx(void);
void        vn_script_clear_wait_fx(void);
bool        vn_script_is_ended(void);
void        vn_script_export_state(vn_save_state_t *st);
void        vn_script_import_state(const vn_save_state_t *st);

/* Internal core/audio hooks (kosvn_core.c) */
void        vn_audio_update(void);
const char *vn_core_get_app_id(void);
const char *vn_audio_get_bgm_name(void);

#endif /* __KOSVN_INTERNAL_H */
