#include "beeper_task_private.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

#include <netutils/cJSON.h>
#include <netutils/cJSON_Utils.h>

#include "pdjson/pdjson.h"

#include <olm/olm.h>
#include <olm/sas.h>

typedef struct {
    WOLFSSL_CTX * wolfssl_ctx;
    struct addrinfo * peer;
} beeper_task_https_ctx_t;

typedef struct {
    WOLFSSL * wolfssl_ssl;
    bool blocking;
} beeper_task_https_conn_t;

typedef enum {
    BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED,
    BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED,
    BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED
} beeper_task_device_key_status_t;

struct beeper_task_t {
    char * username;
    char * password;
    beeper_task_event_cb_t event_cb;
    void * event_cb_user_data;
    char * upath;
    // queue_t queue;
    pthread_t thread;

    int rng_fd;
    beeper_task_https_ctx_t https_ctx;
    beeper_task_https_conn_t https_conn[2];
    char * user_id;
    char * auth_header;
    char * device_id;
    OlmAccount * olm_account;
    unsigned long long txid;
};

typedef struct {
    beeper_task_https_conn_t * conn;
    int data_full_len;
    int position;
    int recent;
    int chunk_remaining_len;
    bool has_read_a_chunk_already;
    bool peek_val_ready;
    bool capturing;
    int capture_start_pos;
    uint8_t * capture_data;
    int capture_data_len;
    int capture_data_capacity;
} stream_data_t;

#define STRING_LITERAL_LEN(s) (sizeof(s) - 1)

#define BEEPER_MATRIX_URL "matrix.beeper.com"
#define ONE_TIME_KEY_COUNT_TARGET 10

#define HEADERS_ALLOC_CHUNK_SZ 1000
#define OK_STATUS_START "HTTP/1.1 2"
#define CONTENT_LENGTH_HEADER "\r\nContent-Length:"

static const unsigned char beeper_matrix_root_cert[] = R"(-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)";

static void * gen_random(int rng_fd, size_t n)
{
    void * random_data = malloc(n);
    assert(random_data);
    size_t br = read(rng_fd, random_data, n);
    assert(br == n);
    return random_data;
}

static void pickle_account(beeper_task_t * t)
{
    size_t account_pickle_length = olm_pickle_account_length(t->olm_account);
    char * account_pickle = malloc(account_pickle_length);
    assert(account_pickle);
    size_t olm_res = olm_pickle_account(t->olm_account, NULL, 0, account_pickle, account_pickle_length);
    assert(olm_res == account_pickle_length);
    char * account_pickle_path;
    int res = asprintf(&account_pickle_path, "%solm_account_pickle", t->upath);
    assert(res != -1);
    int fd = open(account_pickle_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd != -1);
    free(account_pickle_path);
    ssize_t bw = write(fd, account_pickle, account_pickle_length);
    assert(account_pickle_length == bw);
    res = close(fd);
    assert(res == 0);
    free(account_pickle);
}

cJSON * get_n_one_time_keys(beeper_task_t * t, int n)
{
    size_t otks_buf_len = olm_account_one_time_keys_length(t->olm_account);
    char * otks_buf = malloc(otks_buf_len);
    assert(otks_buf);
    size_t olm_res = olm_account_one_time_keys(t->olm_account, otks_buf, otks_buf_len);
    assert(olm_res == otks_buf_len);

    cJSON * otks_json = cJSON_ParseWithLength(otks_buf, otks_buf_len);
    int otks_n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(otks_json, "curve25519"));
    assert(otks_n <= n); /* assured by the usage pattern */

    if(otks_n < n) {
        cJSON_Delete(otks_json);

        int new_needed_n = n - otks_n;
        size_t random_length = olm_account_generate_one_time_keys_random_length(
            t->olm_account, new_needed_n);
        void * random = gen_random(t->rng_fd, random_length);
        olm_res = olm_account_generate_one_time_keys(t->olm_account, new_needed_n,
                                                     random, random_length);
        assert(olm_res == new_needed_n);
        free(random);

        otks_buf_len = olm_account_one_time_keys_length(t->olm_account);
        otks_buf = realloc(otks_buf, otks_buf_len);
        assert(otks_buf);
        olm_res = olm_account_one_time_keys(t->olm_account, otks_buf, otks_buf_len);
        assert(olm_res == otks_buf_len);

        otks_json = cJSON_ParseWithLength(otks_buf, otks_buf_len);
        otks_n = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(otks_json, "curve25519"));
        assert(otks_n == n);
    }

    free(otks_buf);
    return otks_json;
}

static cJSON * unwrap_cjson(cJSON * item)
{
    assert(item);
    return item;
}

static void https_ctx_init(beeper_task_https_ctx_t * ctx)
{
    static bool wolfssl_is_init = false;
    if (!wolfssl_is_init) {
        wolfssl_is_init = true;
        assert(wolfSSL_Init() == SSL_SUCCESS);
    }

    ctx->wolfssl_ctx = wolfSSL_CTX_new(wolfTLSv1_2_client_method());
    assert(ctx->wolfssl_ctx);

    assert(wolfSSL_CTX_load_verify_buffer(ctx->wolfssl_ctx,
           beeper_matrix_root_cert, STRING_LITERAL_LEN(beeper_matrix_root_cert),
           SSL_FILETYPE_PEM) == WOLFSSL_SUCCESS);

    assert(SSL_SUCCESS == wolfSSL_CTX_UseSNI(ctx->wolfssl_ctx, WOLFSSL_SNI_HOST_NAME,
                                             BEEPER_MATRIX_URL, STRING_LITERAL_LEN(BEEPER_MATRIX_URL)));

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    assert(0 == getaddrinfo(BEEPER_MATRIX_URL, "443", &hints, &ctx->peer));
}

static void https_ctx_deinit(beeper_task_https_ctx_t * ctx)
{
    freeaddrinfo(ctx->peer);
    wolfSSL_CTX_free(ctx->wolfssl_ctx);
}

