#include "beeper_task_private.h"

#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/ssl.h>

#include <netutils/cJSON.h>
#include <netutils/cJSON_Utils.h>

#include "pdjson/pdjson.h"

#include <olm/olm.h>
#include <olm/sas.h>
#include <olm/base64.h> /* this is not a public API. it could change anytime */

#include <mcp/mcp_lvgl.h>

#define ROOM_SESSION_LRU_COUNT 4
#define USER_SESSION_LRU_COUNT 4

typedef struct {
    WOLFSSL_CTX * wolfssl_ctx;
    struct addrinfo * peer;
} beeper_task_https_ctx_t;

typedef struct {
    WOLFSSL * wolfssl_ssl;
    beeper_task_https_ctx_t * ctx;
    bool blocking;
} beeper_task_https_conn_t;

typedef enum {
    BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED,
    BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED,
    BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED
} beeper_task_device_key_status_t;

typedef enum {
    BEEPER_TASK_RECEIVED_EVENT_STOP,
    BEEPER_TASK_RECEIVED_EVENT_SAS_MATCHES,
    BEEPER_TASK_RECEIVED_EVENT_REQUEST_MESSAGES,
} beeper_task_received_event_t;

typedef struct {
    char * room_id;
    char * chunk_id;
    beeper_task_direction_t direction;
} beeper_received_message_request_t;

typedef struct {
    beeper_task_received_event_t event_code;
    void * data;
} beeper_task_queue_item_t;

typedef struct {
    OlmSession * sessions[USER_SESSION_LRU_COUNT];
} user_sessions_t;

typedef struct {
    uint8_t type;
    const char * ciphertext;
    size_t ciphertext_len;
    const char * sender_key;
    size_t sender_key_len;
    char * plaintext;
} user_session_lru_cmp_user_data_t;

typedef struct {
} user_session_lru_destroy_user_data_t;

typedef struct {
    char * key_id;
    char * key_value;
} key_list_key_t;

typedef struct {
    char * device_id;
    bool was_signed_by_ssk;
    uint32_t device_key_count;
    key_list_key_t * device_keys;
} key_list_device_t;

typedef struct {
    char * user_id;
    bool outdated;
    bool dont_save;
    bool all_devices;
    bool ssk_was_signed_by_master;
    uint32_t device_count;
    uint32_t master_key_count;
    uint32_t self_signing_key_count;
    key_list_device_t * devices;
    key_list_key_t * master_keys;
    key_list_key_t * self_signing_keys;
} key_list_user_t;

typedef struct {
    char * linebuf;
    size_t n;
    FILE * f;
} key_list_init_t_;

typedef enum {
    MATRIX_EVENT_TYPE_NULL,
    MATRIX_EVENT_TYPE_M_ROOM_MEMBER,
    MATRIX_EVENT_TYPE_M_ROOM_CANONICAL_ALIAS,
    MATRIX_EVENT_TYPE_M_ROOM_NAME,
    MATRIX_EVENT_TYPE_M_ROOM_MESSAGE,
    MATRIX_EVENT_TYPE_M_ROOM_ENCRYPTED,
    MATRIX_EVENT_TYPE_M_BRIDGE,
} matrix_event_type_t;

typedef enum {
    MATRIX_JOINED_ROOM_NULL,
    MATRIX_JOINED_ROOM_TIMELINE,
    MATRIX_JOINED_ROOM_STATE,
} matrix_joined_room_t;

typedef enum {
    ROOM_NAME_TYPE_NULL,
    ROOM_NAME_TYPE_MEMBERS,
    ROOM_NAME_TYPE_CANONICAL_ALIAS,
    ROOM_NAME_TYPE_NAME,
} room_name_type_t;

typedef enum {
    ROOM_DECRYPTING_STATE_BRIDGEBOT,
    ROOM_DECRYPTING_STATE_SESSION,
} room_decrypting_state_t;

typedef struct {
    char * message_id;
    char * ciphertext;
} room_decrypting_message_t;

typedef struct {
    char * session_id;
    char * sender_key;
    room_decrypting_state_t state;
    beeper_array_t msgs;
} room_decrypting_t;

typedef struct {
    char * session_id;
    char * pickle;
    OlmInboundGroupSession * session;
    bool needs_save;
} room_session_t;

typedef struct {
    char * room_id;
    bool dir_created;
    room_name_type_t name_type;
    char * name;
    beeper_array_t members;
    beeper_array_t decrypting;
    char * bridgebot;
    room_session_t session_lru[ROOM_SESSION_LRU_COUNT];
} room_t;

typedef struct {
    beeper_task_t * t;
    room_t * room;
} room_session_lru_user_data_t;

typedef struct {
    char * user_id;
    char * display_name;
} room_member_t;

struct beeper_task_t {
    char * username;
    char * password;
    beeper_task_event_cb_t event_cb;
    void * event_cb_user_data;
    char * upath;
    beeper_queue_t queue;
    pthread_mutex_t queue_mutex;
    pthread_t thread;

    uint64_t txnid;
    uint64_t txnid_range_end;
    int rng_fd;
    beeper_task_https_ctx_t https_ctx;
    beeper_task_https_conn_t https_conn[2];
    char * user_id;
    char * auth_header;
    char * device_id;
    OlmAccount * olm_account;

    bool user_sessions_dir_created;
    bool rooms_dir_created;

    // bool key_list_got_initial_changes;
    bool key_list_has_outdated;
    bool key_list_needs_save;
    uint32_t key_list_user_count;
    key_list_user_t * key_list_users;
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

#define DEBUG 1
#if DEBUG
    #define debug(fmt, ...) do { \
        fprintf(stderr, fmt "\n", ##__VA_ARGS__); \
        fflush(stderr); \
    } while(0)
#else
    #define debug(fmt, ...)
#endif

#define STRING_LITERAL_LEN(s) (sizeof(s) - 1)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_LEN(arr) (sizeof(arr) / sizeof(*(arr)))

#define BEEPER_MATRIX_URL "matrix.beeper.com"
#define ONE_TIME_KEY_COUNT_TOPUP 15
#define ONE_TIME_KEY_COUNT_LOW_WATERMARK 10

#define TXNID_INCREMNET 0x10000ull

#define REQUEST_RETRY_COUNT 1
#define HEADERS_ALLOC_CHUNK_SZ 1000
#define DUMMY_BUF_SIZE 64
#define OK_STATUS_START "HTTP/1.1 2"
#define CONTENT_LENGTH_HEADER "\r\nContent-Length:"

#define TXNID_FMT "%016"PRIX64
#define TXNID_SIZE 16
#define SENDTODEVICE_PATH_FMT(event_type) "sendToDevice/"event_type"/"TXNID_FMT
#define SENDTODEVICE_PATH_SIZE(event_type) (STRING_LITERAL_LEN("sendToDevice/"event_type"/") + TXNID_SIZE)

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

static cJSON * unwrap_cjson(cJSON * item);
static cJSON * sign_json(beeper_task_t * t, cJSON * json);

static char * base64_encode(const char * input)
{
    size_t input_len = strlen(input);
    size_t encoded_len = _olm_encode_base64_length(input_len);
    char * encoded = beeper_asserting_malloc(encoded_len + 1);
    _olm_encode_base64((const uint8_t *) input, input_len, (uint8_t *) encoded);
    encoded[encoded_len] = '\0';
    return encoded;
}

static void base64_filename_safe(char * s)
{
    char c;
    while((c = *s)) {
        if(c == '+') *s = '-';
        else if(c == '/') *s = '_';
        s++;
    }
}

static char * txnid_make_path_(beeper_task_t * t)
{
    char * txnid_path;
    int res = asprintf(&txnid_path, "%stxnid", t->upath);
    assert(res != -1);
    return txnid_path;
}

static void txnid_range_end_update_(beeper_task_t * t, char * txnid_path)
{
    int res, fd;
    ssize_t rwres;
    char txnid_buf[TXNID_SIZE + 1];

    char * free_me = NULL;
    if(txnid_path == NULL) {
        txnid_path = free_me = txnid_make_path_(t);
    }

    t->txnid_range_end = t->txnid + TXNID_INCREMNET;

    snprintf(txnid_buf, sizeof(txnid_buf), TXNID_FMT, t->txnid_range_end);
    debug("open: '%s' CREAT TRUNC WRONLY", txnid_path);
    fd = open(txnid_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    assert(fd >= 0);
    rwres = write(fd, txnid_buf, 16);
    assert(rwres == 16);
    res = close(fd);
    assert(res == 0);

    free(free_me);
}

/* txnIds are supposed to be unique only per access token,
   but Element Web misbehaves unless it is unique per
   device id. */
static void txnid_init(beeper_task_t * t)
{
    int res, fd;
    ssize_t rwres;
    char txnid_buf[17];

    char * txnid_path = txnid_make_path_(t);

    debug("open: '%s' RDONLY", txnid_path);
    fd = open(txnid_path, O_RDONLY);
    if(fd < 0) {
        assert(errno == ENOENT);
        t->txnid = 0;
    }
    else {
        rwres = read(fd, txnid_buf, 16);
        assert(rwres == 16);
        txnid_buf[16] = '\0';
        res = close(fd);
        assert(res == 0);
        t->txnid = strtoull(txnid_buf, NULL, 16);
    }

    txnid_range_end_update_(t, txnid_path);

    free(txnid_path);
}

static uint64_t txnid_next(beeper_task_t * t)
{
    if(t->txnid == t->txnid_range_end) {
        txnid_range_end_update_(t, NULL);
    }

    return t->txnid++;
}

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
    debug("open: '%s' WRONLY CREAT TRUNC", account_pickle_path);
    int fd = open(account_pickle_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd != -1);
    free(account_pickle_path);
    ssize_t bw = write(fd, account_pickle, account_pickle_length);
    assert(account_pickle_length == bw);
    res = close(fd);
    assert(res == 0);
    free(account_pickle);
}

static cJSON * get_n_one_time_keys(beeper_task_t * t, int n)
{
    size_t otks_buf_len = olm_account_one_time_keys_length(t->olm_account);
    char * otks_buf = malloc(otks_buf_len);
    assert(otks_buf);
    size_t olm_res = olm_account_one_time_keys(t->olm_account, otks_buf, otks_buf_len);
    assert(olm_res == otks_buf_len);

    cJSON * otks_json = cJSON_ParseWithLength(otks_buf, otks_buf_len);
    free(otks_buf);
    cJSON * curve_obj = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(otks_json, "curve25519"));
    int otks_n = cJSON_GetArraySize(curve_obj);
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
        otks_buf = malloc(otks_buf_len);
        assert(otks_buf);
        olm_res = olm_account_one_time_keys(t->olm_account, otks_buf, otks_buf_len);
        assert(olm_res == otks_buf_len);

        otks_json = cJSON_ParseWithLength(otks_buf, otks_buf_len);
        free(otks_buf);
        curve_obj = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(otks_json, "curve25519"));
        otks_n = cJSON_GetArraySize(curve_obj);
        assert(otks_n == n);
    }

    cJSON * ret_json = unwrap_cjson(cJSON_CreateObject());
    cJSON * one_time_keys = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(ret_json, "one_time_keys", one_time_keys);

    cJSON * item;
    while((item = curve_obj->child)) {
        cJSON * otk_entry = unwrap_cjson(cJSON_CreateObject());

        char * entry_name;
        assert(-1 != asprintf(&entry_name, "signed_curve25519:%s", item->string));
        assert(cJSON_AddItemToObject(one_time_keys, entry_name, otk_entry));
        free(entry_name);

        cJSON_AddItemToObjectCS(otk_entry, "key", cJSON_DetachItemViaPointer(curve_obj, item));

        cJSON_AddItemToObjectCS(otk_entry, "signatures", sign_json(t, otk_entry));
    }

    cJSON_Delete(otks_json);

    size_t fallback_key_length = olm_account_unpublished_fallback_key_length(t->olm_account);
    char * fallback_key = malloc(fallback_key_length);
    assert(fallback_key);
    olm_res = olm_account_unpublished_fallback_key(t->olm_account, fallback_key, fallback_key_length);
    assert(olm_res == fallback_key_length);
    cJSON * fallback_key_json = cJSON_ParseWithLength(fallback_key, fallback_key_length);
    free(fallback_key);
    cJSON * fallback_curve = cJSON_GetObjectItemCaseSensitive(fallback_key_json, "curve25519");
    if(fallback_curve->child == NULL) {
        cJSON_Delete(fallback_key_json);

        size_t random_length = olm_account_generate_fallback_key_random_length(t->olm_account);
        void * random = gen_random(t->rng_fd, random_length);
        olm_res = olm_account_generate_fallback_key(t->olm_account, random, random_length);
        assert(olm_res == 1);
        free(random);

        fallback_key_length = olm_account_unpublished_fallback_key_length(t->olm_account);
        fallback_key = malloc(fallback_key_length);
        assert(fallback_key);
        olm_res = olm_account_unpublished_fallback_key(t->olm_account, fallback_key, fallback_key_length);
        assert(olm_res == fallback_key_length);
        fallback_key_json = cJSON_ParseWithLength(fallback_key, fallback_key_length);
        free(fallback_key);
        fallback_curve = cJSON_GetObjectItemCaseSensitive(fallback_key_json, "curve25519");
        assert(fallback_curve->child != NULL);
    }
    assert(fallback_curve->child->next == NULL);

    cJSON * fallback_keys = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(ret_json, "fallback_keys", fallback_keys);

    cJSON * fallback_key_entry = unwrap_cjson(cJSON_CreateObject());
    char * entry_name;
    assert(-1 != asprintf(&entry_name, "signed_curve25519:%s", fallback_curve->child->string));
    assert(cJSON_AddItemToObject(fallback_keys, entry_name, fallback_key_entry));
    free(entry_name);

    cJSON_AddItemToObjectCS(fallback_key_entry, "fallback", unwrap_cjson(cJSON_CreateTrue()));

    cJSON_AddItemToObjectCS(fallback_key_entry, "key", cJSON_DetachItemViaPointer(fallback_curve, fallback_curve->child));

    cJSON_AddItemToObjectCS(fallback_key_entry, "signatures", sign_json(t, fallback_key_entry));

    cJSON_Delete(fallback_key_json);

    pickle_account(t);

    return ret_json;
}

