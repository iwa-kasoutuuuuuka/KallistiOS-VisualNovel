/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * include/kosvn/kosvn.h
 * Dedicated C Visual Novel Engine layer for Sega Dreamcast (KallistiOS-VN).
 */

#ifndef __KOSVN_KOSVN_H
#define __KOSVN_KOSVN_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/cdefs.h>
#include <kos/cdefs.h>
#include <dc/pvr.h>

__BEGIN_DECLS

#define VN_MAX_SPRITES   3
#define VN_MAX_CHOICES   4
#define VN_MAX_FLAGS     64
#define VN_MAX_LABELS    64
#define VN_BACKLOG_SIZE  32
#define VN_SAVE_MAGIC    0x4B564E31U /* "KVN1" */
#define VN_SAVE_VERSION  1U

typedef enum vn_sprite_pos {
    VN_POS_LEFT   = 0,
    VN_POS_CENTER = 1,
    VN_POS_RIGHT  = 2
} vn_sprite_pos_t;

typedef struct vn_config {
    const char *title;
    const char *app_id;
    uint16_t    net_debug_port;
    const char *net_debug_ip;
    uint32_t    text_speed_ms;
    uint32_t    auto_advance_ms;
} vn_config_t;

typedef struct vn_tex {
    pvr_ptr_t vram_ptr;
    uint16_t  width;
    uint16_t  height;
    uint32_t  fmt;
} vn_tex_t;

typedef struct vn_save_state {
    uint32_t magic;
    uint32_t version;
    uint32_t script_line;
    char     script_path[64];
    char     bg_name[64];
    char     char_names[VN_MAX_SPRITES][64];
    char     bgm_name[64];
    int32_t  flags[VN_MAX_FLAGS];
} vn_save_state_t;

/* Core lifecycle & main loop */
int  vn_init(const vn_config_t *cfg);
void vn_shutdown(void);
int  vn_run_script(const char *script_path);
int  vn_step_frame(void);

/* Graphics & PVR2 compositor */
int  vn_gfx_init(void);
void vn_gfx_shutdown(void);
int  vn_tex_load_raw16(vn_tex_t *tex, const char *path, uint16_t w, uint16_t h, uint32_t fmt);
int  vn_tex_create_procedural_bg(vn_tex_t *tex, uint16_t w, uint16_t h, int style_id);
int  vn_tex_create_procedural_portrait(vn_tex_t *tex, uint16_t w, uint16_t h, int char_id, int expr_id);
void vn_tex_free(vn_tex_t *tex);
int  vn_bg_set(const char *asset_spec);
int  vn_char_set(vn_sprite_pos_t pos, const char *asset_spec);
void vn_char_clear(vn_sprite_pos_t pos);
void vn_char_clear_all(void);
void vn_fx_fade(uint32_t target_argb, uint32_t duration_ms);
void vn_fx_shake(float amplitude, uint32_t duration_ms);
bool vn_fx_is_busy(void);
void vn_gfx_render_frame(void);

/* Text, Dreamcast BIOS font (Shift-JIS), choices & backlog */
int         vn_text_init(void);
void        vn_text_shutdown(void);
void        vn_text_set_speaker(const char *speaker_sjis);
void        vn_text_set_dialogue(const char *text_sjis);
void        vn_text_update(uint32_t dt_ms, bool fast_forward);
bool        vn_text_is_complete(void);
void        vn_text_reveal_all(void);
void        vn_text_clear_choices(void);
int         vn_text_add_choice(const char *label_sjis, const char *target_label);
int         vn_text_get_choice_count(void);
int         vn_text_get_selected_choice(void);
void        vn_text_move_choice(int delta);
const char *vn_text_get_choice_target(int index);
void        vn_backlog_push(const char *speaker_sjis, const char *text_sjis);
void        vn_backlog_set_visible(bool visible);
bool        vn_backlog_is_visible(void);
void        vn_backlog_scroll(int delta);
void        vn_text_set_status_overlay(const char *status_line);
void        vn_text_render_to_pvr(void);

/* Scenario script VM */
int         vn_script_load(const char *path);
void        vn_script_unload(void);
int         vn_script_jump_label(const char *label);
int         vn_script_advance(void);
void        vn_script_set_flag(int idx, int32_t val);
int32_t     vn_script_get_flag(int idx);
uint32_t    vn_script_get_line(void);
const char *vn_script_get_path(void);

/* VMU save/load & LCD */
int  vn_vmu_save(int slot, const vn_save_state_t *state);
int  vn_vmu_load(int slot, vn_save_state_t *state_out);
void vn_vmu_draw_logo(const char *short_tag);

/* AICA Audio (BGM stream & SFX) */
int  vn_audio_play_bgm(const char *path_or_spec);
void vn_audio_stop_bgm(void);
int  vn_audio_play_se(const char *path_or_spec);

/* LAN / UDP debug telemetry */
int  vn_netdbg_init(const char *dest_ip, uint16_t port);
void vn_netdbg_shutdown(void);
void vn_netdbg_log(const char *fmt, ...);
void vn_netdbg_send_telemetry(uint32_t frame_cnt, float fps, uint32_t script_line, const char *mode_str);

__END_DECLS

#endif /* __KOSVN_KOSVN_H */
