#pragma once

#include <netutils/webclient.h>

#define MCP_TLS_FLAG_PEM 0

typedef struct mcp_tls_ctx_t mcp_tls_ctx_t;

extern const struct webclient_tls_ops mcp_tls_webclient_tls_ops;

mcp_tls_ctx_t * mcp_tls_ctx_create(void);
void mcp_tls_ctx_destroy(mcp_tls_ctx_t * ctx);
int mcp_tls_ctx_add_cert(mcp_tls_ctx_t * ctx, const void * data,
                         size_t size, uint32_t flags);

int mcp_tls_connect(void *ctx,
                    const char *hostname, const char *port,
                    unsigned int timeout_second,
                    struct webclient_tls_connection **connp);
ssize_t mcp_tls_send(void *ctx,
                     struct webclient_tls_connection *conn,
                     const void *buf, size_t len);
ssize_t mcp_tls_recv(void *ctx,
                     struct webclient_tls_connection *conn,
                     void *buf, size_t len);
int mcp_tls_close(void *ctx,
                  struct webclient_tls_connection *conn);
int mcp_tls_get_poll_info(void *ctx,
                          struct webclient_tls_connection *conn,
                          struct webclient_poll_info *info);
