/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include <lvgl/lvgl.h>
#include <lvgl/demos/lv_demos.h>
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#include <uv.h>
#endif

#ifdef CONFIG_SIM_KEYBOARD
#include <nuttx/input/keyboard.h>
#endif

#include <mcp/mcp_lvgl_common_private.h>

#ifdef CONFIG_MCP_APPS_PEANUT_GB
#include <mcp/peanut_gb.h>
#endif
#ifdef CONFIG_MCP_APPS_BEEPER
#include <mcp/beeper.h>
#endif
#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO
#include <mcp/texter_ui.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Should we perform board-specific driver initialization? There are two
 * ways that board initialization can occur:  1) automatically via
 * board_late_initialize() during bootupif CONFIG_BOARD_LATE_INITIALIZE
 * or 2).
 * via a call to boardctl() if the interface is enabled
 * (CONFIG_BOARDCTL=y).
 * If this task is running as an NSH built-in application, then that
 * initialization has probably already been performed otherwise we do it
 * here.
 */

#undef NEED_BOARDINIT

#if defined(CONFIG_BOARDCTL) && !defined(CONFIG_NSH_ARCHINIT)
#  define NEED_BOARDINIT 1
#endif

/****************************************************************************
 * Private Type Declarations
 ****************************************************************************/

#ifdef CONFIG_SIM_KEYBOARD
struct kbd_lv_key_s {
  uint32_t key;
  lv_indev_state_t state;
};

