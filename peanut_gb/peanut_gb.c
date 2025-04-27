#include <mcp/peanut_gb.h>

#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>

#define ENABLE_SOUND 0
#include "Peanut-GB/peanut_gb.h"

#define SHARD_COUNT 16

#define MAX_ROM_SIZE (1024 * 1024)
#define SHARD_SIZE (MAX_ROM_SIZE / SHARD_COUNT)
#define PALETTE_SIZE (4 * 256)
#define SCRW 160
#define SCRH 144

typedef struct {
    char * save_path;
    lv_timer_t * timer;
    uint8_t * cart_ram;
    uint32_t first_tick;
    uint32_t frame_count;
    lv_obj_t * canv;
    bool screenbuf_dirty;
    struct gb_s gb;
    uint8_t * rom_shards[SHARD_COUNT];
    uint8_t screenbuf[PALETTE_SIZE + (SCRW * SCRH)];
} ctx_t;

static uint8_t gb_rom_read(struct gb_s * gb, const uint_fast32_t addr)
{
    ctx_t * ctx = gb->direct.priv;

    uint32_t shard_i = addr / SHARD_SIZE;
    uint32_t offset = addr % SHARD_SIZE;

    return ctx->rom_shards[shard_i][offset];
}

static uint8_t gb_cart_ram_read(struct gb_s * gb, const uint_fast32_t addr)
{
    ctx_t * ctx = gb->direct.priv;
    return ctx->cart_ram[addr];
}

static void gb_cart_ram_write(struct gb_s * gb, const uint_fast32_t addr, const uint8_t val)
{
    ctx_t * ctx = gb->direct.priv;
    ctx->cart_ram[addr] = val;
}

static void gb_error(struct gb_s*, const enum gb_error_e, const uint16_t)
{
    assert(0);
}

static void lcd_draw_line(struct gb_s *gb,
    const uint8_t *pixels,
    const uint_fast8_t line)
{
    ctx_t * ctx = gb->direct.priv;
    ctx->screenbuf_dirty = true;
    memcpy(&ctx->screenbuf[PALETTE_SIZE + (SCRW * line)], pixels, SCRW);
}

static void tim_cb(lv_timer_t * tim)
{
    ctx_t * ctx = lv_timer_get_user_data(tim);

    uint32_t overall_ticks = lv_tick_get() - ctx->first_tick;

    double overall_ticks_d = overall_ticks;
    double frames_should_have_run_d = overall_ticks_d * VERTICAL_SYNC / 1000.0;

    uint32_t frames_should_have_run = frames_should_have_run_d;
    uint32_t frames_to_run = frames_should_have_run > ctx->frame_count
                             ? frames_should_have_run - ctx->frame_count
                             : 0;
    ctx->frame_count += frames_to_run;

    ctx->screenbuf_dirty = false;
    for(uint32_t i = 0; i < frames_to_run; i++) {
        gb_run_frame(&ctx->gb);
    }
    if(ctx->screenbuf_dirty) {
        lv_obj_invalidate(ctx->canv);
    }
}

