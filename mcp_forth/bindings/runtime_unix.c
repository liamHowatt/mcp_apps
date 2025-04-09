#include "bindings.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

const m4_runtime_cb_array_t m4_runtime_lib_unix[] = {
    {"open", {m4_f13, open}},
    {"write", {m4_f13, write}},
    {"read", {m4_f13, read}},
    {"close", {m4_f11, close}},
    {"ioctl", {m4_f13, ioctl}},

    {"o_rdonly", {m4_lit, (void *) (O_RDONLY)}},
    {"o_wronly", {m4_lit, (void *) (O_WRONLY)}},

    {NULL}
};
