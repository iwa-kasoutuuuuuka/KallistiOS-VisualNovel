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

#define BGM_CHUNK_BYTES  8192
/* BGM: 64 KB = 1.5 s of 22 kHz PCM16. With the 8 KB of before, any stall of
   the main loop over ~0.1 s (a picture or voice read from the disc, a VMU
   save) replayed stale music on real hardware. Requests are at most half. */
#define BGM_STREAM_BYTES 65536
#define BGM_REQ_MAX      (BGM_STREAM_BYTES / 2)
/* Volumes (0..255, about linear): music sits ~1.39x above voice like the PS
   at its default options (SsSeqSetVol 82/127 vs XA 126/127 over the renders) */
#define BGM_VOL          255
#define VOICE_VOL        183
#define UI_SE_VOL        163
#define VN_SFX_CACHE_MAX 8

typedef struct vn_sfx_entry {
    char     name[64];
    sfxhnd_t hnd;
} vn_sfx_entry_t;

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
    bool              skipping;     /* R held this frame */
    uint32_t          auto_timer_ms;
    char              notice_msg[32];
    uint32_t          notice_timer_ms;

    /* Audio state */
    bool              audio_ready;
    snd_stream_hnd_t  bgm_stream;
    bool              bgm_playing;
    uint32_t          bgm_fade_ms, bgm_fade_left;   /* BGM fade <ms> */
    int               bgm_proc_mode;
    uint32_t          bgm_phase;
    uint16_t         *bgm_chunk;
    char              bgm_name[64];
    char              bgm_spec[64];          /* what is playing, kept during a fade */
    int               bgm_id;                /* BGM <path> <loop> <id>: -1 = none */
    long              bgm_loop_ofs;          /* file offset the music loops back to */
    FILE             *bgm_wav_fp;
    vn_sfx_entry_t    sfx_cache[VN_SFX_CACHE_MAX];
    int               sfx_cache_cnt;
    int               voice_chan;

    /* Streamed voice (WAV preloaded to RAM, played through its own stream
       so clips longer than the 65534-sample SFX limit are not cut off). */
    snd_stream_hnd_t  voice_stream;
    uint8_t          *voice_pcm;
    uint32_t          voice_bytes;
    uint32_t          voice_pos;
    uint32_t          voice_tail;
    uint8_t           bgm_hold, voice_hold;   /* frames left before the first poll */
    uint32_t          poll_badpos;            /* polls rejected by snd_stream_poll */
    bool              voice_streaming;
    bool              voice_loop;            /* SCENE_SE ... loop */
    bool              voice_se;              /* scene SE: kept over text advance */
    bool              voice_adpcm;           /* the playing clip is 4-bit ADPCM */

    /* Lip-sync table for the current voice (<voice>.LIP: [count u8, shape u8]...) */
    uint8_t           lip_cnt[256];
    uint8_t           lip_shape[256];
    int               lip_n;
    uint32_t          lip_total;
    uint64_t          lip_start_ms;
    uint32_t          lip_dur_ms;

    /* START system menu, title screen, deferred SYSLOAD */
    char              title_script[64];
    bool              quiet_status;
    bool              sys_menu;
    bool              sys_menu_over_choice;  /* re-run the script's CHOICE on close */
    bool              autoplay;              /* vn_config_t.autoplay_switch_every (test) */
    uint32_t          autoplay_switch;
    bool              autoplay_slow;
    uint32_t          auto_choices;
    uint32_t          auto_idle_frames;
    uint32_t          auto_menu_presses;
    bool              load_requested;
    bool              menu_from_script;      /* slot list opened by SYSLOAD */
    int               menu_mode;             /* MENU_MAIN / MENU_SAVE / MENU_LOAD */
    int               last_slot;
    char              title_return_label[32];
    char              se_cursor[64], se_decide[64], se_cancel[64];   /* UI sounds */

    /* Parallel stories (STORY_DEF / STORY): where each one was left */
    int               story_cur;
    bool              story_defined[VN_MAX_STORIES];
    char              story_start[VN_MAX_STORIES][64];
    char              story_label[VN_MAX_STORIES][48];
    vn_story_pos_t    story_pos[VN_MAX_STORIES];
} core_st;

#define SYS_TGT_SAVE  "@sys:save"
#define SYS_TGT_LOAD  "@sys:load"
#define SYS_TGT_TITLE "@sys:title"
#define SYS_TGT_CLOSE "@sys:close"
#define SYS_TGT_STORY "@sys:story"   /* + digit */
#define SYS_TGT_SLOT  "@sys:slot"    /* + digit */
#define SYS_TGT_BACK  "@sys:back"

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

    int bytes = (smp_req > BGM_REQ_MAX) ? BGM_REQ_MAX : smp_req;
    bytes &= ~3;
    if(bytes <= 0) {
        *smp_recv = 0;
        return NULL;
    }
    /* alternate halves: KOS asks for the next block before the DMA of the
       previous one has necessarily finished */
    static int half = 0;
    half ^= 1;
    uint16_t *buf = core_st.bgm_chunk + (size_t)half * (BGM_REQ_MAX / 2);

    if(core_st.bgm_wav_fp) {
        size_t got = fread(buf, 1, (size_t)bytes, core_st.bgm_wav_fp);
        if(got < (size_t)bytes) {
            fseek(core_st.bgm_wav_fp, core_st.bgm_loop_ofs, SEEK_SET);   /* intro, then the loop */
            size_t rest = (size_t)bytes - got;
            size_t got2 = fread((uint8_t *)buf + got, 1, rest, core_st.bgm_wav_fp);
            if(got + got2 < (size_t)bytes) {
                memset((uint8_t *)buf + got + got2, 0,
                       (size_t)bytes - (got + got2));
            }
        }
        *smp_recv = bytes;
        return buf;
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
        buf[i] = (int16_t)val;
    }

    *smp_recv = bytes;
    return buf;
}

bool vn_core_is_skipping(void) {
    return core_st.skipping;
}

const char *vn_core_get_app_id(void) {
    return (core_st.app_id[0] != '\0') ? core_st.app_id : "KOSVN";
}

const char *vn_audio_get_bgm_name(void) {
    return core_st.bgm_name;
}

/* BGM <path> [<loop start sample> [<id>]]: the file is one pass (intro + loop
   section); at its end the music goes back to the loop start. A play of the id
   that is already playing keeps the music going (PS: BGM_LOAD of the current
   number, then \p, only cancels a fade-out and restores the volume). */