static void https_conn_init(beeper_task_https_ctx_t * ctx, beeper_task_https_conn_t * conn)
{
    conn->blocking = true;

    int fd = socket(ctx->peer->ai_family, ctx->peer->ai_socktype, ctx->peer->ai_protocol);
    assert(fd >= 0);

    assert(0 == connect(fd, ctx->peer->ai_addr, ctx->peer->ai_addrlen));

    conn->wolfssl_ssl = wolfSSL_new(ctx->wolfssl_ctx);
    assert(conn->wolfssl_ssl);

    assert(wolfSSL_set_fd(conn->wolfssl_ssl, fd) == SSL_SUCCESS);

    assert(wolfSSL_connect(conn->wolfssl_ssl) == SSL_SUCCESS);
}

static int https_fd(beeper_task_https_conn_t * conn)
{
    return wolfSSL_get_fd(conn->wolfssl_ssl);
}

static void https_set_blocking(beeper_task_https_conn_t * conn, bool blocking)
{
    if(conn->blocking == blocking) return;
    int fd = https_fd(conn);
    int flags = fcntl(fd, F_GETFL, 0);
    assert(flags != -1);
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    assert(-1 != fcntl(fd, F_SETFL, flags));
    conn->blocking = blocking;
}

static void https_conn_deinit(beeper_task_https_conn_t * conn)
{
    int fd = https_fd(conn);
    https_set_blocking(conn, true);
    assert(SSL_SUCCESS == wolfSSL_shutdown(conn->wolfssl_ssl));
    wolfSSL_free(conn->wolfssl_ssl);
    assert(0 == shutdown(fd, SHUT_RDWR));
    assert(0 == close(fd));
}

static void request_send(beeper_task_https_conn_t * conn, const char * method, const char * path,
                         const char * extra_headers, const char * json_str)
{
    int res;

    char * req;
    int req_len;
    if(json_str) {
        req_len = asprintf(&req, "%s /_matrix/client/v3/%s HTTP/1.1\r\n"
                           "Host: matrix.beeper.com\r\n"
                           "Content-Length: %u\r\n"
                           "%s"
                           "\r\n"
                           "%s",
                           method, path, (unsigned) strlen(json_str),
                           extra_headers ? extra_headers : "", json_str);
    }
    else {
        req_len = asprintf(&req, "%s /_matrix/client/v3/%s HTTP/1.1\r\n"
                           "Host: matrix.beeper.com\r\n"
                           "%s"
                           "\r\n",
                           method, path, extra_headers ? extra_headers : "");
    }
    assert(req_len != -1);

    https_set_blocking(conn, true);
    res = wolfSSL_write(conn->wolfssl_ssl, req, req_len);
    assert(res == req_len);

    free(req);
}

static int request_read_chunk_size(beeper_task_https_conn_t * conn)
{
    int res;
    char chunk_head_buf[10];
    res = wolfSSL_read(conn->wolfssl_ssl, chunk_head_buf, 3);
    assert(res > 0);
    int chunk_head_len = 3;
    while(!(chunk_head_buf[chunk_head_len - 2] == '\r'
            && chunk_head_buf[chunk_head_len - 1] == '\n')) {
        assert(chunk_head_len != 10);
        res = wolfSSL_read(conn->wolfssl_ssl, &chunk_head_buf[chunk_head_len], 1);
        assert(res > 0);
        chunk_head_len += 1;
    }
    chunk_head_buf[chunk_head_len - 2] = '\0'; /* C0FFEE\0\n */
    unsigned chunk_sz;
    res = sscanf(chunk_head_buf, "%x", &chunk_sz);
    assert(res == 1);
    return chunk_sz;
}

static void request_skip_chunk_trailer(beeper_task_https_conn_t * conn)
{
    int res;
    char chunk_end_buf[2];
    res = wolfSSL_read(conn->wolfssl_ssl, chunk_end_buf, 2); /* skip trailing \r\n */
    assert(res > 0);
}

static bool request_recv(beeper_task_https_conn_t * conn, int * resp_len_out,
                         bool blocking, int * nonblocking_status_out,
                         bool read_resp_now, char ** resp_out)
{
    int res;

    char first_byte;
    if(!blocking) {
        https_set_blocking(conn, false);
        res = wolfSSL_read(conn->wolfssl_ssl, &first_byte, 1);
        if(res <= 0) {
            res = wolfSSL_get_error(conn->wolfssl_ssl, res);
            assert(res == SSL_ERROR_WANT_READ || res == SSL_ERROR_WANT_WRITE);
            *nonblocking_status_out = res;
            return false;
        }
    }

    https_set_blocking(conn, true);

    char * head = malloc(HEADERS_ALLOC_CHUNK_SZ);
    assert(head);
    int head_len = 0;
    int head_cap = HEADERS_ALLOC_CHUNK_SZ;
    if(!blocking) {
        head[0] = first_byte;
        head_len = 1;
    }
    res = wolfSSL_read(conn->wolfssl_ssl, &head[head_len], 4 - head_len);
    assert(res > 0);
    head_len = 4;
    while(0 != memcmp(head + (head_len - 4), "\r\n\r\n", 4)) {
        if(head_len == head_cap) {
            head_cap += HEADERS_ALLOC_CHUNK_SZ;
            head = realloc(head, head_cap);
            assert(head);
        }
        res = wolfSSL_read(conn->wolfssl_ssl, &head[head_len], 1);
        assert(res > 0);
        head_len += 1;
    };

    head[head_len - 2] = '\0'; /* \r\n\0\n */

    assert(0 == strncmp(head, OK_STATUS_START, STRING_LITERAL_LEN(OK_STATUS_START)));
    assert(NULL != strcasestr(head, "\r\nconnection: keep-alive\r\n"));
    int content_len;
    bool has_content = NULL != strcasestr(head, "\r\nContent-Type:");
    if(has_content) {
        char * content_length_header = strcasestr(head, CONTENT_LENGTH_HEADER);
        if(content_length_header) {
            assert(1 == sscanf(content_length_header + STRING_LITERAL_LEN(CONTENT_LENGTH_HEADER), "%d", &content_len));
        }
        else {
            assert(NULL != strcasestr(head, "\r\nTransfer-Encoding: chunked\r\n"));
            content_len = -1;
        }
    }
    free(head);
    if(!has_content) {
        *resp_len_out = 0;
        return true;
    }

    if(read_resp_now) {
        char * resp;
        if(content_len != -1) {
            resp = malloc(content_len + 1);
            assert(resp);
            res = wolfSSL_read(conn->wolfssl_ssl, resp, content_len);
            assert(res > 0);
        }
        else {
            content_len = 0;
            resp = malloc(1);
            assert(resp);
            int chunk_sz;
            do {
                chunk_sz = request_read_chunk_size(conn);
                if(chunk_sz) {
                    resp = realloc(resp, content_len + chunk_sz + 1);
                    assert(resp);
                    res = wolfSSL_read(conn->wolfssl_ssl, &resp[content_len], chunk_sz);
                    assert(res > 0);
                    content_len += chunk_sz;
                }
                request_skip_chunk_trailer(conn);
            } while(chunk_sz);
        }
        resp[content_len] = '\0';
        *resp_out = resp;
    }

    *resp_len_out = content_len;
    return true;
}

