#include "bindings.h"

#ifdef CONFIG_NETUTILS_PPPD
#include "netutils/pppd.h"
#include <stddef.h>

const m4_runtime_cb_array_t m4_runtime_lib_pppd[] = {
    {"ttynamsiz", {m4_lit, (void *) TTYNAMSIZ}},
    {"pap_username_size", {m4_lit, (void *) PAP_USERNAME_SIZE}},
    {"pap_password_size", {m4_lit, (void *) PAP_PASSWORD_SIZE}},
    {"pppd_settings_s", {m4_lit, (void *) sizeof(struct pppd_settings_s)}},
    {"pppd_settings_s.ttyname", {m4_lit, (void *) offsetof(struct pppd_settings_s, ttyname)}},
#ifdef CONFIG_NETUTILS_PPPD_PAP
    {"pppd_settings_s.pap_username", {m4_lit, (void *) offsetof(struct pppd_settings_s, pap_username)}},
    {"pppd_settings_s.pap_password", {m4_lit, (void *) offsetof(struct pppd_settings_s, pap_password)}},
#endif /*CONFIG_NETUTILS_PPPD_PAP*/
    {"pppd_settings_s.connect_script", {m4_lit, (void *) offsetof(struct pppd_settings_s, connect_script)}},
    {"pppd_settings_s.disconnect_script", {m4_lit, (void *) offsetof(struct pppd_settings_s, disconnect_script)}},
    {"pppd", {m4_f11, pppd}},

    {NULL}
};

#endif /*CONFIG_NETUTILS_PPPD*/
