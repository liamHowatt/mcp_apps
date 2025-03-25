#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <arch/board/mcp/mcp_pins_defs.h>

#define MCPD_OK            0
#define MCPD_DOESNT_EXIST -1
#define MCPD_BUSY         -2
#define MCPD_RESOURCE_UNAVAILABLE -3
#define MCPD_BAD_REQUEST  -4

#define MCPD_CON_NULL     -1

typedef int mcpd_con_t;

int mcpd_connect(mcpd_con_t * con_dst, int peer_id);
void mcpd_disconnect(mcpd_con_t con);

void mcpd_write(mcpd_con_t con, const void * data, uint32_t len);
void mcpd_read(mcpd_con_t con, void * data, uint32_t len);

int mcpd_gpio_acquire(mcpd_con_t con, unsigned socketno, unsigned pinno);
void mcpd_gpio_set(mcpd_con_t con, unsigned gpio_id, bool en);

int mcpd_resource_acquire(mcpd_con_t con, mcpd_pins_type_t type);
int mcpd_resource_route(mcpd_con_t con, unsigned resource_id, unsigned io_type,
    unsigned socketno, unsigned pinno);
const char * mcpd_resource_get_path(mcpd_con_t con, unsigned resource_id);

#ifdef __cplusplus
} /*extern "C"*/
#endif
