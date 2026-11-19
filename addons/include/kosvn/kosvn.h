/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * include/kosvn/kosvn.h
 * Dedicated C Visual Novel Engine layer for Sega Dreamcast (KallistiOS-VN).
 */

#ifndef __KOSVN_KOSVN_H
#define __KOSVN_KOSVN_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/cdefs.h>
#include <kos/cdefs.h>
#include <dc/pvr.h>

__BEGIN_DECLS

#define VN_MAX_SPRITES     3
#define VN_MAX_CHOICES     6
#define VN_MAX_FLAGS       256
#define VN_MAX_BITS        4608   /* 1-bit flags (BIT / IF_BIT / OPTION_BIT) */
#define VN_MAX_LABELS      512    /* per script */
#define VN_MAX_SITES       2
#define VN_MAX_CALL_DEPTH  8
#define VN_BACKLOG_SIZE    32
#define VN_SAVE_MAGIC      0x4B564E32U /* "KVN2" */
#define VN_SAVE_VERSION    9U
#define VN_SAVE_SLOTS      4      /* VMU files <app>00..03, about 9 blocks each */
#define VN_MAX_STORIES     3      /* parallel stories (STORY_DEF), e.g. two heroes */

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
    const char *title_script;   /* START menu "back to title" target (NULL: START quits) */
    bool        quiet_status;   /* status line: only notices and SKIP/AUTO, no key help */
    bool        autoplay;       /* test mode: skip all text, random options (no pad needed) */
    uint32_t    autoplay_switch_every; /* autoplay: switch STORY every n choices (0 = never) */
    bool        autoplay_slow;         /* autoplay without skip: lines auto-advance, voices,
                                          SEs, fades play (exercises audio / effects) */
    const char *title_return_label; /* "back to title" jumps to this label (e.g. skip the opening) */
    const char *se_cursor;      /* UI sounds (SE asset spec); NULL = built-in proc: sounds */
    const char *se_decide;
    const char *se_cancel;
} vn_config_t;

typedef struct vn_tex {
    pvr_ptr_t vram_ptr;
    uint16_t  width;
    uint16_t  height;
    uint32_t  fmt;
    uint16_t  img_w;   /* used part of the texture (0 = all of it) */
    uint16_t  img_h;
} vn_tex_t;

typedef struct vn_site_save {
    uint8_t  initialized;
    uint8_t  locked;
    uint16_t call_depth;
    uint32_t script_line;
    uint32_t call_stack[VN_MAX_CALL_DEPTH];
    char     site_name[32];
    char     bg_name[64];
    char     char_names[VN_MAX_SPRITES][64];
    char     cutin_name[64];
    char     speaker_name[64];
    char     bgm_name[64];
} vn_site_save_t;

/* Where a story that is not being played was left (STORY / STORY_DEF). */
typedef struct vn_story_pos {
    uint8_t  valid;
    uint8_t  pad[3];
    uint32_t script_line;
    char     script_path[64];
    char     bg_name[64];
    char     bgm_name[64];
    uint32_t local_bits[4];   /* STORY_LOCAL_BITS range (bit 0 = range start) */
    uint32_t bg_line1;        /* 1 + line of the BG command that was really shown, 0 = unknown */
    uint32_t track0_line1;    /* 1 + line of the OVERLAY_TRACK 0 running, 0 = none */
} vn_story_pos_t;

typedef struct vn_save_state {
    uint32_t       magic;
    uint32_t       version;
    uint32_t       script_line;
    char           script_path[64];
    char           bg_name[64];
    char           char_names[VN_MAX_SPRITES][64];
    char           speaker_name[64];
    char           bgm_name[64];
    int32_t        flags[VN_MAX_FLAGS];
    uint32_t       bits[VN_MAX_BITS / 32];
    char           cutin_name[64];
    uint8_t        multi_sight_enabled;
    uint8_t        active_site;
    uint16_t       call_depth;
    uint32_t       call_stack[VN_MAX_CALL_DEPTH];
    vn_site_save_t sites[VN_MAX_SITES];
    int32_t        story_cur;                  /* story being played, -1 = none */
    vn_story_pos_t stories[VN_MAX_STORIES];    /* where each story was left */
    char           label[48];                  /* slot list: script + start of the line (Shift-JIS) */
    uint32_t       bg_line1;                   /* 1 + line of the BG really shown, 0 = unknown */
    uint32_t       track0_line1;               /* 1 + line of the OVERLAY_TRACK 0 running */
    uint32_t       save_seq;                   /* newer copy of a slot wins (A/B files) */
} vn_save_state_t;