static cJSON * unwrap_cjson(cJSON * item)
{
    assert(item);
    return item;
}

static double unwrap_number(cJSON * item)
{
    assert(cJSON_IsNumber(item));
    return cJSON_GetNumberValue(item);
}

static bool cjson_array_has_string(cJSON * array, const char * string)
{
    cJSON * child;
    cJSON_ArrayForEach(child, array) {
        char * child_str_val = cJSON_GetStringValue(child);
        assert(child_str_val);
        if(0 == strcmp(child_str_val, string)) {
            return true;
        }
    }
    return false;
}

static void https_ctx_init(beeper_task_https_ctx_t * ctx)
{
    assert(wolfSSL_Init() == SSL_SUCCESS);

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
    conn->ctx = ctx;
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
    int res;
    int fd = https_fd(conn);
    https_set_blocking(conn, true);
    res = wolfSSL_shutdown(conn->wolfssl_ssl);
    assert(res == SSL_SUCCESS || res == SSL_SHUTDOWN_NOT_DONE);
    wolfSSL_free(conn->wolfssl_ssl);
    assert(0 == shutdown(fd, SHUT_RDWR));
    assert(0 == close(fd));
}

static bool read_all_wolfssl(WOLFSSL * wolfssl_ssl, void * dst, int sz, bool allow_failure)
{
    while(sz) {
        int res = wolfSSL_read(wolfssl_ssl, dst, sz);
        if(res <= 0) {
            assert(allow_failure);
            return false;
        }
        dst += res;
        sz -= res;
    }
    return true;
}

static void dummy_read(WOLFSSL * wolfssl_ssl, int len)
{
    for( ; len > 0; len -= DUMMY_BUF_SIZE) {
        uint8_t dummy_buf[DUMMY_BUF_SIZE];
        read_all_wolfssl(wolfssl_ssl, dummy_buf, len < DUMMY_BUF_SIZE ? len : DUMMY_BUF_SIZE, false);
    }
}

