#include <mcp/peanut_gb.h>

#include <lvgl/lvgl.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <nuttx/input/keyboard.h>

#define ENABLE_SOUND 0
#include "Peanut-GB/peanut_gb.h"

#define SHARD_COUNT 16

#define MAX_ROM_SIZE (1024 * 1024)
#define SHARD_SIZE (MAX_ROM_SIZE / SHARD_COUNT)
#define SCRW 160
#define SCRH 144

typedef struct {
    lv_obj_t * base_obj;
    char * save_path;
    lv_timer_t * timer;
    uint8_t * cart_ram;
    uint32_t first_tick;
    uint32_t frame_count;
    lv_obj_t * canv;
    int keypad_fd;
    uint8_t size_val;
    bool screenbuf_dirty;
    uint8_t screen_joypad_mask;
    uint8_t uinput_joypad_mask;
    bool style_is_init;
    lv_style_t btn_style;
    struct gb_s gb;
    uint8_t * rom_shards[SHARD_COUNT];
    bool dirty_rows[SCRH];
    uint16_t palette[0x23 + 1];
    uint8_t raw_buf[SCRH][SCRW];
    uint16_t canv_buf[SCRH][SCRW];
} ctx_t;

static void open_menu(ctx_t * ctx);

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
    ctx->dirty_rows[line] = true;
    memcpy(ctx->raw_buf[line], pixels, SCRW);
}

static bool get_menu_is_open(lv_obj_t * base_obj)
{
    lv_obj_t * base_obj_youngest_child = lv_obj_get_child(base_obj, -1);
    assert(base_obj_youngest_child);
    return lv_obj_get_class(base_obj_youngest_child) == &lv_list_class;
}

static void update_uinput_joypad(ctx_t * ctx)
{
    ssize_t rwres;

    if(ctx->keypad_fd < 0) {
        return;
    }

    bool menu_is_open_init = false;
    bool menu_is_open;

    struct keyboard_event_s keypad_event;
    while(sizeof(keypad_event) == (rwres = read(ctx->keypad_fd, &keypad_event, sizeof(keypad_event)))) {
        if(!menu_is_open_init) {
            menu_is_open_init = true;
            menu_is_open = get_menu_is_open(ctx->base_obj);
        }
        if(menu_is_open) {
            continue;
        }

        uint8_t mask;
        switch(keypad_event.code) {
            case 103: /* KEY_UP */
                mask = JOYPAD_UP;
                break;
            case 108: /* KEY_DOWN */
                mask = JOYPAD_DOWN;
                break;
            case 105: /* KEY_LEFT */
                mask = JOYPAD_LEFT;
                break;
            case 106: /* KEY_RIGHT */
                mask = JOYPAD_RIGHT;
                break;
            case 1: /* KEY_ESC */
                mask = JOYPAD_B;
                break;
            case 28: /* KEY_ENTER */
                mask = JOYPAD_A;
                break;
            case 0x13b: /* BTN_START */
                mask = JOYPAD_START;
                break;
            case 0x13a: /* BTN_SELECT */
                mask = JOYPAD_SELECT;
                break;
            default:
                open_menu(ctx);
                menu_is_open = true;
                continue;
        }
        if(keypad_event.type == KEYBOARD_PRESS) {
            ctx->uinput_joypad_mask |= mask;
        } else {
            ctx->uinput_joypad_mask &= ~mask;
        }
    }
    assert(rwres < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

    ctx->gb.direct.joypad = ~(ctx->uinput_joypad_mask | ctx->screen_joypad_mask);
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
        update_uinput_joypad(ctx);
        gb_run_frame(&ctx->gb);
    }
    if(ctx->screenbuf_dirty) {
        for(uint32_t i = 0; i < SCRH; i++) {
            if(ctx->dirty_rows[i]) {
                ctx->dirty_rows[i] = false;
                for(uint32_t j = 0; j < SCRW; j++) {
                    ctx->canv_buf[i][j] = ctx->palette[ctx->raw_buf[i][j]];
                }
            }
        }
        lv_obj_invalidate(ctx->canv);
    }
}

static void btn_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code != LV_EVENT_PRESSED && code != LV_EVENT_RELEASED){
        return;
    }

    ctx_t * ctx = lv_event_get_user_data(e);
    lv_obj_t * btn = lv_event_get_target_obj(e);
    uint8_t mask = (uintptr_t)lv_obj_get_user_data(btn);
    if(code == LV_EVENT_PRESSED) {
        ctx->screen_joypad_mask |= mask;
    } else {
        ctx->screen_joypad_mask &= ~mask;
    }

    ctx->gb.direct.joypad = ~(ctx->uinput_joypad_mask | ctx->screen_joypad_mask);
}

