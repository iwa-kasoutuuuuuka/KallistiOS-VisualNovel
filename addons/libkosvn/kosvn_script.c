/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_script.c
 * Line-oriented Visual Novel scenario parser and VM.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <kos/dbglog.h>

#include "kosvn_internal.h"

typedef struct vn_label_entry {
    char     name[64];
    uint32_t line_idx;
    uint32_t hash;
} vn_label_entry_t;

static struct {
    char             path[64];
    char            *raw_buf;
    char           **lines;
    uint32_t         line_count;
    uint32_t         pc;
    uint32_t         cur_cmd_line;
    bool             ended;
    bool             waiting_fx;
    uint32_t         wait_ms;      /* >0: WAIT timer, otherwise WAIT_FX */
    uint32_t         bg_line1;     /* 1 + line of the last BG executed, 0 = none */
    vn_label_entry_t labels[VN_MAX_LABELS];
    int              label_count;
    int32_t          flags[VN_MAX_FLAGS];
    uint32_t         bits[VN_MAX_BITS / 32]; /* survive script loads (JUMP) */
    uint32_t         call_stack[VN_MAX_CALL_DEPTH];
    int              call_depth;
    bool             multi_sight_enabled;
    int              active_site;
    vn_site_save_t   sites[VN_MAX_SITES];
    char             site_start_labels[VN_MAX_SITES][64];
    char             site_dialogues[VN_MAX_SITES][512];
} vm_st;

static inline uint32_t fnv1a_hash(const char *s) {
    uint32_t h = 2166136261U;
    if(!s)
        return h;
    while(*s != '\0') {
        h ^= (uint8_t)*s++;
        h *= 16777619U;
    }
    return h;
}

static bool eval_cond(int idx, const char *op, int32_t rhs) {
    if(!op || op[0] == '\0')
        return false;
    int32_t lhs = vn_script_get_flag(idx);
    char c0 = op[0];
    char c1 = op[1];
    if(c0 == '=' && c1 == '=' && op[2] == '\0') return (lhs == rhs);
    if(c0 == '!' && c1 == '=' && op[2] == '\0') return (lhs != rhs);
    if(c0 == '>') {
        if(c1 == '=' && op[2] == '\0') return (lhs >= rhs);
        if(c1 == '\0') return (lhs > rhs);
    }
    if(c0 == '<') {
        if(c1 == '=' && op[2] == '\0') return (lhs <= rhs);
        if(c1 == '\0') return (lhs < rhs);
    }
    return false;
}

static void update_site_hud(void) {
    if(!vm_st.multi_sight_enabled) {
        vn_text_set_site_badge("");
        return;
    }
    const char *name = vm_st.sites[vm_st.active_site].site_name;
    if(name[0] != '\0') {
        char badge[48];
        snprintf(badge, sizeof(badge), "SITE: %s", name);
        vn_text_set_site_badge(badge);
    } else {
        vn_text_set_site_badge("");
    }
}

static void save_site_snapshot(vn_site_save_t *dst) {
    dst->initialized = 1U;
    dst->script_line = vm_st.cur_cmd_line;
    dst->call_depth = (uint16_t)vm_st.call_depth;
    for(int i = 0; i < VN_MAX_CALL_DEPTH; i++)
        dst->call_stack[i] = vm_st.call_stack[i];
    vn_copy_str(dst->bg_name, sizeof(dst->bg_name), vn_gfx_get_bg_name());
    for(int i = 0; i < VN_MAX_SPRITES; i++) {
        vn_copy_str(dst->char_names[i], sizeof(dst->char_names[i]),
                    vn_gfx_get_char_name((vn_sprite_pos_t)i));
    }
    vn_copy_str(dst->cutin_name, sizeof(dst->cutin_name), vn_cutin_get_name());
    vn_copy_str(dst->speaker_name, sizeof(dst->speaker_name), vn_text_get_speaker());
    vn_copy_str(dst->bgm_name, sizeof(dst->bgm_name), vn_audio_get_bgm_name());
    if(vm_st.active_site >= 0 && vm_st.active_site < VN_MAX_SITES) {
        vn_copy_str(vm_st.site_dialogues[vm_st.active_site],
                    sizeof(vm_st.site_dialogues[vm_st.active_site]),
                    vn_text_get_dialogue());
    }
}