static void request_send(beeper_task_https_conn_t * conn, const char * method, const char * path,
                         const char * extra_headers, const char * json_str)
{
    int res;

    debug("%s %s %s", method, path, json_str ? json_str : "(no body)");

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
    read_all_wolfssl(conn->wolfssl_ssl, chunk_head_buf, 3, false);
    int chunk_head_len = 3;
    while(!(chunk_head_buf[chunk_head_len - 2] == '\r'
            && chunk_head_buf[chunk_head_len - 1] == '\n')) {
        assert(chunk_head_len != 10);
        read_all_wolfssl(conn->wolfssl_ssl, &chunk_head_buf[chunk_head_len], 1, false);
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
    dummy_read(conn->wolfssl_ssl, 2); /* skip trailing \r\n */
}

static bool request_recv(beeper_task_https_conn_t * conn, int * resp_len_out,
                         bool blocking, int * nonblocking_status_out,
                         bool read_resp_now, char ** resp_out,
                         bool allow_failure, bool * failed_p)
{
    int res;

    if(failed_p) *failed_p = false;

    char first_byte;
    if(!blocking) {
        https_set_blocking(conn, false);
        res = wolfSSL_read(conn->wolfssl_ssl, &first_byte, 1);
        if(res <= 0) {
            res = wolfSSL_get_error(conn->wolfssl_ssl, res);
            if(res == SSL_ERROR_WANT_READ || res == SSL_ERROR_WANT_WRITE) {
                *nonblocking_status_out = res;
                return false;
            }
            if(allow_failure) {
                if(failed_p) *failed_p = true;
                return false;
            }
            assert(0);
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
    bool read_succeeded = read_all_wolfssl(conn->wolfssl_ssl, &head[head_len], 4 - head_len, true);
    if(!read_succeeded) {
        assert(allow_failure);
        if(failed_p) *failed_p = true;
        free(head);
        return false;
    }
    head_len = 4;
    while(0 != memcmp(head + (head_len - 4), "\r\n\r\n", 4)) {
        if(head_len == head_cap) {
            head_cap += HEADERS_ALLOC_CHUNK_SZ;
            head = realloc(head, head_cap);
            assert(head);
        }
        read_all_wolfssl(conn->wolfssl_ssl, &head[head_len], 1, false);
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
        if(resp_len_out) *resp_len_out = 0;
        if(resp_out) *resp_out = NULL;
        return true;
    }

    if(read_resp_now) {
        char * resp;
        if(content_len != -1) {
            if(resp_out) {
                resp = beeper_asserting_malloc(content_len + 1);
                read_all_wolfssl(conn->wolfssl_ssl, resp, content_len, false);
            }
            else {
                dummy_read(conn->wolfssl_ssl, content_len);
            }
        }
        else {
            content_len = 0;
            if(resp_out) {
                resp = beeper_asserting_malloc(1);
            }
            int chunk_sz;
            do {
                chunk_sz = request_read_chunk_size(conn);
                if(chunk_sz) {
                    if(resp_out) {
                        resp = beeper_asserting_realloc(resp, content_len + chunk_sz + 1);
                        read_all_wolfssl(conn->wolfssl_ssl, &resp[content_len], chunk_sz, false);
                    }
                    else {
                        dummy_read(conn->wolfssl_ssl, chunk_sz);
                    }
                    content_len += chunk_sz;
                }
                request_skip_chunk_trailer(conn);
            } while(chunk_sz);
        }
        if(resp_out) {
            resp[content_len] = '\0';
            *resp_out = resp;
        }
    }

    if(resp_len_out) *resp_len_out = content_len;
    return true;
}

static char * request(beeper_task_https_conn_t * conn, const char * method, const char * path,
                      const char * extra_headers, const char * json_str, bool save_response)
{
    int tries_left = REQUEST_RETRY_COUNT;
    while(1) {
        request_send(conn, method, path, extra_headers, json_str);
        char * resp = NULL;
        bool read_succeeded = request_recv(conn, NULL, true, NULL, true, save_response ? &resp : NULL, true, NULL);
        if(!read_succeeded) {
            assert(tries_left);
            debug("retrying request %d more time(s)", tries_left);
            tries_left -= 1;
            beeper_task_https_ctx_t * conn_ctx = conn->ctx;
            https_conn_deinit(conn);
            https_conn_init(conn_ctx, conn);
            continue;
        }
        return resp;
    }
}

static void request_recv_more(beeper_task_https_conn_t * conn, char * dst, int len)
{
    https_set_blocking(conn, true);
    read_all_wolfssl(conn->wolfssl_ssl, dst, len, false);
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
    /* unfortunately we need to deep-copy the json until
       https://github.com/DaveGamble/cJSON/pull/908 is merged.
       Items cannot be added to an object/array after it's been sorted.
     */
    cJSON * json_copy = unwrap_cjson(cJSON_Duplicate(json, true));

    recursively_sort_json_objects(json_copy);
    char * stringified = cJSON_PrintUnformatted(json_copy);
    assert(stringified);

    cJSON_Delete(json_copy);

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

static bool verify_signature(OlmUtility * olm_verify, cJSON * to_verify,
                             const char * user_id, const key_list_key_t * key_pool, uint32_t key_pool_len)
{
    cJSON * sigs = cJSON_GetObjectItemCaseSensitive(to_verify, "signatures");
    cJSON * user_sigs = cJSON_GetObjectItemCaseSensitive(sigs, user_id);
    if(!user_sigs) {
        return false;
    }

    char * supposed_signature = NULL;
    char * key;
    for(uint32_t i = 0; i < key_pool_len; i++) {
        cJSON * user_sig = cJSON_GetObjectItemCaseSensitive(user_sigs, key_pool[i].key_id);
        if(user_sig) {
            supposed_signature = cJSON_GetStringValue(user_sig);
            assert(supposed_signature);
            key = key_pool[i].key_value;
            break;
        }
    }
    if(!supposed_signature) {
        return false;
    }

    cJSON_DetachItemViaPointer(to_verify, sigs);
    cJSON * unsigned_ = cJSON_DetachItemFromObjectCaseSensitive(to_verify, "unsigned");

    char * stringified = canonical_json(to_verify);

    void * free_me = NULL;
    if(!olm_verify) {
        olm_verify = free_me = beeper_asserting_malloc(olm_utility_size());
    }
    olm_utility(olm_verify);

    size_t olm_verify_res = olm_ed25519_verify(olm_verify,
                                               key, strlen(key),
                                               stringified, strlen(stringified),
                                               supposed_signature, strlen(supposed_signature));
    bool ret = olm_verify_res == 0;

    olm_clear_utility(olm_verify);
    free(free_me);

    free(stringified);

    cJSON_AddItemToObjectCS(to_verify, "signatures", sigs);
    cJSON_AddItemToObjectCS(to_verify, "unsigned", unsigned_);

    return ret;
}

static char * user_sessions_make_dir_and_path(beeper_task_t * t, const char * user_id)
{
    int res;

    char * encoded_user_id = base64_encode(user_id);
    base64_filename_safe(encoded_user_id);

    char * path;
    res = asprintf(&path, "%susers/%s", t->upath, encoded_user_id);
    assert(res > 0);

    free(encoded_user_id);

    if(!t->user_sessions_dir_created) {
        t->user_sessions_dir_created = true;

        char * last_sep = strrchr(path, '/');
        *last_sep = '\0';

        res = mkdir(path, 0755);
        assert(res == 0 || errno == EEXIST);

        *last_sep = '/';
    }

    return path;
}

static void user_sessions_load(user_sessions_t * sessions, beeper_task_t * t, const char * user_id)
{
    int res;
    ssize_t rwres;
    size_t olmres;

    memset(sessions, 0, sizeof(*sessions));

    char * path = user_sessions_make_dir_and_path(t, user_id);
    debug("fopen: '%s' r", path);
    FILE * f = fopen(path, "r");
    free(path);

    if(!f) {
        assert(errno == ENOENT);
        return;
    }

    char * lineptr = NULL;
    size_t n = 0;

    for(size_t i = 0; i < ARRAY_LEN(sessions->sessions); i++) {
        errno = 0;
        rwres = getline(&lineptr, &n, f);
        if(rwres < 0) {
            assert(errno == 0);
            break;
        }

        assert(rwres >= 2);
        size_t line_len = rwres - 1;
        assert(lineptr[line_len] == '\n');

        sessions->sessions[i] = beeper_asserting_malloc(olm_session_size());
        olm_session(sessions->sessions[i]);
        /* lineptr contents are "destroyed" */
        olmres = olm_unpickle_session(sessions->sessions[i], NULL, 0, lineptr, line_len);
        assert(olmres != olm_error());
    }

    free(lineptr);

    res = fclose(f);
    assert(res == 0);
}

static void user_sessions_save(user_sessions_t * sessions, beeper_task_t * t, const char * user_id)
{
    int res;
    size_t olmres;

    char * path = user_sessions_make_dir_and_path(t, user_id);
    debug("fopen: '%s' w", path);
    FILE * f = fopen(path, "w");
    assert(f);
    free(path);

    void * pickle_buf = NULL;
    size_t pickle_buf_size = 0;

    for(size_t i = 0; i < ARRAY_LEN(sessions->sessions); i++) {
        if(sessions->sessions[i] == NULL) {
            break;
        }

        size_t pickle_size = olm_pickle_session_length(sessions->sessions[i]);
        assert(pickle_size != olm_error()); /* sanity */
        if(pickle_size > pickle_buf_size) {
            pickle_buf_size = pickle_size;
            free(pickle_buf);
            pickle_buf = beeper_asserting_malloc(pickle_buf_size);
        }

        olmres = olm_pickle_session(sessions->sessions[i], NULL, 0, pickle_buf, pickle_buf_size);
        assert(olmres == pickle_size);

        size_t fwriteres = fwrite(pickle_buf, 1, pickle_size, f);
        assert(fwriteres == pickle_size);
        res = putc('\n', f);
        assert(res != EOF);
    }

    free(pickle_buf);

    res = fclose(f);
    assert(res == 0);
}

static void user_sessions_destroy(user_sessions_t * sessions)
{
    for(size_t i = 0; i < ARRAY_LEN(sessions->sessions); i++) {
        if(sessions->sessions[i] == NULL) {
            break;
        }

        olm_clear_session(sessions->sessions[i]);
        free(sessions->sessions[i]);
    }
}

static char * user_session_decrypt(OlmSession * session, uint8_t type, const char * ciphertext, size_t ciphertext_len)
{
    char * ciphertext_buf = beeper_asserting_malloc(ciphertext_len); /* olm overwrites the input. */

    memcpy(ciphertext_buf, ciphertext, ciphertext_len);
    size_t plaintext_max_len = olm_decrypt_max_plaintext_length(session, type, ciphertext_buf, ciphertext_len);
    if(plaintext_max_len == olm_error()) {
        free(ciphertext_buf);
        return NULL;
    }

    char * plaintext = beeper_asserting_malloc(plaintext_max_len + 1);

    memcpy(ciphertext_buf, ciphertext, ciphertext_len);
    size_t plaintext_len = olm_decrypt(session, type, ciphertext_buf, ciphertext_len, plaintext, plaintext_max_len);
    free(ciphertext_buf);
    if(plaintext_len == olm_error()) {
        free(plaintext);
        return NULL;
    }
    assert(plaintext_len <= plaintext_max_len);

    plaintext[plaintext_len] = '\0';

    if(plaintext_len < plaintext_max_len) {
        plaintext = beeper_asserting_realloc(plaintext, plaintext_len + 1);
    }

    return plaintext;
}

static void user_session_destroy(void * session_v, void * user_data)
{
    OlmSession * session = *(OlmSession **)session_v;

    if(session == NULL) {
        return;
    }

    olm_clear_session(session);
    free(session);
}

static bool user_session_cmp(void * session_v, void * user_data)
{
    OlmSession * session = *(OlmSession **)session_v;
    user_session_lru_cmp_user_data_t * ud = user_data;

    assert(ud->plaintext == NULL);

    if(session == NULL) {
        return false;
    }

    if(ud->type == 0) {
        char * ciphertext_buf = beeper_asserting_malloc(ud->ciphertext_len);
        memcpy(ciphertext_buf, ud->ciphertext, ud->ciphertext_len); /* olm_matches_inbound_session_from destroys the ciphertext */
        size_t olmres = olm_matches_inbound_session_from(session, ud->sender_key, ud->sender_key_len, ciphertext_buf, ud->ciphertext_len);
        free(ciphertext_buf);
        if(olmres != 1) {
            return false;
        }
    }

    ud->plaintext = user_session_decrypt(session, ud->type, ud->ciphertext, ud->ciphertext_len);

    return ud->plaintext != NULL;
}

static const beeper_lru_class_t user_session_lru_class = {
    .capacity = USER_SESSION_LRU_COUNT,
    .item_size = sizeof(OlmSession *),
    .destroy = user_session_destroy,
    .cmp = user_session_cmp,
};

static cJSON * decrypt(beeper_task_t * t, const char * ciphertext, uint8_t type, const char * user_id,
                       const char * sender_key)
{
    size_t olmres;

    user_sessions_t sessions;
    user_sessions_load(&sessions, t, user_id);

    size_t ciphertext_len = strlen(ciphertext);
    size_t sender_key_len = strlen(sender_key);

    user_session_lru_cmp_user_data_t cmp_data = {
        .type = type,
        .ciphertext = ciphertext,
        .ciphertext_len = ciphertext_len,
        .sender_key = sender_key,
        .sender_key_len = sender_key_len,
        .plaintext = NULL,
    };
    OlmSession ** session_p = beeper_lru_get(&user_session_lru_class, sessions.sessions, &cmp_data);
    OlmSession * session = session_p ? *session_p : NULL;
    char * plaintext = cmp_data.plaintext;
    assert((session != NULL) == (plaintext != NULL));

    bool remove_otk = false;

    if(plaintext == NULL) {
        if(type == 0) {
            session = beeper_asserting_malloc(olm_session_size());
            olm_session(session);
            char * ciphertext_buf = beeper_asserting_malloc(ciphertext_len);
            memcpy(ciphertext_buf, ciphertext, ciphertext_len); /* olm_create_inbound_session_from destroys the ciphertext */
            olmres = olm_create_inbound_session_from(session, t->olm_account, sender_key, sender_key_len, ciphertext_buf, ciphertext_len);
            free(ciphertext_buf);
            if(olmres == olm_error()) {
                free(session);
                user_sessions_destroy(&sessions);
                return NULL;
            }
            plaintext = user_session_decrypt(session, type, ciphertext, ciphertext_len);
            if(plaintext == NULL) {
                free(session);
                user_sessions_destroy(&sessions);
                return NULL;
            }
            beeper_lru_add_unchecked(&user_session_lru_class, sessions.sessions, &session, NULL);
            remove_otk = true;
        }
        else {
            user_sessions_destroy(&sessions);
            return NULL;
        }
    }

    user_sessions_save(&sessions, t, user_id);

    if(remove_otk) {
        olmres = olm_remove_one_time_keys(t->olm_account, session);
        if(olmres == olm_error()) {
            assert(olm_account_last_error_code(t->olm_account) == OLM_BAD_MESSAGE_KEY_ID);
        }
        else {
            /* can have a false-positive where the key was the fallback key and the
               account is unchanged. Save the account anyways.
             */
            pickle_account(t);
        }
    }

    cJSON * retval = unwrap_cjson(cJSON_Parse(plaintext));
    free(plaintext);

    user_sessions_destroy(&sessions);
    return retval;
}

static char * key_list_make_path_(beeper_task_t * t)
{
    char * key_list_path;
    int res = asprintf(&key_list_path, "%skey_list", t->upath);
    assert(res >= 0);
    return key_list_path;
}

static void key_list_key_free_array_(key_list_key_t * keys, uint32_t length) {
    for(uint32_t i = 0; i < length; i++) {
        free(keys[i].key_id);
        free(keys[i].key_value);
    }
    free(keys);
}

static void key_list_devices_free_(key_list_user_t * user)
{
    for(uint32_t i = 0; i < user->device_count; i++) {
        free(user->devices[i].device_id);
        key_list_key_free_array_(user->devices[i].device_keys, user->devices[i].device_key_count);
    }
    free(user->devices);
}

static void key_list_user_free_(key_list_user_t * user)
{
    free(user->user_id);
    key_list_devices_free_(user);
    key_list_key_free_array_(user->master_keys, user->master_key_count);
    key_list_key_free_array_(user->self_signing_keys, user->self_signing_key_count);
}

static char * key_list_init_getline_helper_(key_list_init_t_ * dat)
{
    ssize_t res;
    assert(1 < (res = getline(&dat->linebuf, &dat->n, dat->f)));
    dat->linebuf[res - 1] = '\0'; /* replace the \n with \0 */
    char * ret = strdup(dat->linebuf);
    assert(ret);
    return ret;
}

static uint32_t key_list_init_scan_helper_(FILE * f)
{
    uint32_t num;
    assert(1 == fscanf(f, "%"SCNu32"\n", &num));
    return num;
}

static void key_list_init_read_keys_helper_(key_list_key_t ** keys, uint32_t * count, key_list_init_t_ * dat)
{
    *count = key_list_init_scan_helper_(dat->f);
    if(*count) *keys = beeper_asserting_malloc(*count * sizeof(**keys));
    for(uint32_t i = 0; i < *count; i++) {
        key_list_key_t * key = &(*keys)[i];
        key->key_id = key_list_init_getline_helper_(dat);
        key->key_value = key_list_init_getline_helper_(dat);
    }
}

static void key_list_init(beeper_task_t * t)
{
    char * path = key_list_make_path_(t);
    debug("fopen: '%s' r", path);
    FILE * f = fopen(path, "r");
    free(path);
    if(!f) {
        assert(errno == ENOENT);
        // t->key_list_got_initial_changes = true;
        return;
    }

    key_list_init_t_ dat = {
        .linebuf = NULL,
        .n = 0,
        .f = f
    };

    t->key_list_user_count = key_list_init_scan_helper_(f);
    if(t->key_list_user_count) t->key_list_users = beeper_asserting_calloc(t->key_list_user_count, sizeof(*t->key_list_users));
    for(uint32_t i = 0; i < t->key_list_user_count; i++) {
        key_list_user_t * user = &t->key_list_users[i];
        user->user_id = key_list_init_getline_helper_(&dat);
        user->all_devices = key_list_init_scan_helper_(f);
        user->ssk_was_signed_by_master = key_list_init_scan_helper_(f);
        user->device_count = key_list_init_scan_helper_(f);
        if(user->device_count) user->devices = beeper_asserting_calloc(user->device_count, sizeof(*user->devices));
        for(uint32_t j = 0; j < user->device_count; j++) {
            key_list_device_t * device = &user->devices[j];
            device->device_id = key_list_init_getline_helper_(&dat);
            device->was_signed_by_ssk = key_list_init_scan_helper_(f);
            key_list_init_read_keys_helper_(&device->device_keys, &device->device_key_count, &dat);
        }
        key_list_init_read_keys_helper_(&user->master_keys, &user->master_key_count, &dat);
        key_list_init_read_keys_helper_(&user->self_signing_keys, &user->self_signing_key_count, &dat);
    }

    free(dat.linebuf);
    assert(0 == fclose(f));
}

static void key_list_update_helper_(cJSON * keys_json, key_list_key_t ** res, uint32_t * res_len)
{
    uint32_t arr_len = cJSON_GetArraySize(keys_json);
    key_list_key_t * arr = arr_len ? beeper_asserting_malloc(arr_len * sizeof(*arr)) : NULL;
    assert(cJSON_IsObject(keys_json));
    uint32_t j = 0;
    cJSON * key_json;
    cJSON_ArrayForEach(key_json, keys_json) {
        arr[j].key_id = beeper_asserting_strdup(key_json->string);
        char * strval = cJSON_GetStringValue(key_json);
        assert(strval);
        arr[j].key_value = beeper_asserting_strdup(strval);

        j++;
    }

    *res = arr;
    *res_len = arr_len;
}

static void key_list_update_(beeper_task_t * t)
{
    if(!t->key_list_has_outdated) {
        return;
    }
    t->key_list_has_outdated = false;

    cJSON * req_json = unwrap_cjson(cJSON_CreateObject());
    cJSON * req_json_device_keys = unwrap_cjson(cJSON_CreateObject());
    cJSON * req_json_timeout = unwrap_cjson(cJSON_CreateNumber(10000));
    cJSON_AddItemToObjectCS(req_json, "device_keys", req_json_device_keys);
    cJSON_AddItemToObjectCS(req_json, "timeout", req_json_timeout);

    for(uint32_t i = 0; i < t->key_list_user_count; i++)
    {
        key_list_user_t * user = &t->key_list_users[i];

        if(!user->outdated) {
            continue;
        }

        cJSON * device_array = unwrap_cjson(cJSON_CreateArray());
        cJSON_AddItemToObjectCS(req_json_device_keys, user->user_id, device_array);

        if(!user->all_devices) {
            assert(user->device_count > 0);
            for(uint32_t j = 0; j < user->device_count; j++) {
                cJSON_AddItemToArray(device_array, unwrap_cjson(cJSON_CreateStringReference(user->devices[j].device_id)));
            }
        }
    }

    char * req_json_str = cJSON_PrintUnformatted(req_json);
    assert(req_json_str);
    cJSON_Delete(req_json);

    char * resp = request(&t->https_conn[0], "POST", "keys/query", t->auth_header, req_json_str, true);
    free(req_json_str);
    cJSON * resp_json = unwrap_cjson(cJSON_Parse(resp));
    free(resp);

    cJSON * device_keys_json = cJSON_GetObjectItemCaseSensitive(resp_json, "device_keys");
    cJSON * master_keys_json = cJSON_GetObjectItemCaseSensitive(resp_json, "master_keys");
    cJSON * self_signing_keys_json = cJSON_GetObjectItemCaseSensitive(resp_json, "self_signing_keys");

    OlmUtility * olm_verify = NULL;
    for(uint32_t i = 0; i < t->key_list_user_count; i++)
    {
        key_list_user_t * user = &t->key_list_users[i];

        if(!user->outdated) {
            continue;
        }
        user->outdated = false;

        bool had_something = false;

        key_list_key_free_array_(user->master_keys, user->master_key_count);
        user->master_key_count = 0;
        user->master_keys = NULL;
        cJSON * msk_keys_json = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(master_keys_json, user->user_id), "keys");
        if(msk_keys_json) {
            had_something = true;
            key_list_update_helper_(msk_keys_json, &user->master_keys, &user->master_key_count);
        }

        key_list_key_free_array_(user->self_signing_keys, user->self_signing_key_count);
        user->self_signing_key_count = 0;
        user->self_signing_keys = NULL;
        user->ssk_was_signed_by_master = false;
        cJSON * ssk_json = cJSON_GetObjectItemCaseSensitive(self_signing_keys_json, user->user_id);
        cJSON * ssk_keys_json = cJSON_GetObjectItemCaseSensitive(ssk_json, "keys");
        if(ssk_keys_json) {
            key_list_update_helper_(ssk_keys_json, &user->self_signing_keys, &user->self_signing_key_count);
            if(user->self_signing_key_count) {
                had_something = true;
                if(!olm_verify) olm_verify = beeper_asserting_malloc(olm_utility_size());
                user->ssk_was_signed_by_master = verify_signature(olm_verify, ssk_json, user->user_id, user->master_keys, user->master_key_count);
            }
        }

        cJSON * dk_json = cJSON_GetObjectItemCaseSensitive(device_keys_json, user->user_id);

        if(user->all_devices) {
            key_list_devices_free_(user);
            user->device_count = cJSON_GetArraySize(dk_json);
            user->devices = user->device_count ? beeper_asserting_calloc(user->device_count, sizeof(*user->devices)) : NULL;

            uint32_t j = 0;
            cJSON * device_json;
            assert(cJSON_IsObject(dk_json));
            cJSON_ArrayForEach(device_json, dk_json) {
                user->devices[j].device_id = beeper_asserting_strdup(device_json->string);
                j++;
            }
        }

        for(uint32_t j = 0; j < user->device_count; j++) {
            key_list_device_t * device = &user->devices[j];
            cJSON * device_json = cJSON_GetObjectItemCaseSensitive(dk_json, device->device_id);
            cJSON * device_keys_json = cJSON_GetObjectItemCaseSensitive(device_json, "keys");
            if(device_json) {
                key_list_key_free_array_(device->device_keys, device->device_key_count);
                device->device_key_count = 0;
                device->device_keys = NULL;
                device->was_signed_by_ssk = false;
            }
            if(!device_keys_json) {
                if(device->device_key_count) {
                    had_something = true;
                }
                continue;
            }
            key_list_update_helper_(device_keys_json, &device->device_keys, &device->device_key_count);
            if(device->device_key_count) {
                had_something = true;
                if(!olm_verify) olm_verify = beeper_asserting_malloc(olm_utility_size());
                device->was_signed_by_ssk = verify_signature(olm_verify, device_json, user->user_id, user->self_signing_keys, user->self_signing_key_count);
            }
        }

        user->dont_save = !had_something;
        if(had_something) {
            t->key_list_needs_save = true;
        }
    }
    free(olm_verify);

    cJSON_Delete(resp_json);
}

static void key_list_save_writestr_helper_(const char * s, FILE * f)
{
    assert(EOF != fputs(s, f));
    assert(EOF != putc('\n', f));
}

static void key_list_save_writeu32_helper_(uint32_t u, FILE * f)
{
    assert(0 <= fprintf(f, "%"PRIu32"\n", u));
}

static void key_list_save_writekeys_helper_(key_list_key_t * keys, uint32_t count, FILE * f)
{
    key_list_save_writeu32_helper_(count, f);
    for(uint32_t i = 0; i < count; i++) {
        key_list_key_t * key = &keys[i];
        key_list_save_writestr_helper_(key->key_id, f);
        key_list_save_writestr_helper_(key->key_value, f);
    }
}

static void key_list_save(beeper_task_t * t)
{
    key_list_update_(t);

    if(!t->key_list_needs_save) {
        return;
    }
    t->key_list_needs_save = false;

    debug("saving key list");

    char * path = key_list_make_path_(t);
    debug("fopen: '%s' w", path);
    FILE * f = fopen(path, "w");
    assert(f);
    free(path);

    uint32_t saving_user_count = 0;
    for(uint32_t i = 0; i < t->key_list_user_count; i++)
        saving_user_count += !t->key_list_users[i].dont_save;
    key_list_save_writeu32_helper_(saving_user_count, f);
    for(uint32_t i = 0; i < t->key_list_user_count; i++) {
        key_list_user_t * user = &t->key_list_users[i];
        if(user->dont_save)
            continue;
        key_list_save_writestr_helper_(user->user_id, f);
        key_list_save_writeu32_helper_(user->all_devices, f);
        key_list_save_writeu32_helper_(user->ssk_was_signed_by_master, f);
        uint32_t saving_device_count = 0;
        for(uint32_t j = 0; j < user->device_count; j++)
            saving_device_count += 0 < user->devices[j].device_key_count;
        key_list_save_writeu32_helper_(saving_device_count, f);
        for(uint32_t j = 0; j < user->device_count; j++) {
            key_list_device_t * device = &user->devices[j];
            if(!device->device_key_count)
                continue;
            key_list_save_writestr_helper_(device->device_id, f);
            key_list_save_writeu32_helper_(device->was_signed_by_ssk, f);
            key_list_save_writekeys_helper_(device->device_keys, device->device_key_count, f);
        }
        key_list_save_writekeys_helper_(user->master_keys, user->master_key_count, f);
        key_list_save_writekeys_helper_(user->self_signing_keys, user->self_signing_key_count, f);
    }

    assert(0 == fclose(f));
}

static bool key_list_should_save(beeper_task_t * t)
{
    return t->key_list_has_outdated || t->key_list_needs_save;
}

static void key_list_destroy(beeper_task_t * t)
{
    key_list_save(t);
    for(uint32_t i = 0; i < t->key_list_user_count; i++) {
        key_list_user_free_(&t->key_list_users[i]);
    }
    free(t->key_list_users);
}

static key_list_device_t * key_list_user_device_get(beeper_task_t * t, key_list_user_t * user, const char * device_id)
{
    key_list_device_t * device = NULL;

    for(uint32_t i = 0; i < user->device_count; i++) {
        if(0 == strcmp(user->devices[i].device_id, device_id)) {
            device = &user->devices[i];
            break;
        }
    }

    if(!device) {
        user->devices = beeper_asserting_realloc(user->devices, ++user->device_count * sizeof(*user->devices));
        device = &user->devices[user->device_count - 1];
        memset(device, 0, sizeof(*device));
        device->device_id = beeper_asserting_strdup(device_id);
        user->outdated = true;
        t->key_list_has_outdated = true;
    }

    if(user->outdated) {
        key_list_update_(t);
    }

    return device;
}

static key_list_user_t * key_list_user_get_(beeper_task_t * t, const char * user_id)
{
    uint32_t user_count = t->key_list_user_count;
    key_list_user_t * users = t->key_list_users;
    for(uint32_t i = 0; i < user_count; i++) {
        key_list_user_t * user = &users[i];
        if(0 == strcmp(user->user_id, user_id)) {
            return user;
        }
    }

    t->key_list_users = beeper_asserting_realloc(t->key_list_users, ++t->key_list_user_count * sizeof(*t->key_list_users));

    key_list_user_t * user = &t->key_list_users[t->key_list_user_count - 1];

    memset(user, 0, sizeof(*user));
    user->user_id = beeper_asserting_strdup(user_id);
    user->outdated = true;
    t->key_list_has_outdated = true;

    return user;
}

static void key_list_device_get(beeper_task_t * t, const char * user_id, key_list_user_t ** user_dst,
                                                   const char * device_id, key_list_device_t ** device_dst)
{
    key_list_user_t * user = key_list_user_get_(t, user_id);
    if(user_dst) *user_dst = user;
    key_list_device_t * device = key_list_user_device_get(t, user, device_id);
    if(device_dst) *device_dst = device;
}

static void key_list_user_device_get_all(beeper_task_t * t, key_list_user_t * user)
{
    if(!user->all_devices) {
        user->all_devices = true;
        user->outdated = true;
        t->key_list_has_outdated = true;
    }

    if(user->outdated) {
        key_list_update_(t);
    }
}

static key_list_user_t * key_list_device_get_all(beeper_task_t * t, const char * user_id)
{
    key_list_user_t * user = key_list_user_get_(t, user_id);
    key_list_user_device_get_all(t, user);
    return user;
}

static void key_list_apply_devicelists(beeper_task_t * t, cJSON * devicelists)
{
    cJSON * changed = cJSON_GetObjectItemCaseSensitive(devicelists, "changed");
    cJSON * left = cJSON_GetObjectItemCaseSensitive(devicelists, "left");
    assert(!changed || cJSON_IsArray(changed));
    assert(!left || cJSON_IsArray(left));

    if(changed || left) {
        uint32_t original_user_count = t->key_list_user_count;

        for(uint32_t i = 0; i < t->key_list_user_count; i++) {
            if(left && cjson_array_has_string(left, t->key_list_users[i].user_id)) {
                key_list_user_free_(&t->key_list_users[i]);
                /* this slot is now a hole. fill it with the member at the end. */
                /* repeat this value of `i` in the next loop iteration */
                t->key_list_users[i] = t->key_list_users[t->key_list_user_count - 1];
                t->key_list_user_count--;
                i--;
                t->key_list_needs_save = true;
            }
            else if(changed && cjson_array_has_string(changed, t->key_list_users[i].user_id)) {
                t->key_list_users[i].outdated = true;
                t->key_list_has_outdated = true;
            }
        }

        if(original_user_count != t->key_list_user_count) {
            if(t->key_list_user_count) {
                t->key_list_users = beeper_asserting_realloc(t->key_list_users, t->key_list_user_count * sizeof(*t->key_list_users));
            } else {
                free(t->key_list_users);
                t->key_list_users = NULL;
            }
        }
    }
}

static beeper_task_device_key_status_t device_key_status(beeper_task_t * t)
{
    key_list_device_t * device;
    key_list_device_get(t, t->user_id, NULL, t->device_id, &device);

    if(device->device_key_count == 0) {
        return BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED;
    }

    if(!device->was_signed_by_ssk) {
        return BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED;
    }

    return BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED;
}

static void room_title_event_send(beeper_task_t * t, const char * room_id, const char * name)
{
    debug("room name event: %s: %s", room_id, name);
    size_t room_id_len = strlen(room_id);
    size_t room_name_len = strlen(name);
    char * title = beeper_asserting_malloc(room_id_len + 1 + room_name_len + 1);
    memcpy(title, room_id, room_id_len + 1);
    memcpy(title + (room_id_len + 1), name, room_name_len + 1);
    t->event_cb(BEEPER_TASK_EVENT_ROOM_TITLE, title, t->event_cb_user_data);
}

static void room_member_create(void * room_member_v, void * user_data)
{
    room_member_t * room_member = room_member_v;
    room_member->display_name = NULL;
}

static void room_member_destroy(void * room_member_v, void * user_data)
{
    room_member_t * room_member = room_member_v;
    free(room_member->display_name);
}

static char * room_make_path(beeper_task_t * t, room_t * room, const char * child)
{
    char * encoded_room_id = base64_encode(room->room_id);
    base64_filename_safe(encoded_room_id);

    char * room_path;
    int res = asprintf(&room_path, "%srooms/%s/%s", t->upath, encoded_room_id, child);
    assert(res > 0);

    free(encoded_room_id);

    return room_path;
}

static void create_rooms_dir(beeper_task_t * t)
{
    if(t->rooms_dir_created) {
        return;
    }
    t->rooms_dir_created = true;

    char * rooms_path;
    int res = asprintf(&rooms_path, "%srooms", t->upath);
    assert(res > 0);
    res = mkdir(rooms_path, 0755);
    assert(res == 0 || errno == EEXIST);
    free(rooms_path);
}

static void room_create_dir(beeper_task_t * t, room_t * room)
{
    if(room->dir_created) {
        return;
    }
    room->dir_created = true;

    create_rooms_dir(t);

    char * room_path = room_make_path(t, room, "");
    int res = mkdir(room_path, 0755);
    assert(res == 0 || errno == EEXIST);
    free(room_path);
}

static char * room_session_make_path(beeper_task_t * t, room_t * room, const char * session_id)
{
    char * session_id_safe = beeper_asserting_strdup(session_id);
    base64_filename_safe(session_id_safe);
    char * session_path = room_make_path(t, room, session_id_safe);
    free(session_id_safe);
    return session_path;
}

static char * room_session_make_dirs_and_path(beeper_task_t * t, room_t * room, const char * session_id)
{
    room_create_dir(t, room);
    return room_session_make_path(t, room, session_id);
}

static void room_session_save(beeper_task_t * t, room_t * room, room_session_t * session)
{
    if(!session->needs_save) {
        return;
    }
    session->needs_save = false;

    size_t pickle_len = olm_pickle_inbound_group_session_length(session->session);
    assert(pickle_len != olm_error());
    char * pickle = beeper_asserting_malloc(pickle_len + 1);
    size_t olm_res = olm_pickle_inbound_group_session(session->session, NULL, 0, pickle, pickle_len);
    assert(olm_res == pickle_len);
    pickle[pickle_len] = '\0';

    if(session->pickle && 0 == strcmp(pickle, session->pickle)) {
        free(pickle);
        return;
    }

    free(session->pickle);
    session->pickle = pickle;

    char * session_path = room_session_make_dirs_and_path(t, room, session->session_id);

    debug("open: '%s' CREAT TRUNC WRONLY", session_path);
    int fd = open(session_path, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    assert(fd >= 0);
    free(session_path);
    ssize_t rwres = write(fd, pickle, pickle_len);
    assert(rwres == pickle_len);
    int res = close(fd);
    assert(res == 0);
}

static void room_session_destroy(void * session_v, void * user_data)
{
    room_session_t * session = session_v;
    room_session_lru_user_data_t * ud = user_data;

    if(session->session_id) {
        room_session_save(ud->t, ud->room, session);

        free(session->session_id);
        free(session->pickle);
        olm_clear_inbound_group_session(session->session);
        free(session->session);
    }
}

static bool room_session_cmp(void * session_v, void * user_data)
{
    room_session_t * session = session_v;
    char * session_id = user_data;

    return session->session_id && 0 == strcmp(session_id, session->session_id);
}

static const beeper_lru_class_t room_session_lru_class = {
    .capacity = ROOM_SESSION_LRU_COUNT,
    .item_size = sizeof(room_session_t),
    .destroy = room_session_destroy,
    .cmp = room_session_cmp,
};

static void room_create(void * room_v, void * user_data)
{
    room_t * room = room_v;
    beeper_task_t * t = user_data;
    beeper_dict_item_memzero(room, sizeof(*room));
    room->name_type = ROOM_NAME_TYPE_MEMBERS;
    beeper_array_init(&room->members, sizeof(room_member_t));
    beeper_array_init(&room->decrypting, sizeof(room_decrypting_t));
    room_title_event_send(t, room->room_id, room->room_id);
}

static void room_decrypting_destroy(void * decrypting_v, void * user_data)
{
    room_decrypting_t * decrypting = decrypting_v;

    free(decrypting->session_id);
    free(decrypting->sender_key);

    uint32_t msg_count = beeper_array_len(&decrypting->msgs);
    room_decrypting_message_t * msgs = beeper_array_data(&decrypting->msgs);
    for(uint32_t i = 0; i < msg_count; i++) {
        free(msgs[i].message_id);
        free(msgs[i].ciphertext);
    }
    beeper_array_destroy(&decrypting->msgs);
}

static void room_destroy(void * room_v, void * user_data)
{
    room_t * room = room_v;
    beeper_task_t * t = user_data;
    room_session_lru_user_data_t lru_user_data = {.t = t, .room = room};
    beeper_lru_destroy(&room_session_lru_class, room->session_lru, &lru_user_data);
    free(room->name);
    beeper_dict_destroy(&room->members, room_member_destroy, NULL);
    beeper_array_destroy_custom(&room->decrypting, room_decrypting_destroy, NULL);
    free(room->bridgebot);
}

static char * room_session_decrypt(room_session_t * session, const char * ciphertext)
{
    size_t ciphertext_len = strlen(ciphertext);

    char * ciphertext_buf = beeper_asserting_malloc(ciphertext_len); /* olm overwrites the input. */

    memcpy(ciphertext_buf, ciphertext, ciphertext_len);
    size_t plaintext_max_len = olm_group_decrypt_max_plaintext_length(session->session, (uint8_t *) ciphertext_buf, ciphertext_len);
    assert(plaintext_max_len != olm_error());

    char * plaintext = beeper_asserting_malloc(plaintext_max_len);

    memcpy(ciphertext_buf, ciphertext, ciphertext_len);
    size_t plaintext_len = olm_group_decrypt(session->session, (uint8_t *) ciphertext_buf, ciphertext_len, (uint8_t *) plaintext, plaintext_max_len, NULL);
    free(ciphertext_buf);
    if(plaintext_len == olm_error()) {
        free(plaintext);
        debug("olm_group_decrypt ERROR: %s", olm_inbound_group_session_last_error(session->session));
        return NULL;
    }
    assert(plaintext_len <= plaintext_max_len);

    cJSON * plaintext_json = unwrap_cjson(cJSON_ParseWithLength(plaintext, plaintext_len));
    free(plaintext);

    char * body = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(plaintext_json, "content"), "body"));
    assert(body);
    body = beeper_asserting_strdup(body);

    cJSON_Delete(plaintext_json);

    session->needs_save = true;

    return body;
}

static void room_decrypting_continue_with_bridgebot(beeper_task_t * t, room_t * room, room_decrypting_t * decrypting)
{
    decrypting->state = ROOM_DECRYPTING_STATE_SESSION;

    key_list_user_t * bridgebot_user = key_list_device_get_all(t, room->bridgebot);

    uint64_t key_request_txnid = txnid_next(t);
    char key_request_txnid_buf[TXNID_SIZE + 1];
    snprintf(key_request_txnid_buf, sizeof(key_request_txnid_buf), TXNID_FMT, key_request_txnid);

    cJSON * cont = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(cont, "action", unwrap_cjson(cJSON_CreateStringReference("request")));
    cJSON * body = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(cont, "body", body);
    cJSON_AddItemToObjectCS(body, "algorithm", unwrap_cjson(cJSON_CreateStringReference("m.megolm.v1.aes-sha2")));
    cJSON_AddItemToObjectCS(body, "room_id", unwrap_cjson(cJSON_CreateStringReference(room->room_id)));
    cJSON_AddItemToObjectCS(body, "sender_key", unwrap_cjson(cJSON_CreateStringReference(decrypting->sender_key)));
    cJSON_AddItemToObjectCS(body, "session_id", unwrap_cjson(cJSON_CreateStringReference(decrypting->session_id)));
    cJSON_AddItemToObjectCS(cont, "request_id", unwrap_cjson(cJSON_CreateStringReference(key_request_txnid_buf)));
    cJSON_AddItemToObjectCS(cont, "requesting_device_id", unwrap_cjson(cJSON_CreateStringReference(t->device_id)));

    cJSON * key_request_full = unwrap_cjson(cJSON_CreateObject());
    cJSON * users_obj = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(key_request_full, "messages", users_obj);
    cJSON * devices_obj = unwrap_cjson(cJSON_CreateObject());
    cJSON_AddItemToObjectCS(users_obj, room->bridgebot, devices_obj);

    for(uint32_t i = 0; i < bridgebot_user->device_count; i++) {
        cJSON * to_add;
        if(i == 0) {
            to_add = cont;
        }
        else {
            /* wasteful. improve later. */
            to_add = unwrap_cjson(cJSON_Duplicate(cont, true));
        }
        cJSON_AddItemToObjectCS(devices_obj, bridgebot_user->devices[i].device_id, to_add);
    }

    char * request_json_str = cJSON_PrintUnformatted(key_request_full);
    assert(request_json_str);

    cJSON_Delete(key_request_full);
    if(bridgebot_user->device_count == 0) cJSON_Delete(cont);

    char path[SENDTODEVICE_PATH_SIZE("m.room_key_request") + 1];
    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.room_key_request"), txnid_next(t));

    request(&t->https_conn[0], "PUT", path, t->auth_header, request_json_str, false);

    free(request_json_str);
}

static char * room_decrypt_message(beeper_task_t * t, room_t * room, cJSON * encrypted_event)
{
    cJSON * content = cJSON_GetObjectItemCaseSensitive(encrypted_event, "content");
    assert(cJSON_IsObject(content));

    char * algorithm = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "algorithm"));
    assert(algorithm && 0 == strcmp(algorithm, "m.megolm.v1.aes-sha2"));

    char * session_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "session_id"));
    assert(session_id);

    room_decrypting_t * decrypting = beeper_dict_get(&room->decrypting, session_id);
    if(decrypting) {
        char * event_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(encrypted_event, "event_id"));
        assert(event_id);

        bool was_created;
        room_decrypting_message_t * msg = beeper_dict_get_create(&decrypting->msgs, event_id, NULL, &was_created, NULL);
        if(was_created) {
            char * ciphertext = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "ciphertext"));
            assert(ciphertext);

            msg->ciphertext = beeper_asserting_strdup(ciphertext);
        }

        return NULL;
    }

    room_session_t * session = beeper_lru_get(&room_session_lru_class, room->session_lru, session_id);

    if(!session) {
        char * session_path = room_session_make_dirs_and_path(t, room, session_id);
        char * pickle = beeper_read_text_file(session_path);
        free(session_path);
        if(pickle) {
            room_session_lru_user_data_t lru_user_data = {.t = t, .room = room};
            session = beeper_lru_add_unchecked(&room_session_lru_class, room->session_lru, NULL, &lru_user_data);
            session->session_id = beeper_asserting_strdup(session_id);
            session->pickle = beeper_asserting_strdup(pickle),
            session->session = beeper_asserting_malloc(olm_inbound_group_session_size()),
            session->needs_save = false,
            olm_inbound_group_session(session->session);
            /* destroys `pickle` */
            size_t olmres = olm_unpickle_inbound_group_session(session->session, NULL, 0, pickle, strlen(pickle));
            assert(olmres != olm_error());
            free(pickle);
        }
    }

    char * ciphertext = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "ciphertext"));
    assert(ciphertext);

    if(session) {
        char * ret = room_session_decrypt(session, ciphertext);
        if(ret == NULL) {
            debug("room_session_decrypt failed in room_decrypt_message: %s %s", room->room_id, room->name ? room->name : "(unnamed)");
        }
        return ret;
    }

    char * event_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(encrypted_event, "event_id"));
    assert(event_id);
    char * sender_key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "sender_key"));
    assert(sender_key);

    decrypting = beeper_array_append(&room->decrypting, NULL);
    decrypting->session_id = beeper_asserting_strdup(session_id);
    decrypting->sender_key = beeper_asserting_strdup(sender_key);
    decrypting->state = ROOM_DECRYPTING_STATE_BRIDGEBOT;
    beeper_array_init(&decrypting->msgs, sizeof(room_decrypting_message_t));
    room_decrypting_message_t * msg = beeper_array_append(&decrypting->msgs, NULL);
    msg->message_id = beeper_asserting_strdup(event_id);
    msg->ciphertext = beeper_asserting_strdup(ciphertext);
    if(room->bridgebot) {
        room_decrypting_continue_with_bridgebot(t, room, decrypting);
    }

    return NULL;
}