static lv_obj_t * create_pad(lv_obj_t * base_obj, lv_align_t align,
                             const int32_t * col_dsc, const int32_t * row_dsc)
{
    lv_obj_t * pad = lv_obj_create(base_obj);
    lv_obj_set_align(pad, align);
    lv_obj_set_size(pad, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(pad, LV_COORD_MAX, 0);
    lv_obj_set_style_grid_column_dsc_array(pad, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(pad, row_dsc, 0);
    lv_obj_set_style_pad_gap(pad, 0, 0);
    lv_obj_set_layout(pad, LV_LAYOUT_GRID);
    return pad;
}

static void create_left_btn(ctx_t * ctx, lv_obj_t * left, const void * img_src,
                            int32_t col_pos, int32_t row_pos, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_image_create(left);
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col_pos, 1, LV_GRID_ALIGN_STRETCH, row_pos, 1);
    lv_image_set_src(btn, img_src);
    lv_image_set_inner_align(btn, LV_IMAGE_ALIGN_CENTER);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void create_right_btn(ctx_t * ctx, lv_obj_t * right,
                             int32_t col_pos, int32_t row_pos, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_obj_create(right);
    lv_obj_remove_style_all(btn);
    lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col_pos, 1, LV_GRID_ALIGN_STRETCH, row_pos, 1);
    lv_obj_set_style_radius(btn, LV_COORD_MAX, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void create_mid_btn(ctx_t * ctx, lv_obj_t * mid, uint8_t joypad_mask)
{
    lv_obj_t * btn = lv_obj_create(mid);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 30, 15);
    lv_obj_set_style_radius(btn, LV_COORD_MAX, 0);
    lv_obj_add_style(btn, &ctx->btn_style, 0);

    lv_obj_set_user_data(btn, (void *)(uintptr_t) joypad_mask);
    lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_ALL, ctx);
}

static void close_menu_cb(lv_event_t * e)
{
    lv_obj_delete(lv_obj_get_parent(lv_event_get_target_obj(e)));
}

static void save_cb(lv_event_t * e)
{
    int res;
    ssize_t rwres;

    ctx_t * ctx = lv_event_get_user_data(e);

    int fd = open(ctx->save_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0);

    uint32_t cart_ram_size = gb_get_save_size(&ctx->gb);
    rwres = write(fd, ctx->cart_ram, cart_ram_size);
    assert(rwres == cart_ram_size);

    res = close(fd);
    assert(res == 0);
}

static void resize_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);

    lv_obj_t * canv = lv_obj_get_child(ctx->base_obj, 0);

    ++ctx->size_val;
    while(1) {
        switch(ctx->size_val) {
            case 0:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_TOP_MID);
                lv_image_set_scale(canv, LV_SCALE_NONE);
                lv_image_set_offset_y(canv, 0);
                break;
            case 1:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_STRETCH);
                break;
            case 2:
                lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_CONTAIN);
                lv_image_set_offset_y(canv, -((lv_obj_get_height(canv) - lv_image_get_transformed_height(canv)) / 2));
                break;
            default:
                ctx->size_val = 0;
                continue;
        }

        break;
    }
}

static void open_menu(ctx_t * ctx)
{
    lv_obj_t * base_obj = ctx->base_obj;
    if(get_menu_is_open(base_obj)) {
        return;
    }

    lv_obj_t * menu = lv_list_create(base_obj);
    lv_gridnav_add(menu, LV_GRIDNAV_CTRL_NONE);
    lv_group_add_obj(lv_group_get_default(), menu);
    lv_obj_center(menu);

    lv_obj_t * btn;

    btn = lv_list_add_button(menu, LV_SYMBOL_PLAY, "Resume");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, close_menu_cb, LV_EVENT_CLICKED, NULL);

    btn = lv_list_add_button(menu, LV_SYMBOL_SAVE, "Save");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, save_cb, LV_EVENT_CLICKED, ctx);

    btn = lv_list_add_button(menu, LV_SYMBOL_IMAGE, "Resize");
    lv_group_remove_obj(btn);
    lv_obj_add_event_cb(btn, resize_cb, LV_EVENT_CLICKED, ctx);
}

static void menu_btn_clicked_cb(lv_event_t * e)
{
    ctx_t * ctx = lv_event_get_user_data(e);
    open_menu(ctx);
}

