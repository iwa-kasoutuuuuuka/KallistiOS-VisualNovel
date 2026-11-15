/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_core.c
 * Engine core loop, controller input handling, Auto/Skip, VMU save/load glue & AICA audio.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <arch/timer.h>
#include <kos/dbglog.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>
#include <dc/sound/sfxmgr.h>
#include <dc/sound/stream.h>

#include "kosvn_internal.h"

#define BGM_CHUNK_BYTES 8192

static struct {
    bool              initialized;
    bool              quit_requested;
    char              title[64];
    char              app_id[20];
    uint32_t          text_speed_ms;
    uint32_t          auto_advance_ms;

    uint64_t          last_ms;
    uint32_t          frame_cnt;
    uint32_t          fps_accum_ms;
    uint32_t          fps_accum_frames;
    float             current_fps;
    uint32_t          last_telemetry_line;

    uint32_t          prev_btns;
    bool              auto_mode;
    uint32_t          auto_timer_ms;
    char              notice_msg[32];
    uint32_t          notice_timer_ms;

    /* Audio state */
    bool              audio_ready;
    snd_stream_hnd_t  bgm_stream;
    bool              bgm_playing;
    int               bgm_proc_mode;
    uint32_t          bgm_phase;
    uint16_t         *bgm_chunk;
    char              bgm_name[64];
    FILE             *bgm_wav_fp;
} core_st;

/* 32-entry signed sine wave table scaled to [-12000, +12000] */
static const int16_t sin32_lut[32] = {
        0,  2341,  4592,  6667,  8485,  9978, 11086, 11769,
    12000, 11769, 11086,  9978,  8485,  6667,  4592,  2341,
        0, -2341, -4592, -6667, -8485, -9978,-11086,-11769,
   -12000,-11769,-11086, -9978, -8485, -6667, -4592, -2341
};