static void room_save_sessions(beeper_task_t * t, room_t * room)
{
    for(size_t i = 0; i < ARRAY_LEN(room->session_lru); i++) {
        room_session_t * session = &room->session_lru[i];
        if(!session->session_id) {
            return;
        }
        room_session_save(t, room, session);
    }
}

static void handle_room_key_event(beeper_task_t * t, beeper_array_t * room_dict, cJSON * to_device_json)
{
    char * event_type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(to_device_json, "type"));
    assert(event_type);
    size_t (*import_func)(OlmInboundGroupSession *session, uint8_t const * session_key, size_t session_key_length);
    if(0 == strcmp(event_type, "m.room_key")) {
        import_func = olm_init_inbound_group_session;
    }
    else if(0 == strcmp(event_type, "m.forwarded_room_key")) {
        import_func = olm_import_inbound_group_session;
    }
    else {
        assert(0);
    }

    cJSON * content = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(to_device_json, "content"));
    char * algorithm = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "algorithm"));
    assert(algorithm);
    if(0 != strcmp(algorithm, "m.megolm.v1.aes-sha2")) return;
    char * room_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "room_id"));
    assert(room_id);
    char * session_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "session_id"));
    assert(session_id);
    char * session_key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "session_key"));
    assert(session_key);

    room_t * room = beeper_dict_get_create(room_dict, room_id, room_create, NULL, t);

    room_decrypting_t * decrypting = beeper_dict_get(&room->decrypting, session_id);
    if(decrypting) {

        room_session_lru_user_data_t lru_user_data = {.t = t, .room = room};
        room_session_t * session = beeper_lru_add_unchecked(&room_session_lru_class, room->session_lru, NULL, &lru_user_data);
        session->session_id = decrypting->session_id; /* take ownership */
        decrypting->session_id = NULL;
        session->pickle = NULL;
        session->session = beeper_asserting_malloc(olm_inbound_group_session_size());
        olm_inbound_group_session(session->session);
        char * session_key_buf = beeper_asserting_strdup(session_key);
        size_t olmres = import_func(session->session, (uint8_t *) session_key_buf, strlen(session_key_buf));
        assert(olmres != olm_error());
        free(session_key_buf);
        session->needs_save = true;

        uint32_t msg_count = beeper_array_len(&decrypting->msgs);
        room_decrypting_message_t * msgs = beeper_array_data(&decrypting->msgs);
        for(uint32_t i = 0; i < msg_count; i++) {

            char * text = room_session_decrypt(session, msgs[i].ciphertext);
            if(text == NULL) {
                debug("room_session_decrypt failed in handle_room_key_event: %s %s", room->room_id, room->name ? room->name : "(unnamed)");
                break;
            }

            beeper_task_message_decrypted_t * decrypted_event = beeper_asserting_malloc(sizeof(*decrypted_event));
            decrypted_event->room_id = beeper_asserting_strdup(room_id);
            decrypted_event->message_id = msgs[i].message_id; /* take ownership */
            msgs[i].message_id = NULL;
            decrypted_event->text = text;

            t->event_cb(BEEPER_TASK_EVENT_MESSAGE_DECRYPTED, decrypted_event, t->event_cb_user_data);

        }

        room_decrypting_destroy(decrypting, NULL);
        beeper_array_remove_item(&room->decrypting, decrypting);

        room_session_save(t, room, session);

        return;
    }

    if(beeper_lru_get_no_rearrange(&room_session_lru_class, room->session_lru, session_id)) {
        return;
    }

    char * session_path = room_session_make_path(t, room, session_id);
    debug("open: '%s' RDONLY", session_path);
    int fd = open(session_path, O_RDONLY);
    free(session_path);
    if(fd >= 0) {
        int res = close(fd);
        assert(res == 0);
        return;
    }
    assert(errno == ENOENT);

    room_session_lru_user_data_t lru_user_data = {.t = t, .room = room};
    room_session_t * session = beeper_lru_add_unchecked(&room_session_lru_class, room->session_lru, NULL, &lru_user_data);
    session->session_id = beeper_asserting_strdup(session_id);
    session->pickle = NULL;
    session->session = beeper_asserting_malloc(olm_inbound_group_session_size());
    olm_inbound_group_session(session->session);
    char * session_key_buf = beeper_asserting_strdup(session_key);
    size_t olmres = import_func(session->session, (uint8_t *) session_key_buf, strlen(session_key_buf));
    assert(olmres != olm_error());
    free(session_key_buf);
    session->needs_save = true;

    room_session_save(t, room, session);
}

