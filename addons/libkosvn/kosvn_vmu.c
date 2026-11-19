/* KallistiOS Visual Novel Extension (libkosvn)
 *
 * addons/libkosvn/kosvn_vmu.c
 * Dreamcast VMU save/load packaging and 48x32 monochrome LCD logo renderer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdalign.h>
#include <kos/dbglog.h>
#include <dc/maple.h>
#include <dc/maple/vmu.h>
#include <dc/vmu_pkg.h>
#include <kos/fs.h>

#include "kosvn_internal.h"

/* Files on the memory card found by maple_enum_type (any port A-D, slot 1-2),
   named after the game's app_id (up to 6 letters / digits, upper case) so two
   libkosvn games do not share files: <ID><slot>A / <ID><slot>B and <ID>CF. */
static void vmu_file_path(const maple_device_t *vmud, const char *tail, char *out, size_t out_sz) {
    char id[8];
    const char *a = vn_core_get_app_id();
    size_t n = 0;
    for(; a && *a && n < 6; a++) {
        char c = *a;
        if(c >= 'a' && c <= 'z') c = (char)(c - 32);
        if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            id[n++] = c;
    }
    if(n == 0) { memcpy(id, "KOSVN", 5); n = 5; }
    id[n] = '\0';
    snprintf(out, out_sz, "/vmu/%c%d/%s%s", 'a' + vmud->port, vmud->unit, id, tail);
}

/* Game-supplied icon / names (vn_vmu_set_icon / vn_vmu_set_desc) */
static bool     custom_icon;
static uint16_t custom_pal[16];
static uint8_t  custom_data[512];
static char     desc_short_s[17] = "KOS-VN Save";
static char     desc_long_prefix[24] = "KallistiOS-VN";

void vn_vmu_set_icon(const uint16_t pal[16], const uint8_t data[512]) {
    custom_icon = pal && data;
    if(custom_icon) {
        memcpy(custom_pal, pal, sizeof(custom_pal));
        memcpy(custom_data, data, sizeof(custom_data));
    }
}

void vn_vmu_set_desc(const char *dshort, const char *dlong_prefix) {
    if(dshort)
        vn_copy_str(desc_short_s, sizeof(desc_short_s), dshort);
    if(dlong_prefix)
        vn_copy_str(desc_long_prefix, sizeof(desc_long_prefix), dlong_prefix);
}

static void generate_save_icon(uint16_t pal[16], uint8_t icon[512]) {
    if(custom_icon) {
        memcpy(pal, custom_pal, sizeof(custom_pal));
        memcpy(icon, custom_data, sizeof(custom_data));
        return;
    }
    memset(pal, 0, sizeof(uint16_t) * 16U);
    memset(icon, 0, 512U);

    /* ARGB4444 icon palette */
    pal[0] = 0xF124U; /* Deep navy */
    pal[1] = 0xF36CU; /* Slate blue */
    pal[2] = 0xF6BFU; /* Dreamcast sky blue */
    pal[3] = 0xFFFFU; /* White */
    pal[4] = 0xFFD5U; /* Warm gold */

    for(int y = 0; y < 32; y++) {
        for(int x = 0; x < 32; x++) {
            uint8_t col = 0;
            if(x == 0 || x == 31 || y == 0 || y == 31) {
                col = 2;
            } else if(y >= 20 && y <= 29 && x >= 3 && x <= 28) {
                /* Mini dialogue box emblem */
                col = (y == 20 || y == 29 || x == 3 || x == 28) ? 3 : 1;
            } else if(y >= 6 && y <= 14 && (x + y) % 5 == 0) {
                col = 4;
            }
            size_t idx = (size_t)y * 16U + (size_t)(x / 2);
            if((x & 1) == 0)
                icon[idx] |= (uint8_t)(col << 4);
            else
                icon[idx] |= col;
        }
    }
}