static char * request(beeper_task_https_conn_t * conn, const char * method, const char * path,
                      const char * extra_headers, const char * json_str)
{
    request_send(conn, method, path, extra_headers, json_str);
    char * resp;
    int resp_len;
    request_recv(conn, &resp_len, true, NULL, true, &resp);
    return resp;
}

static void request_recv_more(beeper_task_https_conn_t * conn, char * dst, int len)
{
    https_set_blocking(conn, true);
    int res = wolfSSL_read(conn->wolfssl_ssl, dst, len);
    assert(res == len);
}

static void recursively_sort_json_objects(cJSON * json)
{
    cJSON * child;
    cJSON_ArrayForEach(child, json) {
        recursively_sort_json_objects(child);
    }
    if(cJSON_IsObject(json)) {
        cJSONUtils_SortObjectCaseSensitive(json);
    }
}

static char * canonical_json(cJSON * json)
{
    recursively_sort_json_objects(json);
    char * stringified = cJSON_PrintUnformatted(json);
    assert(stringified);
    return stringified;
}

static cJSON * sign_json(beeper_task_t * t, cJSON * json)
{
    char * stringified = canonical_json(json);

    size_t signature_length = olm_account_signature_length(t->olm_account);
    char * signature = malloc(signature_length + 1);
    assert(signature);
    assert(signature_length == olm_account_sign(t->olm_account,
        stringified, strlen(stringified),
        signature, signature_length));
    signature[signature_length] = '\0';
    free(stringified);

    cJSON * ret_obj = unwrap_cjson(cJSON_CreateObject());
    cJSON * user_signatures_obj = unwrap_cjson(cJSON_CreateObject());
    assert(cJSON_AddItemToObject(ret_obj, t->user_id, user_signatures_obj));
    char * ed_key_id;
    assert(-1 != asprintf(&ed_key_id, "ed25519:%s", t->device_id));
    assert(cJSON_AddItemToObject(user_signatures_obj, ed_key_id, unwrap_cjson(cJSON_CreateString(signature))));
    free(ed_key_id);

    free(signature);

    return ret_obj;
}

static beeper_task_device_key_status_t device_key_status(beeper_task_t * t, beeper_task_https_conn_t * conn)
{
    char * keys_query_json_str;
    assert(-1 != asprintf(&keys_query_json_str, "{\"device_keys\":{\"%s\":[\"%s\"]},\"timeout\":10000}",
                          t->user_id, t->device_id));
    // cJSON * keys_query_json = unwrap_cjson(cJSON_CreateObject());
    // cJSON * keys_query_json_device_id_array;
    // {
    //     cJSON * device_keys = unwrap_cjson(cJSON_CreateObject());
    //     cJSON_AddItemToObjectCS(keys_query_json, "device_keys", device_keys);

    //     keys_query_json_device_id_array = unwrap_cjson(cJSON_CreateArray());
    //     cJSON_AddItemToObjectCS(device_keys, t->user_id, keys_query_json_device_id_array);
    //     cJSON_AddItemToArray(keys_query_json_device_id_array,
    //                          unwrap_cjson(cJSON_CreateStringReference(t->device_id)));

    //     cJSON_AddItemToObjectCS(keys_query_json, "timeout",
    //                             unwrap_cjson(cJSON_CreateNumber(10000)));
    // }
    // char * keys_query_json_str = cJSON_PrintUnformatted(keys_query_json);
    // assert(keys_query_json_str);

    char * resp_str = request(conn, "POST", "keys/query",
                              t->auth_header, keys_query_json_str);
    assert(resp_str);
    free(keys_query_json_str);

    cJSON * resp_json = unwrap_cjson(cJSON_Parse(resp_str));
    free(resp_str);
    beeper_task_device_key_status_t ret = BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED;
    {
        // Devices which haven't uploaded their keys
        // yet do not have an entry in the response.
        cJSON * device_keys = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
            unwrap_cjson(cJSON_GetObjectItemCaseSensitive(resp_json, "device_keys")),
            t->user_id
        ));
        cJSON * this_device = cJSON_GetObjectItemCaseSensitive(device_keys, t->device_id);
        if(!this_device) {
            ret = BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED;
            goto ret_out;
        }

        // Unverified devices' keys are
        // NOT signed by the self signing key,
        cJSON * self_signing_keys = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
            unwrap_cjson(cJSON_GetObjectItemCaseSensitive(resp_json, "self_signing_keys")),
            t->user_id
        ));
        cJSON * ssk_keys = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(self_signing_keys, "keys"));
        assert(cJSON_IsObject(ssk_keys));
        assert(cJSON_GetArraySize(ssk_keys) == 1);
        cJSON * ssk_key = ssk_keys->child;
        char * ssk_key_id = ssk_key->string;

        cJSON * this_device_signatures = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
            unwrap_cjson(cJSON_GetObjectItemCaseSensitive(this_device, "signatures")),
            t->user_id
        ));
        cJSON * this_device_ssk_signature = cJSON_GetObjectItemCaseSensitive(this_device_signatures, ssk_key_id);
        if(!this_device_ssk_signature) {
            ret = BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED;
            goto ret_out;
        }

        // if the signatures are all valid, this device is verified
        // cJSON * master_keys = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
        //     unwrap_cjson(cJSON_GetObjectItemCaseSensitive(resp_json, "master_keys")),
        //     t->user_id
        // ));
        // cJSON * master_keys_signatures = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
        //     unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
        //         master_keys,
        //         "signatures"
        //     )),
        //     t->user_id
        // ));
        // assert(cJSON_IsObject(master_keys_signatures));
        // assert(cJSON_GetArraySize(master_keys_signatures) == 1);
        // cJSON * master_keys_signature = master_keys_signatures->child;
        // char * master_keys_signature_id = master_keys_signature->string;
        // assert(0 == strncmp("ed25519:", master_keys_signature_id, 8));
        // char * master_device_id = &master_keys_signature_id[8];

        // cJSON_AddItemToArray(keys_query_json_device_id_array,
        //                      unwrap_cjson(cJSON_CreateStringReference(master_device_id)));
    }

