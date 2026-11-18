/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_internal.h
 * Internal module declarations and shared helpers.
 */

#ifndef __KOSVN_INTERNAL_H
#define __KOSVN_INTERNAL_H

#include <stddef.h>
#include <stdio.h>
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
void        vn_gfx_draw_txr_quad4(pvr_list_t list, const vn_tex_t *tex, const float xs[4], const float ys[4], float z, float u0, float v0, float u1, float v1, uint32_t argb);
int         vn_overlay_track(const char *arg);
void        vn_overlay_lip_map(const char *arg);
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
void        vn_text_set_choice_cursor(int x, int y, int step);   /* x < 0: normal cards */
void        vn_text_set_box_visible(bool visible);
void        vn_text_set_area(uint32_t x, uint32_t y, uint32_t w);   /* w = 0: off */
void        vn_text_set_choice_cancel(const char *target);
const char *vn_text_get_choice_cancel(void);

/* Parallel stories and system requests (kosvn_core.c) */
void        vn_story_define(int id, const char *start_script, const char *label_sjis);
void        vn_story_set_current(int id);
void        vn_story_reset(void);
void        vn_core_request_load(void);
void        vn_core_swallow_input(void);
FILE       *vn_fopen(const char *path, const char *mode);   /* fopen with vn_set_path_remap */
void        vn_script_replay_scene(uint32_t line);

/* Internal script hooks (kosvn_script.c) */
int         vn_script_jump_line(uint32_t line);
bool        vn_script_is_waiting_fx(void);
void        vn_script_clear_wait_fx(void);
bool        vn_script_tick_wait(uint32_t dt_ms);

/* Overlay layers (kosvn_overlay.c) */
int         vn_overlay_command(const char *arg);
void        vn_overlay_clear_all(void);
void        vn_overlay_clear(int slot);
void        vn_overlay_update(uint32_t dt_ms);
void        vn_overlay_render(float sx, float sy);
void        vn_overlay_set_speaker(const char *arg);

/* Lip-sync (kosvn_core.c): current mouth shape 0..n, or -1 when silent */
int         vn_audio_lip_shape(void);
int         vn_audio_play_scene_se(const char *path, bool loop);
int         vn_text_set_choice_map(const char *map_raw, const char *pawn_raw);
bool        vn_text_is_map_choice(void);
void        vn_text_choice_start(void);
int         vn_text_add_choice_at(const char *label_sjis, const char *target_label,
                                  int dot_x, int dot_y, int pawn_x, int pawn_y, int rank);
void        vn_story_set_local_bits(int from, int to);
void        vn_audio_fade_bgm(uint32_t ms);
bool        vn_script_is_ended(void);
void        vn_script_export_state(vn_save_state_t *st);
void        vn_script_import_state(const vn_save_state_t *st);

/* Internal core/audio hooks (kosvn_core.c) */
void        vn_audio_update(void);
const char *vn_core_get_app_id(void);
bool        vn_core_is_skipping(void);
const char *vn_audio_get_bgm_name(void);

#endif /* __KOSVN_INTERNAL_H */