/* Write one package (header + icon + data) to path. 0 ok, -2 write failed. */
static int write_pkg(const char *path, const char *desc_long, const void *data, int len) {
    uint16_t icon_pal[16];
    uint8_t icon_data[512];
    generate_save_icon(icon_pal, icon_data);

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    vn_copy_str(pkg.desc_short, sizeof(pkg.desc_short), desc_short_s);
    vn_copy_str(pkg.desc_long, sizeof(pkg.desc_long), desc_long);
    vn_copy_str(pkg.app_id, sizeof(pkg.app_id), vn_core_get_app_id());
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = len;
    memcpy(pkg.icon_pal, icon_pal, sizeof(icon_pal));
    pkg.icon_data = icon_data;
    pkg.eyecatch_data = NULL;
    pkg.data = (const uint8_t *)data;

    uint8_t *pkg_out = NULL;
    int pkg_size = 0;
    if(vmu_pkg_build(&pkg, &pkg_out, &pkg_size) < 0 || !pkg_out) {
        dbglog(DBG_ERROR, "kosvn_vmu: vmu_pkg_build failed\n");
        return -2;
    }
    /* O_META: write the package as built (without it fs_vmu adds / strips the
       header itself, and vmu_pkg_parse() on reading could never succeed). */
    file_t fd = fs_open(path, O_WRONLY | O_TRUNC | O_META);
    if(fd == FILEHND_INVALID) {
        free(pkg_out);
        dbglog(DBG_ERROR, "kosvn_vmu: open('%s') for writing failed\n", path);
        return -2;
    }
    ssize_t written = fs_write(fd, pkg_out, (size_t)pkg_size);
    int close_rc = fs_close(fd);
    free(pkg_out);
    if(written != (ssize_t)pkg_size || close_rc < 0) {
        dbglog(DBG_ERROR, "kosvn_vmu: write failed %d/%d (close %d)\n", (int)written, pkg_size, close_rc);
        return -2;
    }
    dbglog(DBG_INFO, "kosvn_vmu: saved %d bytes to %s\n", pkg_size, path);
    return 0;
}

/* Read the data of one package into out (exactly len bytes). 0 ok, -1 not there / bad. */
static int read_pkg(const char *path, void *out, int len) {
    file_t fd = fs_open(path, O_RDONLY | O_META);   /* raw: keep the package header */
    if(fd == FILEHND_INVALID)
        return -1;
    size_t max_sz = 16384U;
    uint8_t *buf = (uint8_t *)malloc(max_sz);
    if(!buf) {
        fs_close(fd);
        return -1;
    }
    ssize_t got = fs_read(fd, buf, max_sz);
    fs_close(fd);
    vmu_pkg_t pkg;
    if(got <= 0 || vmu_pkg_parse(buf, (size_t)got, &pkg) < 0 || pkg.data_len != len || !pkg.data) {
        free(buf);
        return -1;
    }
    memcpy(out, pkg.data, (size_t)len);
    free(buf);
    return 0;
}

static int read_state(const char *path, vn_save_state_t *st) {
    if(read_pkg(path, st, (int)sizeof(*st)) < 0)
        return -1;
    if(st->magic != VN_SAVE_MAGIC || st->version != VN_SAVE_VERSION) {
        dbglog(DBG_WARNING, "kosvn_vmu: %s: other save version\n", path);
        return -1;
    }
    return 0;
}

/* Each slot has two files, <ID><n>A and <ID><n>B. A save goes to the one that
   does not hold the newest copy, and only after it is written is the older one
   deleted, so a card pulled out while saving still has the previous save. The
   newer valid copy (save_seq) is the one loaded. */
static int slot_newest(const maple_device_t *vmud, int slot, vn_save_state_t *st, int *which) {
    char path[40], tail[8];
    vn_save_state_t a, b;
    snprintf(tail, sizeof(tail), "%dA", slot);
    vmu_file_path(vmud, tail, path, sizeof(path));
    bool ha = read_state(path, &a) == 0;
    snprintf(tail, sizeof(tail), "%dB", slot);
    vmu_file_path(vmud, tail, path, sizeof(path));
    bool hb = read_state(path, &b) == 0;
    if(!ha && !hb)
        return -1;
    bool use_b = hb && (!ha || b.save_seq > a.save_seq);
    if(st)
        *st = use_b ? b : a;
    if(which)
        *which = use_b ? 1 : 0;
    return 0;
}