static bool room_event_can_be_used_to_init_message_data(cJSON * room_event)
{
    cJSON * content;
    char * type;
    char * algorithm;
    return (content = cJSON_GetObjectItemCaseSensitive(room_event, "content"))
            && (type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(room_event, "type")))
            && ((
                    0 == strcmp(type, "m.room.encrypted")
                    && (algorithm = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "algorithm")))
                    && 0 == strcmp(algorithm, "m.megolm.v1.aes-sha2")
                    && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(content, "session_id"))
                    && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(content, "ciphertext"))
                    && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(content, "sender_key"))
                )
                || (
                    0 == strcmp(type, "m.room.message")
                    && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(content, "body"))
                )
            )
            && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(room_event, "event_id"))
            && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(room_event, "sender"))
            && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(room_event, "origin_server_ts"))
    ;
}

static void event_data_message_init_from_room_event(
    beeper_task_t * t, room_t * room, beeper_task_event_data_message_t * el, cJSON * room_event)
{
    char * event_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(room_event, "event_id"));
    assert(event_id);
    el->message_id = beeper_asserting_strdup(event_id);
    cJSON * content = cJSON_GetObjectItemCaseSensitive(room_event, "content");
    assert(cJSON_IsObject(content));
    cJSON * body = cJSON_GetObjectItemCaseSensitive(content, "body");
    if(body) {
        assert(cJSON_IsString(body));
        el->text = beeper_asserting_strdup(cJSON_GetStringValue(body));
    }
    else {
        el->text = room_decrypt_message(t, room, room_event);
    }
    char * sender = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(room_event, "sender"));
    assert(sender);
    el->member = 0 == strcmp(sender, t->user_id) ? BEEPER_TASK_MEMBER_YOU : BEEPER_TASK_MEMBER_THEM;
    double origin_server_ts = unwrap_number(cJSON_GetObjectItemCaseSensitive(room_event, "origin_server_ts"));
    /* casting double to uint64_t */
    el->timestamp = origin_server_ts;
}