struct kbd_s {
  int fd;
  struct kbd_lv_key_s last_key_event;
  bool has_last_key_event;
  uint8_t modifier_data;
  lv_indev_state_t last_state;
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char shift_lut[] = {
  ' ', /* ' ' */
  '!', /* ! */
  '"', /* " */
  '#', /* # */
  '$', /* $ */
  '%', /* % */
  '&', /* & */
  '"', /* ' */
  '(', /* ( */
  ')', /* ) */
  '*', /* * */
  '+', /* + */
  '<', /* , */
  '_', /* - */
  '>', /* . */
  '?', /* / */
  ')', /* 0 */
  '!', /* 1 */
  '@', /* 2 */
  '#', /* 3 */
  '$', /* 4 */
  '%', /* 5 */
  '^', /* 6 */
  '&', /* 7 */
  '*', /* 8 */
  '(', /* 9 */
  ':', /* : */
  ':', /* ; */
  '<', /* < */
  '+', /* = */
  '>', /* > */
  '?', /* ? */
  '@', /* @ */
  'A', /* A */
  'B', /* B */
  'C', /* C */
  'D', /* D */
  'E', /* E */
  'F', /* F */
  'G', /* G */
  'H', /* H */
  'I', /* I */
  'J', /* J */
  'K', /* K */
  'L', /* L */
  'M', /* M */
  'N', /* N */
  'O', /* O */
  'P', /* P */
  'Q', /* Q */
  'R', /* R */
  'S', /* S */
  'T', /* T */
  'U', /* U */
  'V', /* V */
  'W', /* W */
  'X', /* X */
  'Y', /* Y */
  'Z', /* Z */
  '{', /* [ */
  '|', /* \ */
  '}', /* ] */
  '^', /* ^ */
  '_', /* _ */
  '~', /* ` */
  'A', /* a */
  'B', /* b */
  'C', /* c */
  'D', /* d */
  'E', /* e */
  'F', /* f */
  'G', /* g */
  'H', /* h */
  'I', /* i */
  'J', /* j */
  'K', /* k */
  'L', /* l */
  'M', /* m */
  'N', /* n */
  'O', /* o */
  'P', /* p */
  'Q', /* q */
  'R', /* r */
  'S', /* s */
  'T', /* t */
  'U', /* u */
  'V', /* v */
  'W', /* w */
  'X', /* x */
  'Y', /* y */
  'Z', /* z */
  '{', /* { */
  '|', /* | */
  '}', /* } */
  '~'  /* ~ */
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
static void lv_nuttx_uv_loop(uv_loop_t *loop, lv_nuttx_result_t *result)
{
  lv_nuttx_uv_t uv_info;
  void *data;

  uv_loop_init(loop);

  lv_memset(&uv_info, 0, sizeof(uv_info));
  uv_info.loop = loop;
  uv_info.disp = result->disp;
  uv_info.indev = result->indev;
#ifdef CONFIG_UINPUT_TOUCH
  uv_info.uindev = result->utouch_indev;
#endif

  data = lv_nuttx_uv_init(&uv_info);
  uv_run(loop, UV_RUN_DEFAULT);
  lv_nuttx_uv_deinit(&data);
}
#endif

#ifdef CONFIG_SIM_KEYBOARD
static bool kbd_try_read_key(uint8_t * modifiers, int fd, struct kbd_lv_key_s * key_event_lv)
{
  ssize_t rwres;

  struct keyboard_event_s key_event;
  while((rwres = read(fd, &key_event, sizeof(key_event))) == sizeof(key_event)) {
    switch(key_event.code) {
      case XK_Up:       key_event_lv->key = LV_KEY_UP;        break;
      case XK_Down:     key_event_lv->key = LV_KEY_DOWN;      break;
      case XK_Left:     key_event_lv->key = LV_KEY_LEFT;      break;
      case XK_Right:    key_event_lv->key = LV_KEY_RIGHT;     break;
      case XK_Return:   key_event_lv->key = LV_KEY_ENTER;     break;
      case XK_Escape:   key_event_lv->key = LV_KEY_ESC;       break;
      case XK_Tab:      key_event_lv->key = *modifiers ? LV_KEY_PREV : LV_KEY_NEXT; break;
      case XK_Delete:   key_event_lv->key = LV_KEY_DEL;       break;
      case XK_BackSpace:key_event_lv->key = LV_KEY_BACKSPACE; break;
      case XK_Home:     key_event_lv->key = LV_KEY_HOME;      break;
      case XK_End:      key_event_lv->key = LV_KEY_END;       break;
      case XK_Shift_L:
      case XK_Shift_R: {
        uint8_t mask = 1 << (key_event.code == XK_Shift_R);
        if(key_event.type == KEYBOARD_PRESS) {
          *modifiers |= mask;
        } else {
          *modifiers &= ~mask;
        }
        continue;
      }
      case ' ' ... '~':
        key_event_lv->key = *modifiers ? shift_lut[key_event.code - ' ']
                                       : key_event.code;
        break;
      default:
        continue;
    }

    key_event_lv->state = key_event.type == KEYBOARD_PRESS
                          ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

    return true;
  }
  assert(rwres < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));

  return false;
}

static void kbd_read(lv_indev_t * indev, lv_indev_data_t * data)
{
  struct kbd_s * kbd = lv_indev_get_driver_data(indev);

  if(kbd->has_last_key_event || kbd_try_read_key(&kbd->modifier_data, kbd->fd, &kbd->last_key_event)) {
    data->key = kbd->last_key_event.key;
    kbd->last_state = kbd->last_key_event.state;

    bool another_key_was_read = kbd_try_read_key(&kbd->modifier_data, kbd->fd, &kbd->last_key_event);
    kbd->has_last_key_event = another_key_was_read;
    data->continue_reading  = another_key_was_read;
  }

  data->state = kbd->last_state;
}

static lv_indev_t * kbd_create(struct kbd_s * kbd)
{
  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
  lv_indev_set_read_cb(indev, kbd_read);
  lv_indev_set_driver_data(indev, kbd);

  kbd->fd = open("/dev/kbd", O_RDONLY | O_NONBLOCK);
  assert(kbd->fd >= 0);

  kbd->has_last_key_event = false;
  kbd->modifier_data = 0;
  kbd->last_state = LV_INDEV_STATE_RELEASED;

  return indev;
}
#endif

static void make_enc_kpd_group(void)
{
    lv_group_t * g = lv_group_create();
    lv_group_set_default(g);

    lv_indev_t * indev = NULL;
    while((indev = lv_indev_get_next(indev))) {
        lv_indev_set_group(indev, g);
    }
}

static void app_obj_delete_cb(lv_event_t * e)
{
  lv_obj_remove_flag(lv_obj_get_child(lv_screen_active(), 0), LV_OBJ_FLAG_HIDDEN);
}

static void app_clicked_cb(lv_event_t * e)
{
  lv_obj_add_flag(lv_obj_get_child(lv_screen_active(), 0), LV_OBJ_FLAG_HIDDEN);
  lv_obj_t * base_obj = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(base_obj);
  lv_obj_set_size(base_obj, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(base_obj, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(base_obj, LV_OPA_COVER, 0);
  lv_obj_add_event_cb(base_obj, app_obj_delete_cb, LV_EVENT_DELETE, NULL);
  void (*app_cb)(lv_obj_t * base_obj) = lv_event_get_user_data(e);
  app_cb(base_obj);
}

static void quit_clicked_cb(lv_event_t * e)
{
  bool * quit = lv_event_get_user_data(e);
  *quit = true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main or lv_demos_main
 *
 * Description:
 *
 * Input Parameters:
 *   Standard argc and argv
 *
 * Returned Value:
 *   Zero on success; a positive, non-zero value on failure.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int res;
  lv_nuttx_dsc_t info;
  lv_nuttx_result_t result;

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
  uv_loop_t ui_loop;
  lv_memzero(&ui_loop, sizeof(ui_loop));
#endif

  if (lv_is_initialized())
    {
      LV_LOG_ERROR("LVGL already initialized! aborting.");
      return -1;
    }

#ifdef NEED_BOARDINIT
  /* Perform board-specific driver initialization */

  boardctl(BOARDIOC_INIT, 0);

#endif

  lv_init();

  mcp_lvgl_poll_init();

  lv_nuttx_dsc_init(&info);

#ifdef CONFIG_LV_USE_NUTTX_LCD
  info.fb_path = "/dev/lcd0";
#endif

#ifdef CONFIG_INPUT_TOUCHSCREEN
  info.input_path = CONFIG_MCP_APPS_SIM_LVGL_INPUT_DEVPATH;
#endif

  lv_nuttx_init(&info, &result);

  if (result.disp == NULL)
    {
      LV_LOG_ERROR("lv_demos initialization failure!");
      return 1;
    }

#ifdef CONFIG_SIM_KEYBOARD
  struct kbd_s kbd;
  kbd_create(&kbd);
#endif

  res = mount("", "/mnt/host", "hostfs", 0, "fs=/root/nuttx_hostfs/");
  assert(res == 0);

  make_enc_kpd_group();

  lv_obj_t * list = lv_list_create(lv_screen_active());
  lv_gridnav_add(list, LV_GRIDNAV_CTRL_NONE);
  lv_group_add_obj(lv_group_get_default(), list);
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_style_border_width(list, 0, 0);

  bool quit = false;

  lv_obj_t * btn = lv_list_add_button(list, LV_SYMBOL_POWER, NULL);
  lv_group_remove_obj(btn);
  lv_obj_add_event_cb(btn, quit_clicked_cb, LV_EVENT_CLICKED, &quit);
#ifdef CONFIG_MCP_APPS_PEANUT_GB
  btn = lv_list_add_button(list, NULL, "Peanut GB");
  lv_group_remove_obj(btn);
  lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, peanut_gb_app_run);
#endif
#ifdef CONFIG_MCP_APPS_BEEPER
  btn = lv_list_add_button(list, NULL, "Beeper");
  lv_group_remove_obj(btn);
  lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, beeper_app_run);
#endif
#ifdef CONFIG_MCP_APPS_TEXTER_UI_DEMO
  btn = lv_list_add_button(list, NULL, "Texter UI Demo");
  lv_group_remove_obj(btn);
  lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, texter_ui_demo_app_run);
#endif

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
  lv_nuttx_uv_loop(&ui_loop, &result);
#else
  while(mcp_lvgl_poll_run(lv_timer_handler()) && !quit);
#endif

// demo_end:
  lv_nuttx_deinit(&result);
  mcp_lvgl_poll_deinit();
  lv_deinit();

  boardctl(BOARDIOC_POWEROFF, 0);
  assert(0);

  return 0;
}