static char *trim_ascii(char *s) {
    while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    size_t len = strlen(s);
    while(len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                      s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static char *sjis_find_ascii(char *s, char target) {
    for(size_t i = 0; s[i] != '\0'; i++) {
        uint8_t c = (uint8_t)s[i];
        uint8_t m = c & 0xF0U;
        if((c & 0x80U) != 0U && (m == 0x80U || m == 0x90U || m == 0xE0U) && s[i + 1] != '\0') {
            i++;
            continue;
        }
        if(s[i] == target)
            return &s[i];
    }
    return NULL;
}

static void add_parsed_option(const char *arg_str) {
    char opt_copy[256];
    vn_copy_str(opt_copy, sizeof(opt_copy), arg_str);
    char *sep = sjis_find_ascii(opt_copy, '|');
    if(!sep)
        return;
    *sep = '\0';
    char *lbl = trim_ascii(opt_copy);
    char *tgt = trim_ascii(sep + 1);
    /* map place: "@dot_x,dot_y,pawn_x,pawn_y,rank label" */
    int dx, dy, px, py, rk, used = 0;
    if(lbl[0] == '@' && sscanf(lbl + 1, "%d,%d,%d,%d,%d %n", &dx, &dy, &px, &py, &rk, &used) == 5 && used > 0) {
        vn_text_add_choice_at(lbl + 1 + used, tgt, dx, dy, px, py, rk);
        return;
    }
    vn_text_add_choice(lbl, tgt);
}

static void unescape_newlines(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for(size_t i = 0; src[i] != '\0' && j + 1 < dst_sz; i++) {
        uint8_t c = (uint8_t)src[i];
        uint8_t m = c & 0xF0U;
        /* Shift-JIS trail bytes can be 0x5C ('\\'), so skip full 2-byte glyphs untouched. */
        if((c & 0x80U) != 0U && (m == 0x80U || m == 0x90U || m == 0xE0U) && src[i + 1] != '\0') {
            if(j + 2 >= dst_sz)
                break;
            dst[j++] = src[i];
            dst[j++] = src[i + 1];
            i++;
            continue;
        }
        if(src[i] == '\\' && src[i + 1] == 'n') {
            dst[j++] = '\n';
            i++;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static vn_sprite_pos_t parse_pos(const char *tok) {
    if(strcmp(tok, "left") == 0) return VN_POS_LEFT;
    if(strcmp(tok, "right") == 0) return VN_POS_RIGHT;
    return VN_POS_CENTER;
}

static const char *split_cmd(const char *line, char *cmd_out, size_t cmd_sz) {
    size_t i = 0;
    while(line[i] != '\0' && line[i] != ' ' && line[i] != '\t' && i + 1 < cmd_sz) {
        cmd_out[i] = line[i];
        i++;
    }
    cmd_out[i] = '\0';
    const char *rest = line + i;
    while(*rest == ' ' || *rest == '\t')
        rest++;
    return rest;
}

void vn_script_unload(void) {
    if(vm_st.lines) {
        free(vm_st.lines);
        vm_st.lines = NULL;
    }
    if(vm_st.raw_buf) {
        free(vm_st.raw_buf);
        vm_st.raw_buf = NULL;
    }
    vm_st.line_count = 0;
    vm_st.pc = 0;
    vm_st.cur_cmd_line = 0;
    vm_st.label_count = 0;
    vm_st.call_depth = 0;
    vm_st.multi_sight_enabled = false;
    vm_st.active_site = 0;
    memset(vm_st.sites, 0, sizeof(vm_st.sites));
    memset(vm_st.site_start_labels, 0, sizeof(vm_st.site_start_labels));
    memset(vm_st.site_dialogues, 0, sizeof(vm_st.site_dialogues));
    vm_st.ended = true;
    vm_st.waiting_fx = false;
    vm_st.bg_line1 = 0U;
    vm_st.wait_ms = 0U;
    vm_st.path[0] = '\0';
    vn_text_set_site_badge("");
}

int vn_script_load(const char *path) {
    dbglog(DBG_INFO, "kosvn_script: load '%s'\n", path ? path : "(null)");
    if(!path)
        return -1;

    vn_script_unload();

    FILE *fp = vn_fopen(path, "rb");
    if(!fp) {
        dbglog(DBG_ERROR, "kosvn_script: cannot open '%s'\n", path);
        return -1;
    }

    size_t cap = 4096U;
    size_t len = 0U;
    char *buf = (char *)malloc(cap);
    if(!buf) {
        fclose(fp);
        return -1;
    }

    for(;;) {
        if(len + 1024U >= cap) {
            cap *= 2U;
            char *nb = (char *)realloc(buf, cap);
            if(!nb) {
                free(buf);
                fclose(fp);
                return -1;
            }
            buf = nb;
        }
        size_t n = fread(buf + len, 1, 1024U, fp);
        len += n;
        if(n < 1024U)
            break;
    }
    fclose(fp);
    buf[len] = '\0';

    uint32_t cnt = 1U;
    for(size_t i = 0; i < len; i++) {
        if(buf[i] == '\n')
            cnt++;
    }

    char **arr = (char **)calloc(cnt, sizeof(char *));
    if(!arr) {
        free(buf);
        return -1;
    }

    uint32_t idx = 0U;
    char *cur = buf;
    for(;;) {
        char *nl = strchr(cur, '\n');
        if(nl)
            *nl = '\0';
        arr[idx] = trim_ascii(cur);

        char cmd[32];
        const char *arg = split_cmd(arr[idx], cmd, sizeof(cmd));
        if(strcmp(cmd, "LABEL") == 0 && arg[0] != '\0' && vm_st.label_count >= VN_MAX_LABELS) {
            dbglog(DBG_ERROR, "kosvn_script: '%s' has more than %d labels, '%s' dropped\n",
                   path, VN_MAX_LABELS, arg);
        } else if(strcmp(cmd, "LABEL") == 0 && arg[0] != '\0') {
            vn_label_entry_t *le = &vm_st.labels[vm_st.label_count++];
            vn_copy_str(le->name, sizeof(le->name), arg);
            le->line_idx = idx;
            le->hash = fnv1a_hash(le->name);
        }

        idx++;
        if(!nl)
            break;
        cur = nl + 1;
    }

    vm_st.raw_buf = buf;
    vm_st.lines = arr;
    vm_st.line_count = idx;
    vm_st.pc = 0U;
    vm_st.cur_cmd_line = 0U;
    vm_st.call_depth = 0;
    vm_st.multi_sight_enabled = false;
    vm_st.active_site = 0;
    memset(vm_st.sites, 0, sizeof(vm_st.sites));
    memset(vm_st.site_start_labels, 0, sizeof(vm_st.site_start_labels));
    memset(vm_st.site_dialogues, 0, sizeof(vm_st.site_dialogues));
    vm_st.ended = false;
    vm_st.waiting_fx = false;
    vm_st.wait_ms = 0U;
    vn_copy_str(vm_st.path, sizeof(vm_st.path), path);
    vn_text_set_site_badge("");
    return 0;
}

int vn_script_jump_label(const char *label) {
    if(!label)
        return -1;
    uint32_t h = fnv1a_hash(label);
    for(int i = 0; i < vm_st.label_count; i++) {
        if(vm_st.labels[i].hash == h && strcmp(vm_st.labels[i].name, label) == 0) {
            vm_st.pc = vm_st.labels[i].line_idx;
            return 0;
        }
    }
    dbglog(DBG_WARNING, "kosvn_script: unknown label '%s'\n", label);
    return -1;
}

static void restore_choice_prompt_before(uint32_t line) {
    if(line >= vm_st.line_count || !vm_st.lines)
        return;
    char cmd[32];
    split_cmd(vm_st.lines[line], cmd, sizeof(cmd));
    if(strcmp(cmd, "CHOICE") != 0)
        return;

    for(int32_t i = (int32_t)line - 1; i >= 0; i--) {
        const char *ln = vm_st.lines[i];
        if(ln[0] == '\0' || ln[0] == '#')
            continue;
        char prev_cmd[32];
        const char *prev_arg = split_cmd(ln, prev_cmd, sizeof(prev_cmd));
        if(strcmp(prev_cmd, "TEXT") == 0) {
            char text_buf[512];
            unescape_newlines(text_buf, sizeof(text_buf), prev_arg);
            vn_text_set_dialogue(text_buf);
            return;
        }
        if(strcmp(prev_cmd, "LABEL") == 0)
            break;
    }
}

int vn_script_jump_line(uint32_t line) {
    if(line >= vm_st.line_count)
        return -1;
    vm_st.pc = line;
    vm_st.cur_cmd_line = line;
    vm_st.ended = false;
    vm_st.waiting_fx = false;
    vm_st.wait_ms = 0U;
    restore_choice_prompt_before(line);
    return 0;
}

void vn_script_set_flag(int idx, int32_t val) {
    if(idx >= 0 && idx < VN_MAX_FLAGS)
        vm_st.flags[idx] = val;
}

int32_t vn_script_get_flag(int idx) {
    if(idx < 0 || idx >= VN_MAX_FLAGS)
        return 0;
    return vm_st.flags[idx];
}

void vn_script_add_flag(int idx, int32_t delta) {
    if(idx >= 0 && idx < VN_MAX_FLAGS)
        vm_st.flags[idx] += delta;
}

static vn_movie_handler_t movie_handler;

void vn_set_movie_handler(vn_movie_handler_t fn) {
    movie_handler = fn;
}

void vn_script_set_bit(int idx, bool on) {
    if(idx < 0 || idx >= VN_MAX_BITS)
        return;
    if(on)
        vm_st.bits[idx >> 5] |= (1U << (idx & 31));
    else
        vm_st.bits[idx >> 5] &= ~(1U << (idx & 31));
}

bool vn_script_get_bit(int idx) {
    if(idx < 0 || idx >= VN_MAX_BITS)
        return false;
    return (vm_st.bits[idx >> 5] >> (idx & 31)) & 1U;
}

/* Called once per frame while the VM is blocked on WAIT / WAIT_FX.
   Returns true when the wait is over. */
bool vn_script_tick_wait(uint32_t dt_ms) {
    if(vm_st.wait_ms > 0U) {
        if(vm_st.wait_ms <= dt_ms) {
            vm_st.wait_ms = 0U;
            return true;
        }
        vm_st.wait_ms -= dt_ms;
        return false;
    }
    return !vn_fx_is_busy();
}

int vn_script_call_label(const char *label) {
    if(!label)
        return -1;
    if(vm_st.call_depth < VN_MAX_CALL_DEPTH)
        vm_st.call_stack[vm_st.call_depth++] = vm_st.cur_cmd_line;
    return vn_script_jump_label(label);
}

int vn_script_return(void) {
    if(vm_st.call_depth <= 0) {
        dbglog(DBG_WARNING, "kosvn_script: RET with empty call stack\n");
        return -1;
    }
    uint32_t ret_line = vm_st.call_stack[--vm_st.call_depth];
    if(ret_line < vm_st.line_count)
        vm_st.pc = ret_line;
    return 0;
}

int vn_site_init(int site_id, const char *site_name_sjis, const char *start_label) {
    if(site_id < 0 || site_id >= VN_MAX_SITES || !start_label)
        return -1;

    vm_st.multi_sight_enabled = true;
    vn_copy_str(vm_st.sites[site_id].site_name, sizeof(vm_st.sites[site_id].site_name), site_name_sjis);
    vn_copy_str(vm_st.site_start_labels[site_id], sizeof(vm_st.site_start_labels[site_id]), start_label);
    vm_st.sites[site_id].initialized = 0U;
    vm_st.sites[site_id].locked = 0U;
    update_site_hud();
    return 0;
}

int vn_site_switch(int site_id) {
    if(site_id < 0 || site_id >= VN_MAX_SITES)
        return -1;

    vm_st.multi_sight_enabled = true;
    if(site_id == vm_st.active_site && vm_st.sites[site_id].initialized)
        return 0;

    if(vm_st.sites[vm_st.active_site].initialized)
        save_site_snapshot(&vm_st.sites[vm_st.active_site]);

    vm_st.active_site = site_id;
    update_site_hud();

    vn_site_save_t *dst = &vm_st.sites[site_id];
    if(dst->initialized) {
        vn_bg_set(dst->bg_name);
        for(int i = 0; i < VN_MAX_SPRITES; i++)
            vn_char_set((vn_sprite_pos_t)i, dst->char_names[i]);
        vn_cutin_set(dst->cutin_name);
        vn_text_set_speaker(dst->speaker_name);
        if(strcmp(dst->bgm_name, vn_audio_get_bgm_name()) != 0)
            vn_audio_play_bgm(dst->bgm_name);
        vm_st.call_depth = (dst->call_depth <= VN_MAX_CALL_DEPTH) ? (int)dst->call_depth : 0;
        for(int i = 0; i < VN_MAX_CALL_DEPTH; i++)
            vm_st.call_stack[i] = dst->call_stack[i];
        if(dst->script_line < vm_st.line_count)
            vm_st.pc = dst->script_line;
        if(vm_st.site_dialogues[site_id][0] != '\0') {
            vn_text_set_dialogue(vm_st.site_dialogues[site_id]);
        } else if(dst->script_line < vm_st.line_count) {
            restore_choice_prompt_before(dst->script_line);
        }
        vm_st.ended = false;
        vm_st.waiting_fx = false;
        vm_st.wait_ms = 0U;
    } else {
        dst->initialized = 1U;
        vm_st.call_depth = 0;
        vn_char_clear_all();
        vn_cutin_clear();
        vn_text_set_speaker("");
        vn_script_jump_label(vm_st.site_start_labels[site_id]);
    }
    return 0;
}

int vn_site_get_active(void) { return vm_st.active_site; }
bool vn_site_is_enabled(void) { return vm_st.multi_sight_enabled; }

const char *vn_site_get_name(int site_id) {
    if(site_id < 0 || site_id >= VN_MAX_SITES)
        return "";
    return vm_st.sites[site_id].site_name;
}

void vn_site_set_locked(int site_id, bool locked) {
    if(site_id >= 0 && site_id < VN_MAX_SITES)
        vm_st.sites[site_id].locked = locked ? 1U : 0U;
}

bool vn_site_is_locked(int site_id) {
    if(site_id < 0 || site_id >= VN_MAX_SITES)
        return false;
    return vm_st.sites[site_id].locked != 0U;
}

void vn_script_export_state(vn_save_state_t *st) {
    if(!st)
        return;
    if(vm_st.multi_sight_enabled && vm_st.sites[vm_st.active_site].initialized)
        save_site_snapshot(&vm_st.sites[vm_st.active_site]);
    st->multi_sight_enabled = vm_st.multi_sight_enabled ? 1U : 0U;
    st->active_site = (uint8_t)vm_st.active_site;
    st->call_depth = (uint16_t)vm_st.call_depth;
    for(int i = 0; i < VN_MAX_CALL_DEPTH; i++)
        st->call_stack[i] = vm_st.call_stack[i];
    for(int i = 0; i < VN_MAX_SITES; i++)
        st->sites[i] = vm_st.sites[i];
}

void vn_script_import_state(const vn_save_state_t *st) {
    if(!st)
        return;
    vm_st.multi_sight_enabled = (st->multi_sight_enabled != 0U);
    vm_st.active_site = (st->active_site < VN_MAX_SITES) ? (int)st->active_site : 0;
    vm_st.call_depth = (st->call_depth <= VN_MAX_CALL_DEPTH) ? (int)st->call_depth : 0;
    for(int i = 0; i < VN_MAX_CALL_DEPTH; i++)
        vm_st.call_stack[i] = st->call_stack[i];
    for(int i = 0; i < VN_MAX_SITES; i++)
        vm_st.sites[i] = st->sites[i];
    memset(vm_st.site_dialogues, 0, sizeof(vm_st.site_dialogues));
    update_site_hud();
}

uint32_t vn_script_get_line(void) { return vm_st.cur_cmd_line; }
const char *vn_script_get_path(void) { return vm_st.path; }
bool vn_script_is_waiting_fx(void) { return vm_st.waiting_fx; }
void vn_script_clear_wait_fx(void) { vm_st.waiting_fx = false; vm_st.wait_ms = 0U; }
bool vn_script_is_ended(void) { return vm_st.ended; }

/* After a load or story switch: rebuild what the screen showed at <line>
   (the BG and the overlays / speaker set after it), without running anything
   else. The BG from an earlier script is restored by the caller. */
uint32_t vn_script_get_bg_line1(void) { return vm_st.bg_line1; }

/* Rebuild the picture of a saved moment. bg_line1 (1 + line of the BG command
   that was really executed) is used when known: the BG written just above the
   saved line is often not the one on screen (hub labels are reached by GOTO
   from other places). That BG and the OVERLAY lines right after it are
   replayed, then NAME / TEXTBOX from the text above the saved line. */
void vn_script_replay_scene(uint32_t line, uint32_t bg_line1) {
    if(line > vm_st.line_count)
        line = vm_st.line_count;
    uint32_t from = 0U;
    for(uint32_t i = line; i > 0U; i--) {
        char cmd[32];
        split_cmd(vm_st.lines[i - 1U], cmd, sizeof(cmd));
        if(strcmp(cmd, "BG") == 0) {
            from = i - 1U;
            break;
        }
    }
    bool known = false;
    if(bg_line1 > 0U && bg_line1 - 1U < vm_st.line_count) {
        char cmd[32];
        split_cmd(vm_st.lines[bg_line1 - 1U], cmd, sizeof(cmd));
        known = strcmp(cmd, "BG") == 0;
    }
    if(known) {
        vm_st.bg_line1 = bg_line1;    /* a later save keeps pointing at it */
        for(uint32_t i = bg_line1 - 1U; i < vm_st.line_count; i++) {
            char cmd[32];
            const char *arg = split_cmd(vm_st.lines[i], cmd, sizeof(cmd));
            if(i == bg_line1 - 1U) {
                vn_overlay_clear_all();
                vn_bg_set(arg);
            } else if(strcmp(cmd, "OVERLAY") == 0) {
                vn_overlay_command(arg);
            } else if(strcmp(cmd, "OVERLAY_TRACK") == 0) {
                vn_overlay_track(arg);
            } else if(strcmp(cmd, "WAIT_FX") != 0) {
                break;
            }
        }
    }
    for(uint32_t i = from; i < line; i++) {
        char cmd[32];
        const char *arg = split_cmd(vm_st.lines[i], cmd, sizeof(cmd));
        if(known && (strcmp(cmd, "BG") == 0 || strncmp(cmd, "OVERLAY", 7) == 0))
            continue;
        if(strcmp(cmd, "BG") == 0) {
            vn_overlay_clear_all();
            vn_bg_set(arg);
        } else if(strcmp(cmd, "OVERLAY") == 0) {
            vn_overlay_command(arg);
        } else if(strcmp(cmd, "OVERLAY_TRACK") == 0) {
            vn_overlay_track(arg);
        } else if(strcmp(cmd, "OVERLAY_CLEAR") == 0) {
            if(arg[0] >= '0' && arg[0] <= '9')
                vn_overlay_clear(atoi(arg));
            else
                vn_overlay_clear_all();
        } else if(strcmp(cmd, "NAME") == 0) {
            vn_text_set_speaker(strcmp(arg, "none") == 0 ? "" : arg);
        } else if(strcmp(cmd, "TEXTBOX") == 0) {
            vn_text_set_box_visible(strcmp(arg, "off") != 0);
        }
    }
    /* TEXT_AREA is set once at the top of a script */
    for(uint32_t i = 0; i < line && i < 4U; i++) {
        char cmd[32];
        const char *arg = split_cmd(vm_st.lines[i], cmd, sizeof(cmd));
        if(strcmp(cmd, "TEXT_AREA") != 0)
            continue;
        unsigned x = 0, y = 0, w = 0;
        if(sscanf(arg, "%u %u %u", &x, &y, &w) == 3)
            vn_text_set_area(x, y, w);
        else
            vn_text_set_area(0, 0, 0);
    }
}

int vn_script_advance(void) {
    uint32_t budget = 0U;
    while(vm_st.pc < vm_st.line_count) {
        uint32_t line_idx = vm_st.pc;
        if(++budget > 20000U) {
            /* a loop of non-blocking lines: stop here instead of hanging
               (the line runs again on the next A press) */
            dbglog(DBG_WARNING, "kosvn_script: no blocking line for 20000 commands at %lu\n",
                   (unsigned long)(line_idx + 1U));
            vm_st.cur_cmd_line = line_idx;
            return 1;
        }
        const char *line = vm_st.lines[vm_st.pc++];
        if(line[0] == '\0' || line[0] == '#')
            continue;

        char cmd[32];
        const char *arg = split_cmd(line, cmd, sizeof(cmd));

        if(strcmp(cmd, "LABEL") == 0) {
            continue;
        } else if(strcmp(cmd, "BG") == 0) {
            /* BG <path> [ms] : ms = crossfade (PS \d 2), cut while skipping */
            vm_st.bg_line1 = line_idx + 1U;
            vn_overlay_clear_all();
            if(vn_core_is_skipping())
                vn_bg_set(arg);
            else
                vn_bg_crossfade(arg);
        } else if(strcmp(cmd, "OVERLAY") == 0) {
            vn_overlay_command(arg);
        } else if(strcmp(cmd, "OVERLAY_TRACK") == 0) {
            vn_overlay_track(arg);
        } else if(strcmp(cmd, "OVERLAY_CLEAR") == 0) {
            if(arg[0] >= '0' && arg[0] <= '9')
                vn_overlay_clear(atoi(arg));
            else
                vn_overlay_clear_all();
        } else if(strcmp(cmd, "CHAR") == 0) {
            char pos_tok[16];
            const char *spec = split_cmd(arg, pos_tok, sizeof(pos_tok));
            vn_char_set(parse_pos(pos_tok), spec);
        } else if(strcmp(cmd, "CLEAR_CHAR") == 0) {
            if(arg[0] == '\0' || strcmp(arg, "all") == 0)
                vn_char_clear_all();
            else
                vn_char_clear(parse_pos(arg));
        } else if(strcmp(cmd, "NAME") == 0) {
            if(strcmp(arg, "none") == 0)
                vn_text_set_speaker("");
            else
                vn_text_set_speaker(arg);
        } else if(strcmp(cmd, "TEXT") == 0) {
            char text_buf[512];
            unescape_newlines(text_buf, sizeof(text_buf), arg);
            vn_text_clear_choices();
            vn_text_set_dialogue(text_buf);
            vm_st.cur_cmd_line = line_idx;
            return 1;
        } else if(strcmp(cmd, "CHOICE") == 0) {
            vn_text_clear_choices();
            while(vm_st.pc < vm_st.line_count) {
                char opt_cmd[32];
                const char *opt_arg = split_cmd(vm_st.lines[vm_st.pc], opt_cmd, sizeof(opt_cmd));
                if(strcmp(opt_cmd, "OPTION") == 0) {
                    vm_st.pc++;
                    add_parsed_option(opt_arg);
                } else if(strcmp(opt_cmd, "OPTION_IF") == 0) {
                    vm_st.pc++;
                    int fidx = 0;
                    char op[8] = {0};
                    long fval = 0;
                    int consumed = 0;
                    if(sscanf(opt_arg, "%d %7s %ld %n", &fidx, op, &fval, &consumed) == 3 &&
                       consumed > 0 && eval_cond(fidx, op, (int32_t)fval)) {
                        add_parsed_option(opt_arg + consumed);
                    }
                } else if(strcmp(opt_cmd, "MAP") == 0) {
                    /* MAP <map raw16> <marker raw16> : the options are places on it */
                    vm_st.pc++;
                    char mp[64] = {0}, pp[64] = {0};
                    sscanf(opt_arg, "%63s %63s", mp, pp);
                    vn_text_set_choice_map(mp, pp);
                } else if(strcmp(opt_cmd, "CANCEL") == 0) {
                    /* CANCEL <target> : where B goes (not shown as an item) */
                    vm_st.pc++;
                    vn_text_set_choice_cancel(trim_ascii((char *)opt_arg));
                } else if(strcmp(opt_cmd, "OPTION_BIT") == 0) {
                    /* OPTION_BIT <bit> <0|1> <text> | <target> : shown when bit == value */
                    vm_st.pc++;
                    int bidx = 0, bval = 0, consumed = 0;
                    if(sscanf(opt_arg, "%d %d %n", &bidx, &bval, &consumed) == 2 &&
                       consumed > 0 && vn_script_get_bit(bidx) == (bval != 0)) {
                        add_parsed_option(opt_arg + consumed);
                    }
                } else {
                    break;
                }
            }
            if(vn_text_get_choice_count() == 0) {
                /* Every option was conditional and none applies: take the
                   cancel route if there is one, otherwise carry on. */
                char cancel[64];
                vn_copy_str(cancel, sizeof(cancel), vn_text_get_choice_cancel());
                dbglog(DBG_WARNING, "kosvn_script: CHOICE at line %lu has no options%s%s\n",
                       (unsigned long)(line_idx + 1U), cancel[0] ? ", cancel -> " : "", cancel);
                vn_text_clear_choices();
                if(cancel[0] != '\0')
                    vn_script_jump_label(cancel);
                continue;
            }
            vn_text_choice_start();
            vn_text_reveal_all();
            vm_st.cur_cmd_line = line_idx;
            return 1;
        } else if(strcmp(cmd, "BIT") == 0) {
            int idx = 0, val = 0;
            if(sscanf(arg, "%d %d", &idx, &val) == 2)
                vn_script_set_bit(idx, val != 0);
        } else if(strcmp(cmd, "BIT_RANGE") == 0) {
            int from = 0, to = 0, val = 0;
            if(sscanf(arg, "%d %d %d", &from, &to, &val) == 3) {
                for(int i = from; i <= to; i++)
                    vn_script_set_bit(i, val != 0);
            }
        } else if(strcmp(cmd, "IF_BIT") == 0) {
            /* IF_BIT <bit> <0|1> GOTO <label> : jump when bit == value */
            int idx = 0, val = 0;
            char kw[16] = {0};
            char tgt[64] = {0};
            if(sscanf(arg, "%d %d %15s %63s", &idx, &val, kw, tgt) == 4 &&
               strcmp(kw, "GOTO") == 0 && vn_script_get_bit(idx) == (val != 0))
                vn_script_jump_label(tgt);
        } else if(strcmp(cmd, "WAIT") == 0) {
            /* WAIT <ms> : pause the script; A / skip cuts it short */
            unsigned long ms = 0;
            if(sscanf(arg, "%lu", &ms) == 1 && ms > 0U) {
                vm_st.waiting_fx = true;
                vm_st.wait_ms = (uint32_t)ms;
                vm_st.cur_cmd_line = line_idx;
                return 1;
            }
        } else if(strcmp(cmd, "MOVIE") == 0) {
            if(movie_handler && !vn_core_is_skipping()) {
                /* The movie needs the AICA streams; resume the BGM afterwards. */
                char bgm[64];
                vn_copy_str(bgm, sizeof(bgm), vn_audio_get_bgm_name());
                vn_audio_stop_voice();
                vn_audio_stop_bgm();
                if(movie_handler(arg) < 0)
                    dbglog(DBG_WARNING, "kosvn_script: MOVIE '%s' failed\n", arg);
                if(bgm[0] != '\0')
                    vn_audio_play_bgm(bgm);
                vn_core_swallow_input();   /* the A that skipped the movie */
            }
        } else if(strcmp(cmd, "JUMP") == 0) {
            /* JUMP <script_path> [label] : continue in another script file.
               Flags and bits are kept; labels, call stack and sites are reset. */
            char p[64] = {0};
            char lbl[64] = {0};
            if(sscanf(arg, "%63s %63s", p, lbl) < 1 || vn_script_load(p) < 0) {
                dbglog(DBG_ERROR, "kosvn_script: JUMP to '%s' failed\n", p);
                vm_st.ended = true;
                return 0;
            }
            if(lbl[0] != '\0')
                vn_script_jump_label(lbl);
        } else if(strcmp(cmd, "STORY_LOCAL_BITS") == 0) {
            /* STORY_LOCAL_BITS <from> <to> : bits kept per story (max 128) */
            int a = -1, b = -1;
            sscanf(arg, "%d %d", &a, &b);
            vn_story_set_local_bits(a, b);
        } else if(strcmp(cmd, "STORY_DEF") == 0) {
            /* STORY_DEF <id> <start_script> <menu label> */
            int id = 0, consumed = 0;
            char p[64] = {0};
            if(sscanf(arg, "%d %63s %n", &id, p, &consumed) == 2 && consumed > 0)
                vn_story_define(id, p, arg + consumed);
        } else if(strcmp(cmd, "STORY") == 0) {
            vn_story_set_current(atoi(arg));
        } else if(strcmp(cmd, "STORY_RESET") == 0) {
            vn_story_reset();
        } else if(strcmp(cmd, "SYSLOAD") == 0) {
            /* Load the VMU save at the start of the next frame; if there is
               none the script carries on with the next line. */
            vn_core_request_load();
            vm_st.cur_cmd_line = line_idx;
            return 1;
        } else if(strcmp(cmd, "TEXTBOX") == 0) {
            vn_text_set_box_visible(strcmp(arg, "off") != 0);
        } else if(strcmp(cmd, "TEXT_AREA") == 0) {
            /* TEXT_AREA <x> <y> <w> | off : the background has its own text
               window; speaker on its first row, dialogue on the next three */
            unsigned x = 0, y = 0, w = 0;
            if(sscanf(arg, "%u %u %u", &x, &y, &w) == 3)
                vn_text_set_area(x, y, w);
            else
                vn_text_set_area(0, 0, 0);
        } else if(strcmp(cmd, "CHOICE_CURSOR") == 0) {
            /* CHOICE_CURSOR <x> <y> <step> | off : the items are part of the
               picture, only a cursor is drawn left of item n at y + n*step */
            int x = -1, y = 0, step = 0;
            if(sscanf(arg, "%d %d %d", &x, &y, &step) == 3)
                vn_text_set_choice_cursor(x, y, step);
            else
                vn_text_set_choice_cursor(-1, 0, 0);
        } else if(strcmp(cmd, "GOTO") == 0) {
            vn_script_jump_label(arg);
        } else if(strcmp(cmd, "CALL") == 0) {
            if(vm_st.call_depth < VN_MAX_CALL_DEPTH)
                vm_st.call_stack[vm_st.call_depth++] = vm_st.pc;
            vn_script_jump_label(arg);
        } else if(strcmp(cmd, "RET") == 0) {
            vn_script_return();
        } else if(strcmp(cmd, "SET") == 0) {
            int idx = 0;
            long val = 0;
            if(sscanf(arg, "%d %ld", &idx, &val) == 2)
                vn_script_set_flag(idx, (int32_t)val);
        } else if(strcmp(cmd, "ADD") == 0) {
            int idx = 0;
            long delta = 0;
            if(sscanf(arg, "%d %ld", &idx, &delta) == 2)
                vn_script_add_flag(idx, (int32_t)delta);
        } else if(strcmp(cmd, "IF") == 0) {
            int idx = 0;
            char op[8] = {0};
            long rhs_l = 0;
            char kw[16] = {0};
            char tgt[64] = {0};
            if(sscanf(arg, "%d %7s %ld %15s %63s", &idx, op, &rhs_l, kw, tgt) == 5 &&
               strcmp(kw, "GOTO") == 0) {
                if(eval_cond(idx, op, (int32_t)rhs_l))
                    vn_script_jump_label(tgt);
            }
        } else if(strcmp(cmd, "IF_AND") == 0) {
            int i1 = 0, i2 = 0;
            char op1[8] = {0}, op2[8] = {0};
            long v1 = 0, v2 = 0;
            char kw[16] = {0};
            char tgt[64] = {0};
            if(sscanf(arg, "%d %7s %ld %d %7s %ld %15s %63s",
                      &i1, op1, &v1, &i2, op2, &v2, kw, tgt) == 8 &&
               strcmp(kw, "GOTO") == 0) {
                if(eval_cond(i1, op1, (int32_t)v1) && eval_cond(i2, op2, (int32_t)v2))
                    vn_script_jump_label(tgt);
            }
        } else if(strcmp(cmd, "CUTIN") == 0) {
            vn_cutin_set(arg);
        } else if(strcmp(cmd, "SITE_INIT") == 0) {
            int sid = 0;
            char sname[32] = {0};
            char slabel[64] = {0};
            if(sscanf(arg, "%d %31s %63s", &sid, sname, slabel) == 3)
                vn_site_init(sid, sname, slabel);
        } else if(strcmp(cmd, "SITE_SWITCH") == 0) {
            vn_site_switch(atoi(arg));
        } else if(strcmp(cmd, "SITE_LOCK") == 0) {
            int sid = vm_st.active_site;
            int lk = 1;
            int n = sscanf(arg, "%d %d", &sid, &lk);
            if(n == 0) {
                vn_site_set_locked(vm_st.active_site, true);
            } else {
                vn_site_set_locked(sid, lk != 0);
            }
        } else if(strcmp(cmd, "FADE") == 0) {
            unsigned long col = 0;
            unsigned long ms = 0;
            if(sscanf(arg, "%lx %lu", &col, &ms) == 2)
                vn_fx_fade((uint32_t)col, (uint32_t)ms);
        } else if(strcmp(cmd, "WAIT_FX") == 0) {
            vm_st.waiting_fx = true;
            vm_st.cur_cmd_line = line_idx;
            return 1;
        } else if(strcmp(cmd, "SHAKE") == 0) {
            float amp = 6.0f;
            unsigned long ms = 300U;
            if(sscanf(arg, "%f %lu", &amp, &ms) >= 1)
                vn_fx_shake(amp, (uint32_t)ms);
        } else if(strcmp(cmd, "BGM") == 0) {
            if(strcmp(arg, "stop") == 0)
                vn_audio_stop_bgm();
            else if(strncmp(arg, "fade ", 5) == 0)      /* BGM fade <ms> */
                vn_audio_fade_bgm((uint32_t)(atoi(arg + 5) > 0 ? atoi(arg + 5) : 0));
            else
                vn_audio_play_bgm(arg);
        } else if(strcmp(cmd, "SE") == 0) {
            vn_audio_play_se(arg);
        } else if(strcmp(cmd, "SCENE_SE") == 0) {
            /* SCENE_SE <wav> [loop] | stop : PS scene sound on the voice stream */
            char p[64] = {0}, opt[8] = {0};
            sscanf(arg, "%63s %7s", p, opt);
            if(strcmp(p, "stop") == 0 || vn_core_is_skipping())
                vn_audio_stop_voice();
            else
                vn_audio_play_scene_se(p, strcmp(opt, "loop") == 0);
        } else if(strcmp(cmd, "LIP_MAP") == 0) {
            vn_overlay_lip_map(arg);           /* LIP_MAP <frames> <f0..f4> */
        } else if(strcmp(cmd, "SPEAKER") == 0) {
            vn_overlay_set_speaker(arg);   /* owner of the next VOICE (lip<N>) */
        } else if(strcmp(cmd, "VOICE") == 0) {
            if(strcmp(arg, "stop") == 0 || vn_core_is_skipping())
                vn_audio_stop_voice();   /* no point loading a voice we skip past */
            else
                vn_audio_play_voice(arg);
        } else if(strcmp(cmd, "END") == 0) {
            vm_st.cur_cmd_line = line_idx;
            vm_st.ended = true;
            return 0;
        } else {
            dbglog(DBG_WARNING, "kosvn_script: ignoring unknown cmd '%s'\n", cmd);
        }
    }

    vm_st.cur_cmd_line = vm_st.line_count;
    vm_st.ended = true;
    return 0;
}