static void create_sceen_joypad(lv_obj_t * base_obj)
{
    ctx_t * ctx = lv_obj_get_user_data(base_obj);

    static const int32_t left_col_dsc[] = {30, 30, 30, LV_GRID_TEMPLATE_LAST};
    static const int32_t left_row_dsc[] = {30, 30, 30, LV_GRID_TEMPLATE_LAST};
    lv_obj_t * left = create_pad(base_obj, LV_ALIGN_BOTTOM_LEFT, left_col_dsc, left_row_dsc);
    create_left_btn(ctx, left, LV_SYMBOL_UP, 1, 0, JOYPAD_UP);
    create_left_btn(ctx, left, LV_SYMBOL_DOWN, 1, 2, JOYPAD_DOWN);
    create_left_btn(ctx, left, LV_SYMBOL_LEFT, 0, 1, JOYPAD_LEFT);
    create_left_btn(ctx, left, LV_SYMBOL_RIGHT, 2, 1, JOYPAD_RIGHT);

    static const int32_t right_col_dsc[] = {45, 45, LV_GRID_TEMPLATE_LAST};
    static const int32_t right_row_dsc[] = {45, 45, LV_GRID_TEMPLATE_LAST};
    lv_obj_t * right = create_pad(base_obj, LV_ALIGN_BOTTOM_RIGHT, right_col_dsc, right_row_dsc);
    create_right_btn(ctx, right, 1, 0, JOYPAD_A);
    create_right_btn(ctx, right, 0, 1, JOYPAD_B);

    lv_obj_t * mid = lv_obj_create(base_obj);
    lv_obj_align(mid, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(mid, LV_COORD_MAX, 0);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(mid, 5, 0);

    create_mid_btn(ctx, mid, JOYPAD_SELECT);
    create_mid_btn(ctx, mid, JOYPAD_START);

    lv_obj_t * menu_btn = lv_image_create(base_obj);
    lv_obj_align(menu_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_image_set_src(menu_btn, LV_SYMBOL_LIST);
    lv_obj_add_flag(menu_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(menu_btn, menu_btn_clicked_cb, LV_EVENT_CLICKED, ctx);
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
    free(ctx->save_path);
    res = asprintf(&ctx->save_path, "%s.sav", rom_path);
    assert(res >= 0);
    int fd = open(rom_path, O_RDONLY);
    free(rom_path);

    if(fd < 0) {
        fprintf(stderr, "could not open ROM\n");
        return;
    }

    res = fstat(fd, &statbuf);
    assert(res == 0);
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

    lv_style_init(&ctx->btn_style);
    lv_style_set_border_opa(&ctx->btn_style, LV_OPA_COVER);
    lv_style_set_border_width(&ctx->btn_style, 2);
    lv_style_set_border_color(&ctx->btn_style, lv_color_black());
    ctx->style_is_init = true;

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
            memset(ctx->cart_ram, 0xff, cart_ram_size);
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
    lv_image_set_inner_align(canv, LV_IMAGE_ALIGN_TOP_MID);
    lv_obj_set_size(canv, LV_PCT(100), LV_PCT(100));
    lv_canvas_set_buffer(canv, ctx->canv_buf, SCRW, SCRH, LV_COLOR_FORMAT_RGB565);
    for(uint32_t shade_i = 0; shade_i < 4; shade_i++) {
        uint8_t gray_value = (3 - shade_i) * 255 / 3;
        lv_color_t color_value = {.red=gray_value, .green=gray_value, .blue=gray_value};
        for(uint32_t layer_i = 0; layer_i < 3; layer_i++) {
            ctx->palette[(layer_i << 4) | shade_i] = lv_color_to_u16(color_value);
        }
    }

    ctx->keypad_fd = open("/dev/ukeyboard", O_RDONLY | O_NONBLOCK);
    assert(ctx->keypad_fd >= 0 || errno == ENOENT);

    lv_indev_t * indev = NULL;
    while((indev = lv_indev_get_next(indev))) {
        if(lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            create_sceen_joypad(base_obj);
            break;
        }
    }

    ctx->first_tick = lv_tick_get();
    ctx->frame_count = 1;
    ctx->canv = canv;
    update_uinput_joypad(ctx);
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
    if(ctx->keypad_fd >= 0) assert(0 == close(ctx->keypad_fd));
    if(ctx->style_is_init) lv_style_reset(&ctx->btn_style);
    free(ctx);
}

void peanut_gb_app_run(lv_obj_t * base_obj)
{
    ctx_t * ctx = calloc(1, sizeof(*ctx));
    assert(ctx);
    ctx->base_obj = base_obj;
    ctx->keypad_fd = -1;
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