int vn_audio_play_bgm(const char *path_or_spec) {
    char path[64] = {0};
    long loop = 0;
    int id = -1;
    if(path_or_spec)
        sscanf(path_or_spec, "%63s %ld %d", path, &loop, &id);
    if(id >= 0 && core_st.bgm_playing && id == core_st.bgm_id &&
       strcmp(path_or_spec, core_st.bgm_spec) == 0) {
        core_st.bgm_fade_ms = core_st.bgm_fade_left = 0U;
        if(core_st.bgm_stream != SND_STREAM_INVALID)
            snd_stream_volume(core_st.bgm_stream, BGM_VOL);
        vn_copy_str(core_st.bgm_name, sizeof(core_st.bgm_name), core_st.bgm_spec);
        return 0;
    }
    vn_audio_stop_bgm();
    if(!core_st.audio_ready || !path_or_spec || path[0] == '\0' ||
       strcmp(path, "none") == 0) {
        return 0;
    }

    if(!core_st.bgm_chunk) {
        core_st.bgm_chunk = (uint16_t *)memalign(32, 2U * BGM_REQ_MAX);
        if(!core_st.bgm_chunk)
            return -1;
    }

    uint32_t rate = 22050U;
    int stereo = 0;
    if(strncmp(path_or_spec, "proc:", 5) == 0) {
        core_st.bgm_proc_mode = (strstr(path_or_spec + 5, "tense") != NULL) ? 1 : 0;
        core_st.bgm_phase = 0;
    } else {
        core_st.bgm_wav_fp = vn_fopen(path, "rb");
        if(!core_st.bgm_wav_fp) {
            dbglog(DBG_WARNING, "kosvn_audio: cannot open BGM '%s'\n", path);
            return -1;
        }
        /* Skip standard 44-byte WAV header (mono PCM16: 2 bytes a sample) */
        fseek(core_st.bgm_wav_fp, 44, SEEK_SET);
        core_st.bgm_loop_ofs = 44L + 2L * (loop > 0 ? loop : 0);
    }

    core_st.bgm_stream = snd_stream_alloc(bgm_stream_cb, BGM_STREAM_BYTES);
    if(core_st.bgm_stream == SND_STREAM_INVALID) {
        if(core_st.bgm_wav_fp) {
            fclose(core_st.bgm_wav_fp);
            core_st.bgm_wav_fp = NULL;
        }
        return -1;
    }

    core_st.bgm_playing = true;
    core_st.bgm_fade_ms = core_st.bgm_fade_left = 0U;
    vn_copy_str(core_st.bgm_name, sizeof(core_st.bgm_name), path_or_spec);
    vn_copy_str(core_st.bgm_spec, sizeof(core_st.bgm_spec), path_or_spec);
    core_st.bgm_id = id;
    snd_stream_start(core_st.bgm_stream, rate, stereo);
    snd_stream_volume(core_st.bgm_stream, BGM_VOL);
    core_st.bgm_hold = 2U;    /* see vn_audio_update */
    dbglog(DBG_INFO, "kosvn_audio: BGM '%s' started\n", path_or_spec);
    return 0;
}

/* BGM fade <ms>: lower the volume to 0 over ms (the script goes on), then stop. */
void vn_audio_fade_bgm(uint32_t ms) {
    if(!core_st.bgm_playing || ms == 0U) {
        vn_audio_stop_bgm();
        return;
    }
    if(ms > 120000U)
        ms = 120000U;
    core_st.bgm_fade_ms = core_st.bgm_fade_left = ms;
    core_st.bgm_name[0] = '\0';     /* a save / story switch now resumes silence */
}

