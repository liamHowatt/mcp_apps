#include <mcp/tls.h>

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/time.h>

mcp_tls_ctx_t * mcp_tls_ctx_create(void)
{
    int res;

    res = wolfSSL_Init();
    if(res != SSL_SUCCESS) return NULL;

    WOLFSSL_CTX * sslctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    if(sslctx == NULL) {
        res = wolfSSL_Cleanup();
        assert(res == SSL_SUCCESS);
        return NULL;
    }

    return (mcp_tls_ctx_t *) sslctx;
}

void mcp_tls_ctx_destroy(mcp_tls_ctx_t * ctx)
{
    WOLFSSL_CTX * sslctx = (WOLFSSL_CTX *) ctx;
    int res;

    wolfSSL_CTX_free(sslctx);

    // it won't actually cleanup if the internal reference count is nonzero
    res = wolfSSL_Cleanup();
    assert(res == SSL_SUCCESS);
}

int mcp_tls_ctx_add_cert(mcp_tls_ctx_t * ctx, const void * data,
                         size_t size, uint32_t flags)
{
    WOLFSSL_CTX * sslctx = (WOLFSSL_CTX *) ctx;
    (void)flags;
    int res;

    res = wolfSSL_CTX_load_verify_buffer(sslctx,
        data, size, SSL_FILETYPE_PEM);
    if(res != SSL_SUCCESS) return -EINVAL;

    return 0;
}

int mcp_tls_connect(void *ctx,
                    const char *hostname, const char *port,
                    unsigned int timeout_second,
                    struct webclient_tls_connection **connp)
{
    WOLFSSL_CTX * sslctx = (WOLFSSL_CTX *) ctx;
    int res;
    struct addrinfo * peer;
    int fd;
    WOLFSSL * ssl;
    int retval = -EINVAL;

    res = wolfSSL_CTX_UseSNI(sslctx, WOLFSSL_SNI_HOST_NAME, hostname, strlen(hostname));
    if(res != SSL_SUCCESS) return -EINVAL;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    res = getaddrinfo(hostname, port, &hints, &peer);
    if(res != 0) return res == EAI_SYSTEM ? -errno : -EINVAL;

    fd = socket(peer->ai_family, peer->ai_socktype, peer->ai_protocol);
    if(fd < 0) {
        retval = -errno;
        goto cleanup_addrinfo;
    }

    const struct timeval timeout = {.tv_sec=timeout_second};
    res = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    if(res != 0) {
        retval = -errno;
        goto cleanup_socket;
    }
    res = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if(res != 0) {
        retval = -errno;
        goto cleanup_socket;
    }

    res = connect(fd, peer->ai_addr, peer->ai_addrlen);
    if(res != 0) {
        retval = -errno;
        goto cleanup_socket;
    }

    ssl = wolfSSL_new(sslctx);
    if(ssl == NULL) goto cleanup_socket;

    res = wolfSSL_set_fd(ssl, fd);
    if(res != SSL_SUCCESS) goto cleanup_ssl;
    res = wolfSSL_connect(ssl);
    if(res != SSL_SUCCESS) goto cleanup_ssl;

    freeaddrinfo(peer);
    *connp = (struct webclient_tls_connection *) ssl;
    return 0;
cleanup_ssl:
    wolfSSL_free(ssl);
cleanup_socket:
    res = close(fd);
    assert(res == 0);
cleanup_addrinfo:
    freeaddrinfo(peer);
    return retval;
}

ssize_t mcp_tls_send(void *ctx,
                     struct webclient_tls_connection *conn,
                     const void *buf, size_t len)
{
    (void)ctx;
    WOLFSSL * ssl = (WOLFSSL *) conn;
    int res;

    res = wolfSSL_write(ssl, buf, len);
    // map fatal errors to timeout
    return res >= 0 ? res : -ETIMEDOUT;
}

ssize_t mcp_tls_recv(void *ctx,
                     struct webclient_tls_connection *conn,
                     void *buf, size_t len)
{
    (void)ctx;
    WOLFSSL * ssl = (WOLFSSL *) conn;
    int res;

    // wolfSSL_read can read 0 < res < len
    // and must be called again. webclient.c does this.
    res = wolfSSL_read(ssl, buf, len);
    // map fatal errors to timeout
    return res >= 0 ? res : -ETIMEDOUT;
}

int mcp_tls_close(void *ctx,
                  struct webclient_tls_connection *conn)
{
    (void)ctx;
    WOLFSSL * ssl = (WOLFSSL *) conn;
    int res;

    int fd = wolfSSL_get_fd(ssl);
    wolfSSL_free(ssl);
    res = close(fd);
    return res == 0 ? 0 : -errno;
}

int mcp_tls_get_poll_info(void *ctx,
                          struct webclient_tls_connection *conn,
                          struct webclient_poll_info *info)
{
    (void)ctx;
    WOLFSSL * ssl = (WOLFSSL *) conn;

    info->fd = wolfSSL_get_fd(ssl);
    info->flags = 0;

    return 0;
}

const struct webclient_tls_ops mcp_tls_webclient_tls_ops = {
    .connect = mcp_tls_connect,
    .send = mcp_tls_send,
    .recv = mcp_tls_recv,
    .close = mcp_tls_close,
    .get_poll_info = mcp_tls_get_poll_info,
    .init_connection = NULL, // unimplemented
};
