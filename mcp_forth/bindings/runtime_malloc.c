#include "bindings.h"
#include <stdlib.h>

const m4_runtime_cb_array_t m4_runtime_lib_malloc[] = {
    {"malloc", {m4_f11, malloc}},
    {"calloc", {m4_f12, calloc}},
    {"realloc", {m4_f12, realloc}},
    {"free", {m4_f01, free}},

    {NULL}
};