void vn_audio_stop_bgm(void) {
    core_st.bgm_fade_ms = core_st.bgm_fade_left = 0U;
    core_st.bgm_id = -1;
    core_st.bgm_spec[0] = '\0';
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

static sfxhnd_t load_or_cache_sfx(const char *path_or_spec) {
    if(!core_st.audio_ready || !path_or_spec || path_or_spec[0] == '\0')
        return SFXHND_INVALID;

    for(int i = 0; i < core_st.sfx_cache_cnt; i++) {
        if(strcmp(core_st.sfx_cache[i].name, path_or_spec) == 0)
            return core_st.sfx_cache[i].hnd;
    }

    sfxhnd_t hnd = SFXHND_INVALID;
    if(strncmp(path_or_spec, "proc:", 5) == 0) {
        uint32_t samples = 3500U;
        uint32_t freq = 880U;
        if(strstr(path_or_spec, "voice_kojiro") != NULL) {
            samples = 6000U;
            freq = 520U;
        } else if(strstr(path_or_spec, "voice_marina") != NULL) {
            samples = 6000U;
            freq = 740U;
        } else if(strstr(path_or_spec, "select") != NULL) {
            freq = 660U;
        } else if(strstr(path_or_spec, "flash") != NULL) {
            freq = 440U;
        }

        size_t bytes = samples * 2U;
        int16_t *pcm = (int16_t *)memalign(32, bytes);
        if(!pcm)
            return SFXHND_INVALID;

        for(uint32_t i = 0; i < samples; i++) {
            uint32_t idx = ((i * freq * 32U) / 22050U) & 31U;
            int32_t decay = (int32_t)(samples - i);
            pcm[i] = (int16_t)(((int32_t)sin32_lut[idx] * decay) / (int32_t)samples);
        }

        hnd = snd_sfx_load_raw_buf((char *)pcm, bytes, 22050U, 16, 1);
        free(pcm);
    } else {
        { char rp[160]; hnd = snd_sfx_load(vn_resolve_path(path_or_spec, rp, sizeof(rp))); }
    }

    if(hnd == SFXHND_INVALID)
        return SFXHND_INVALID;

    if(core_st.sfx_cache_cnt >= VN_SFX_CACHE_MAX) {
        snd_sfx_unload(core_st.sfx_cache[0].hnd);
        for(int i = 1; i < VN_SFX_CACHE_MAX; i++)
            core_st.sfx_cache[i - 1] = core_st.sfx_cache[i];
        core_st.sfx_cache_cnt = VN_SFX_CACHE_MAX - 1;
    }

    vn_sfx_entry_t *ent = &core_st.sfx_cache[core_st.sfx_cache_cnt++];
    vn_copy_str(ent->name, sizeof(ent->name), path_or_spec);
    ent->hnd = hnd;
    return hnd;
}

int vn_audio_play_se(const char *path_or_spec) {
    sfxhnd_t hnd = load_or_cache_sfx(path_or_spec);
    if(hnd == SFXHND_INVALID)
        return (core_st.audio_ready && path_or_spec && path_or_spec[0] != '\0') ? -1 : 0;

    snd_sfx_play(hnd, UI_SE_VOL, 128);
    return 0;
}

static uint8_t loop_bufs[2][BGM_CHUNK_BYTES] __attribute__((aligned(32)));
static int loop_turn = 0;   /* the other one may still be in a G2 DMA */

/* Silence for the stream buffer: 0 for PCM; for AICA ADPCM nibbles 0 / 8
   (+ / - the smallest step) so the decoder stays at 0. */
static void fill_silence(uint8_t *p, uint32_t n) {
    memset(p, core_st.voice_adpcm ? 0x80 : 0x00, n);
}

static void *voice_stream_cb(snd_stream_hnd_t hnd, int smp_req, int *smp_recv) {
    (void)hnd;
    uint32_t want = (smp_req > 0) ? (uint32_t)smp_req : 0U;
    if(want > sizeof(loop_bufs[0]))
        want = sizeof(loop_bufs[0]);
    if(!core_st.voice_pcm) {
        *smp_recv = 0;
        return NULL;
    }
    if(core_st.voice_loop && core_st.voice_pos >= core_st.voice_bytes)
        core_st.voice_pos = 0U;   /* the PS loops by replaying the whole sound */
    uint32_t left = (core_st.voice_pos < core_st.voice_bytes) ?
                    core_st.voice_bytes - core_st.voice_pos : 0U;
    if(left >= want) {
        void *p = core_st.voice_pcm + core_st.voice_pos;
        core_st.voice_pos += want;
        *smp_recv = (int)want;
        return p;
    }
    /* Last part of the clip (or past its end): always hand over a full buffer.
       Returning nothing makes the AICA replay the old buffer contents, which
       on real hardware repeats the clip's last syllable until the release. */
    loop_turn ^= 1;
    uint8_t *loop_buf = loop_bufs[loop_turn];
    memcpy(loop_buf, core_st.voice_pcm + core_st.voice_pos, left);
    if(core_st.voice_loop && core_st.voice_bytes >= want - left) {
        /* looping scene SE: join the end and the start, no silent gap */
        memcpy(loop_buf + left, core_st.voice_pcm, want - left);
        core_st.voice_pos = want - left;
    } else {
        fill_silence(loop_buf + left, want - left);
        core_st.voice_pos = core_st.voice_bytes;
    }
    *smp_recv = (int)want;
    return loop_buf;
}

static void voice_stream_release(void) {
    if(core_st.voice_stream != SND_STREAM_INVALID) {
        snd_stream_stop(core_st.voice_stream);
        snd_stream_destroy(core_st.voice_stream);
        core_st.voice_stream = SND_STREAM_INVALID;
    }
    if(core_st.voice_pcm) {
        free(core_st.voice_pcm);
        core_st.voice_pcm = NULL;
    }
    core_st.voice_bytes = 0U;
    core_st.voice_pos = 0U;
    core_st.voice_tail = 0U;
    core_st.voice_streaming = false;
    core_st.voice_loop = false;
    core_st.voice_se = false;
    core_st.lip_n = 0;
}

static void lip_load(const char *wav_path, uint32_t dur_ms);

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Load a mono 16-bit PCM or 4-bit AICA ADPCM WAV fully into RAM and play it
   on a stream. Returns 0 on success, -1 if the file is not usable. */
static int voice_stream_start(const char *path) {
    FILE *fp = vn_fopen(path, "rb");
    if(!fp)
        return -1;

    uint8_t hdr[12];
    if(fread(hdr, 1, 12, fp) != 12 || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return -1;
    }

    uint32_t rate = 0U, data_bytes = 0U;
    uint16_t chans = 0U, bits = 0U, fmt_tag = 0U;
    bool have_data = false;
    for(;;) {
        uint8_t ch[8];
        if(fread(ch, 1, 8, fp) != 8)
            break;
        uint32_t sz = rd_le32(ch + 4);
        if(memcmp(ch, "fmt ", 4) == 0) {
            uint8_t f[16];
            if(sz < 16 || fread(f, 1, 16, fp) != 16)
                break;
            fmt_tag = (uint16_t)(f[0] | (f[1] << 8));
            chans = (uint16_t)(f[2] | (f[3] << 8));
            rate = rd_le32(f + 4);
            bits = (uint16_t)(f[14] | (f[15] << 8));
            if(sz > 16)
                fseek(fp, (long)((sz - 16) + (sz & 1U)), SEEK_CUR);
        } else if(memcmp(ch, "data", 4) == 0) {
            data_bytes = sz;
            have_data = true;
            break;
        } else {
            fseek(fp, (long)(sz + (sz & 1U)), SEEK_CUR);
        }
    }

    /* 0x14 / 0x20 = Yamaha (AICA) 4-bit ADPCM as written by KOS utils/wav2adpcm */
    bool adpcm = (fmt_tag == 0x14U || fmt_tag == 0x20U) && bits == 4U;
    if(!have_data || !(adpcm || (fmt_tag == 1U && bits == 16U)) ||
       (chans != 1U && chans != 2U) || rate == 0U) {
        fclose(fp);
        return -1;
    }
    if(chans == 2U) {
        /* KOS streams want split channels; keep voices mono. */
        dbglog(DBG_WARNING, "kosvn_audio: stereo voice '%s' not supported, use mono\n", path);
        fclose(fp);
        return -1;
    }

    uint8_t *pcm = (uint8_t *)memalign(32, (data_bytes + 31U) & ~31U);
    if(!pcm) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(pcm, 1, data_bytes, fp);
    fclose(fp);

    /* 8 KB for both formats. snd_stream_poll only asks for data in steps of
       2048 bytes and at least half a buffer, so an ADPCM buffer of 2 KB was
       never refilled: real hardware looped its first 0.2 s ("yo-yo-yo") until
       the line was advanced. The release tail (60 frames) covers the longest
       queue: 16384 ADPCM samples at 18900 Hz = 0.87 s. */
    core_st.voice_stream = snd_stream_alloc(voice_stream_cb, BGM_CHUNK_BYTES);
    if(core_st.voice_stream == SND_STREAM_INVALID) {
        free(pcm);
        return -1;
    }
    core_st.voice_pcm = pcm;
    core_st.voice_bytes = (uint32_t)got;
    core_st.voice_pos = 0U;
    core_st.voice_streaming = true;
    core_st.voice_adpcm = adpcm;
    uint32_t samples = adpcm ? (uint32_t)got * 2U : (uint32_t)got / 2U;
    if(adpcm)
        snd_stream_start_adpcm(core_st.voice_stream, rate, 0);
    else
        snd_stream_start(core_st.voice_stream, rate, 0);
    snd_stream_volume(core_st.voice_stream, VOICE_VOL);
    core_st.voice_hold = 2U;
    lip_load(path, (uint32_t)(((uint64_t)samples * 1000U) / rate));
    dbglog(DBG_INFO, "kosvn_audio: voice '%s' %luHz %lu samples (%s stream, lip %d)\n",
           path, (unsigned long)rate, (unsigned long)samples, adpcm ? "adpcm" : "pcm16",
           core_st.lip_n);
    return 0;
}

/* Load "<voice path without .WAV>.LIP" if it exists. The counts are relative:
   they are stretched over the clip's real duration. */
static void lip_load(const char *wav_path, uint32_t dur_ms) {
    core_st.lip_n = 0;
    core_st.lip_total = 0U;
    char p[72];
    size_t n = strlen(wav_path);
    if(n < 5 || n + 1 > sizeof(p))
        return;
    memcpy(p, wav_path, n - 4);
    memcpy(p + n - 4, ".LIP", 5);
    FILE *fp = vn_fopen(p, "rb");
    if(!fp)
        return;
    uint8_t buf[512];
    size_t got = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    for(size_t i = 0; i + 1 < got && core_st.lip_n < 256; i += 2) {
        core_st.lip_cnt[core_st.lip_n] = buf[i];
        core_st.lip_shape[core_st.lip_n] = buf[i + 1];
        core_st.lip_total += buf[i];
        core_st.lip_n++;
    }
    core_st.lip_start_ms = timer_ms_gettime64();
    core_st.lip_dur_ms = dur_ms;
}

int vn_audio_lip_shape(void) {
    if(core_st.lip_n == 0 || core_st.lip_total == 0U || core_st.lip_dur_ms == 0U ||
       (!core_st.voice_streaming && core_st.voice_chan < 0))
        return -1;
    uint64_t el = timer_ms_gettime64() - core_st.lip_start_ms;
    if(el >= core_st.lip_dur_ms)
        return -1;
    uint32_t pos = (uint32_t)((el * core_st.lip_total) / core_st.lip_dur_ms);
    uint32_t acc = 0U;
    for(int i = 0; i < core_st.lip_n; i++) {
        acc += core_st.lip_cnt[i];
        if(pos < acc)
            return core_st.lip_shape[i];
    }
    return -1;
}

static bool has_wav_ext(const char *s) {
    size_t n = strlen(s);
    return n > 4 && (strcmp(s + n - 4, ".wav") == 0 || strcmp(s + n - 4, ".WAV") == 0);
}

void vn_audio_stop_voice(void) {
    if(!core_st.audio_ready)
        return;
    if(core_st.voice_chan >= 0) {
        snd_sfx_stop(core_st.voice_chan);
        core_st.voice_chan = -1;
    }
    if(core_st.voice_streaming || core_st.voice_stream != SND_STREAM_INVALID)
        voice_stream_release();
}

int vn_audio_play_voice(const char *path_or_spec) {
    vn_audio_stop_voice();
    if(!core_st.audio_ready || !path_or_spec || path_or_spec[0] == '\0' ||
       strcmp(path_or_spec, "none") == 0) {
        return 0;
    }

    if(strncmp(path_or_spec, "proc:", 5) != 0 && has_wav_ext(path_or_spec)) {
        if(voice_stream_start(path_or_spec) == 0)
            return 0;
        dbglog(DBG_WARNING, "kosvn_audio: voice stream failed for '%s', trying SFX\n", path_or_spec);
    }

    sfxhnd_t hnd = load_or_cache_sfx(path_or_spec);
    if(hnd == SFXHND_INVALID)
        return -1;

    core_st.voice_chan = snd_sfx_play(hnd, VOICE_VOL, 128);
    return (core_st.voice_chan >= 0) ? 0 : -1;
}

/* Scene sound effect (PS SE opcode). The PS plays it on the voice XA
   stream, so it takes the voice's place (a new voice stops it), but it keeps
   playing while the player reads on. "loop" replays it until stopped. */
int vn_audio_play_scene_se(const char *path, bool loop) {
    int r = vn_audio_play_voice(path);
    if(r == 0 && core_st.voice_streaming) {
        core_st.voice_loop = loop;
        core_st.voice_se = true;
        core_st.lip_n = 0;
    }
    return r;
}

/* Text advance / choice made: stop the line's voice, not a scene SE. */
static void stop_line_voice(void) {
    if(!core_st.voice_se)
        vn_audio_stop_voice();
}

void vn_audio_update(void) {
    if(core_st.bgm_fade_ms > 0U && core_st.bgm_playing && core_st.bgm_stream != SND_STREAM_INVALID) {
        uint32_t step = 1000U / 60U;
        if(core_st.bgm_fade_left <= step) {
            vn_audio_stop_bgm();
        } else {
            core_st.bgm_fade_left -= step;
            snd_stream_volume(core_st.bgm_stream,
                              (int)(((uint32_t)BGM_VOL * core_st.bgm_fade_left) / core_st.bgm_fade_ms));
        }
    }
    /* No poll for 2 frames after a start: the buffer is prefilled, and until
       the ARM has run the START command the channel position read back is
       not this stream's (a stale CA, or with old KOS packet garbage). */
    if(core_st.bgm_playing && core_st.bgm_stream != SND_STREAM_INVALID) {
        if(core_st.bgm_hold > 0U)
            core_st.bgm_hold--;
        else if(snd_stream_poll(core_st.bgm_stream) < 0)
            core_st.poll_badpos++;
    }
    if(core_st.voice_streaming && core_st.voice_stream != SND_STREAM_INVALID) {
        if(core_st.voice_hold > 0U)
            core_st.voice_hold--;
        else if(snd_stream_poll(core_st.voice_stream) < 0)
            core_st.poll_badpos++;
        /* Release once the clip and roughly one stream buffer of tail have played. */
        if(!core_st.voice_loop && core_st.voice_pos >= core_st.voice_bytes && ++core_st.voice_tail > 60U)
            voice_stream_release();
    }
}

int vn_init(const vn_config_t *cfg) {
    if(core_st.initialized)
        return 0;

    memset(&core_st, 0, sizeof(core_st));
    core_st.bgm_stream = SND_STREAM_INVALID;
    core_st.bgm_id = -1;
    core_st.voice_stream = SND_STREAM_INVALID;
    core_st.voice_chan = -1;

    vn_copy_str(core_st.title, sizeof(core_st.title),
                (cfg && cfg->title) ? cfg->title : "KallistiOS-VN");
    vn_copy_str(core_st.app_id, sizeof(core_st.app_id),
                (cfg && cfg->app_id) ? cfg->app_id : "KOSVN");
    core_st.text_speed_ms = (cfg && cfg->text_speed_ms > 0U) ? cfg->text_speed_ms : 26U;
    core_st.auto_advance_ms = (cfg && cfg->auto_advance_ms > 0U) ? cfg->auto_advance_ms : 1800U;
    vn_copy_str(core_st.title_script, sizeof(core_st.title_script),
                (cfg && cfg->title_script) ? cfg->title_script : "");
    core_st.story_cur = -1;
    core_st.quiet_status = cfg && cfg->quiet_status;
    vn_copy_str(core_st.title_return_label, sizeof(core_st.title_return_label),
                (cfg && cfg->title_return_label) ? cfg->title_return_label : "");
    vn_copy_str(core_st.se_cursor, sizeof(core_st.se_cursor),
                (cfg && cfg->se_cursor) ? cfg->se_cursor : "proc:select");
    vn_copy_str(core_st.se_decide, sizeof(core_st.se_decide),
                (cfg && cfg->se_decide) ? cfg->se_decide : "proc:chime");
    vn_copy_str(core_st.se_cancel, sizeof(core_st.se_cancel),
                (cfg && cfg->se_cancel) ? cfg->se_cancel : "proc:select");
    core_st.autoplay = cfg && cfg->autoplay;
    core_st.autoplay_switch = cfg ? cfg->autoplay_switch_every : 0U;
    core_st.autoplay_slow = cfg && cfg->autoplay_slow;

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
    vn_audio_stop_voice();
    vn_audio_stop_bgm();
    if(core_st.bgm_chunk) {
        free(core_st.bgm_chunk);
        core_st.bgm_chunk = NULL;
    }
    if(core_st.audio_ready) {
        snd_sfx_unload_all();
        snd_stream_shutdown();
        core_st.sfx_cache_cnt = 0;
        core_st.audio_ready = false;
    }

    vn_script_unload();
    vn_text_shutdown();
    vn_gfx_shutdown();
    vn_netdbg_shutdown();
    core_st.initialized = false;
}

/* ---- Data path remap (e.g. /cd/ -> /pc/... when run from dcload-ip) ---- */
static char remap_from[32];
static char remap_to[96];

void vn_set_path_remap(const char *from, const char *to) {
    vn_copy_str(remap_from, sizeof(remap_from), (from && to) ? from : "");
    vn_copy_str(remap_to, sizeof(remap_to), (from && to) ? to : "");
    if(remap_from[0] != '\0')
        dbglog(DBG_INFO, "kosvn: data path remap '%s' -> '%s'\n", remap_from, remap_to);
}

const char *vn_resolve_path(const char *path, char *buf, size_t buf_sz) {
    size_t n = strlen(remap_from);
    if(!path || n == 0U || strncmp(path, remap_from, n) != 0 || !buf || buf_sz == 0U)
        return path;
    snprintf(buf, buf_sz, "%s%s", remap_to, path + n);
    return buf;
}

FILE *vn_fopen(const char *path, const char *mode) {
    char buf[160];
    return fopen(vn_resolve_path(path, buf, sizeof(buf)), mode);
}

/* ---- Parallel stories --------------------------------------------------
   A game with two heroes (e.g. one story per hero) defines both stories in its
   title script (STORY_DEF) and every scenario script says which story it is
   (STORY n). The bits are shared; each story keeps its own position, so the
   START menu can switch to the other story where the PS asked for a disc swap. */

void vn_story_define(int id, const char *start_script, const char *label_sjis) {
    if(id < 0 || id >= VN_MAX_STORIES)
        return;
    core_st.story_defined[id] = true;
    vn_copy_str(core_st.story_start[id], sizeof(core_st.story_start[id]), start_script);
    vn_copy_str(core_st.story_label[id], sizeof(core_st.story_label[id]), label_sjis);
}

void vn_story_set_current(int id) {
    core_st.story_cur = (id >= 0 && id < VN_MAX_STORIES) ? id : -1;
}

void vn_story_reset(void) {
    memset(core_st.story_pos, 0, sizeof(core_st.story_pos));
    core_st.story_cur = -1;
}

/* Buttons held now (e.g. the A that skipped a movie) do not count as presses. */
void vn_core_swallow_input(void) {
    core_st.prev_btns = 0xFFFFFFFFU;
}

void vn_core_request_load(void) {
    core_st.load_requested = true;
}

/* STORY_LOCAL_BITS <from> <to>: bits each story uses for its own scene state
   (e.g. scene work bits cleared and reused by every scene). They are kept per story,
   so switching stories mid-scene does not hand one story the other's bits. */
static int local_from = -1, local_to = -1;

void vn_story_set_local_bits(int from, int to) {
    if(from < 0 || to < from || to - from >= 128) {
        local_from = local_to = -1;
        return;
    }
    local_from = from;
    local_to = to;
}

static void local_bits_store(vn_story_pos_t *p) {
    memset(p->local_bits, 0, sizeof(p->local_bits));
    for(int i = local_from; local_from >= 0 && i <= local_to; i++)
        if(vn_script_get_bit((uint32_t)i))
            p->local_bits[(i - local_from) >> 5] |= 1U << ((i - local_from) & 31);
}

static void local_bits_load(const vn_story_pos_t *p) {
    for(int i = local_from; local_from >= 0 && i <= local_to; i++)
        vn_script_set_bit((uint32_t)i, (p->local_bits[(i - local_from) >> 5] >> ((i - local_from) & 31)) & 1U);
}

/* Where the player is now: the TEXT / CHOICE / WAIT line being shown. */
static void capture_pos(vn_story_pos_t *p) {
    memset(p, 0, sizeof(*p));
    p->valid = 1U;
    p->script_line = vn_script_get_line();
    vn_copy_str(p->script_path, sizeof(p->script_path), vn_script_get_path());
    vn_copy_str(p->bg_name, sizeof(p->bg_name), vn_gfx_get_bg_name());
    vn_copy_str(p->bgm_name, sizeof(p->bgm_name), vn_audio_get_bgm_name());
    p->bg_line1 = vn_script_get_bg_line1();
    p->track0_line1 = vn_script_get_track0_line1();
    local_bits_store(p);
}

/* Reset what a script leaves behind on screen before resuming elsewhere. */
static void reset_presentation(void) {
    vn_audio_stop_voice();
    vn_fx_reset();
    vn_overlay_clear_all();
    vn_char_clear_all();
    vn_cutin_clear();
    vn_text_clear_choices();
    vn_text_set_choice_cursor(-1, 0, 0);
    vn_text_set_box_visible(true);
    vn_text_set_window_visible(true);
    vn_text_set_speaker("");
    vn_text_set_dialogue("");
    vn_script_clear_wait_fx();
}

/* Resume at a saved position: reload the script, rebuild the screen of that
   moment (BG + overlays + speaker) and run the line again. */
static int resume_pos(const vn_story_pos_t *p) {
    reset_presentation();
    if(strcmp(p->script_path, vn_script_get_path()) != 0 || vn_script_is_ended()) {
        if(vn_script_load(p->script_path) < 0)
            return -1;
    }
    vn_bg_set(p->bg_name);
    vn_script_replay_scene(p->script_line, p->bg_line1, p->track0_line1);
    if(p->bgm_name[0] != '\0')
        vn_audio_play_bgm(p->bgm_name);
    else
        vn_audio_stop_bgm();
    vn_script_jump_line(p->script_line);
    vn_script_advance();
    return 0;
}

static void notice(const char *msg, uint32_t ms) {
    vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), msg);
    core_st.notice_timer_ms = ms;
}