static void received_event_data_destroy(const beeper_task_queue_item_t * queue_item)
{
    switch(queue_item->event_code) {
        case BEEPER_TASK_RECEIVED_EVENT_REQUEST_MESSAGES:
            beeper_received_message_request_t * ev_data = queue_item->data;
            free(ev_data->room_id);
            free(ev_data->chunk_id);
            free(ev_data);
            break;
        default:
            break;
    }
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

static bool while_object(json_stream * pdjson, const char ** key_dst)
{
    enum json_type e = json_next(pdjson);
    if(e != JSON_OBJECT_END) {
        assert(e == JSON_STRING);
        *key_dst = json_get_string(pdjson, NULL);
        return true;
    }
    return false;
}

static bool while_array(json_stream * pdjson, enum json_type expected_e)
{
    enum json_type e = json_next(pdjson);
    if(e != JSON_ARRAY_END) {
        assert(e == expected_e);
        return true;
    }
    return false;
}

static void exit_datastructure(json_stream * pdjson)
{
    size_t depth = json_get_depth(pdjson);
    while(1) {
        enum json_type e = json_next(pdjson);
        if((e == JSON_OBJECT_END || e == JSON_ARRAY_END)
           && json_get_depth(pdjson) < depth) {
            break;
        }
    }
}

static void sas_process_peer_mac_and_send_done(
    beeper_task_t * t,
    cJSON * sas_peer_mac_content,
    OlmSAS * sas_olm_sas,
    const char * sas_device_id,
    const char * sas_txid
)
{
    char * info;
    int info_len = asprintf(&info,
        "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%sKEY_IDS",
        t->user_id,
        sas_device_id,
        t->user_id,
        t->device_id,
        sas_txid
    );
    assert(info_len > 0);

    char * key_ids[2];
    char * key_macs[2];

    cJSON * item = cJSON_GetObjectItemCaseSensitive(sas_peer_mac_content, "mac");
    assert(cJSON_IsObject(item));
    item = item->child;
    assert(cJSON_IsString(item));
    key_ids[0] = item->string;
    key_macs[0] = item->valuestring;
    item = item->next;
    assert(cJSON_IsString(item));
    key_ids[1] = item->string;
    key_macs[1] = item->valuestring;
    assert(item->next == NULL);

    char * first_key;
    char * second_key;
    if(strcmp(key_ids[0], key_ids[1]) < 0) {
        first_key = key_ids[0];
        second_key = key_ids[1];
    } else {
        first_key = key_ids[1];
        second_key = key_ids[0];
    }

    char * key_list;
    int key_list_len = asprintf(&key_list, "%s,%s", first_key, second_key);
    assert(key_list_len > 0);

    size_t mac_len = olm_sas_mac_length(sas_olm_sas);
    char * mac = beeper_asserting_malloc(mac_len);

    size_t olm_mac_res = olm_sas_calculate_mac_fixed_base64(sas_olm_sas,
                                               key_list, key_list_len,
                                               info, info_len,
                                               mac, mac_len);
    assert(olm_mac_res == 0);

    free(key_list);
    free(info);

    char * key_list_mac = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(sas_peer_mac_content, "keys"));
    assert(key_list_mac);

    assert(mac_len == strlen(key_list_mac) && 0 == memcmp(mac, key_list_mac, mac_len));

    key_list_user_t * user;
    key_list_device_t * peer_device;
    key_list_device_get(t, t->user_id, &user, sas_device_id, &peer_device);
    assert(user->master_key_count == 1);
    assert(user->ssk_was_signed_by_master);
    assert(peer_device->device_key_count > 0);
    assert(peer_device->was_signed_by_ssk);

    bool did_master_key = false;
    bool did_device_key = false;
    for(int i = 0; i < 2; i++) {
        char * key_val = NULL;
        if(!did_master_key) {
            if(0 == strcmp(key_ids[i], user->master_keys[0].key_id)) {
                did_master_key = true;
                key_val = user->master_keys[0].key_value;
            }
        }
        if(!did_device_key && !key_val) {
            for(uint32_t j = 0; j < peer_device->device_key_count; j++) {
                if(0 == strcmp(key_ids[i], peer_device->device_keys[j].key_id)) {
                    did_device_key = true;
                    key_val = peer_device->device_keys[j].key_value;
                    break;
                }
            }
        }
        assert(key_val);

        info_len = asprintf(&info,
            "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%s%s",
            t->user_id,
            sas_device_id,
            t->user_id,
            t->device_id,
            sas_txid,
            key_ids[i]
        );
        assert(info_len > 0);

        olm_mac_res = olm_sas_calculate_mac_fixed_base64(sas_olm_sas,
                                            key_val, strlen(key_val),
                                            info, info_len,
                                            mac, mac_len);
        assert(olm_mac_res == 0);

        free(info);

        assert(mac_len == strlen(key_macs[i]) && 0 == memcmp(mac, key_macs[i], mac_len));
    }

    free(mac);

    char * req_json_str;
    assert(0 < asprintf(&req_json_str,
        "{"
            "\"messages\":{"
                "\"%s\":{"
                    "\"%s\":{"
                        "\"transaction_id\":\"%s\""
                    "}"
                "}"
            "}"
        "}",
        t->user_id,
        sas_device_id,
        sas_txid
    ));

    char path[SENDTODEVICE_PATH_SIZE("m.key.verification.done") + 1];
    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.key.verification.done"), txnid_next(t));

    request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str, false);

    free(req_json_str);
}

