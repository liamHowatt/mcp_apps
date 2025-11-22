#include "bindings.h"

#ifdef CONFIG_MCP_APPS_MCPD
#include <mcp/mcpd.h>
#include <arch/board/mcp/mcp_pins_defs.h>

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>

#include <termios.h>

static int mcpd_driver_connect(void * param, m4_stack_t * stack)
{
    if(!(stack->len + 2 <= stack->max)) return M4_STACK_OVERFLOW_ERROR;
    stack->data += 2;
    stack->len += 2;

    int res;
    char * peer_str = getenv("MCP_PEER");
    if(peer_str == NULL) {
        stack->data[-1] = MCPD_ENV_NOT_SET;
        return 0;
    }
    int peer = atoi(peer_str);
    mcpd_con_t con;
    res = mcpd_connect(&con, peer);
    if(res != MCPD_OK) {
        stack->data[-1] = res;
        return 0;
    }
    uint8_t byte = 1; /* protocol 1: the driver protocol */
    mcpd_write(con, &byte, 1);
    mcpd_read(con, &byte, 1);
    if(byte != 0) {
        mcpd_disconnect(con);
        stack->data[-1] = MCPD_PROTOCOL_NOT_SUP;
        return 0;
    }
    stack->data[-2] = (int) con;
    stack->data[-1] = MCPD_OK;
    return 0;
}

static int mcpd_peer_id(void * param, m4_stack_t * stack)
{
    if(!(stack->len < stack->max)) return M4_STACK_OVERFLOW_ERROR;
    char * peer_str = getenv("MCP_PEER");
    stack->data[0] = peer_str !=  NULL ? atoi(peer_str) : MCPD_ENV_NOT_SET;
    stack->data += 1;
    stack->len += 1;
    return 0;
}

static int mcpd_acatpath(void * param, m4_stack_t * stack)
{
    if(!(stack->len)) return M4_STACK_UNDERFLOW_ERROR;
    char ** strp = (char **) stack->data - 1;
    char * peer_str = getenv("MCP_PEER");
    if(peer_str == NULL) {
        *strp = NULL;
        return 0;
    }
    int res = asprintf(strp, "/mnt/mcp/%d/%s", atoi(peer_str), *strp);
    assert(res != -1);
    return 0;
}

static int mcpd_uart_set_baud(void * param, m4_stack_t * stack)
{
    if(stack->len < 2) return M4_STACK_UNDERFLOW_ERROR;
    int fd = stack->data[-2];
    unsigned baud = stack->data[-1];
    stack->data -= 1;
    stack->len -= 1;
    stack->data[-1] = MCPD_ERROR;

    speed_t speed_val;
    switch(baud) {
        case       0: speed_val =       B0; break;
        case      50: speed_val =      B50; break;
        case      75: speed_val =      B75; break;
        case     110: speed_val =     B110; break;
        case     134: speed_val =     B134; break;
        case     150: speed_val =     B150; break;
        case     200: speed_val =     B200; break;
        case     300: speed_val =     B300; break;
        case     600: speed_val =     B600; break;
        case    1200: speed_val =    B1200; break;
        case    1800: speed_val =    B1800; break;
        case    2400: speed_val =    B2400; break;
        case    4800: speed_val =    B4800; break;
        case    9600: speed_val =    B9600; break;
        case   19200: speed_val =   B19200; break;
        case   38400: speed_val =   B38400; break;

        case   57600: speed_val =   B57600; break;
        case  115200: speed_val =  B115200; break;
        case  230400: speed_val =  B230400; break;
        case  460800: speed_val =  B460800; break;
        case  500000: speed_val =  B500000; break;
        case  576000: speed_val =  B576000; break;
        case  921600: speed_val =  B921600; break;
        case 1000000: speed_val = B1000000; break;
        case 1152000: speed_val = B1152000; break;
        case 1500000: speed_val = B1500000; break;
        case 2000000: speed_val = B2000000; break;
        case 2500000: speed_val = B2500000; break;
        case 3000000: speed_val = B3000000; break;
        case 3500000: speed_val = B3500000; break;
        case 4000000: speed_val = B4000000; break;
        default: return 0;
    }

    int res;
    struct termios tio;
    res = tcgetattr(fd, &tio);
    if(res) return 0;
    res = cfsetspeed(&tio, speed_val);
    if(res) return 0;
    res = tcsetattr(fd, TCSAFLUSH, &tio); /* TCSAFLUSH: write all output and discard input */
    if(res) return 0;

    stack->data[-1] = MCPD_OK;
    return 0;
}

