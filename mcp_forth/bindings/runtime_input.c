#include "bindings.h"

#ifdef CONFIG_INPUT
#include <nuttx/input/keyboard.h>
#include <stddef.h>

const m4_runtime_cb_array_t m4_runtime_lib_input[] = {
    {"keyboard_event_s", {m4_lit, (void *) sizeof(struct keyboard_event_s)}},
    {"keyboard_event_s.code", {m4_lit, (void *) offsetof(struct keyboard_event_s, code)}},
    {"keyboard_event_s.type", {m4_lit, (void *) offsetof(struct keyboard_event_s, type)}},

    {NULL}
};

#endif /*CONFIG_INPUT*/