ret_out:
    cJSON_Delete(resp_json);
    // cJSON_Delete(keys_query_json);
    return ret;
}

static void stream_data_init(
    stream_data_t * sd,
    beeper_task_https_conn_t * conn,
    int data_full_len
)
{
    memset(sd, 0, sizeof(*sd));
    sd->conn = conn;
    sd->data_full_len = data_full_len;
}

static void stream_data_deinit(stream_data_t * sd)
{
    free(sd->capture_data);
}

static void _stream_data_capture_push_val(stream_data_t * sd, uint8_t val)
{
    if(!sd->capturing) return;
    if(sd->capture_data_len == sd->capture_data_capacity) {
        sd->capture_data_capacity += 1000;
        sd->capture_data = realloc(sd->capture_data, sd->capture_data_capacity);
        assert(sd->capture_data);
    }
    sd->capture_data[sd->capture_data_len++] = val;
}

static int _stream_data_return(stream_data_t * sd, int val)
{
    sd->recent = val;
    if(val == EOF) return val;
    sd->position += 1;
    _stream_data_capture_push_val(sd, val);
    return val;
}

static int stream_data_peek(void * user_data)
{
    stream_data_t * sd = user_data;
    if(sd->peek_val_ready) {
        return sd->recent;
    }
    sd->peek_val_ready = true;

    if(sd->data_full_len != -1) {
        if(sd->position >= sd->data_full_len) return _stream_data_return(sd, EOF);
    }
    else {
        if(sd->chunk_remaining_len == 0) {
            if(sd->has_read_a_chunk_already) {
                request_skip_chunk_trailer(sd->conn);
            }
            int chunk_sz = request_read_chunk_size(sd->conn);
            if(chunk_sz > 0) {
                sd->chunk_remaining_len = chunk_sz;
            }
            else {
                sd->chunk_remaining_len = -1;
                request_skip_chunk_trailer(sd->conn);
            }
            sd->has_read_a_chunk_already = true;
        }
        if(sd->chunk_remaining_len == -1) return _stream_data_return(sd, EOF);
        sd->chunk_remaining_len -= 1;
    }
    uint8_t new_byte;
    request_recv_more(sd->conn, (char *) &new_byte, 1);
    return _stream_data_return(sd, new_byte);
}

static int stream_data_get(void * user_data)
{
    stream_data_t * sd = user_data;
    if(sd->peek_val_ready) {
        sd->peek_val_ready = false;
        return sd->recent;
    }
    int ret = stream_data_peek(sd);
    sd->peek_val_ready = false;
    return ret;
}

static void stream_data_start_capture(stream_data_t * sd)
{
    assert(!sd->capturing);
    sd->capturing = true;
    sd->capture_data_len = 0;
    if(sd->position > 0) {
        assert(sd->recent != EOF);
        sd->capture_start_pos = sd->position - 1;
        _stream_data_capture_push_val(sd, sd->recent);
    } else {
        sd->capture_start_pos = 0;
    }
}

static const char * stream_data_get_capture(stream_data_t * sd, int start, int len)
{
    assert(sd->capturing);
    sd->capturing = false;
    assert(start >= sd->capture_start_pos);
    int offset = start - sd->capture_start_pos;
    assert(len <= sd->capture_data_len - offset);
    return (char *) sd->capture_data + offset;
}