static void switch_story(int id) {
    if(id < 0 || id >= VN_MAX_STORIES || !core_st.story_defined[id] || id == core_st.story_cur)
        return;
    int from = core_st.story_cur;
    if(core_st.story_cur >= 0)
        capture_pos(&core_st.story_pos[core_st.story_cur]);
    vn_story_pos_t p = core_st.story_pos[id];
    if(!p.valid) {
        /* Not started yet: begin at the story's first script. */
        memset(&p, 0, sizeof(p));
        p.valid = 1U;
        vn_copy_str(p.script_path, sizeof(p.script_path), core_st.story_start[id]);
        reset_presentation();
        vn_audio_stop_bgm();
        vn_bg_set("none");
        local_bits_load(&p);      /* all clear */
        if(vn_script_load(p.script_path) < 0)
            return;
        vn_script_advance();
    } else {
        local_bits_load(&p);
        if(resume_pos(&p) < 0)
            return;
    }
    vn_netdbg_log("Story %d -> %d (%s line %lu)", from, id, p.script_path,
                  (unsigned long)p.script_line);
    core_st.story_cur = id;   /* the script's STORY line sets it too */
}

/* ---- Save slots ---------------------------------------------------------- */
#define NOTICE_SAVED     "\x83\x5a\x81\x5b\x83\x75\x82\xb5\x82\xdc\x82\xb5\x82\xbd"              /* セーブしました */
#define NOTICE_SAVE_FAIL "\x83\x5a\x81\x5b\x83\x75\x82\xc5\x82\xab\x82\xdc\x82\xb9\x82\xf1\x82\xc5\x82\xb5\x82\xbd" /* セーブできませんでした */
#define NOTICE_NO_VMU    "\x82\x75\x82\x6c\x82\x74\x82\xaa\x8c\xa9\x82\xc2\x82\xa9\x82\xe8\x82\xdc\x82\xb9\x82\xf1" /* ＶＭＵが見つかりません */
#define NOTICE_NO_SAVE   "\x83\x5a\x81\x5b\x83\x75\x83\x66\x81\x5b\x83\x5e\x82\xaa\x82\xa0\x82\xe8\x82\xdc\x82\xb9\x82\xf1" /* セーブデータがありません */
#define NOTICE_LOAD_FAIL "\x83\x8d\x81\x5b\x83\x68\x82\xc5\x82\xab\x82\xdc\x82\xb9\x82\xf1\x82\xc5\x82\xb5\x82\xbd" /* ロードできませんでした */
#define NOTICE_LOADED    "\x83\x8d\x81\x5b\x83\x68\x82\xb5\x82\xdc\x82\xb5\x82\xbd"              /* ロードしました */
#define TXT_EMPTY_SLOT   "\x81\x69\x8b\xf3\x82\xab\x81\x6a"                                      /* （空き） */
#define TXT_BACK         "\x96\xdf\x82\xe9"                                                      /* 戻る */

