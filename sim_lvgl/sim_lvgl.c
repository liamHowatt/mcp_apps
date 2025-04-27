/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <sys/boardctl.h>
#include <sys/mount.h>
#include <assert.h>

#include <lvgl/lvgl.h>
#include <lvgl/demos/lv_demos.h>
#ifdef CONFIG_LV_USE_NUTTX_LIBUV
#include <uv.h>
#endif

#ifdef CONFIG_MCP_APPS_PEANUT_GB
#include <mcp/peanut_gb.h>
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

/****************************************************************************
 * Private Data
 ****************************************************************************/

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

static void app_clicked_cb(lv_event_t * e)
{
  lv_obj_clean(lv_screen_active());
  lv_obj_t * base_obj = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(base_obj);
  lv_obj_set_size(base_obj, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(base_obj, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(base_obj, LV_OPA_COVER, 0);
  void (*app_cb)(lv_obj_t * base_obj) = lv_event_get_user_data(e);
  app_cb(base_obj);
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

  res = mount("", "/mnt/host", "hostfs", 0, "fs=/root/nuttx_hostfs/");
  assert(res == 0);

  lv_obj_t * list = lv_list_create(lv_screen_active());
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_style_border_width(list, 0, 0);

  lv_obj_t * btn;
  (void)btn;
#ifdef CONFIG_MCP_APPS_PEANUT_GB
  btn = lv_list_add_button(list, NULL, "Peanut GB");
  lv_obj_add_event_cb(btn, app_clicked_cb, LV_EVENT_CLICKED, peanut_gb_app_run);
#endif

#ifdef CONFIG_LV_USE_NUTTX_LIBUV
  lv_nuttx_uv_loop(&ui_loop, &result);
#else
  while (1)
    {
      uint32_t idle;
      idle = lv_timer_handler();

      /* Minimum sleep of 1ms */

      idle = idle ? idle : 1;
      usleep(idle * 1000);
    }
#endif

// demo_end:
  lv_nuttx_deinit(&result);
  lv_deinit();

  return 0;
}