static void *bgm_stream_cb(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {
    (void)hnd;
    if(!core_st.bgm_chunk || !core_st.bgm_playing) {
        *smp_recv = 0;
        return NULL;
    }

    int bytes = (smp_req > BGM_CHUNK_BYTES) ? BGM_CHUNK_BYTES : smp_req;
    bytes &= ~3;
    if(bytes <= 0) {
        *smp_recv = 0;
        return NULL;
    }

    if(core_st.bgm_wav_fp) {
        size_t got = fread(core_st.bgm_chunk, 1, (size_t)bytes, core_st.bgm_wav_fp);
        if(got < (size_t)bytes) {
            fseek(core_st.bgm_wav_fp, 44, SEEK_SET);
            size_t rest = (size_t)bytes - got;
            size_t got2 = fread((uint8_t *)core_st.bgm_chunk + got, 1, rest, core_st.bgm_wav_fp);
            if(got + got2 < (size_t)bytes) {
                memset((uint8_t *)core_st.bgm_chunk + got + got2, 0,
                       (size_t)bytes - (got + got2));
            }
        }
        *smp_recv = bytes;
        return core_st.bgm_chunk;
    }

    int samples = bytes / 2;
    /* Soft procedural ambient chord at 22050 Hz */
    static const uint32_t chord_calm[4]  = { 196U, 247U, 294U, 392U };
    static const uint32_t chord_tense[4] = { 165U, 196U, 233U, 311U };
    const uint32_t *chord = (core_st.bgm_proc_mode == 1) ? chord_tense : chord_calm;

    for(int i = 0; i < samples; i++) {
        uint32_t t = core_st.bgm_phase++;
        uint32_t note = (t / 33075U) & 3U; /* Switch arpeggio note every ~1.5s */
        uint32_t freq = chord[note];
        uint32_t idx1 = ((t * freq * 32U) / 22050U) & 31U;
        uint32_t idx2 = ((t * (freq / 2U) * 32U) / 22050U) & 31U;
        int32_t env = 255 - (int32_t)(((t % 33075U) * 180U) / 33075U);
        int32_t val = ((int32_t)sin32_lut[idx1] + (int32_t)sin32_lut[idx2] / 2) * env / 1024;
        core_st.bgm_chunk[i] = (int16_t)val;
    }

    *smp_recv = bytes;
    return core_st.bgm_chunk;
}

const char *vn_core_get_app_id(void) {
    return (core_st.app_id[0] != '\0') ? core_st.app_id : "KOSVN";
}

const char *vn_audio_get_bgm_name(void) {
    return core_st.bgm_name;
}

int vn_audio_play_bgm(const char *path_or_spec) {
    vn_audio_stop_bgm();
    if(!core_st.audio_ready || !path_or_spec || path_or_spec[0] == '\0' ||
       strcmp(path_or_spec, "none") == 0) {
        return 0;
    }

    if(!core_st.bgm_chunk) {
        core_st.bgm_chunk = (uint16_t *)memalign(32, BGM_CHUNK_BYTES);
        if(!core_st.bgm_chunk)
            return -1;
    }

    uint32_t rate = 22050U;
    int stereo = 0;
    if(strncmp(path_or_spec, "proc:", 5) == 0) {
        core_st.bgm_proc_mode = (strstr(path_or_spec + 5, "tense") != NULL) ? 1 : 0;
        core_st.bgm_phase = 0;
    } else {
        core_st.bgm_wav_fp = fopen(path_or_spec, "rb");
        if(!core_st.bgm_wav_fp) {
            dbglog(DBG_WARNING, "kosvn_audio: cannot open BGM '%s'\n", path_or_spec);
            return -1;
        }
        /* Skip standard 44-byte WAV header */
        fseek(core_st.bgm_wav_fp, 44, SEEK_SET);
    }

    core_st.bgm_stream = snd_stream_alloc(bgm_stream_cb, BGM_CHUNK_BYTES);
    if(core_st.bgm_stream == SND_STREAM_INVALID) {
        if(core_st.bgm_wav_fp) {
            fclose(core_st.bgm_wav_fp);
            core_st.bgm_wav_fp = NULL;
        }
        return -1;
    }

    core_st.bgm_playing = true;
    vn_copy_str(core_st.bgm_name, sizeof(core_st.bgm_name), path_or_spec);
    snd_stream_start(core_st.bgm_stream, rate, stereo);
    snd_stream_volume(core_st.bgm_stream, 150);
    return 0;
}

void vn_audio_stop_bgm(void) {
    if(core_st.bgm_playing && core_st.bgm_stream != SND_STREAM_INVALID) {
        snd_stream_stop(core_st.bgm_stream);
        snd_stream_destroy(core_st.bgm_stream);
        core_st.bgm_stream = SND_STREAM_INVALID;
    }
    if(core_st.bgm_wav_fp) {
        fclose(core_st.bgm_wav_fp);
        core_st.bgm_wav_fp = NULL;
    }
    core_st.bgm_playing = false;
    core_st.bgm_name[0] = '\0';
}

int vn_audio_play_se(const char *path_or_spec) {
    if(!core_st.audio_ready || !path_or_spec || path_or_spec[0] == '\0')
        return 0;

    sfxhnd_t hnd = SFXHND_INVALID;
    if(strncmp(path_or_spec, "proc:", 5) == 0) {
        uint32_t samples = 3500U;
        size_t bytes = samples * 2U;
        int16_t *pcm = (int16_t *)memalign(32, bytes);
        if(!pcm)
            return -1;

        uint32_t freq = 880U;
        if(strstr(path_or_spec, "select") != NULL) freq = 660U;
        else if(strstr(path_or_spec, "flash") != NULL) freq = 440U;

        for(uint32_t i = 0; i < samples; i++) {
            uint32_t idx = ((i * freq * 32U) / 22050U) & 31U;
            int32_t decay = (int32_t)(samples - i);
            pcm[i] = (int16_t)(((int32_t)sin32_lut[idx] * decay) / (int32_t)samples);
        }

        hnd = snd_sfx_load_raw_buf((char *)pcm, bytes, 22050U, 16, 1);
        free(pcm);
    } else {
        hnd = snd_sfx_load(path_or_spec);
    }

    if(hnd == SFXHND_INVALID)
        return -1;

    snd_sfx_play(hnd, 200, 128);
    return 0;
}

void vn_audio_update(void) {
    if(core_st.bgm_playing && core_st.bgm_stream != SND_STREAM_INVALID) {
        snd_stream_poll(core_st.bgm_stream);
    }
}

int vn_init(const vn_config_t *cfg) {
    if(core_st.initialized)
        return 0;

    memset(&core_st, 0, sizeof(core_st));
    core_st.bgm_stream = SND_STREAM_INVALID;

    vn_copy_str(core_st.title, sizeof(core_st.title),
                (cfg && cfg->title) ? cfg->title : "KallistiOS-VN");
    vn_copy_str(core_st.app_id, sizeof(core_st.app_id),
                (cfg && cfg->app_id) ? cfg->app_id : "KOSVN");
    core_st.text_speed_ms = (cfg && cfg->text_speed_ms > 0U) ? cfg->text_speed_ms : 26U;
    core_st.auto_advance_ms = (cfg && cfg->auto_advance_ms > 0U) ? cfg->auto_advance_ms : 1800U;

    if(vn_gfx_init() < 0)
        return -1;
    if(vn_text_init() < 0) {
        vn_gfx_shutdown();
        return -1;
    }
    vn_text_set_speed(core_st.text_speed_ms);

    if(snd_stream_init() >= 0) {
        core_st.audio_ready = true;
    } else {
        dbglog(DBG_WARNING, "kosvn_core: snd_stream_init failed; continuing silent\n");
    }

    if(cfg)
        vn_netdbg_init(cfg->net_debug_ip, cfg->net_debug_port);

    core_st.last_ms = timer_ms_gettime64();
    core_st.last_telemetry_line = (uint32_t)-1;
    core_st.initialized = true;
    vn_netdbg_log("Engine initialized: %s", core_st.title);
    return 0;
}

void vn_shutdown(void) {
    if(!core_st.initialized)
        return;

    vn_netdbg_log("Engine shutting down");
    vn_audio_stop_bgm();
    if(core_st.bgm_chunk) {
        free(core_st.bgm_chunk);
        core_st.bgm_chunk = NULL;
    }
    if(core_st.audio_ready) {
        snd_sfx_unload_all();
        snd_stream_shutdown();
        core_st.audio_ready = false;
    }

    vn_script_unload();
    vn_text_shutdown();
    vn_gfx_shutdown();
    vn_netdbg_shutdown();
    core_st.initialized = false;
}

static void handle_quick_save(void) {
    vn_save_state_t st;
    memset(&st, 0, sizeof(st));
    st.magic = VN_SAVE_MAGIC;
    st.version = VN_SAVE_VERSION;
    st.script_line = vn_script_get_line();
    vn_copy_str(st.script_path, sizeof(st.script_path), vn_script_get_path());
    vn_copy_str(st.bg_name, sizeof(st.bg_name), vn_gfx_get_bg_name());
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_copy_str(st.char_names[i], sizeof(st.char_names[i]),
                    vn_gfx_get_char_name((vn_sprite_pos_t)i));
    }
    vn_copy_str(st.bgm_name, sizeof(st.bgm_name), vn_audio_get_bgm_name());
    for(int i = 0; i < VN_MAX_FLAGS; i++) {
        st.flags[i] = vn_script_get_flag(i);
    }

    if(vn_vmu_save(0, &st) == 0) {
        vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[SAVED VMU0]");
        vn_netdbg_log("Saved slot 0 at line %lu", (unsigned long)st.script_line);
    } else {
        vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[NO VMU FOUND]");
    }
    core_st.notice_timer_ms = 1800U;
}

