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

#include "kosvn_internal.h"

static void build_vmu_path(int slot, char *out, size_t out_sz) {
    int s = (slot < 0) ? 0 : (slot % 10);
    snprintf(out, out_sz, "/vmu/a1/KOSVN%02d", s);
}

static void generate_save_icon(uint16_t pal[16], uint8_t icon[512]) {
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

int vn_vmu_save(int slot, const vn_save_state_t *state) {
    if(!state)
        return -1;

    maple_device_t *vmud = maple_enum_type(0, MAPLE_FUNC_MEMCARD);
    if(!vmud) {
        dbglog(DBG_WARNING, "kosvn_vmu: no VMU memcard detected for save\n");
        return -1;
    }

    uint16_t icon_pal[16];
    uint8_t icon_data[512];
    generate_save_icon(icon_pal, icon_data);

    vmu_pkg_t pkg;
    memset(&pkg, 0, sizeof(pkg));
    vn_copy_str(pkg.desc_short, sizeof(pkg.desc_short), "KOS-VN Save");
    snprintf(pkg.desc_long, sizeof(pkg.desc_long), "KallistiOS-VN Slot %d (L:%lu)",
             slot, (unsigned long)state->script_line);
    vn_copy_str(pkg.app_id, sizeof(pkg.app_id), vn_core_get_app_id());
    pkg.icon_cnt = 1;
    pkg.icon_anim_speed = 0;
    pkg.eyecatch_type = VMUPKG_EC_NONE;
    pkg.data_len = (int)sizeof(vn_save_state_t);
    memcpy(pkg.icon_pal, icon_pal, sizeof(icon_pal));
    pkg.icon_data = icon_data;
    pkg.eyecatch_data = NULL;
    pkg.data = (const uint8_t *)state;

    uint8_t *pkg_out = NULL;
    int pkg_size = 0;
    if(vmu_pkg_build(&pkg, &pkg_out, &pkg_size) < 0 || !pkg_out) {
        dbglog(DBG_ERROR, "kosvn_vmu: vmu_pkg_build failed\n");
        return -1;
    }

    char path[32];
    build_vmu_path(slot, path, sizeof(path));
    FILE *fp = fopen(path, "wb");
    if(!fp) {
        free(pkg_out);
        dbglog(DBG_ERROR, "kosvn_vmu: fopen('%s', wb) failed\n", path);
        return -1;
    }

    size_t written = fwrite(pkg_out, 1, (size_t)pkg_size, fp);
    fclose(fp);
    free(pkg_out);

    if(written != (size_t)pkg_size) {
        dbglog(DBG_ERROR, "kosvn_vmu: short write %zu/%d\n", written, pkg_size);
        return -1;
    }

    dbglog(DBG_INFO, "kosvn_vmu: saved %d bytes to %s\n", pkg_size, path);
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

    char path[32];
    build_vmu_path(slot, path, sizeof(path));
    FILE *fp = fopen(path, "rb");
    if(!fp) {
        dbglog(DBG_WARNING, "kosvn_vmu: save file '%s' not found\n", path);
        return -1;
    }

    /* VMU blocks fit comfortably within 128 KB max */
    size_t max_sz = 16384U;
    uint8_t *buf = (uint8_t *)malloc(max_sz);
    if(!buf) {
        fclose(fp);
        return -1;
    }

    size_t fsz = fread(buf, 1, max_sz, fp);
    fclose(fp);
    if(fsz == 0) {
        free(buf);
        return -1;
    }

    vmu_pkg_t pkg;
    if(vmu_pkg_parse(buf, fsz, &pkg) < 0) {
        free(buf);
        dbglog(DBG_ERROR, "kosvn_vmu: vmu_pkg_parse failed\n");
        return -1;
    }

    if(pkg.data_len != (int)sizeof(vn_save_state_t) || !pkg.data) {
        free(buf);
        dbglog(DBG_ERROR, "kosvn_vmu: incompatible save payload size %d\n", pkg.data_len);
        return -1;
    }

    vn_save_state_t tmp;
    memcpy(&tmp, pkg.data, sizeof(vn_save_state_t));
    free(buf);

    if(tmp.magic != VN_SAVE_MAGIC || tmp.version != VN_SAVE_VERSION) {
        dbglog(DBG_ERROR, "kosvn_vmu: invalid save magic/version\n");
        return -1;
    }

    memcpy(state_out, &tmp, sizeof(vn_save_state_t));
    dbglog(DBG_INFO, "kosvn_vmu: loaded slot %d (line %lu)\n",
           slot, (unsigned long)state_out->script_line);
    return 0;
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