static void * thread(void * arg)
{
    int res;
    size_t olm_res;
    beeper_task_t * t = arg;

    res = mkdir(t->upath, 0755);
    assert(res == 0 || errno == EEXIST);

    txnid_init(t);

    debug("open: '"BEEPER_RANDOM_PATH"' RDONLY");
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

    char * resp_str = request(&t->https_conn[0], "POST", "login", NULL, login_json_str, true);
    assert(resp_str);
    free(login_json_str);
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
            debug("open: '%s' WRONLY EXCL CREAT", device_id_path);
            int fd = open(device_id_path, O_WRONLY | O_EXCL | O_CREAT, 0666);
            assert(fd != -1);
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

    free(device_id_path);

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

    char * identity_key_curve25519;
    char * identity_key_ed25519;
    {
        size_t identity_keys_length = olm_account_identity_keys_length(t->olm_account);
        char * identity_keys = malloc(identity_keys_length);
        assert(identity_keys);
        olm_res = olm_account_identity_keys(t->olm_account, identity_keys, identity_keys_length);
        assert(olm_res == identity_keys_length);
        cJSON * identity_keys_json = cJSON_ParseWithLength(identity_keys, identity_keys_length);
        free(identity_keys);

        identity_key_curve25519 = strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(identity_keys_json, "curve25519")));
        assert(identity_key_curve25519);
        identity_key_ed25519 = strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(identity_keys_json, "ed25519")));
        assert(identity_key_ed25519);

        cJSON_Delete(identity_keys_json);
    }

    key_list_init(t);

    /* check whether we've uploaded our keys yet and whether our keys */
    /* have been signed by the account's self signing key (device is verified). */
    beeper_task_device_key_status_t device_key_status_res = device_key_status(t);

    if(device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_NOT_UPLOADED) {
        cJSON * upload_keys_json = get_n_one_time_keys(t, ONE_TIME_KEY_COUNT_TOPUP);

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
                identity_key_curve25519))));
            free(curve_device_key_id);

            char * ed_device_key_id;
            assert(-1 != asprintf(&ed_device_key_id, "ed25519:%s", t->device_id));
            assert(cJSON_AddItemToObject(keys, ed_device_key_id, unwrap_cjson(cJSON_CreateStringReference(
                identity_key_ed25519))));
            free(ed_device_key_id);

            cJSON_AddItemToObjectCS(device_keys, "user_id", unwrap_cjson(cJSON_CreateStringReference(t->user_id)));

            cJSON_AddItemToObjectCS(device_keys, "signatures", sign_json(t, device_keys));
        }

        char * upload_keys_json_str = cJSON_PrintUnformatted(upload_keys_json);
        assert(upload_keys_json_str);

        cJSON_Delete(upload_keys_json);

        request(&t->https_conn[0], "POST", "keys/upload",
                                  t->auth_header, upload_keys_json_str, false);
        free(upload_keys_json_str);

        device_key_status_res = BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED;
    }

    if(device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_NOT_VERIFIED) {
        olm_account_mark_keys_as_published(t->olm_account);
        pickle_account(t);
    }

    bool is_verified = device_key_status_res == BEEPER_TASK_DEVICE_KEY_STATUS_IS_VERIFIED;
    t->event_cb(BEEPER_TASK_EVENT_VERIFICATION_STATUS, (void *)(uintptr_t)is_verified, t->event_cb_user_data);

    https_conn_init(&t->https_ctx, &t->https_conn[1]);
    request_send(&t->https_conn[1], "GET", "sync?timeout=30000", t->auth_header, NULL);

    enum { SAS_STEP_REQUEST, SAS_STEP_START, SAS_STEP_KEY,
           SAS_STEP_MAC_NEED_MATCH_AND_MAC, SAS_STEP_MAC_NEED_MATCH, SAS_STEP_MAC_NEED_MAC,
           SAS_STEP_DONE, SAS_STEP_END
        } sas_step = SAS_STEP_REQUEST;
    char * sas_txid = NULL;
    char * sas_device_id = NULL;
    OlmSAS * sas_olm_sas = NULL;
    cJSON * sas_peer_mac_content = NULL;

    beeper_array_t room_dict;
    beeper_array_init(&room_dict, sizeof(room_t));

    struct pollfd pfd[2] = {
        {.fd = https_fd(&t->https_conn[1])},
        {.fd = beeper_queue_get_poll_fd(&t->queue), .events = POLLIN}
    };
    while(1) {
        beeper_task_queue_item_t queue_item;
        assert(0 == pthread_mutex_lock(&t->queue_mutex));
        bool queue_popped = beeper_queue_pop(&t->queue, &queue_item);
        assert(0 == pthread_mutex_unlock(&t->queue_mutex));
        if(queue_popped) {
            beeper_task_received_event_t e = queue_item.event_code;
            if(e == BEEPER_TASK_RECEIVED_EVENT_STOP) {
                break;
            }
            else if(e == BEEPER_TASK_RECEIVED_EVENT_SAS_MATCHES) {
                if(sas_step == SAS_STEP_MAC_NEED_MATCH_AND_MAC || sas_step == SAS_STEP_MAC_NEED_MATCH) {
                    /* send our mac now that our device confirmed the match */
                    char * info;
                    int info_len = asprintf(&info,
                        "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%sKEY_IDS",
                        t->user_id,
                        t->device_id,
                        t->user_id,
                        sas_device_id,
                        sas_txid
                    );
                    assert(info_len > 0);

                    char * device_key_ed25519_id;
                    assert(0 < asprintf(&device_key_ed25519_id, "ed25519:%s", t->device_id));

                    key_list_user_t * user;
                    key_list_device_get(t, t->user_id, &user, t->device_id, NULL);
                    assert(user->master_key_count == 1);

                    char * first_key;
                    char * second_key;
                    if(strcmp(device_key_ed25519_id, user->master_keys[0].key_id) < 0) {
                        first_key = device_key_ed25519_id;
                        second_key = user->master_keys[0].key_id;
                    } else {
                        first_key = user->master_keys[0].key_id;
                        second_key = device_key_ed25519_id;
                    }

                    char * key_list;
                    int key_list_len = asprintf(&key_list, "%s,%s", first_key, second_key);
                    assert(key_list_len > 0);

                    size_t mac_len = olm_sas_mac_length(sas_olm_sas);
                    char * macs = beeper_asserting_malloc(mac_len * 3);

                    size_t olm_mac_res = olm_sas_calculate_mac_fixed_base64(sas_olm_sas,
                                                               key_list, key_list_len,
                                                               info, info_len,
                                                               macs, mac_len);
                    assert(olm_mac_res == 0);

                    free(key_list);
                    free(info);

                    char * key_ids[2] = {device_key_ed25519_id, user->master_keys[0].key_id};
                    char * key_vals[2] = {identity_key_ed25519, user->master_keys[0].key_value};
                    for(int i = 0; i < 2; i++) {
                        info_len = asprintf(&info,
                            "MATRIX_KEY_VERIFICATION_MAC%s%s%s%s%s%s",
                            t->user_id,
                            t->device_id,
                            t->user_id,
                            sas_device_id,
                            sas_txid,
                            key_ids[i]
                        );
                        assert(info_len > 0);

                        olm_mac_res = olm_sas_calculate_mac_fixed_base64(sas_olm_sas,
                                                            key_vals[i], strlen(key_vals[i]),
                                                            info, info_len,
                                                            macs + (i+1) * mac_len, mac_len);
                        assert(olm_mac_res == 0);

                        free(info);
                    }

                    char * req_json_str;
                    assert(0 < asprintf(&req_json_str,
                        "{"
                            "\"messages\":{"
                                "\"%s\":{"
                                    "\"%s\":{"
                                        "\"keys\":\"%.*s\","
                                        "\"mac\":{"
                                            "\"%s\":\"%.*s\","
                                            "\"%s\":\"%.*s\""
                                        "},"
                                        "\"transaction_id\":\"%s\""
                                    "}"
                                "}"
                            "}"
                        "}",
                        t->user_id,
                        sas_device_id,
                        (int) mac_len, macs,
                        key_ids[0], (int) mac_len, macs + mac_len,
                        key_ids[1], (int) mac_len, macs + mac_len * 2,
                        sas_txid
                    ));

                    free(macs);
                    free(device_key_ed25519_id);

                    char path[SENDTODEVICE_PATH_SIZE("m.key.verification.mac") + 1];
                    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.key.verification.mac"), txnid_next(t));

                    request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str, false);

                    free(req_json_str);

                    if(sas_step == SAS_STEP_MAC_NEED_MATCH) {
                        assert(sas_peer_mac_content);
                        sas_process_peer_mac_and_send_done(
                            t,
                            sas_peer_mac_content,
                            sas_olm_sas,
                            sas_device_id,
                            sas_txid
                        );
                        cJSON_Delete(sas_peer_mac_content);
                        sas_peer_mac_content = NULL;
                        sas_step = SAS_STEP_DONE;
                    }
                    else { /* sas_step == SAS_STEP_MAC_NEED_MATCH_AND_MAC */
                        sas_step = SAS_STEP_MAC_NEED_MAC;
                    }
                }
            }
            else if(e == BEEPER_TASK_RECEIVED_EVENT_REQUEST_MESSAGES) {
                beeper_received_message_request_t * msgs_req_ev = queue_item.data;

                bool was_created;
                room_t * room = beeper_dict_get_create(&room_dict, msgs_req_ev->room_id, room_create, &was_created, t);
                assert(!was_created);

                char * path;
                if(msgs_req_ev->chunk_id) {
                    assert(0 < asprintf(&path,
                        "rooms/%s/messages?dir=%c&from=%s",
                        msgs_req_ev->room_id,
                        msgs_req_ev->direction == BEEPER_TASK_DIRECTION_UP ? 'b' : 'f',
                        msgs_req_ev->chunk_id
                    ));
                }
                else {
                    assert(0 < asprintf(&path,
                        "rooms/%s/messages?dir=%c",
                        msgs_req_ev->room_id,
                        msgs_req_ev->direction == BEEPER_TASK_DIRECTION_UP ? 'b' : 'f'
                    ));
                }
                char * msgs_resp = request(&t->https_conn[0], "GET", path, t->auth_header, NULL, true);
                free(path);
                cJSON * msgs_json = unwrap_cjson(cJSON_Parse(msgs_resp));
                free(msgs_resp);

                cJSON * chunk = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(msgs_json, "chunk"));

                uint32_t message_count = 0;
                cJSON * chunk_item;
                cJSON_ArrayForEach(chunk_item, chunk) {
                    if(room_event_can_be_used_to_init_message_data(chunk_item)) {
                        message_count += 1;
                    }
                }

                beeper_task_messages_event_data_t * msgs_resp_event = beeper_asserting_calloc(1,
                    sizeof(*msgs_resp_event) + message_count * sizeof(msgs_resp_event->messages[0]));
                msgs_resp_event->room_id = msgs_req_ev->room_id; /* take ownership */
                msgs_req_ev->room_id = NULL;
                msgs_resp_event->this_chunk_id = msgs_req_ev->chunk_id; /* take ownership */
                msgs_req_ev->chunk_id = NULL;
                cJSON * next_chunk_id_json = cJSON_GetObjectItemCaseSensitive(msgs_json, "end");
                if(next_chunk_id_json) {
                    char * next_chunk_id = cJSON_GetStringValue(next_chunk_id_json);
                    assert(next_chunk_id);
                    msgs_resp_event->next_chunk_id = beeper_asserting_strdup(next_chunk_id);
                }
                msgs_resp_event->direction = msgs_req_ev->direction;
                msgs_resp_event->message_count = message_count;

                uint32_t i = 0;
                cJSON_ArrayForEach(chunk_item, chunk) {
                    if(!room_event_can_be_used_to_init_message_data(chunk_item)) {
                        continue;
                    }

                    event_data_message_init_from_room_event(t, room, &msgs_resp_event->messages[i], chunk_item);

                    i++;
                }

                cJSON_Delete(msgs_json);

                t->event_cb(BEEPER_TASK_EVENT_ROOM_MESSAGES, msgs_resp_event, t->event_cb_user_data);

                room_save_sessions(t, room);
            }
            else assert(0);

            received_event_data_destroy(&queue_item);

            continue;
        }

        bool something_happened = false;
        while(1) {
            int resp_len;
            int nonblocking_status;
            bool response_was_received = request_recv(&t->https_conn[1], &resp_len,
                                                      false, &nonblocking_status,
                                                      false, NULL,
                                                      false, NULL);
            if(!response_was_received) {
                pfd[0].events = nonblocking_status == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
                break;
            }
            something_happened = true;

            char * next_batch = NULL;
            cJSON * device_lists = NULL;

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
            bool was_created;
            assert(JSON_OBJECT == json_next(&pdjson));
            while(JSON_OBJECT_END != (e = json_next(&pdjson))) {
                assert(e == JSON_STRING);
                object_key_string = json_get_string(&pdjson, NULL);
                if(0 == strcmp("next_batch", object_key_string)) {
                    assert(json_next(&pdjson) == JSON_STRING);
                    assert(!next_batch);
                    next_batch = beeper_asserting_strdup(json_get_string(&pdjson, NULL));
                }
                else if(0 == strcmp("device_one_time_keys_count", object_key_string)) {
                    assert(JSON_OBJECT == json_next(&pdjson));
                    while(while_object(&pdjson, &object_key_string)) {
                        if(0 == strcmp("signed_curve25519", object_key_string)) {
                            assert(JSON_NUMBER == json_next(&pdjson));
                            int remaining_otks = json_get_number(&pdjson); /* cast double to int */
                            debug("one-time key count: %d", remaining_otks);

                            if(remaining_otks < ONE_TIME_KEY_COUNT_LOW_WATERMARK) {
                                cJSON * upload_keys_json = get_n_one_time_keys(t, ONE_TIME_KEY_COUNT_TOPUP - remaining_otks);

                                char * upload_keys_json_str = cJSON_PrintUnformatted(upload_keys_json);
                                assert(upload_keys_json_str);
                                cJSON_Delete(upload_keys_json);

                                request(&t->https_conn[0], "POST", "keys/upload", t->auth_header, upload_keys_json_str, false);
                                free(upload_keys_json_str);

                                olm_account_mark_keys_as_published(t->olm_account);
                                pickle_account(t);
                            }

                            exit_datastructure(&pdjson);
                            break;
                        }
                        else assert(JSON_ERROR != json_skip(&pdjson));
                    }
                }
                else if(0 == strcmp("device_lists", object_key_string)) {
                    assert(JSON_OBJECT == json_peek(&pdjson));
                    stream_data_start_capture(&sd);
                    start = json_get_position(&pdjson) - 1;
                    assert(JSON_ERROR != json_skip(&pdjson));
                    len = json_get_position(&pdjson) - start;
                    capture = stream_data_get_capture(&sd, start, len);
                    assert(!device_lists);
                    device_lists = unwrap_cjson(cJSON_ParseWithLength(capture, len));
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
                                    debug("m.key.verification.request");
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
                                    double timestamp = unwrap_number(cJSON_GetObjectItemCaseSensitive(content, "timestamp"));
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
                                    char path[SENDTODEVICE_PATH_SIZE("m.key.verification.ready") + 1];
                                    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.key.verification.ready"), txnid_next(t));
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
                                    request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str, false);
                                    free(req_json_str);
                                    sas_txid = strdup(transaction_id);
                                    assert(sas_txid);
                                    sas_device_id = strdup(from_device);
                                    assert(sas_device_id);
                                    sas_step = SAS_STEP_START;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.start")) {
                                    __label__ denied;
                                    debug("m.key.verification.start");
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

                                    char path[MAX(SENDTODEVICE_PATH_SIZE("m.key.verification.accept"),
                                                  SENDTODEVICE_PATH_SIZE("m.key.verification.key")) + 1];

                                    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.key.verification.accept"), txnid_next(t));
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
                                    request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str, false);
                                    free(req_json_str);
                                    free(commitment);

                                    snprintf(path, sizeof(path), SENDTODEVICE_PATH_FMT("m.key.verification.key"), txnid_next(t));
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
                                    request(&t->https_conn[0], "PUT", path, t->auth_header, req_json_str, false);
                                    free(req_json_str);
                                    free(pubkey);

                                    sas_step = SAS_STEP_KEY;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.key")) {
                                    __label__ denied;
                                    debug("m.key.verification.key");
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

                                    sas_step = SAS_STEP_MAC_NEED_MATCH_AND_MAC;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.mac")) {
                                    __label__ denied;
                                    debug("m.key.verification.mac");
                                    if((sas_step != SAS_STEP_MAC_NEED_MATCH_AND_MAC && sas_step != SAS_STEP_MAC_NEED_MAC)
                                       || 0 != strcmp(sender, t->user_id)) goto denied;
                                    char * transaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "transaction_id"));
                                    assert(transaction_id);
                                    if(0 != strcmp(transaction_id, sas_txid)) goto denied;
                                    if(sas_step == SAS_STEP_MAC_NEED_MAC) {
                                        sas_process_peer_mac_and_send_done(
                                            t,
                                            content,
                                            sas_olm_sas,
                                            sas_device_id,
                                            sas_txid
                                        );
                                        sas_step = SAS_STEP_DONE;
                                    }
                                    else { /* sas_step == SAS_STEP_MAC_NEED_MATCH_AND_MAC */
                                        assert(!sas_peer_mac_content);
                                        sas_peer_mac_content = cJSON_DetachItemViaPointer(to_device_event, content);
                                        sas_step = SAS_STEP_MAC_NEED_MATCH;
                                    }
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.key.verification.done")) {
                                    __label__ denied;
                                    debug("m.key.verification.done");
                                    if(sas_step != SAS_STEP_DONE || 0 != strcmp(sender, t->user_id)) goto denied;
                                    char * transaction_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "transaction_id"));
                                    assert(transaction_id);
                                    if(0 != strcmp(transaction_id, sas_txid)) goto denied;
                                    t->event_cb(BEEPER_TASK_EVENT_SAS_COMPLETE, NULL, t->event_cb_user_data);
                                    sas_step = SAS_STEP_END;
                                    denied:
                                }
                                else if(0 == strcmp(type, "m.room.encrypted")) {
                                    __label__ denied;
                                    char * algorithm = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "algorithm"));
                                    if(!algorithm || 0 != strcmp(algorithm, "m.olm.v1.curve25519-aes-sha2")) goto denied;
                                    cJSON * ciphertext_mapping = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(content, "ciphertext"));
                                    cJSON * ciphertext_info = cJSON_GetObjectItemCaseSensitive(ciphertext_mapping, identity_key_curve25519);
                                    if(!ciphertext_info) goto denied;
                                    char * ciphertext = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ciphertext_info, "body"));
                                    assert(ciphertext);
                                    double type = unwrap_number(cJSON_GetObjectItemCaseSensitive(ciphertext_info, "type"));
                                    assert(type == 0.0 || type == 1.0);
                                    char * sender_key = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(content, "sender_key"));
                                    assert(sender_key);
                                    cJSON * olm_payload = decrypt(t, ciphertext, type, sender, sender_key);
                                    if(olm_payload == NULL) {
                                        debug("decrypt failed on m.room.encrypted event");
                                        goto denied;
                                    }
                                    char * event_type = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(olm_payload, "type"));
                                    assert(event_type);
                                    if(0 == strcmp(event_type, "m.room_key") || 0 == strcmp(event_type, "m.forwarded_room_key")) {
                                        handle_room_key_event(t, &room_dict, olm_payload);
                                    }
                                    cJSON_Delete(olm_payload);
                                    denied:
                                }
                                else {
                                    debug("unhandled to-device message: %.*s", len, capture);
                                }
                                cJSON_Delete(to_device_event);
                            }
                            json_next(&pdjson);
                        }
                        else assert(JSON_ERROR != json_skip(&pdjson));
                    }
                }
                else if(0 == strcmp("rooms", object_key_string)) {
                    assert(JSON_OBJECT == json_next(&pdjson));
                    while(while_object(&pdjson, &object_key_string)) {
                        if(0 == strcmp("join", object_key_string)) {
                            assert(json_next(&pdjson) == JSON_OBJECT);
                            while(while_object(&pdjson, &object_key_string)) {
                                debug("room: %s", object_key_string);
                                room_t * room = beeper_dict_get_create(&room_dict, object_key_string, room_create, NULL, t);
                                assert(JSON_OBJECT == json_next(&pdjson));
                                while(while_object(&pdjson, &object_key_string)) {
                                    matrix_joined_room_t joined_room_obj_type = MATRIX_JOINED_ROOM_NULL;
                                    if(0 == strcmp("timeline", object_key_string)) joined_room_obj_type = MATRIX_JOINED_ROOM_TIMELINE;
                                    else if(0 == strcmp("state", object_key_string)) joined_room_obj_type = MATRIX_JOINED_ROOM_STATE;
                                    if(joined_room_obj_type == MATRIX_JOINED_ROOM_TIMELINE || joined_room_obj_type == MATRIX_JOINED_ROOM_STATE) {
                                        assert(json_next(&pdjson) == JSON_OBJECT);
                                        char * timeline_prev_batch = NULL;
                                        beeper_task_messages_event_data_t * timeline_messages_event = NULL;
                                        while(while_object(&pdjson, &object_key_string)) {
                                            if(joined_room_obj_type == MATRIX_JOINED_ROOM_TIMELINE && 0 == strcmp("prev_batch", object_key_string)) {
                                                assert(json_next(&pdjson) == JSON_STRING);
                                                assert(timeline_prev_batch == NULL);
                                                timeline_prev_batch = beeper_asserting_strdup(json_get_string(&pdjson, NULL));
                                            }
                                            else if(0 == strcmp("events", object_key_string)) {
                                                assert(json_next(&pdjson) == JSON_ARRAY);
                                                while(while_array(&pdjson, JSON_OBJECT)) {
                                                    stream_data_start_capture(&sd);
                                                    start = json_get_position(&pdjson) - 1;
                                                    while(while_object(&pdjson, &object_key_string)) {
                                                        if(0 == strcmp("type", object_key_string)) {
                                                            assert(json_next(&pdjson) == JSON_STRING);
                                                            const char * type_string = json_get_string(&pdjson, NULL);
                                                            matrix_event_type_t event_type = MATRIX_EVENT_TYPE_NULL;
                                                            if(0 == strcmp("m.room.member", type_string)) event_type = MATRIX_EVENT_TYPE_M_ROOM_MEMBER;
                                                            else if(0 == strcmp("m.room.canonical_alias", type_string)) event_type = MATRIX_EVENT_TYPE_M_ROOM_CANONICAL_ALIAS;
                                                            else if(0 == strcmp("m.room.name", type_string)) event_type = MATRIX_EVENT_TYPE_M_ROOM_NAME;
                                                            else if(0 == strcmp("m.room.message", type_string)) event_type = MATRIX_EVENT_TYPE_M_ROOM_MESSAGE;
                                                            else if(0 == strcmp("m.room.encrypted", type_string)) event_type = MATRIX_EVENT_TYPE_M_ROOM_ENCRYPTED;
                                                            else if(0 == strcmp("m.bridge", type_string)) event_type = MATRIX_EVENT_TYPE_M_BRIDGE;
                                                            room_name_type_t room_event_name_type = ROOM_NAME_TYPE_NULL;
                                                            switch(room->name_type) {
                                                                case ROOM_NAME_TYPE_MEMBERS:
                                                                    if(event_type == MATRIX_EVENT_TYPE_M_ROOM_MEMBER) {
                                                                        room_event_name_type = ROOM_NAME_TYPE_MEMBERS;
                                                                        break;
                                                                    }
                                                                    /* fall through */
                                                                case ROOM_NAME_TYPE_CANONICAL_ALIAS:
                                                                    if(event_type == MATRIX_EVENT_TYPE_M_ROOM_CANONICAL_ALIAS) {
                                                                        room_event_name_type = ROOM_NAME_TYPE_CANONICAL_ALIAS;
                                                                        break;
                                                                    }
                                                                    /* fall through */
                                                                case ROOM_NAME_TYPE_NAME:
                                                                    if(event_type == MATRIX_EVENT_TYPE_M_ROOM_NAME) {
                                                                        room_event_name_type = ROOM_NAME_TYPE_NAME;
                                                                        break;
                                                                    }
                                                                    /* fall through */
                                                                default:
                                                                    break;
                                                            }
                                                            bool should_capture = room_event_name_type != ROOM_NAME_TYPE_NULL
                                                                                  || event_type == MATRIX_EVENT_TYPE_M_ROOM_MESSAGE
                                                                                  || event_type == MATRIX_EVENT_TYPE_M_ROOM_ENCRYPTED
                                                                                  || event_type == MATRIX_EVENT_TYPE_M_BRIDGE;
                                                            if(!should_capture) {
                                                                sd.capturing = false;
                                                            }
                                                            exit_datastructure(&pdjson);
                                                            if(should_capture) {
                                                                len = json_get_position(&pdjson) - start;
                                                                capture = stream_data_get_capture(&sd, start, len);
                                                                debug("capture: %.*s", len, capture);
                                                                cJSON * room_event = unwrap_cjson(cJSON_ParseWithLength(capture, len));
                                                                if(room_event_name_type != ROOM_NAME_TYPE_NULL) {
                                                                    cJSON * content = unwrap_cjson(cJSON_GetObjectItemCaseSensitive(
                                                                        room_event, "content"));
                                                                    bool non_member_name_ok = false;
                                                                    if(room_event_name_type == ROOM_NAME_TYPE_MEMBERS) {
                                                                        char * membership = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                                                            content, "membership"));
                                                                        assert(membership);
                                                                        if(0 == strcmp("join", membership)) {
                                                                            char * user_id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                                                                room_event, "state_key"));
                                                                            assert(user_id);
                                                                            char * display_name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                                                                content, "displayname"));
                                                                            room_member_t * room_member = beeper_dict_get_create(&room->members, user_id, room_member_create, &was_created, NULL);
                                                                            bool display_name_unchanged = !was_created && ((display_name == NULL && room_member->display_name == NULL)
                                                                                                                        || (display_name && room_member->display_name && 0 == strcmp(display_name, room_member->display_name)));
                                                                            if(!display_name_unchanged) {
                                                                                free(room_member->display_name);
                                                                                room_member->display_name = display_name ? beeper_asserting_strdup(display_name) : NULL;

                                                                                size_t room_member_count = beeper_array_len(&room->members);
                                                                                room_member_t * room_members = beeper_array_data(&room->members);
                                                                                size_t title_size = (room_member_count - 1) * 2 + 1;
                                                                                for(size_t i = 0; i < room_member_count; i++)
                                                                                    title_size += strlen(room_members[i].display_name ? room_members[i].display_name : room_members[i].user_id);
                                                                                char * title = beeper_asserting_malloc(title_size);
                                                                                char * title_p = title;
                                                                                for(size_t i = 0; i < room_member_count; i++) {
                                                                                    char * part = room_members[i].display_name ? room_members[i].display_name : room_members[i].user_id;
                                                                                    title_p = stpcpy(title_p, part);
                                                                                    if(i + 1 < room_member_count) {
                                                                                        title_p[0] = ',';
                                                                                        title_p[1] = ' ';
                                                                                        title_p += 2;
                                                                                    }
                                                                                }
                                                                                room_title_event_send(t, room->room_id, title);
                                                                                free(title);
                                                                            }
                                                                        }
                                                                    }
                                                                    else if(room_event_name_type == ROOM_NAME_TYPE_CANONICAL_ALIAS) {
                                                                        char * alias = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                                                            content, "alias"));
                                                                        if(alias && alias[0]) {
                                                                            free(room->name);
                                                                            room->name = beeper_asserting_strdup(alias);
                                                                            non_member_name_ok = true;
                                                                        }
                                                                    }
                                                                    else if(room_event_name_type == ROOM_NAME_TYPE_NAME) {
                                                                        char * name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(
                                                                            content, "name"));
                                                                        if(name && name[0]) {
                                                                            free(room->name);
                                                                            room->name = beeper_asserting_strdup(name);
                                                                            non_member_name_ok = true;
                                                                        }
                                                                    }
                                                                    else assert(0);
                                                                    if(room_event_name_type > ROOM_NAME_TYPE_MEMBERS && non_member_name_ok) {
                                                                        room_title_event_send(t, room->room_id, room->name);
                                                                        beeper_dict_reset(&room->members, room_member_destroy, NULL);
                                                                        room->name_type = room_event_name_type;
                                                                    }
                                                                }
                                                                if(event_type == MATRIX_EVENT_TYPE_M_ROOM_MESSAGE || event_type == MATRIX_EVENT_TYPE_M_ROOM_ENCRYPTED) {
                                                                    if(room_event_can_be_used_to_init_message_data(room_event)) {
                                                                        if(timeline_messages_event == NULL) {
                                                                            timeline_messages_event = beeper_asserting_calloc(1, sizeof(*timeline_messages_event)
                                                                                                                                 + 1 * sizeof(timeline_messages_event->messages[0]));
                                                                            timeline_messages_event->room_id = beeper_asserting_strdup(room->room_id);
                                                                            timeline_messages_event->direction = BEEPER_TASK_DIRECTION_DOWN;
                                                                            timeline_messages_event->message_count = 1;
                                                                        }
                                                                        else {
                                                                            timeline_messages_event->message_count += 1;
                                                                            timeline_messages_event = beeper_asserting_realloc(timeline_messages_event,
                                                                                                                               sizeof(*timeline_messages_event)
                                                                                                                               + timeline_messages_event->message_count
                                                                                                                               * sizeof(timeline_messages_event->messages[0]));
                                                                        }
                                                                        beeper_task_event_data_message_t * message_element = &timeline_messages_event->messages[timeline_messages_event->message_count - 1];
                                                                        event_data_message_init_from_room_event(t, room, message_element, room_event);
                                                                    }
                                                                }
                                                                else if(event_type == MATRIX_EVENT_TYPE_M_BRIDGE) {
                                                                    char * bridgebot = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(room_event, "content"), "bridgebot"));
                                                                    if(bridgebot && bridgebot[0]) {
                                                                        free(room->bridgebot);
                                                                        room->bridgebot = beeper_asserting_strdup(bridgebot);
                                                                        uint32_t room_decrypting_count = beeper_array_len(&room->decrypting);
                                                                        room_decrypting_t * room_decrypting = beeper_array_data(&room->decrypting);
                                                                        for(uint32_t i = 0; i < room_decrypting_count; i++) {
                                                                            if(room_decrypting[i].state == ROOM_DECRYPTING_STATE_BRIDGEBOT) {
                                                                                room_decrypting_continue_with_bridgebot(t, room, &room_decrypting[i]);
                                                                            }
                                                                        }
                                                                    }
                                                                }
                                                                cJSON_Delete(room_event);
                                                            }
                                                            break;
                                                        }
                                                        else assert(JSON_ERROR != json_skip(&pdjson));
                                                    }
                                                    sd.capturing = false;
                                                }
                                            }
                                            else assert(JSON_ERROR != json_skip(&pdjson));
                                        }
                                        if(timeline_messages_event) {
                                            timeline_messages_event->next_chunk_id = timeline_prev_batch; /* take ownership */
                                            timeline_prev_batch = NULL;
                                            t->event_cb(BEEPER_TASK_EVENT_ROOM_MESSAGES, timeline_messages_event, t->event_cb_user_data);
                                            room_save_sessions(t, room);
                                        }
                                        free(timeline_prev_batch);
                                    }
                                    else assert(JSON_ERROR != json_skip(&pdjson));
                                }
                            }
                        }
                        else assert(JSON_ERROR != json_skip(&pdjson));
                    }
                }
                else assert(JSON_ERROR != json_skip(&pdjson));
            }
            assert(JSON_DONE == json_next(&pdjson));
            json_close(&pdjson);
            stream_data_deinit(&sd);

            if(device_lists) {
                key_list_apply_devicelists(t, device_lists);
                cJSON_Delete(device_lists);
            }

            if(key_list_should_save(t)) {
                key_list_save(t);
                assert(!key_list_should_save(t));
            }

            assert(next_batch);
            char * sync_path_with_since;
            assert(-1 != asprintf(&sync_path_with_since, "sync?timeout=30000&since=%s", next_batch));
            free(next_batch);
            request_send(&t->https_conn[1], "GET", sync_path_with_since, t->auth_header, NULL);
            free(sync_path_with_since);
        }

        if(something_happened) {
            continue;
        }

        res = poll(pfd, 2, -1);
        assert(res > 0);
    }

    beeper_dict_destroy(&room_dict, room_destroy, t);

    cJSON_Delete(sas_peer_mac_content);
    if(sas_olm_sas) olm_clear_sas(sas_olm_sas);
    free(sas_olm_sas);
    free(sas_device_id);
    free(sas_txid);

    key_list_destroy(t);

    free(identity_key_curve25519);
    free(identity_key_ed25519);
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

    beeper_queue_init(&t->queue, sizeof(beeper_task_queue_item_t));
    assert(0 == pthread_mutex_init(&t->queue_mutex, NULL));

    assert(0 == pthread_create(&t->thread, NULL, thread, t));

    return t;
}

