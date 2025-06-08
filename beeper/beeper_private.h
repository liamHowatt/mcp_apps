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