static void handle_quick_load(void) {
    vn_save_state_t st;
    if(vn_vmu_load(0, &st) < 0) {
        vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[NO SAVE DATA]");
        core_st.notice_timer_ms = 1800U;
        return;
    }

    if(strcmp(st.script_path, vn_script_get_path()) != 0)
        vn_script_load(st.script_path);
    for(int i = 0; i < VN_MAX_FLAGS; i++)
        vn_script_set_flag(i, st.flags[i]);

    vn_bg_set(st.bg_name);
    for(int i = 0; i < VN_MAX_SPRITES; i++)
        vn_char_set((vn_sprite_pos_t)i, st.char_names[i]);
    vn_audio_play_bgm(st.bgm_name);

    uint32_t target_line = (st.script_line > 0U) ? (st.script_line - 1U) : 0U;
    vn_script_jump_line(target_line);
    vn_script_advance();

    vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[LOADED VMU0]");
    core_st.notice_timer_ms = 1800U;
    vn_netdbg_log("Loaded slot 0 -> line %lu", (unsigned long)st.script_line);
}

int vn_step_frame(void) {
    if(!core_st.initialized || core_st.quit_requested)
        return 0;

    uint64_t now = timer_ms_gettime64();
    uint32_t dt_ms = (now > core_st.last_ms) ? (uint32_t)(now - core_st.last_ms) : 16U;
    if(dt_ms > 100U) dt_ms = 100U;
    core_st.last_ms = now;

    core_st.frame_cnt++;
    core_st.fps_accum_ms += dt_ms;
    core_st.fps_accum_frames++;
    if(core_st.fps_accum_ms >= 500U) {
        core_st.current_fps = (float)core_st.fps_accum_frames * 1000.0f / (float)core_st.fps_accum_ms;
        core_st.fps_accum_ms = 0;
        core_st.fps_accum_frames = 0;
    }

    uint32_t btns = 0;
    bool skip_active = false;
    bool l_held = false;
    maple_device_t *pad = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if(pad) {
        cont_state_t *cst = (cont_state_t *)maple_dev_status(pad);
        if(cst) {
            btns = cst->buttons;
            l_held = (cst->ltrig > 64);
            skip_active = (cst->rtrig > 64);
        }
    }
    uint32_t pressed = btns & ~core_st.prev_btns;
    core_st.prev_btns = btns;

    if(pressed & CONT_START) {
        core_st.quit_requested = true;
        return 0;
    }

    if(core_st.notice_timer_ms > 0U) {
        if(core_st.notice_timer_ms <= dt_ms) {
            core_st.notice_timer_ms = 0U;
            core_st.notice_msg[0] = '\0';
        } else {
            core_st.notice_timer_ms -= dt_ms;
        }
    }

    /* Quick Save / Quick Load via L-Trigger */
    if(l_held && (pressed & CONT_A)) {
        handle_quick_save();
    } else if(l_held && (pressed & CONT_B)) {
        handle_quick_load();
    } else if(pressed & CONT_Y) {
        vn_backlog_set_visible(!vn_backlog_is_visible());
    } else if(vn_backlog_is_visible()) {
        if(pressed & CONT_DPAD_UP) vn_backlog_scroll(1);
        else if(pressed & CONT_DPAD_DOWN) vn_backlog_scroll(-1);
        else if(pressed & (CONT_A | CONT_B)) vn_backlog_set_visible(false);
    } else {
        if(pressed & CONT_X) {
            core_st.auto_mode = !core_st.auto_mode;
            core_st.auto_timer_ms = 0;
        }
        if(pressed & CONT_B) {
            vn_text_set_window_visible(!vn_text_is_window_visible());
        }

        if(vn_text_get_choice_count() > 0 && vn_text_is_complete()) {
            if(pressed & CONT_DPAD_UP) {
                vn_text_move_choice(-1);
                vn_audio_play_se("proc:select");
            } else if(pressed & CONT_DPAD_DOWN) {
                vn_text_move_choice(1);
                vn_audio_play_se("proc:select");
            } else if(pressed & CONT_A) {
                int sel = vn_text_get_selected_choice();
                const char *tgt = vn_text_get_choice_target(sel);
                vn_audio_play_se("proc:chime");
                vn_backlog_push(vn_text_get_speaker(), vn_text_get_dialogue());
                vn_text_clear_choices();
                if(tgt)
                    vn_script_jump_label(tgt);
                vn_script_advance();
            }
        } else if(vn_script_is_waiting_fx()) {
            if(!vn_fx_is_busy() || skip_active || (pressed & CONT_A)) {
                vn_script_clear_wait_fx();
                vn_script_advance();
            }
        } else {
            if(!vn_text_is_complete()) {
                if((pressed & CONT_A) || skip_active) {
                    vn_text_reveal_all();
                }
            } else {
                if(core_st.auto_mode)
                    core_st.auto_timer_ms += dt_ms;

                bool advance_now = (pressed & CONT_A) || skip_active ||
                                   (core_st.auto_mode && core_st.auto_timer_ms >= core_st.auto_advance_ms);
                if(advance_now) {
                    core_st.auto_timer_ms = 0;
                    vn_text_set_window_visible(true);
                    vn_backlog_push(vn_text_get_speaker(), vn_text_get_dialogue());
                    vn_script_advance();
                }
            }
        }
    }

    vn_gfx_update(dt_ms);
    vn_text_update(dt_ms, skip_active);
    vn_audio_update();

    const char *mode_tag = skip_active ? "SKIP" : (core_st.auto_mode ? "AUTO" : "PLAY");
    char status[96];
    if(core_st.notice_msg[0] != '\0') {
        snprintf(status, sizeof(status), "%s  %s", mode_tag, core_st.notice_msg);
    } else {
        snprintf(status, sizeof(status), "%s  A:Next X:Auto Y:Log R:Skip START:Exit", mode_tag);
    }
    vn_text_set_status_overlay(status);

    vn_gfx_render_frame();

    uint32_t cur_line = vn_script_get_line();
    if((core_st.frame_cnt % 60U) == 0U || cur_line != core_st.last_telemetry_line) {
        core_st.last_telemetry_line = cur_line;
        vn_netdbg_send_telemetry(core_st.frame_cnt, core_st.current_fps, cur_line, mode_tag);
    }

    return vn_script_is_ended() ? 0 : 1;
}

int vn_run_script(const char *script_path) {
    if(!core_st.initialized)
        return -1;

    if(vn_script_load(script_path) < 0)
        return -1;

    core_st.quit_requested = false;
    vn_script_advance();

    while(vn_step_frame() != 0)
        ;

    return 0;
}
