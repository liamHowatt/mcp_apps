#include "bindings.h"

#ifdef CONFIG_SPI_DRIVER
#include <nuttx/spi/spi_transfer.h>
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

    {NULL}
};

#endif /*CONFIG_SPI_DRIVER*/
