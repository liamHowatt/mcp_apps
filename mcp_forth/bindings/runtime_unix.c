#include "bindings.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// static int rt_open(void * param, m4_stack_t * stack)
// {
//     if(!(stack->len >= 2)) return M4_STACK_UNDERFLOW_ERROR;
//     stack->data[-2] = open(stack->data[-2], stack->data[-1], 0666);
//     stack->data -= 1;
//     stack->len -= 1;
//     return 0;
// }

const m4_runtime_cb_array_t m4_runtime_lib_unix[] = {
    // {"open", {rt_open}},
    {"open", {m4_f13, open}},
    {"close", {m4_f11, close}},
    {"ioctl", {m4_f13, ioctl}},

    {"o_rdonly", {m4_lit, (void *) (O_RDONLY)}},

    {NULL}
};
