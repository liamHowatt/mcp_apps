#pragma once

#include <assert.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>
#include <inttypes.h>

#define BEEPER_ROOT_PATH "/mnt/host/appdata/beeper/"
#define BEEPER_DEVICE_DISPLAY_NAME "MCP Apps"
#define BEEPER_RANDOM_PATH "/mnt/host/random"
#define BEEPER_NEW_MESSAGE_READ_AHEAD 30

/* beeper_util.c */
typedef struct {
    size_t item_size;
    size_t len;
    size_t cap;
    void * data;
} beeper_array_t;
typedef struct {
    beeper_array_t a;
    int evfd;
} beeper_queue_t;
typedef struct beeper_ll_t beeper_ll_t;
struct beeper_ll_t {
    beeper_ll_t * up;
    beeper_ll_t * down;
};
typedef struct {
    size_t capacity;
    size_t item_size;
    void (*destroy)(void * item, void * user_data);
    bool (*cmp)(void * item, void * user_data);
} beeper_lru_class_t;
typedef struct beeper_rcstr_t beeper_rcstr_t;
typedef void (*beeper_dict_cb_t)(void * item, void * user_data);
void * beeper_asserting_calloc(size_t nmemb, size_t size);
void * beeper_asserting_malloc(size_t size);
void * beeper_asserting_realloc(void * ptr, size_t size);
char * beeper_asserting_strdup(const char * s);
char * beeper_read_text_file(const char * path);
void beeper_array_init(beeper_array_t * ba, size_t item_size);
void beeper_array_destroy(beeper_array_t * ba);
void beeper_array_destroy_custom(beeper_array_t * ba, void (*cb)(void * item, void * user_data), void * user_data);
size_t beeper_array_len(beeper_array_t * ba);
void * beeper_array_data(beeper_array_t * ba);
void * beeper_array_append(beeper_array_t * ba, const void * to_append);
void beeper_array_remove(beeper_array_t * ba, size_t index);
void beeper_array_remove_item(beeper_array_t * ba, void * to_remove);
void beeper_array_reset(beeper_array_t * ba);
void beeper_queue_init(beeper_queue_t * bq, size_t item_size);
void beeper_queue_destroy(beeper_queue_t * bq);
void beeper_queue_push(beeper_queue_t * bq, const void * to_push);
bool beeper_queue_pop(beeper_queue_t * bq, void * pop_dst);
int beeper_queue_get_poll_fd(const beeper_queue_t * bq);
void * beeper_dict_get_create(beeper_array_t * ba, const char * key,
                              beeper_dict_cb_t create_cb, bool * was_created, void * user_data);
void * beeper_dict_get(beeper_array_t * ba, const char * key);
void beeper_dict_destroy(beeper_array_t * ba, beeper_dict_cb_t destroy_cb, void * user_data);
void beeper_dict_reset(beeper_array_t * ba, beeper_dict_cb_t destroy_cb, void * user_data);
void beeper_dict_item_memzero(void * item, size_t item_size);
void beeper_ll_list_init(beeper_ll_t * list);
bool beeper_ll_list_is_empty(beeper_ll_t * list);
beeper_ll_t * beeper_ll_list_top(beeper_ll_t * list);
beeper_ll_t * beeper_ll_list_bottom(beeper_ll_t * list);
void beeper_ll_list_add_top(beeper_ll_t * list, beeper_ll_t * link);
void beeper_ll_list_add_bottom(beeper_ll_t * list, beeper_ll_t * link);
void beeper_ll_link_init(beeper_ll_t * link);
void beeper_ll_link_remove(beeper_ll_t * link);
bool beeper_ll_link_is_in_a_list(beeper_ll_t * link);
beeper_ll_t * beeper_ll_list_link_up(beeper_ll_t * list, beeper_ll_t * link);
beeper_ll_t * beeper_ll_list_link_down(beeper_ll_t * list, beeper_ll_t * link);
void beeper_ll_list_link_insert_above(beeper_ll_t * list, beeper_ll_t * link, beeper_ll_t * below);
void beeper_ll_list_link_insert_below(beeper_ll_t * list, beeper_ll_t * link, beeper_ll_t * above);
beeper_rcstr_t * beeper_rcstr_create(const char * s);
beeper_rcstr_t * beeper_rcstr_create_maybe(beeper_rcstr_t ** already, const char * s);
void beeper_rcstr_incref(beeper_rcstr_t * rcstr);
void beeper_rcstr_decref(beeper_rcstr_t * rcstr);
char * beeper_rcstr_str(beeper_rcstr_t * rcstr);
void * beeper_lru_get_no_rearrange(const beeper_lru_class_t * class, void * array, void * cmp_user_data);
void * beeper_lru_get(const beeper_lru_class_t * class, void * array, void * cmp_user_data);
void * beeper_lru_add_unchecked(const beeper_lru_class_t * class, void * array, void * to_add, void * destroy_user_data);
void beeper_lru_destroy(const beeper_lru_class_t * class, void * array, void * destroy_user_data);
