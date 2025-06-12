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
typedef void (*beeper_dict_cb_t)(void * item);
void * beeper_asserting_calloc(size_t nmemb, size_t size);
void * beeper_asserting_malloc(size_t size);
void * beeper_asserting_realloc(void * ptr, size_t size);
char * beeper_asserting_strdup(const char * s);
char * beeper_read_text_file(const char * path);
void beeper_array_init(beeper_array_t * ba, size_t item_size);
void beeper_array_destroy(beeper_array_t * ba);
size_t beeper_array_len(beeper_array_t * ba);
void * beeper_array_data(beeper_array_t * ba);
void beeper_array_append(beeper_array_t * ba, const void * to_append);
void beeper_array_remove(beeper_array_t * ba, size_t index);
void beeper_array_reset(beeper_array_t * ba);
void beeper_queue_init(beeper_queue_t * bq, size_t item_size);
void beeper_queue_destroy(beeper_queue_t * bq);
void beeper_queue_push(beeper_queue_t * bq, const void * to_push);
bool beeper_queue_pop(beeper_queue_t * bq, void * pop_dst);
int beeper_queue_get_poll_fd(const beeper_queue_t * bq);
void * beeper_dict_get_create(beeper_array_t * ba, const char * key,
                              beeper_dict_cb_t create_cb, bool * was_created);
void beeper_dict_destroy(beeper_array_t * ba, beeper_dict_cb_t destroy_cb);
void beeper_dict_item_memzero(void * item, size_t item_size);
