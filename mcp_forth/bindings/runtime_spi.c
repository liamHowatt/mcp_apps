#include "bindings.h"

#ifdef CONFIG_SPI_DRIVER
#include <nuttx/spi/spi_transfer.h>
#include <arch/board/mcp/mcp_pins_defs.h>
#include <stddef.h>

const m4_runtime_cb_array_t m4_runtime_lib_spi[] = {
    {"spiioc_transfer", {m4_lit, (void *) (SPIIOC_TRANSFER)}},

    {"spi_trans_s", {m4_lit, (void *) sizeof(struct spi_trans_s)}},
    {"spi_trans_s.nwords", {m4_lit, (void *) offsetof(struct spi_trans_s, nwords)}},
    {"spi_trans_s.txbuffer", {m4_lit, (void *) offsetof(struct spi_trans_s, txbuffer)}},
    {"spi_trans_s.rxbuffer", {m4_lit, (void *) offsetof(struct spi_trans_s, rxbuffer)}},

    {"spi_sequence_s", {m4_lit, (void *) sizeof(struct spi_sequence_s)}},
    {"spi_sequence_s.nbits", {m4_lit, (void *) offsetof(struct spi_sequence_s, nbits)}},
    {"spi_sequence_s.frequency", {m4_lit, (void *) offsetof(struct spi_sequence_s, frequency)}},
    {"spi_sequence_s.ntrans", {m4_lit, (void *) offsetof(struct spi_sequence_s, ntrans)}},
    {"spi_sequence_s.trans", {m4_lit, (void *) offsetof(struct spi_sequence_s, trans)}},

    {"mcp_pins_type_spi", {m4_lit, (void *) MCP_PINS_TYPE_SPI}},
    {"mcp_pins_spi_clk", {m4_lit, (void *) MCP_PINS_SPI_CLK}},
    {"mcp_pins_spi_miso", {m4_lit, (void *) MCP_PINS_SPI_MISO}},
    {"mcp_pins_spi_mosi", {m4_lit, (void *) MCP_PINS_SPI_MOSI}},
    {"mcp_pins_spi_cs", {m4_lit, (void *) MCP_PINS_SPI_CS}},

    {NULL}
};

#endif /*CONFIG_SPI_DRIVER*/