int vn_vmu_save(int slot, const vn_save_state_t *state) {
    if(!state)
        return -1;
    maple_device_t *vmud = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmud) {
        dbglog(DBG_WARNING, "kosvn_vmu: no VMU memcard detected for save\n");
        return -1;
    }
    int s = (slot < 0) ? 0 : (slot % 10);
    vn_save_state_t prev;
    int which = 1;                    /* no save yet: write A */
    uint32_t seq = 0U;
    if(slot_newest(vmud, s, &prev, &which) == 0)
        seq = prev.save_seq;
    vn_save_state_t st = *state;
    st.save_seq = seq + 1U;
    char path[40], old[40], tail[8], dl[40];
    snprintf(tail, sizeof(tail), "%d%c", s, which ? 'A' : 'B');
    vmu_file_path(vmud, tail, path, sizeof(path));
    snprintf(tail, sizeof(tail), "%d%c", s, which ? 'B' : 'A');
    vmu_file_path(vmud, tail, old, sizeof(old));
    snprintf(dl, sizeof(dl), "%s %d", desc_long_prefix, slot + 1);
    int rc = write_pkg(path, dl, &st, (int)sizeof(st));
    if(rc < 0) {
        /* no room for a second copy: overwrite in place (the old safety is lost) */
        if(seq > 0U && write_pkg(old, dl, &st, (int)sizeof(st)) == 0)
            return 0;
        fs_unlink(path);              /* do not leave a half-written copy */
        return rc;
    }
    fs_unlink(old);
    return 0;
}

int vn_vmu_load(int slot, vn_save_state_t *state_out) {
    if(!state_out)
        return -1;
    maple_device_t *vmud = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmud) {
        dbglog(DBG_WARNING, "kosvn_vmu: no VMU memcard detected for load\n");
        return -1;
    }
    int s = (slot < 0) ? 0 : (slot % 10);
    if(slot_newest(vmud, s, state_out, NULL) < 0)
        return -1;
    dbglog(DBG_INFO, "kosvn_vmu: loaded slot %d (line %lu, save %lu)\n", slot,
           (unsigned long)state_out->script_line, (unsigned long)state_out->save_seq);
    return 0;
}

/* Settings file <ID>CF: len bytes chosen by the caller. */
int vn_vmu_save_cfg(const void *data, int len) {
    maple_device_t *vmud = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmud)
        return -1;
    char path[40], dl[40];
    vmu_file_path(vmud, "CF", path, sizeof(path));
    snprintf(dl, sizeof(dl), "%s settings", desc_long_prefix);
    return write_pkg(path, dl, data, len);
}

int vn_vmu_load_cfg(void *data, int len) {
    maple_device_t *vmud = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmud)
        return -1;
    char path[40];
    vmu_file_path(vmud, "CF", path, sizeof(path));
    return read_pkg(path, data, len);
}

static void lcd_set_pixel(uint8_t bmp[192], int x, int y) {
    if(x < 0 || x >= 48 || y < 0 || y >= 32)
        return;
    int bit_idx = y * 48 + x;
    bmp[bit_idx >> 3] |= (uint8_t)(0x80U >> (bit_idx & 7));
}

void vn_vmu_draw_logo(const char *short_tag) {
    maple_device_t *lcd = maple_enum_type(0, MAPLE_FUNC_LCD);
    if(!lcd)
        return;

    alignas(4) uint8_t bmp[192];
    memset(bmp, 0, sizeof(bmp));

    /* Outer border frame */
    for(int x = 0; x < 48; x++) {
        lcd_set_pixel(bmp, x, 0);
        lcd_set_pixel(bmp, x, 31);
    }
    for(int y = 0; y < 32; y++) {
        lcd_set_pixel(bmp, 0, y);
        lcd_set_pixel(bmp, 47, y);
    }

    /* Stylized "VN" monogram centered on 48x32 LCD */
    for(int y = 7; y <= 23; y++) {
        int vx_l = 10 + (y - 7) / 3;
        int vx_r = 22 - (y - 7) / 3;
        lcd_set_pixel(bmp, vx_l, y);
        lcd_set_pixel(bmp, vx_l + 1, y);
        lcd_set_pixel(bmp, vx_r, y);
        lcd_set_pixel(bmp, vx_r + 1, y);

        /* 'N' */
        lcd_set_pixel(bmp, 27, y);
        lcd_set_pixel(bmp, 28, y);
        lcd_set_pixel(bmp, 37, y);
        lcd_set_pixel(bmp, 38, y);
        int nx = 28 + ((y - 7) * 9) / 16;
        lcd_set_pixel(bmp, nx, y);
        lcd_set_pixel(bmp, nx + 1, y);
    }

    /* Bottom status dots encoded from optional tag hash */
    if(short_tag) {
        size_t n = strlen(short_tag);
        for(size_t i = 0; i < n && i < 16; i++) {
            if(((uint8_t)short_tag[i] & 1U) != 0U) {
                lcd_set_pixel(bmp, 8 + (int)i * 2, 27);
            }
        }
    }

    vmu_draw_lcd_rotated(lcd, bmp);
}