enum { MENU_MAIN = 0, MENU_SAVE = 1, MENU_LOAD = 2 };

/* Copy at most max bytes of Shift-JIS text without splitting a 2-byte char. */
static void sjis_head(char *dst, size_t dst_sz, const char *src, size_t max) {
    size_t i = 0;
    if(max >= dst_sz)
        max = dst_sz - 1U;
    while(src && src[i] != '\0' && src[i] != '\n') {
        uint8_t c = (uint8_t)src[i];
        size_t w = ((c >= 0x81U && c <= 0x9fU) || (c >= 0xe0U && c <= 0xfcU)) ? 2U : 1U;
        if(i + w > max || (w == 2U && src[i + 1] == '\0'))
            break;
        i += w;
    }
    if(src)
        memcpy(dst, src, i);
    dst[i] = '\0';
}

static void handle_save(int slot) {
    vn_save_state_t st;
    memset(&st, 0, sizeof(st));
    st.magic = VN_SAVE_MAGIC;
    st.version = VN_SAVE_VERSION;
    st.script_line = vn_script_get_line();
    vn_copy_str(st.script_path, sizeof(st.script_path), vn_script_get_path());
    vn_copy_str(st.bg_name, sizeof(st.bg_name), vn_gfx_get_bg_name());
    st.bg_line1 = vn_script_get_bg_line1();
    st.track0_line1 = vn_script_get_track0_line1();
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_copy_str(st.char_names[i], sizeof(st.char_names[i]),
                    vn_gfx_get_char_name((vn_sprite_pos_t)i));
    }
    vn_copy_str(st.speaker_name, sizeof(st.speaker_name), vn_text_get_speaker());
    vn_copy_str(st.bgm_name, sizeof(st.bgm_name), vn_audio_get_bgm_name());
    vn_copy_str(st.cutin_name, sizeof(st.cutin_name), vn_cutin_get_name());
    for(int i = 0; i < VN_MAX_FLAGS; i++) {
        st.flags[i] = vn_script_get_flag(i);
    }
    for(int i = 0; i < VN_MAX_BITS; i++) {
        if(vn_script_get_bit(i))
            st.bits[i >> 5] |= (1U << (i & 31));
    }
    vn_script_export_state(&st);
    st.story_cur = core_st.story_cur;
    memcpy(st.stories, core_st.story_pos, sizeof(st.stories));

    /* Slot list label: script name (S003) + the start of the line shown. */
    const char *base = strrchr(st.script_path, '/');
    char name[16], head[40];
    vn_copy_str(name, sizeof(name), base ? base + 1 : st.script_path);
    char *dot = strchr(name, '.');
    if(dot)
        *dot = '\0';
    sjis_head(head, sizeof(head), vn_text_get_dialogue(), 26U);
    snprintf(st.label, sizeof(st.label), "%s %s", name, head);

    int rc = vn_vmu_save(slot, &st);
    if(rc == 0) {
        notice(NOTICE_SAVED, 1800U);
        core_st.last_slot = slot;
        vn_netdbg_log("Saved slot %d at %s line %lu", slot, st.script_path,
                      (unsigned long)st.script_line);
    } else {
        /* -1: no memory card; -2: found but not written (full, removed, ...) */
        notice(rc == -2 ? NOTICE_SAVE_FAIL : NOTICE_NO_VMU, 2400U);
    }
}

