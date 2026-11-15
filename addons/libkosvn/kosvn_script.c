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
} vn_label_entry_t;

static struct {
    char             path[64];
    char            *raw_buf;
    char           **lines;
    uint32_t         line_count;
    uint32_t         pc;
    bool             ended;
    bool             waiting_fx;
    vn_label_entry_t labels[VN_MAX_LABELS];
    int              label_count;
    int32_t          flags[VN_MAX_FLAGS];
} vm_st;

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
    vm_st.label_count = 0;
    vm_st.ended = true;
    vm_st.waiting_fx = false;
    vm_st.path[0] = '\0';
}

int vn_script_load(const char *path) {
    if(!path)
        return -1;

    vn_script_unload();

    FILE *fp = fopen(path, "rb");
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
        if(strcmp(cmd, "LABEL") == 0 && arg[0] != '\0' && vm_st.label_count < VN_MAX_LABELS) {
            vn_label_entry_t *le = &vm_st.labels[vm_st.label_count++];
            vn_copy_str(le->name, sizeof(le->name), arg);
            le->line_idx = idx;
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
    vm_st.ended = false;
    vm_st.waiting_fx = false;
    vn_copy_str(vm_st.path, sizeof(vm_st.path), path);
    return 0;
}

int vn_script_jump_label(const char *label) {
    if(!label)
        return -1;
    for(int i = 0; i < vm_st.label_count; i++) {
        if(strcmp(vm_st.labels[i].name, label) == 0) {
            vm_st.pc = vm_st.labels[i].line_idx;
            return 0;
        }
    }
    dbglog(DBG_WARNING, "kosvn_script: unknown label '%s'\n", label);
    return -1;
}

int vn_script_jump_line(uint32_t line) {
    if(line >= vm_st.line_count)
        return -1;
    vm_st.pc = line;
    vm_st.ended = false;
    vm_st.waiting_fx = false;
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

uint32_t vn_script_get_line(void) { return vm_st.pc; }
const char *vn_script_get_path(void) { return vm_st.path; }
bool vn_script_is_waiting_fx(void) { return vm_st.waiting_fx; }
void vn_script_clear_wait_fx(void) { vm_st.waiting_fx = false; }
bool vn_script_is_ended(void) { return vm_st.ended; }

int vn_script_advance(void) {
    while(vm_st.pc < vm_st.line_count) {
        const char *line = vm_st.lines[vm_st.pc++];
        if(line[0] == '\0' || line[0] == '#')
            continue;

        char cmd[32];
        const char *arg = split_cmd(line, cmd, sizeof(cmd));

        if(strcmp(cmd, "LABEL") == 0) {
            continue;
        } else if(strcmp(cmd, "BG") == 0) {
            vn_bg_set(arg);
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
            return 1;
        } else if(strcmp(cmd, "CHOICE") == 0) {
            vn_text_clear_choices();
            while(vm_st.pc < vm_st.line_count) {
                char opt_cmd[32];
                const char *opt_arg = split_cmd(vm_st.lines[vm_st.pc], opt_cmd, sizeof(opt_cmd));
                if(strcmp(opt_cmd, "OPTION") != 0)
                    break;
                vm_st.pc++;

                char opt_copy[256];
                vn_copy_str(opt_copy, sizeof(opt_copy), opt_arg);
                char *sep = sjis_find_ascii(opt_copy, '|');
                if(sep) {
                    *sep = '\0';
                    char *lbl = trim_ascii(opt_copy);
                    char *tgt = trim_ascii(sep + 1);
                    vn_text_add_choice(lbl, tgt);
                }
            }
            vn_text_reveal_all();
            return 1;
        } else if(strcmp(cmd, "GOTO") == 0) {
            vn_script_jump_label(arg);
        } else if(strcmp(cmd, "SET") == 0) {
            int idx = 0;
            long val = 0;
            if(sscanf(arg, "%d %ld", &idx, &val) == 2)
                vn_script_set_flag(idx, (int32_t)val);
        } else if(strcmp(cmd, "IF") == 0) {
            int idx = 0;
            char op[8] = {0};
            long rhs_l = 0;
            char kw[16] = {0};
            char tgt[64] = {0};
            if(sscanf(arg, "%d %7s %ld %15s %63s", &idx, op, &rhs_l, kw, tgt) == 5 &&
               strcmp(kw, "GOTO") == 0) {
                int32_t lhs = vn_script_get_flag(idx);
                int32_t rhs = (int32_t)rhs_l;
                bool match = false;
                if(strcmp(op, "==") == 0) match = (lhs == rhs);
                else if(strcmp(op, "!=") == 0) match = (lhs != rhs);
                else if(strcmp(op, ">=") == 0) match = (lhs >= rhs);
                else if(strcmp(op, "<=") == 0) match = (lhs <= rhs);
                else if(strcmp(op, ">") == 0)  match = (lhs > rhs);
                else if(strcmp(op, "<") == 0)  match = (lhs < rhs);
                if(match)
                    vn_script_jump_label(tgt);
            }
        } else if(strcmp(cmd, "FADE") == 0) {
            unsigned long col = 0;
            unsigned long ms = 0;
            if(sscanf(arg, "%lx %lu", &col, &ms) == 2)
                vn_fx_fade((uint32_t)col, (uint32_t)ms);
        } else if(strcmp(cmd, "WAIT_FX") == 0) {
            vm_st.waiting_fx = true;
            return 1;
        } else if(strcmp(cmd, "SHAKE") == 0) {
            float amp = 6.0f;
            unsigned long ms = 300U;
            if(sscanf(arg, "%f %lu", &amp, &ms) >= 1)
                vn_fx_shake(amp, (uint32_t)ms);
        } else if(strcmp(cmd, "BGM") == 0) {
            if(strcmp(arg, "stop") == 0)
                vn_audio_stop_bgm();
            else
                vn_audio_play_bgm(arg);
        } else if(strcmp(cmd, "SE") == 0) {
            vn_audio_play_se(arg);
        } else if(strcmp(cmd, "END") == 0) {
            vm_st.ended = true;
            return 0;
        } else {
            dbglog(DBG_WARNING, "kosvn_script: ignoring unknown cmd '%s'\n", cmd);
        }
    }

    vm_st.ended = true;
    return 0;
}