static void safe_queue_push(beeper_task_t * t, beeper_task_received_event_t e, void * data)
{
    beeper_task_queue_item_t queue_item = {.event_code = e, .data = data};
    assert(0 == pthread_mutex_lock(&t->queue_mutex));
    beeper_queue_push(&t->queue, &queue_item);
    assert(0 == pthread_mutex_unlock(&t->queue_mutex));
}

void beeper_task_destroy(beeper_task_t * t)
{
    safe_queue_push(t, BEEPER_TASK_RECEIVED_EVENT_STOP, NULL);

    assert(0 == pthread_join(t->thread, NULL));

    assert(0 == pthread_mutex_destroy(&t->queue_mutex));
    beeper_task_queue_item_t queue_item;
    while(beeper_queue_pop(&t->queue, &queue_item)) received_event_data_destroy(&queue_item);
    beeper_queue_destroy(&t->queue);

    free(t->upath);
    free(t->password);
    free(t->username);
    free(t);
}

void beeper_task_event_data_destroy(beeper_task_event_t e, void * event_data)
{
    switch(e) {
        case BEEPER_TASK_EVENT_VERIFICATION_STATUS:
            break;
        case BEEPER_TASK_EVENT_ROOM_MESSAGES: {
            beeper_task_messages_event_data_t * msgs = event_data;
            free(msgs->room_id);
            free(msgs->this_chunk_id);
            free(msgs->next_chunk_id);
            for(uint32_t i = 0; i < msgs->message_count; i++) {
                free(msgs->messages[i].message_id);
                free(msgs->messages[i].text);
            }
            free(msgs);
            break;
        }
        case BEEPER_TASK_EVENT_MESSAGE_DECRYPTED: {
            beeper_task_message_decrypted_t * d = event_data;
            free(d->room_id);
            free(d->message_id);
            free(d->text);
            free(d);
            break;
        }
        default:
            free(event_data);
            break;
    }
}

void beeper_task_sas_matches(beeper_task_t * t)
{
    safe_queue_push(t, BEEPER_TASK_RECEIVED_EVENT_SAS_MATCHES, NULL);
}

void beeper_task_request_messages(beeper_task_t * t, const char * room_id, const char * chunk_id,
                                  beeper_task_direction_t direction)
{
    debug("request messages");

    beeper_received_message_request_t * m_req = beeper_asserting_calloc(1, sizeof(*m_req));
    m_req->room_id = beeper_asserting_strdup(room_id);
    if(chunk_id) m_req->chunk_id = beeper_asserting_strdup(chunk_id);
    m_req->direction = direction;

    safe_queue_push(t, BEEPER_TASK_RECEIVED_EVENT_REQUEST_MESSAGES, m_req);
}