/* Data path remap: files whose path starts with <from> are opened at <to> +
   the rest, e.g. ("/cd/", "/pc/DC/game/cd_root/") to run from dcload-ip with
   the data on the PC. Scripts and saves keep the original paths. NULL = off. */
void        vn_set_path_remap(const char *from, const char *to);
const char *vn_resolve_path(const char *path, char *buf, size_t buf_sz);

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
void vn_tex_free_later(vn_tex_t *tex);  /* free after the PVR is done with it */
bool vn_tex_free_pending(void);         /* wait for the render, free the textures waiting to be freed (e.g. before a movie) */
int  vn_bg_set(const char *asset_spec);
int  vn_bg_crossfade(const char *asset_spec);   /* "<path> <ms>" */
int  vn_char_set(vn_sprite_pos_t pos, const char *asset_spec);
void vn_char_clear(vn_sprite_pos_t pos);
void vn_char_clear_all(void);
void vn_fx_fade(uint32_t target_argb, uint32_t duration_ms);
void vn_fx_reset(void);
void vn_fx_wipe(int mode, uint32_t ms);  /* 1 close to black, 2 open (4-blade clockwise wipe) */             /* no fade (title / load / new script) */
void vn_fx_shake(float amplitude, uint32_t duration_ms);
bool vn_fx_is_busy(void);
int  vn_cutin_set(const char *asset_spec);
void vn_cutin_clear(void);
const char *vn_cutin_get_name(void);
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

/* Scenario script VM & Multi-Sight */
int         vn_script_load(const char *path);
void        vn_script_unload(void);
int         vn_script_jump_label(const char *label);
int         vn_script_advance(void);
void        vn_script_set_flag(int idx, int32_t val);
int32_t     vn_script_get_flag(int idx);
void        vn_script_add_flag(int idx, int32_t delta);
/* MOVIE <path>: the game supplies the player (e.g. kos-ports libmpeg) so that
   libkosvn has no decoder dependency. The handler blocks until playback ends
   and returns 0 (or <0 if the file could not be played). Voice and BGM are
   stopped before it is called and the BGM is restarted afterwards. Without a
   handler (or while skipping) MOVIE is skipped. */
typedef int (*vn_movie_handler_t)(const char *path);
void        vn_set_movie_handler(vn_movie_handler_t fn);

void        vn_script_set_bit(int idx, bool on);
bool        vn_script_get_bit(int idx);
int         vn_script_call_label(const char *label);
int         vn_script_return(void);
uint32_t    vn_script_get_line(void);
const char *vn_script_get_path(void);
int         vn_site_init(int site_id, const char *site_name_sjis, const char *start_label);
int         vn_site_switch(int site_id);
int         vn_site_get_active(void);
bool        vn_site_is_enabled(void);
const char *vn_site_get_name(int site_id);
void        vn_site_set_locked(int site_id, bool locked);
bool        vn_site_is_locked(int site_id);

/* VMU save/load & LCD */
int  vn_vmu_save(int slot, const vn_save_state_t *state);
int  vn_vmu_load(int slot, vn_save_state_t *state_out);
int  vn_vmu_save_cfg(const void *data, int len);   /* settings file <ID>CF */
int  vn_vmu_load_cfg(void *data, int len);
void vn_vmu_draw_logo(const char *short_tag);
/* Save file icon (32x32, 4 bpp, 16-colour ARGB4444 palette) and the names the
   Dreamcast file manager shows; NULL keeps the built-in ones. */
void vn_vmu_set_icon(const uint16_t pal[16], const uint8_t data[512]);
void vn_vmu_set_desc(const char *desc_short, const char *desc_long_prefix);

/* AICA Audio (BGM stream, SFX & Voice) */
int  vn_audio_play_bgm(const char *path_or_spec);
void vn_audio_stop_bgm(void);
int  vn_audio_play_se(const char *path_or_spec);
int  vn_audio_play_voice(const char *path_or_spec);
void vn_audio_stop_voice(void);

/* LAN / UDP debug telemetry */
int  vn_netdbg_init(const char *dest_ip, uint16_t port);
void vn_netdbg_shutdown(void);
void vn_netdbg_log(const char *fmt, ...);
void vn_netdbg_send_telemetry(uint32_t frame_cnt, float fps, uint32_t script_line, const char *mode_str);

__END_DECLS

#endif /* __KOSVN_KOSVN_H */