const m4_runtime_cb_array_t m4_runtime_lib_mcpd[] = {
    {"mcpd_ok", {m4_lit, (void *) (MCPD_OK)}},
    {"mcpd_doesnt_exist", {m4_lit, (void *) (MCPD_DOESNT_EXIST)}},
    {"mcpd_busy", {m4_lit, (void *) (MCPD_BUSY)}},
    {"mcpd_resource_unavailable", {m4_lit, (void *) (MCPD_RESOURCE_UNAVAILABLE)}},
    {"mcpd_bad_request", {m4_lit, (void *) (MCPD_BAD_REQUEST)}},
    {"mcpd_env_not_set", {m4_lit, (void *) (MCPD_ENV_NOT_SET)}},
    {"mcpd_protocol_not_sup", {m4_lit, (void *) (MCPD_PROTOCOL_NOT_SUP)}},
    {"mcpd_ioerror", {m4_lit, (void *) (MCPD_IOERROR)}},
    {"mcpd_noent", {m4_lit, (void *) (MCPD_NOENT)}},
    {"mcpd_nametoolong", {m4_lit, (void *) (MCPD_NAMETOOLONG)}},
    {"mcpd_async_want_write", {m4_lit, (void *) (MCPD_ASYNC_WANT_WRITE)}},
    {"mcpd_async_want_read", {m4_lit, (void *) (MCPD_ASYNC_WANT_READ)}},
    {"mcpd_error", {m4_lit, (void *) (MCPD_ERROR)}},
    {"mcpd_con_null", {m4_lit, (void *) (MCPD_CON_NULL)}},

    {"mcpd_connect", {m4_f12, mcpd_connect}},
    {"mcpd_disconnect", {m4_f01, mcpd_disconnect}},

    {"mcpd_write", {m4_f03, mcpd_write}},
    {"mcpd_read", {m4_f03, mcpd_read}},

    {"mcpd_async_write_start", {m4_f13, mcpd_async_write_start}},
    {"mcpd_async_read_start", {m4_f13, mcpd_async_read_start}},
    {"mcpd_async_continue", {m4_f11, mcpd_async_continue}},
    {"mcpd_get_async_polling_fd", {m4_f11, mcpd_get_async_polling_fd}},

    {"mcpd_gpio_acquire", {m4_f13, mcpd_gpio_acquire}},
    {"mcpd_gpio_set", {m4_f03, mcpd_gpio_set}},

    {"mcpd_resource_acquire", {m4_f13, mcpd_resource_acquire}},
    {"mcpd_resource_route", {m4_f15, mcpd_resource_route}},
    {"mcpd_resource_get_path", {m4_f12, mcpd_resource_get_path}},

    {"mcpd_file_hash", {m4_f13, mcpd_file_hash}},


    /* resources */
    {"mcp_pins_periph_type_spi", {m4_lit, (void *) MCP_PINS_PERIPH_TYPE_SPI}},
    {"mcp_pins_periph_type_uart", {m4_lit, (void *) MCP_PINS_PERIPH_TYPE_UART}},

    {"mcp_pins_driver_type_spi_raw", {m4_lit, (void *) MCP_PINS_DRIVER_TYPE_SPI_RAW}},
    {"mcp_pins_driver_type_spi_sdcard", {m4_lit, (void *) MCP_PINS_DRIVER_TYPE_SPI_SDCARD}},

    {"mcp_pins_driver_type_uart_raw", {m4_lit, (void *) MCP_PINS_DRIVER_TYPE_UART_RAW}},

    {"mcp_pins_pin_spi_clk", {m4_lit, (void *) MCP_PINS_PIN_SPI_CLK}},
    {"mcp_pins_pin_spi_miso", {m4_lit, (void *) MCP_PINS_PIN_SPI_MISO}},
    {"mcp_pins_pin_spi_mosi", {m4_lit, (void *) MCP_PINS_PIN_SPI_MOSI}},
    {"mcp_pins_pin_spi_cs", {m4_lit, (void *) MCP_PINS_PIN_SPI_CS}},

    {"mcp_pins_pin_uart_tx", {m4_lit, (void *) MCP_PINS_PIN_UART_TX}},
    {"mcp_pins_pin_uart_rx", {m4_lit, (void *) MCP_PINS_PIN_UART_RX}},


    /*extensions*/
    {"mcpd_driver_connect", {mcpd_driver_connect}},
    {"mcpd_peer_id", {mcpd_peer_id}},
    {"mcpd_acatpath", {mcpd_acatpath}},

    {"mcpd_uart_set_baud", {mcpd_uart_set_baud}},


    {NULL}
};

#endif /*CONFIG_MCP_APPS_MCPD*/