/* Returns 0 when a save was loaded. */
static int handle_load(int slot) {
    vn_save_state_t st;
    if(vn_vmu_load(slot, &st) < 0) {
        notice(NOTICE_NO_SAVE, 1800U);
        return -1;
    }

    for(int i = 0; i < VN_MAX_FLAGS; i++)
        vn_script_set_flag(i, st.flags[i]);
    for(int i = 0; i < VN_MAX_BITS; i++)
        vn_script_set_bit(i, (st.bits[i >> 5] >> (i & 31)) & 1U);
    memcpy(core_st.story_pos, st.stories, sizeof(core_st.story_pos));

    vn_story_pos_t p;
    memset(&p, 0, sizeof(p));
    p.valid = 1U;
    p.script_line = st.script_line;
    vn_copy_str(p.script_path, sizeof(p.script_path), st.script_path);
    vn_copy_str(p.bg_name, sizeof(p.bg_name), st.bg_name);
    vn_copy_str(p.bgm_name, sizeof(p.bgm_name), st.bgm_name);
    p.bg_line1 = st.bg_line1;
    p.track0_line1 = st.track0_line1;

    /* resume_pos reloads the script, so import the call stack afterwards. */
    reset_presentation();
    if(vn_script_load(p.script_path) < 0) {
        notice(NOTICE_LOAD_FAIL, 1800U);
        return -1;
    }
    for(int i = 0; i < VN_MAX_SPRITES; i++)
        vn_char_set((vn_sprite_pos_t)i, st.char_names[i]);
    vn_cutin_set(st.cutin_name);
    vn_script_import_state(&st);
    resume_pos(&p);
    if(st.speaker_name[0] != '\0')
        vn_text_set_speaker(st.speaker_name);
    core_st.story_cur = st.story_cur;
    core_st.last_slot = slot;

    notice(NOTICE_LOADED, 1800U);
    vn_netdbg_log("Loaded slot %d -> %s line %lu", slot, st.script_path,
                  (unsigned long)st.script_line);
    return 0;
}

static bool in_title(void) {
    return core_st.title_script[0] != '\0' &&
           strcmp(vn_script_get_path(), core_st.title_script) == 0;
}

static void go_title(void) {
    reset_presentation();
    vn_audio_stop_bgm();
    vn_bg_set("none");
    if(vn_script_load(core_st.title_script) == 0) {
        /* e.g. skip the logo / opening movies when coming back from the game */
        if(core_st.title_return_label[0] != '\0')
            vn_script_jump_label(core_st.title_return_label);
        vn_script_advance();
    }
}

/* START menu, drawn with the choice cards. Shift-JIS labels. */
static void open_sys_menu(void) {
    core_st.sys_menu_over_choice = vn_text_get_choice_count() > 0;
    vn_text_reveal_all();
    vn_text_clear_choices();
    vn_text_add_choice("\x83\x5a\x81\x5b\x83\x75", SYS_TGT_SAVE);                   /* セーブ */
    vn_text_add_choice("\x83\x8d\x81\x5b\x83\x68", SYS_TGT_LOAD);                   /* ロード */
    for(int i = 0; i < VN_MAX_STORIES; i++) {
        if(core_st.story_defined[i] && core_st.story_cur >= 0 &&
           core_st.story_defined[core_st.story_cur] && i != core_st.story_cur) {
            static char tgt[VN_MAX_STORIES][16];
            snprintf(tgt[i], sizeof(tgt[i]), SYS_TGT_STORY "%d", i);
            vn_text_add_choice(core_st.story_label[i], tgt[i]);
        }
    }
    if(core_st.title_script[0] != '\0')
        vn_text_add_choice("\x83\x5e\x83\x43\x83\x67\x83\x8b\x82\xd6\x96\xdf\x82\xe9",
                           SYS_TGT_TITLE);                                           /* タイトルへ戻る */
    vn_text_add_choice("\x95\xc2\x82\xb6\x82\xe9", SYS_TGT_CLOSE);                  /* 閉じる */
    vn_text_set_choice_cancel(SYS_TGT_CLOSE);
    core_st.sys_menu = true;
    core_st.menu_mode = MENU_MAIN;
}

/* Back from a slot list to the main menu (keeps "menu over a CHOICE"). */
static void open_sys_menu_keep(void) {
    bool over = core_st.sys_menu_over_choice;
    open_sys_menu();
    core_st.sys_menu_over_choice = over;
}