static void game_file_chosen_cb(lv_event_t * e)
{
    int res;
    ssize_t rwres;
    struct stat statbuf;

    lv_obj_t * fe = lv_event_get_target_obj(e);
    lv_obj_t * base_obj = lv_obj_get_parent(fe);
    ctx_t * ctx = lv_obj_get_user_data(base_obj);

    char * rom_path;
    res = asprintf(&rom_path, "%s%s",
                   lv_file_explorer_get_current_path(fe),
                   lv_file_explorer_get_selected_file_name(fe));
    assert(res >= 0);
    res = asprintf(&ctx->save_path, "%s.sav", rom_path);
    assert(res >= 0);
    int fd = open(rom_path, O_RDONLY);
    free(rom_path);

    if(fd < 0) {
        fprintf(stderr, "could not open ROM\n");
        return;
    }

    res = fstat(fd, &statbuf);
    if(res < 0) {
        fprintf(stderr, "could not stat ROM\n");
        res = close(fd);
        assert(res == 0);
        return;
    }
    uint32_t rom_size = statbuf.st_size;
    if(rom_size > MAX_ROM_SIZE) {
        fprintf(stderr, "ROM too big\n");
        res = close(fd);
        assert(res == 0);
        return;
    }

    uint32_t read_size_remaining = rom_size;
    for(uint32_t i = 0; read_size_remaining; i++) {
        uint8_t * shard = malloc(SHARD_SIZE);
        assert(shard);
        ctx->rom_shards[i] = shard;
        uint32_t this_read_size = read_size_remaining < SHARD_SIZE ? read_size_remaining : SHARD_SIZE;
        rwres = read(fd, shard, this_read_size);
        assert(rwres == this_read_size);
        read_size_remaining -= this_read_size;
    }

    res = close(fd);
    assert(res == 0);

    lv_obj_clean(base_obj);
    lv_obj_set_style_layout(base_obj, LV_LAYOUT_NONE, 0);

    enum gb_init_error_e err = gb_init(&ctx->gb,
                                       gb_rom_read,
                                       gb_cart_ram_read,
                                       gb_cart_ram_write,
                                       gb_error,
                                       ctx);
    assert(err == 0);
    gb_init_lcd(&ctx->gb, lcd_draw_line);

    uint32_t cart_ram_size = gb_get_save_size(&ctx->gb);
    if(cart_ram_size) {
        ctx->cart_ram = malloc(cart_ram_size);
        assert(ctx->cart_ram);

        fd = open(ctx->save_path, O_RDONLY);
        if(fd < 0) {
            assert(errno == ENOENT);
            memset(ctx->cart_ram, 0, cart_ram_size);
        }
        else {
            res = fstat(fd, &statbuf);
            assert(res == 0);
            assert(statbuf.st_size == cart_ram_size);
            rwres = read(fd, ctx->cart_ram, cart_ram_size);
            assert(rwres == cart_ram_size);
        }
    }

    lv_obj_t * canv = lv_canvas_create(base_obj);
    lv_obj_set_align(canv, LV_ALIGN_TOP_MID);
    lv_canvas_set_buffer(canv, ctx->screenbuf, SCRW, SCRH, LV_COLOR_FORMAT_I8);
    for(uint32_t shade_i = 0; shade_i < 4; shade_i++) {
        uint8_t gray_value = (3 - shade_i) * 255 / 3;
        lv_color32_t color_value = {.red=gray_value, .green=gray_value, .blue=gray_value, .alpha=255};
        for(uint32_t layer_i = 0; layer_i < 3; layer_i++) {
            lv_canvas_set_palette(canv, (layer_i << 4) | shade_i, color_value);
        }
    }

    ctx->first_tick = lv_tick_get();
    ctx->frame_count = 1;
    ctx->canv = canv;
    gb_run_frame(&ctx->gb);
    ctx->timer = lv_timer_create(tim_cb, 1000.0 / VERTICAL_SYNC + 1.0, ctx);
}

static void base_obj_delete_cb(lv_event_t * e)
{
    lv_obj_t * base_obj = lv_event_get_target_obj(e);;
    ctx_t * ctx = lv_obj_get_user_data(base_obj);
    for(uint32_t i = 0; i < SHARD_COUNT; i++) {
        free(ctx->rom_shards[i]);
    }
    free(ctx->cart_ram);
    free(ctx->save_path);
    if(ctx->timer) lv_timer_delete(ctx->timer);
    free(ctx);
}

void peanut_gb_app_run(lv_obj_t * base_obj)
{
    ctx_t * ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    lv_obj_set_user_data(base_obj, ctx);
    lv_obj_add_event_cb(base_obj, base_obj_delete_cb, LV_EVENT_DELETE, NULL);

    lv_obj_set_flex_flow(base_obj, LV_FLEX_COLUMN);
    lv_obj_set_flex_align(base_obj, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * label = lv_label_create(base_obj);
    lv_label_set_text_static(label, "pick a game");

    lv_obj_t * fe = lv_file_explorer_create(base_obj);
    lv_obj_set_flex_grow(fe, 1);
    lv_file_explorer_open_dir(fe, "");
    lv_obj_add_event_cb(fe, game_file_chosen_cb, LV_EVENT_VALUE_CHANGED, NULL);
}
