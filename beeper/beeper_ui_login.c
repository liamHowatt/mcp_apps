#include "beeper_ui_private.h"

#define LOGIN_PATH BEEPER_ROOT_PATH "login"
#define TASK_PATH BEEPER_ROOT_PATH "task/"

static void ta_event_cb(lv_event_t * e)
{
    lv_obj_t * ta = lv_event_get_target_obj(e);
    lv_obj_t * kb = lv_event_get_user_data(e);
    lv_keyboard_set_textarea(kb, ta);
}

static void kb_ready_cb(lv_event_t * e)
{
    int res;

    lv_obj_t * kb = lv_event_get_target_obj(e);
    lv_obj_t * username_ta = lv_event_get_user_data(e);
    lv_obj_t * password_ta = lv_obj_get_sibling(username_ta, 1);

    const char * username = lv_textarea_get_text(username_ta);
    const char * password = lv_textarea_get_text(password_ta);

    if(!username[0] || !password[0]) {
        LV_LOG_USER("empty username or password");
        return;
    }

    int fd = open(LOGIN_PATH, O_WRONLY | O_EXCL | O_CREAT, 0644);
    assert(fd != -1);

    ssize_t username_len = strlen(username);
    ssize_t password_len = strlen(password);

    ssize_t bw;
    bw = write(fd, username, username_len);
    assert(bw == username_len);
    bw = write(fd, "\n", 1);
    assert(bw == 1);
    bw = write(fd, password, password_len);
    assert(bw == password_len);

    res = close(fd);
    assert(res == 0);

    res = mkdir(TASK_PATH, 0755);
    assert(res == 0 || errno == EEXIST);

    lv_obj_t * base_obj = lv_obj_get_parent(kb);
    beeper_ui_t * c = lv_obj_get_user_data(base_obj);

    c->task = beeper_task_create(TASK_PATH, username, password, beeper_ui_task_event_cb, c);

    lv_obj_clean(base_obj);
    beeper_ui_verify(base_obj);
}

void beeper_ui_login(lv_obj_t * base_obj)
{
    char * existing_login = beeper_read_text_file(LOGIN_PATH);
    if(existing_login) {
        char * saveptr;
        char * username = strtok_r(existing_login, "\n", &saveptr);
        assert(username != NULL);
        char * password = strtok_r(NULL, "\n", &saveptr);
        assert(password != NULL);

        beeper_ui_t * c = lv_obj_get_user_data(base_obj);

        c->task = beeper_task_create(TASK_PATH, username, password, beeper_ui_task_event_cb, c);
        free(existing_login);

        beeper_ui_verify(base_obj);
        return;
    }

    lv_obj_remove_style_all(base_obj);
    lv_obj_set_size(base_obj, LV_PCT(100), LV_PCT(100));

    lv_obj_t * bg_cont = lv_obj_create(base_obj);
    lv_obj_remove_style_all(bg_cont);
    lv_obj_set_size(bg_cont, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(bg_cont, LV_FLEX_FLOW_COLUMN);

    lv_obj_t * login_label = lv_label_create(bg_cont);
    lv_label_set_text_static(login_label, "Login");

    lv_obj_t * username_ta = lv_textarea_create(bg_cont);
    lv_obj_set_style_max_width(username_ta, LV_PCT(100), 0);
    lv_textarea_set_one_line(username_ta, true);
    lv_textarea_set_placeholder_text(username_ta, "username");

    lv_obj_t * password_ta = lv_textarea_create(bg_cont);
    lv_obj_set_style_max_width(password_ta, LV_PCT(100), 0);
    lv_textarea_set_one_line(password_ta, true);
    lv_textarea_set_placeholder_text(password_ta, "password");
    lv_textarea_set_password_mode(password_ta, true);

    lv_obj_t * kb = lv_keyboard_create(base_obj);

    lv_obj_add_event_cb(username_ta, ta_event_cb, LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(password_ta, ta_event_cb, LV_EVENT_FOCUSED, kb);

    lv_obj_add_event_cb(kb, kb_ready_cb, LV_EVENT_READY, username_ta);
}