/* Slot list for saving or loading: "1: S003 <line>" or "1: (empty)". */
static void open_slot_menu(int mode) {
    static char labels[VN_SAVE_SLOTS][80];
    static char tgts[VN_SAVE_SLOTS][16];
    vn_text_reveal_all();
    vn_text_clear_choices();
    int used = 0;
    for(int i = 0; i < VN_SAVE_SLOTS; i++) {
        vn_save_state_t st;
        if(vn_vmu_load(i, &st) == 0) {
            snprintf(labels[i], sizeof(labels[i]), "%d: %s", i + 1, st.label);
            used++;
        } else {
            snprintf(labels[i], sizeof(labels[i]), "%d: " TXT_EMPTY_SLOT, i + 1);
        }
        snprintf(tgts[i], sizeof(tgts[i]), SYS_TGT_SLOT "%d", i);
        vn_text_add_choice(labels[i], tgts[i]);
    }
    vn_text_add_choice(TXT_BACK, SYS_TGT_BACK);
    vn_text_set_choice_cancel(SYS_TGT_BACK);
    for(int i = 0; i < core_st.last_slot && i < VN_SAVE_SLOTS; i++)
        vn_text_move_choice(1);      /* start on the slot used last */
    core_st.sys_menu = true;
    core_st.menu_mode = mode;
    if(mode == MENU_LOAD && used == 0)
        notice(NOTICE_NO_SAVE, 1800U);
}

/* Leave the menu and go back to where the script was. */
static void close_sys_menu(void) {
    vn_text_clear_choices();
    core_st.sys_menu = false;
    if(core_st.menu_from_script) {
        /* SYSLOAD (title "Load") that did not load: carry on with the next line */
        core_st.menu_from_script = false;
        vn_script_advance();
    } else if(core_st.sys_menu_over_choice) {
        /* The menu replaced the script's options: show that CHOICE again
           (it is the current line). Dialogue under the menu just stays. */
        vn_script_jump_line(vn_script_get_line());
        vn_script_advance();
    }
    core_st.sys_menu_over_choice = false;
}

