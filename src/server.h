#ifndef SERVER_H
#define SERVER_H

#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pg_list.h"

enum http_version {
    HTTP_10,
    HTTP_11
};

enum http_content_type {
    HTTP_CT_BIN,  // application/octet-stream
    HTTP_CT_BMP,  // image/bmp
    HTTP_CT_CSS,  // text/css
    HTTP_CT_CSV,  // text/csv
    HTTP_CT_GIF,  // image/gif
    HTTP_CT_HTML, // text/html
    HTTP_CT_JPEG, // image/jpeg
    HTTP_CT_JS,   // text/javascript
    HTTP_CT_JSON, // application/json
    HTTP_CT_MP3,  // audio/mpeg
    HTTP_CT_MP4,  // video/mp4
    HTTP_CT_OTF,  // font/otf
    HTTP_CT_PNG,  // image/png
    HTTP_CT_PDF,  // application/pdf
    HTTP_CT_SVG,  // image/svg+xml
    HTTP_CT_TTF,  // font/ttf
    HTTP_CT_TXT,  // text/plain
};

enum http_method {
    HTTP_GET,
    HTTP_HEAD
};

enum http_status_code {
    HTTP_OK, // 200

    HTTP_BAD_REQUEST,        // 400
    HTTP_FORBIDDEN,          // 403
    HTTP_NOT_FOUND,          // 404
    HTTP_METHOD_NOT_ALLOWED, // 405
    HTTP_URI_TOO_LONG,       // 414

    HTTP_INTERNAL_SERVER_ERROR, // 500
    HTTP_VERSION_NO_SUPPORTED,  // 505
};

struct http_header {
    const char *name;
    const char *value;
};

struct http_req {
    enum http_method method;
    const char *uri;
    enum http_version version;
};

struct http_response {
    struct http_req *req;
    int code;
    List *headers;
    size_t body_size;
    char *body;
};

enum log_level {
    LOG_TRACE,
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_FATAL
};

struct memory_arena_block {
    size_t size;
    size_t used;
    char *base;
    struct memory_arena_block *next;
};

struct memory_arena {
    struct memory_arena_block *current_block;
    size_t minimum_block_size;
};

enum parse_http_req_result {
    PARSE_HTTP_OK,
    PARSE_HTTP_INVALID_SYNTAX,
    PARSE_HTTP_INVALID_VERSION,
    PARSE_HTTP_URI_TOO_LONG,
    PARSE_HTTP_INVALID_METHOD,
};

struct server_settings {
    size_t uri_length_limit;
    const char *host;
    int port;
    size_t process_count;
    int listen_backlog;
    size_t read_buf_size;
    size_t req_size_limit;
    const char *static_dir;
    enum log_level log_level;
    const char *log_filename;
    bool log_to_stdout;
};

enum connection_state {
    CONN_WAITING,
    CONN_SENDING,
    CONN_COMPLETE,
    CONN_ERR_RECOVERABLE,
    CONN_ERR_UNRECOVERABLE
};

struct active_connection {
    enum connection_state state;
    struct memory_arena arena;
    int sock_fd;

    int file_fd;
    size_t read_buf_size;
    size_t read_buf_len;
    size_t read_buf_cursor;
    char *read_buf;
};

struct worker {
    List *active_conns;

    const struct server_settings *settings;

    jmp_buf req_jmpbuf;
};

extern jmp_buf *g_err_jmpbuf;
extern struct memory_arena *g_memory_arena;

//
// server.c
//
bool run_server(const struct server_settings *settings);
__attribute__((noreturn)) void abort_req(void);
__attribute__((format(printf, 2, 3))) void log_msg(enum log_level level, const char *fmt, ...);
__attribute__((format(printf, 2, 3))) void log_perror(enum log_level level, const char *fmt, ...);

//
// handler.c
//
void process_request_write(struct active_connection *conn);
void process_request(struct worker *worker, struct active_connection *conn);
void error_response(enum http_status_code code, struct active_connection *conn);
bool set_nonblocking(int fd);

//
// http.c
//
enum parse_http_req_result parse_http_req(struct worker *worker, const char *str, struct http_req *req);
enum http_content_type http_conten_type_from_ext(const char *ext);
enum http_content_type http_conten_type_from_filename(const char *name);
const char *http_content_type_str(enum http_content_type type);
const char *http_status_code_str(enum http_status_code code);
int http_status_code_int(enum http_status_code code);
const char *http_method_str(enum http_method method);

//
// memory.c
//
__attribute__((malloc)) void *arena_alloc(struct memory_arena *arena, size_t size);
void *arena_realloc(struct memory_arena *arena, void *memory, size_t old_size, size_t new_size);
void arena_clear(struct memory_arena *arena);

__attribute__((malloc, returns_nonnull)) void *server_alloc(size_t size);
__attribute__((returns_nonnull)) void *server_realloc(void *memory, size_t old_size, size_t size);
__attribute__((malloc, returns_nonnull)) char *server_strdup(const char *src);
__attribute__((malloc, returns_nonnull, format(printf, 1, 2))) char *server_memfmt(const char *fmt, ...);
void server_free(void *memory);

#endif