static void * thread(void * arg)
{
    int res;
    size_t olm_res;
    beeper_task_t * t = arg;

    res = mkdir(t->upath, 0755);
    assert(res == 0 || errno == EEXIST);

    t->rng_fd = open(BEEPER_RANDOM_PATH, O_RDONLY);
    assert(t->rng_fd != -1);

    https_ctx_init(&t->https_ctx);

    https_conn_init(&t->https_ctx, &t->https_conn[0]);

    char * device_id_path;
    res = asprintf(&device_id_path, "%sdevice_id", t->upath);
    assert(res != -1);
    char * device_id = beeper_read_text_file(device_id_path);

    cJSON * login_json = unwrap_cjson(cJSON_CreateObject());
    {
        cJSON * identifier = unwrap_cjson(cJSON_CreateObject());
        cJSON_AddItemToObjectCS(login_json, "identifier", identifier);

        cJSON_AddItemToObjectCS(identifier, "type", unwrap_cjson(cJSON_CreateStringReference("m.id.user")));
        cJSON_AddItemToObjectCS(identifier, "user", unwrap_cjson(cJSON_CreateStringReference(t->username)));

        cJSON_AddItemToObjectCS(login_json, "password", unwrap_cjson(cJSON_CreateStringReference(t->password)));
        cJSON_AddItemToObjectCS(login_json, "type", unwrap_cjson(cJSON_CreateStringReference("m.login.password")));
        cJSON_AddItemToObjectCS(login_json, "initial_device_display_name", unwrap_cjson(cJSON_CreateStringReference(BEEPER_DEVICE_DISPLAY_NAME)));
        if(device_id) {
            cJSON_AddItemToObjectCS(login_json, "device_id", unwrap_cjson(cJSON_CreateStringReference(device_id)));
        }
    }
    char * login_json_str = cJSON_PrintUnformatted(login_json);
    assert(login_json_str);
    cJSON_Delete(login_json);

    char * resp_str = request(&t->https_conn[0], "POST", "login", NULL, login_json_str);
    assert(resp_str);
    free(login_json_str);
    // printf("%s\n", resp_str);
    cJSON * resp_json = unwrap_cjson(cJSON_Parse(resp_str));
    free(resp_str);
    {
        assert(cJSON_IsObject(resp_json));

        char * user_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp_json, "user_id"));
        assert(user_id);
        t->user_id = strdup(user_id);
        assert(t->user_id);

        char * access_token = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp_json, "access_token"));
        assert(access_token);
        res = asprintf(&t->auth_header, "Authorization: Bearer %s\r\n", access_token);
        assert(res != -1);

        char * resp_device_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(resp_json, "device_id"));
        assert(resp_device_id);
        if(device_id) {
            assert(0 == strcmp(device_id, resp_device_id));
            t->device_id = device_id;
        }
        else {
            int fd = open(device_id_path, O_WRONLY | O_EXCL | O_CREAT, 0644);
            assert(fd != -1);
            free(device_id_path);
            ssize_t device_id_len = strlen(resp_device_id);
            ssize_t bw = write(fd, resp_device_id, device_id_len);
            assert(device_id_len == bw);
            res = close(fd);
            assert(res == 0);

            t->device_id = strdup(resp_device_id);
            assert(t->device_id);
        }
    }
    cJSON_Delete(resp_json);

    t->olm_account = malloc(olm_account_size());
    assert(t->olm_account);
    olm_account(t->olm_account);

    char * account_pickle_path;
    res = asprintf(&account_pickle_path, "%solm_account_pickle", t->upath);
    assert(res != -1);
    char * account_pickle = beeper_read_text_file(account_pickle_path);
    free(account_pickle_path);

    if(account_pickle) {
        /* function doc says input pickled buffer will be "destroyed" */
        olm_res = olm_unpickle_account(t->olm_account,
                                       NULL, 0,
                                       account_pickle, strlen(account_pickle));
        free(account_pickle);
    }
    else {
        size_t random_length = olm_create_account_random_length(t->olm_account);
        void * random_data = gen_random(t->rng_fd, random_length);
        olm_res = olm_create_account(t->olm_account, random_data, random_length);
        assert(olm_res == 0);
        free(random_data);
    }

    /* check whether we've uploaded our keys yet and whether our keys */
    /* have been signed by the account's self signing key (device is verified). */
    beeper_task_device_key_status_t device_key_status_res = device_key_status(t, &t->https_conn[0]);

    if(device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED) {
        size_t identity_keys_length = olm_account_identity_keys_length(t->olm_account);
        char * identity_keys = malloc(identity_keys_length);
        assert(identity_keys);
        olm_res = olm_account_identity_keys(t->olm_account, identity_keys, identity_keys_length);
        assert(olm_res == identity_keys_length);
        cJSON * identity_keys_json = cJSON_ParseWithLength(identity_keys, identity_keys_length);
        free(identity_keys);

        cJSON * one_time_keys_json = get_n_one_time_keys(t, ONE_TIME_KEY_COUNT_TARGET);

        size_t fallback_key_length = olm_account_unpublished_fallback_key_length(t->olm_account);
        char * fallback_key = malloc(fallback_key_length);
        assert(fallback_key);
        olm_res = olm_account_unpublished_fallback_key(t->olm_account, fallback_key, fallback_key_length);
        assert(olm_res == fallback_key_length);
        cJSON * fallback_key_json = cJSON_ParseWithLength(fallback_key, fallback_key_length);
        if(cJSON_GetObjectItemCaseSensitive(fallback_key_json, "curve25519")->child == NULL) {
            cJSON_Delete(fallback_key_json);

            size_t random_length = olm_account_generate_fallback_key_random_length(t->olm_account);
            void * random = gen_random(t->rng_fd, random_length);
            olm_res = olm_account_generate_fallback_key(t->olm_account, random, random_length);
            assert(olm_res == 1);
            free(random);

            fallback_key_length = olm_account_unpublished_fallback_key_length(t->olm_account);
            fallback_key = realloc(fallback_key, fallback_key_length);
            assert(fallback_key);
            olm_res = olm_account_unpublished_fallback_key(t->olm_account, fallback_key, fallback_key_length);
            assert(olm_res == fallback_key_length);
            fallback_key_json = cJSON_ParseWithLength(fallback_key, fallback_key_length);
            assert(cJSON_GetObjectItemCaseSensitive(fallback_key_json, "curve25519")->child != NULL);
        }
        free(fallback_key);

        cJSON * upload_keys_json = unwrap_cjson(cJSON_CreateObject());
        {
            cJSON * device_keys = unwrap_cjson(cJSON_CreateObject());
            cJSON_AddItemToObjectCS(upload_keys_json, "device_keys", device_keys);

            cJSON * algorithms = unwrap_cjson(cJSON_CreateArray());
            cJSON_AddItemToObjectCS(device_keys, "algorithms", algorithms);
            cJSON_AddItemToArray(algorithms, unwrap_cjson(cJSON_CreateStringReference("m.olm.v1.curve25519-aes-sha2")));
            cJSON_AddItemToArray(algorithms, unwrap_cjson(cJSON_CreateStringReference("m.megolm.v1.aes-sha2")));

            cJSON_AddItemToObjectCS(device_keys, "device_id", unwrap_cjson(cJSON_CreateStringReference(t->device_id)));

            cJSON * keys = unwrap_cjson(cJSON_CreateObject());
            cJSON_AddItemToObjectCS(device_keys, "keys", keys);

            char * curve_device_key_id;
            assert(-1 != asprintf(&curve_device_key_id, "curve25519:%s", t->device_id));
            assert(cJSON_AddItemToObject(keys, curve_device_key_id, unwrap_cjson(cJSON_CreateStringReference(
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(identity_keys_json, "curve25519"))))));
            free(curve_device_key_id);

            char * ed_device_key_id;
            assert(-1 != asprintf(&ed_device_key_id, "ed25519:%s", t->device_id));
            assert(cJSON_AddItemToObject(keys, ed_device_key_id, unwrap_cjson(cJSON_CreateStringReference(
                cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(identity_keys_json, "ed25519"))))));
            free(ed_device_key_id);

            cJSON_AddItemToObjectCS(device_keys, "user_id", unwrap_cjson(cJSON_CreateStringReference(t->user_id)));

            cJSON_AddItemToObjectCS(device_keys, "signatures", sign_json(t, device_keys));
        }
        {
            cJSON * fallback_keys = unwrap_cjson(cJSON_CreateObject());
            cJSON_AddItemToObjectCS(upload_keys_json, "fallback_keys", fallback_keys);

            cJSON * key_item = cJSON_GetObjectItemCaseSensitive(fallback_key_json, "curve25519")->child;

            cJSON * fallback_key_entry = unwrap_cjson(cJSON_CreateObject());
            char * entry_name;
            assert(-1 != asprintf(&entry_name, "signed_curve25519:%s", key_item->string));
            assert(cJSON_AddItemToObject(fallback_keys, entry_name, fallback_key_entry));
            free(entry_name);

            cJSON_AddItemToObjectCS(fallback_key_entry, "fallback", unwrap_cjson(cJSON_CreateTrue()));

            cJSON_AddItemToObjectCS(fallback_key_entry, "key", unwrap_cjson(cJSON_CreateStringReference(
                cJSON_GetStringValue(key_item))));

            cJSON_AddItemToObjectCS(fallback_key_entry, "signatures", sign_json(t, fallback_key_entry));
        }
        {
            cJSON * one_time_keys = unwrap_cjson(cJSON_CreateObject());
            cJSON_AddItemToObjectCS(upload_keys_json, "one_time_keys", one_time_keys);

            cJSON * key_items = cJSON_GetObjectItemCaseSensitive(one_time_keys_json, "curve25519");
            cJSON * item;
            cJSON_ArrayForEach(item, key_items) {
                cJSON * otk_entry = unwrap_cjson(cJSON_CreateObject());
                char * entry_name;
                assert(-1 != asprintf(&entry_name, "signed_curve25519:%s", item->string));
                assert(cJSON_AddItemToObject(one_time_keys, entry_name, otk_entry));
                free(entry_name);

                cJSON_AddItemToObjectCS(otk_entry, "key", unwrap_cjson(cJSON_CreateStringReference(
                    cJSON_GetStringValue(item))));

                cJSON_AddItemToObjectCS(otk_entry, "signatures", sign_json(t, otk_entry));
            }
        }

        char * upload_keys_json_str = cJSON_PrintUnformatted(upload_keys_json);
        assert(upload_keys_json_str);

        cJSON_Delete(upload_keys_json);
        cJSON_Delete(fallback_key_json);
        cJSON_Delete(one_time_keys_json);
        cJSON_Delete(identity_keys_json);

        pickle_account(t);

        resp_str = request(&t->https_conn[0], "POST", "keys/upload",
                                  t->auth_header, upload_keys_json_str);
        assert(resp_str);
        free(upload_keys_json_str);
        free(resp_str);

        device_key_status_res = BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED;
    }

    if(device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED) {
        olm_res = olm_account_mark_keys_as_published(t->olm_account);
        if(olm_res > 0) {
            pickle_account(t);
        }
    }

    bool is_verified = device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED;
    bool * is_verified_event_data = malloc(sizeof(bool));
    assert(is_verified_event_data);
    *is_verified_event_data = is_verified;
    t->event_cb(BEEPER_TASK_EVENT_VERIFICATION_STATUS, is_verified_event_data, t->event_cb_user_data);

    https_conn_init(&t->https_ctx, &t->https_conn[1]);
    request_send(&t->https_conn[1], "GET", "sync?timeout=30000", t->auth_header, NULL);

    enum { SAS_STEP_REQUEST, SAS_STEP_START, SAS_STEP_KEY, SAS_STEP_IDK } sas_step = SAS_STEP_REQUEST;
    char * sas_txid = NULL;
    char * sas_device_id = NULL;
    OlmSAS * sas_olm_sas = NULL;

    struct pollfd pfd[1] = {
        // {.fd = queue_fd(), .events = POLLIN},
        {.fd = https_fd(&t->https_conn[1])}
    };
    while(1) {
        bool something_happened = false;

        // queue_dequeue();

        while(1) {
            int resp_len;
            int nonblocking_status;
            bool response_was_received = request_recv(&t->https_conn[1], &resp_len,
                                                      false, &nonblocking_status,
                                                      false, NULL);
            if(!response_was_received) {
                pfd[0].events = nonblocking_status == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                break;
            }
            something_happened = true;

            char * sync_path_with_since = NULL;

            stream_data_t sd;
            stream_data_init(&sd, &t->https_conn[1], resp_len);
            json_stream pdjson;
            json_open_user(&pdjson, stream_data_get, stream_data_peek, &sd);
            json_set_streaming(&pdjson, false);

            enum json_type e;
            const char * object_key_string;
            int start;
            int len;
            const char * capture;
            assert(JSON_OBJECT == json_next(&pdjson));
            while(JSON_OBJECT_END != (e = json_next(&pdjson))) {
                assert(e == JSON_STRING);
                object_key_string = json_get_string(&pdjson, NULL);
                if(0 == strcmp("next_batch", object_key_string)) {
                    assert(json_next(&pdjson) == JSON_STRING);
                    assert(-1 != asprintf(&sync_path_with_since, "sync?timeout=30000&since=%s",
                                          json_get_string(&pdjson, NULL)));
                    assert(sync_path_with_since);
                }
                else if(0 == strcmp("to_device", object_key_string)) {
                    assert(JSON_OBJECT == json_next(&pdjson));
                    while(JSON_OBJECT_END != (e = json_next(&pdjson))) {
                        assert(e == JSON_STRING);
                        object_key_string = json_get_string(&pdjson, NULL);
                        if(0 == strcmp("events", object_key_string)) {
                            assert(JSON_ARRAY == json_next(&pdjson));
                            while(JSON_ARRAY_END != (e = json_peek(&pdjson))) {
                                assert(JSON_OBJECT == e);
                                stream_data_start_capture(&sd);
                                start = json_get_position(&pdjson) - 1;
                                assert(JSON_ERROR != json_skip(&pdjson));
                                len = json_get_position(&pdjson) - start;
                                capture = stream_data_get_capture(&sd, start, len);
                                cJSON * to_device_event = unwrap_cjson(cJSON_ParseWithLength(capture, len));
                                char * type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(to_device_event, "type"));
                                assert(type);
                                char * sender = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(to_device_event, "sender"));
                                assert(sender);
                                cJSON * content = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(to_device_event, "content"));
                                if(0 == strcmp(type, "m.key.verification.request")) {
                                    __label__ denied;
                                    if(is_verified || sas_step != SAS_STEP_REQUEST || 0 != strcmp(sender, t->user_id)) goto denied;
                                    cJSON * methods = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(content, "methods"));
                                    assert(cJSON_IsArray(methods));
                                    bool has_sas_method = false;
                                    cJSON * meth;
                                    cJSON_ArrayForEach(meth, methods) {
                                        char * meth_str = cJSON_GetStringValue(meth);
                                        assert(meth_str);
                                        if(0 == strcmp(meth_str, "m.sas.v1")) {
                                            has_sas_method = true;
                                            break;
                                        }
                                    }
                                    if(!has_sas_method) goto denied;
                                    double timestamp = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(content, "timestamp"));
                                    assert(!isnan(timestamp));
                                    struct timespec ts;
                                    assert(0 == clock_gettime(CLOCK_REALTIME, &ts));
                                    double ms_now = ts.tv_sec;
                                    ms_now *= 1000;
                                    long nsec = ts.tv_nsec;
                                    nsec /= 1000000;
                                    ms_now += (double) nsec;
                                    if(timestamp < ms_now - 10.0 * 60.0 * 1000.0
                                       || timestamp > ms_now + 5.0 * 60.0 * 1000.0) goto denied;
                                    char * transaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "transaction_id"));
                                    assert(transaction_id);
                                    char * from_device = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "from_device"));
                                    assert(from_device);
                                    assert(t->txid < ULLONG_MAX);
                                    char * path;
                                    assert(-1 != asprintf(&path, "sendToDevice/m.key.verification.ready/%llx", t->txid++));
                                    char * req_json_str;
                                    assert(-1 != asprintf(&req_json_str,
                                        "{"
                                            "\"messages\":{"
                                                "\"%s\":{"
                                                    "\"%s\":{"
                                                        "\"from_device\": \"%s\","
                                                        "\"methods\":["
                                                            "\"m.sas.v1\""
                                                        "],"
                                                        "\"transaction_id\":\"%s\""
                                                    "}"
                                                "}"
                                            "}"
                                        "}",
                                        sender,
                                        from_device,
                                        t->device_id,
                                        transaction_id
                                    ));
                                    char * empty_resp = request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str);
                                    free(empty_resp);
                                    free(req_json_str);
                                    free(path);
                                    sas_txid = strdup(transaction_id);
                                    assert(sas_txid);
                                    sas_device_id = strdup(from_device);
                                    assert(sas_device_id);
                                    sas_step = SAS_STEP_START;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.start")) {
                                    __label__ denied;
                                    if(sas_step != SAS_STEP_START || 0 != strcmp(sender, t->user_id)) goto denied;
                                    char * from_device = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "from_device"));
                                    assert(from_device);
                                    if(0 != strcmp(from_device, sas_device_id)) goto denied;
                                    char * transaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "transaction_id"));
                                    assert(transaction_id);
                                    if(0 != strcmp(transaction_id, sas_txid)) goto denied;
                                    char * method = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "method"));
                                    assert(method);
                                    if(0 != strcmp(method, "m.sas.v1")) goto denied;
                                    static const char * require[4][2] = {
                                        {"hashes", "sha256"},
                                        {"key_agreement_protocols", "curve25519-hkdf-sha256"},
                                        {"message_authentication_codes", "hkdf-hmac-sha256.v2"},
                                        {"short_authentication_string", "emoji"}
                                    };
                                    for(int i = 0; i < 4; i++) {
                                        cJSON * array = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(content, require[i][0]));
                                        assert(cJSON_IsArray(array));
                                        bool found = false;
                                        cJSON * item;
                                        cJSON_ArrayForEach(item, array) {
                                            char * item_str = cJSON_GetStringValue(item);
                                            assert(item_str);
                                            if(0 == strcmp(item_str, require[i][1])) {
                                                found = true;
                                                break;
                                            }
                                        }
                                        if(!found) goto denied;
                                    }

                                    sas_olm_sas = malloc(olm_sas_size());
                                    assert(sas_olm_sas);
                                    olm_sas(sas_olm_sas);
                                    size_t random_length = olm_create_sas_random_length(sas_olm_sas);
                                    void * random = gen_random(t->rng_fd, random_length);
                                    assert(olm_error() != olm_create_sas(sas_olm_sas, random, random_length));
                                    free(random);

                                    size_t pubkey_length = olm_sas_pubkey_length(sas_olm_sas);
                                    char * pubkey = malloc(pubkey_length);
                                    assert(pubkey);
                                    assert(olm_error() != olm_sas_get_pubkey(sas_olm_sas, pubkey, pubkey_length));

                                    char * canonical_start_event = canonical_json(content);
                                    size_t canonical_start_event_len = strlen(canonical_start_event);

                                    size_t commitment_content_len = pubkey_length + canonical_start_event_len;
                                    char * commitment_content = malloc(commitment_content_len);
                                    assert(commitment_content);
                                    memcpy(commitment_content, pubkey, pubkey_length);
                                    memcpy(commitment_content + pubkey_length, canonical_start_event, canonical_start_event_len);
                                    free(canonical_start_event);

                                    OlmUtility * olm_sha = malloc(olm_utility_size());
                                    assert(olm_sha);
                                    olm_utility(olm_sha);
                                    size_t commitment_len = olm_sha256_length(olm_sha);
                                    char * commitment = malloc(commitment_len);
                                    assert(commitment);
                                    assert(olm_error() != olm_sha256(olm_sha, commitment_content, commitment_content_len,
                                                                     commitment, commitment_len));
                                    olm_clear_utility(olm_sha);
                                    free(olm_sha);
                                    free(commitment_content);

                                    assert(t->txid < ULLONG_MAX);
                                    char * path;
                                    assert(-1 != asprintf(&path, "sendToDevice/m.key.verification.accept/%llx", t->txid++));
                                    char * req_json_str;
                                    assert(-1 != asprintf(&req_json_str,
                                        "{"
                                            "\"messages\":{"
                                                "\"%s\":{"
                                                    "\"%s\":{"
                                                        "\"commitment\":\"%.*s\","
                                                        "\"hash\":\"sha256\","
                                                        "\"key_agreement_protocol\":\"curve25519-hkdf-sha256\","
                                                        "\"message_authentication_code\":\"hkdf-hmac-sha256.v2\","
                                                        "\"method\":\"m.sas.v1\","
                                                        "\"short_authentication_string\":[\"emoji\"],"
                                                        "\"transaction_id\":\"%s\""
                                                    "}"
                                                "}"
                                            "}"
                                        "}",
                                        sender,
                                        from_device,
                                        (int) commitment_len, commitment,
                                        transaction_id
                                    ));
                                    char * empty_resp = request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str);
                                    free(empty_resp);
                                    free(req_json_str);
                                    free(path);
                                    free(commitment);

                                    assert(t->txid < ULLONG_MAX);
                                    assert(-1 != asprintf(&path, "sendToDevice/m.key.verification.key/%llx", t->txid++));
                                    assert(-1 != asprintf(&req_json_str,
                                        "{"
                                            "\"messages\":{"
                                                "\"%s\":{"
                                                    "\"%s\":{"
                                                        "\"key\":\"%.*s\","
                                                        "\"transaction_id\":\"%s\""
                                                    "}"
                                                "}"
                                            "}"
                                        "}",
                                        sender,
                                        from_device,
                                        (int) pubkey_length, pubkey,
                                        transaction_id
                                    ));
                                    empty_resp = request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str);
                                    free(empty_resp);
                                    free(req_json_str);
                                    free(path);
                                    free(pubkey);

                                    sas_step = SAS_STEP_KEY;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.key")) {
                                    __label__ denied;
                                    if(sas_step != SAS_STEP_KEY || 0 != strcmp(sender, t->user_id)) goto denied;
                                    char * transaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "transaction_id"));
                                    assert(transaction_id);
                                    if(0 != strcmp(transaction_id, sas_txid)) goto denied;
                                    char * key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "key"));
                                    assert(key);
                                    /* `olm_sas_set_their_key` doc says the key buffer will be overwritten */
                                    char * key2 = strdup(key);
                                    assert(key2);
                                    assert(olm_error() != olm_sas_set_their_key(sas_olm_sas, key2, strlen(key2)));
                                    free(key2);

                                    size_t my_key_length = olm_sas_pubkey_length(sas_olm_sas);
                                    char * my_key = malloc(my_key_length);
                                    assert(my_key);
                                    assert(olm_error() != olm_sas_get_pubkey(sas_olm_sas, my_key, my_key_length));

                                    char * info;
                                    int info_len = asprintf(&info,
                                        "MATRIX_KEY_VERIFICATION_SAS|%s|%s|%s|%s|%s|%.*s|%s",
                                        sender,
                                        sas_device_id,
                                        key,
                                        sender,
                                        t->device_id,
                                        (int) my_key_length, my_key,
                                        sas_txid
                                    );
                                    assert(info_len != -1);
                                    free(my_key);

                                    uint8_t emoji_bytes[6];
                                    assert(olm_error() != olm_sas_generate_bytes(sas_olm_sas, info, info_len, emoji_bytes, 6));
                                    free(info);

                                    uint64_t sas_int = 0;
                                    for(int i = 0; i < 6; i++) {
                                        sas_int <<= 8;
                                        sas_int |= emoji_bytes[i];
                                    }
                                    uint8_t * emoji_ids = malloc(7);
                                    assert(emoji_ids);
                                    for(int i = 6; i >= 0; i--) {
                                        sas_int >>= 6;
                                        emoji_ids[i] = sas_int & 0x3f;
                                    }
                                    t->event_cb(BEEPER_TASK_EVENT_SAS_EMOJI, emoji_ids, t->event_cb_user_data);

                                    sas_step = SAS_STEP_IDK;
                                    denied:
                                }
                                cJSON_Delete(to_device_event);
                            }
                            json_next(&pdjson);
                        }
                        else assert(JSON_ERROR != json_skip(&pdjson));
                    }
                }
                else assert(JSON_ERROR != json_skip(&pdjson));
            }
            assert(JSON_DONE == json_next(&pdjson));
            json_close(&pdjson);
            stream_data_deinit(&sd);

            assert(sync_path_with_since);
            request_send(&t->https_conn[1], "GET", sync_path_with_since, t->auth_header, NULL);
            free(sync_path_with_since);
        }

        if(something_happened) {
            continue;
        }

        res = poll(pfd, 1, -1);
        assert(res > 0);
    }

    if(sas_olm_sas) olm_clear_sas(sas_olm_sas);
    free(sas_olm_sas);
    free(sas_device_id);
    free(sas_txid);

    olm_clear_account(t->olm_account);
    free(t->olm_account);
    free(t->device_id);
    free(t->auth_header);
    free(t->user_id);
    https_conn_deinit(&t->https_conn[1]);
    https_conn_deinit(&t->https_conn[0]);
    https_ctx_deinit(&t->https_ctx);
    assert(0 == close(t->rng_fd));

    return NULL;
}

beeper_task_t * beeper_task_create(const char * path, const char * username, const char * password,
                                   beeper_task_event_cb_t event_cb, void * event_cb_user_data)
{
    int res;

    beeper_task_t * t = calloc(1, sizeof(beeper_task_t));
    assert(t);

    t->username = strdup(username);
    assert(t->username);
    t->password = strdup(password);
    assert(t->password);
    t->event_cb = event_cb;
    t->event_cb_user_data = event_cb_user_data;
    res = asprintf(&t->upath, "%s%s/", path, username);
    assert(res != -1);

    // queue_init();

    pthread_attr_t thread_attr;
    assert(0 == pthread_attr_init(&thread_attr));
    assert(0 == pthread_attr_setstacksize(&thread_attr, 8192));
    assert(0 == pthread_create(&t->thread, NULL, thread, t));
    assert(0 == pthread_attr_destroy(&thread_attr));

    return t;
}

void beeper_task_destroy(beeper_task_t * t)
{
    assert(0);

    // queue_push("stop");

    assert(0 == pthread_join(t->thread, NULL));

    // queue_destroy();

    free(t->upath);
    free(t->password);
    free(t->username);
    free(t);
}