static void sys_menu_input(uint32_t pressed) {
    if(pressed & CONT_DPAD_UP) {
        vn_text_move_choice(-1);
        vn_audio_play_se(core_st.se_cursor);
        return;
    }
    if(pressed & CONT_DPAD_DOWN) {
        vn_text_move_choice(1);
        vn_audio_play_se(core_st.se_cursor);
        return;
    }
    const char *tgt = NULL;
    if(pressed & CONT_A)
        tgt = vn_text_get_choice_target(vn_text_get_selected_choice());
    else if(pressed & CONT_B)
        tgt = (core_st.menu_mode == MENU_MAIN) ? SYS_TGT_CLOSE : SYS_TGT_BACK;
    else if(pressed & CONT_START)
        tgt = core_st.menu_from_script ? SYS_TGT_BACK : SYS_TGT_CLOSE;
    if(!tgt)
        return;

    char t[32];
    vn_copy_str(t, sizeof(t), tgt);
    vn_audio_play_se((strcmp(t, SYS_TGT_BACK) == 0 || strcmp(t, SYS_TGT_CLOSE) == 0)
                     ? core_st.se_cancel : core_st.se_decide);
    if(strcmp(t, SYS_TGT_SAVE) == 0) {
        open_slot_menu(MENU_SAVE);
    } else if(strcmp(t, SYS_TGT_LOAD) == 0) {
        open_slot_menu(MENU_LOAD);
    } else if(strcmp(t, SYS_TGT_BACK) == 0) {
        if(core_st.menu_from_script)
            close_sys_menu();
        else
            open_sys_menu_keep();
    } else if(strncmp(t, SYS_TGT_SLOT, strlen(SYS_TGT_SLOT)) == 0) {
        int slot = atoi(t + strlen(SYS_TGT_SLOT));
        if(core_st.menu_mode == MENU_SAVE) {
            handle_save(slot);
            close_sys_menu();
        } else {
            vn_text_clear_choices();
            core_st.sys_menu = false;
            bool from_script = core_st.menu_from_script;
            core_st.menu_from_script = false;
            if(handle_load(slot) < 0) {
                core_st.menu_from_script = from_script;   /* empty slot: stay in the list */
                open_slot_menu(MENU_LOAD);
            } else {
                core_st.sys_menu_over_choice = false;
            }
        }
    } else if(strcmp(t, SYS_TGT_TITLE) == 0) {
        vn_text_clear_choices();
        core_st.sys_menu = false;
        core_st.sys_menu_over_choice = false;
        go_title();
    } else if(strncmp(t, SYS_TGT_STORY, strlen(SYS_TGT_STORY)) == 0) {
        vn_text_clear_choices();
        core_st.sys_menu = false;
        core_st.sys_menu_over_choice = false;
        switch_story(atoi(t + strlen(SYS_TGT_STORY)));
    } else {
        close_sys_menu();
    }
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
            core_st.skipping = skip_active;
        }
    }
    uint32_t pressed = btns & ~core_st.prev_btns;
    core_st.prev_btns = btns;

    if(core_st.autoplay) {
        /* Test mode: skip everything, pick random options, and every so many
           choices switch to the other story like a player at a disc swap. */
        if(core_st.autoplay_slow) {
            /* no skip: read like auto mode, a line every 0.4 s after it is shown */
            core_st.auto_mode = true;
            core_st.auto_advance_ms = 400U;
        } else {
            skip_active = true;
            core_st.skipping = true;
        }
        pressed = 0U;
        int other = -1;
        for(int i = 0; i < VN_MAX_STORIES; i++)
            if(core_st.story_defined[i] && core_st.story_cur >= 0 &&
               core_st.story_defined[core_st.story_cur] && i != core_st.story_cur)
                other = i;
        if((core_st.frame_cnt % 3600U) == 0U)
            dbglog(DBG_INFO, "[AUTO] frame %lu choices %lu poll_badpos %lu\n",
                   (unsigned long)core_st.frame_cnt, (unsigned long)core_st.auto_choices,
                   (unsigned long)core_st.poll_badpos);
        /* A story waiting for the other one loops without choices: switch
           after a minute without one, as a player would. */
        if(!core_st.sys_menu)
            core_st.auto_menu_presses = 0U;
        if(core_st.sys_menu) {
            /* START menu / slot list: take the selected item (main menu ->
               slot list -> slot); give up with B if it does not close. */
            if(core_st.frame_cnt & 1U)
                pressed = (++core_st.auto_menu_presses > 6U) ? CONT_B : CONT_A;
        } else if(vn_text_get_choice_count() > 0 && vn_text_is_complete() &&
                  (core_st.auto_choices % 50U) == 25U && !in_title() &&
                  core_st.title_script[0] != '\0') {
            /* exercise the START menu: save, or every other time load */
            bool load = ((core_st.auto_choices / 50U) & 1U) != 0U;
            dbglog(DBG_INFO, "[AUTO] choice %lu: START menu %s (slot %d)\n",
                   (unsigned long)core_st.auto_choices, load ? "load" : "save", core_st.last_slot);
            core_st.auto_choices++;
            open_sys_menu();
            if(load)
                vn_text_move_choice(1);
        } else if(++core_st.auto_idle_frames > 3600U && other >= 0 && core_st.autoplay_switch > 0U) {
            dbglog(DBG_INFO, "[AUTO] no choice for 3600 frames at %s:%lu: switch story %d -> %d\n",
                   vn_script_get_path(), (unsigned long)vn_script_get_line(),
                   core_st.story_cur, other);
            core_st.auto_idle_frames = 0U;
            switch_story(other);
        } else if(!core_st.sys_menu && vn_text_get_choice_count() > 0 && vn_text_is_complete() &&
                  (core_st.frame_cnt & 1U)) {
            core_st.auto_choices++;
            core_st.auto_idle_frames = 0U;
            if(other >= 0 && core_st.autoplay_switch > 0U &&
               (core_st.auto_choices % core_st.autoplay_switch) == 0U) {
                dbglog(DBG_INFO, "[AUTO] choice %lu: switch story %d -> %d\n",
                       (unsigned long)core_st.auto_choices, core_st.story_cur, other);
                switch_story(other);
            } else {
                /* least-taken option of this CHOICE first (random among
                   equals), so free-roam menus get explored, not wandered */
                static uint8_t visits[8192];
                int n = vn_text_get_choice_count();
                uint32_t h = (uint32_t)vn_script_get_line() * 2654435761U;
                for(const char *p = vn_script_get_path(); *p; p++)
                    h = (h ^ (uint8_t)*p) * 16777619U;
                int r = rand() % n;
                uint8_t best = 255U;
                for(int k = 0; k < n; k++) {
                    int i = (r + k) % n;
                    uint8_t v = visits[(h + (uint32_t)i * 40503U) & 8191U];
                    if(v < best) {
                        best = v;
                        r = i;
                    }
                }
                uint8_t *vp = &visits[(h + (uint32_t)r * 40503U) & 8191U];
                if(*vp < 255U)
                    (*vp)++;
                vn_text_move_choice(r - vn_text_get_selected_choice());
                pressed = CONT_A;
            }
        }
    }

    if(core_st.notice_timer_ms > 0U) {
        if(core_st.notice_timer_ms <= dt_ms) {
            core_st.notice_timer_ms = 0U;
            core_st.notice_msg[0] = '\0';
        } else {
            core_st.notice_timer_ms -= dt_ms;
        }
    }

    /* SYSLOAD from the previous frame: the slot list; "back" carries on with
       the script's next line (close_sys_menu). */
    if(core_st.load_requested) {
        core_st.load_requested = false;
        core_st.menu_from_script = true;
        core_st.sys_menu_over_choice = false;
        open_slot_menu(MENU_LOAD);
        pressed = 0U;
    }

    if(pressed & CONT_START) {
        if(core_st.title_script[0] == '\0') {
            core_st.quit_requested = true;     /* no title screen: old behaviour */
            return 0;
        }
        if(in_title()) {
            pressed |= CONT_A;                 /* "Press START" on the title screen */
        } else if(!core_st.sys_menu && !vn_backlog_is_visible()) {
            open_sys_menu();
            pressed = 0U;
        }
        pressed &= ~CONT_START;
    }

    /* Quick Save / Quick Load / Multi-Sight Zap via L-Trigger */
    if(core_st.sys_menu) {
        sys_menu_input(pressed);
    } else if(l_held && (pressed & CONT_A) && !in_title()) {
        handle_save(core_st.last_slot);      /* quick save: the slot used last */
    } else if(l_held && (pressed & CONT_B)) {
        handle_load(core_st.last_slot);
    } else if(l_held && (pressed & CONT_X) && vn_site_is_enabled()) {
        if(vn_site_is_locked(vn_site_get_active())) {
            vn_audio_play_se(core_st.se_cursor);
            vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[SITE LOCKED]");
            core_st.notice_timer_ms = 1400U;
        } else {
            stop_line_voice();
            vn_audio_play_se(core_st.se_cursor);
            vn_text_clear_choices();
            vn_site_switch(1 - vn_site_get_active());
            vn_script_advance();
            vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[SITE SWITCH]");
            core_st.notice_timer_ms = 1400U;
        }
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
        bool choosing = vn_text_get_choice_count() > 0 && vn_text_is_complete();
        if((pressed & CONT_B) && choosing && vn_text_get_choice_cancel()[0] != '\0') {
            /* CANCEL target of this choice (the PS "＄" item) */
            char tgt[64];
            vn_copy_str(tgt, sizeof(tgt), vn_text_get_choice_cancel());
            stop_line_voice();
            vn_audio_play_se(core_st.se_cancel);
            vn_text_clear_choices();
            vn_script_jump_label(tgt);
            vn_script_advance();
            pressed = 0U;
            choosing = false;
        } else if(pressed & CONT_B) {
            vn_text_set_window_visible(!vn_text_is_window_visible());
        }

        if(choosing) {
            if((pressed & CONT_DPAD_UP) ||
               ((pressed & CONT_DPAD_LEFT) && vn_text_is_map_choice())) {
                vn_text_move_choice(-1);
                vn_audio_play_se(core_st.se_cursor);
            } else if((pressed & CONT_DPAD_DOWN) ||
                      ((pressed & CONT_DPAD_RIGHT) && vn_text_is_map_choice())) {
                vn_text_move_choice(1);
                vn_audio_play_se(core_st.se_cursor);
            } else if(pressed & CONT_A) {
                int sel = vn_text_get_selected_choice();
                const char *tgt = vn_text_get_choice_target(sel);
                char target_buf[64];
                vn_copy_str(target_buf, sizeof(target_buf), tgt);
                if(strncmp(target_buf, "@site:", 6) == 0 && vn_site_is_locked(vn_site_get_active())) {
                    vn_audio_play_se(core_st.se_cursor);
                    vn_copy_str(core_st.notice_msg, sizeof(core_st.notice_msg), "[SITE LOCKED]");
                    core_st.notice_timer_ms = 1400U;
                } else {
                    stop_line_voice();
                    vn_audio_play_se(core_st.se_decide);
                    vn_text_clear_choices();
                    if(strncmp(target_buf, "@call:", 6) == 0) {
                        vn_script_call_label(target_buf + 6);
                    } else if(strncmp(target_buf, "@site:", 6) == 0) {
                        vn_site_switch(atoi(target_buf + 6));
                    } else if(target_buf[0] != '\0') {
                        vn_script_jump_label(target_buf);
                    }
                    vn_script_advance();
                }
            }
        } else if(vn_script_is_waiting_fx()) {
            if(vn_script_tick_wait(dt_ms) || skip_active || (pressed & CONT_A)) {
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
                    stop_line_voice();
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
    if(core_st.quiet_status) {
        /* Only what the player needs: notices, and SKIP / AUTO while on. */
        if(core_st.notice_msg[0] != '\0')
            snprintf(status, sizeof(status), "%s", core_st.notice_msg);
        else if(skip_active || core_st.auto_mode)
            snprintf(status, sizeof(status), "%s", mode_tag);
        else
            status[0] = '\0';
    } else if(core_st.notice_msg[0] != '\0') {
        snprintf(status, sizeof(status), "%s  %s", mode_tag, core_st.notice_msg);
    } else if(vn_site_is_enabled()) {
        snprintf(status, sizeof(status), "%s | A:Next Y:Log L+X:Site R:Skip", mode_tag);
    } else {
        snprintf(status, sizeof(status), "%s | A:Next B:Hide X:Auto Y:Log R:Skip", mode_tag);
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

    vn_fx_reset();          /* e.g. the ending faded to black, then the title */
    core_st.quit_requested = false;
    vn_script_advance();

    while(vn_step_frame() != 0)
        ;

    return 0;
}
