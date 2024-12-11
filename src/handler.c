#include "pg_list.h"
#include "server.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum read_req_data_result {
    READ_REQ_DATA_OK,
    READ_REQ_DATA_EMPTY,
    READ_REQ_DATA_TOO_LARGE,
};

enum get_head_info_result {
    HEAD_INFO_OK,
    HEAD_INFO_NOT_FOUND,
    HEAD_INFO_FORBIDDEN,
    HEAD_INFO_OUTSIDE_DIR,
};

static enum read_req_data_result read_req_data(struct worker *worker, struct active_connection *conn, char **req_data) {
    size_t size = worker->settings->read_buf_size;
    assert(conn->read_buf == NULL);
    assert(conn->read_buf_size == 0);
    conn->read_buf = server_alloc(size);
    conn->read_buf_size = size;

    ssize_t nread = read(conn->sock_fd, conn->read_buf, conn->read_buf_size - 1);
    if (nread < 0) {
        log_perror(LOG_ERROR, "read socket failed");
        conn->state = CONN_ERR_RECOVERABLE;
        abort_req();
    }
    if (nread == 0) {
        return READ_REQ_DATA_EMPTY;
    }
    conn->read_buf[nread] = '\0';
    *req_data = conn->read_buf;
    return READ_REQ_DATA_OK;
}

static struct http_header *make_header(const char *name, const char *value) {
    struct http_header *header = server_alloc(sizeof(*header));
    header->name = name;
    header->value = value;
    return header;
}

static void http_response_date(char *buf, size_t buf_len, struct tm *tm) {
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    snprintf(buf, buf_len, "%s, %d %s %d %02d:%02d:%02d GMT", days[tm->tm_wday], tm->tm_mday, months[tm->tm_mon],
             tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec);
}
static struct http_header *make_date_header(void) {
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);

    char buffer[4096] = {0};
    http_response_date(buffer, sizeof(buffer), t);
    return make_header("Date", server_strdup(buffer));
}

void process_request_write(struct active_connection *conn) {
    assert(conn->file_fd != -1);
    for (;;) {
        if (conn->read_buf_len == 0 || conn->read_buf_cursor == conn->read_buf_len) {
            ssize_t nread = read(conn->file_fd, conn->read_buf, conn->read_buf_size);
            if (nread == 0) {
                conn->state = CONN_COMPLETE;
                break;
            }
            if (nread == -1) {
                log_perror(LOG_ERROR, "failed to read from file");
                conn->state = CONN_ERR_UNRECOVERABLE;
                abort_req();
            }
            conn->read_buf_len = nread;
            conn->read_buf_cursor = 0;
        }

        assert(conn->read_buf_len > conn->read_buf_cursor);
        ssize_t nwritten =
            write(conn->sock_fd, conn->read_buf + conn->read_buf_cursor, conn->read_buf_len - conn->read_buf_cursor);
        if (nwritten == -1 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            break;
        }
        if (nwritten == -1) {
            log_perror(LOG_ERROR, "failed to write to socket");
            conn->state = CONN_ERR_UNRECOVERABLE;
            abort_req();
        }
        conn->read_buf_cursor += nwritten;
    }
}

bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void send_response(struct http_response *resp, struct active_connection *conn) {
    char buffer[4096];
    int cursor = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d %s\r\n", http_status_code_int(resp->code),
                          http_status_code_str(resp->code));
    foreach (lc, resp->headers) {
        const struct http_header *header = lfirst(lc);
        int c = snprintf(buffer + cursor, sizeof(buffer) - cursor, "%s: %s\r\n", header->name, header->value);
        cursor += c;
    }
    if (resp->req && resp->req->method != HTTP_HEAD) {
        int c = snprintf(buffer + cursor, sizeof(buffer) - cursor, "\r\n");
        cursor += c;
    }

    ssize_t nwritten = write(conn->sock_fd, buffer, cursor);
    if (nwritten == -1) {
        log_perror(LOG_ERROR, "failed to write to socket");
        conn->state = CONN_ERR_UNRECOVERABLE;
        abort_req();
    }

    if (!resp->req || resp->req->method == HTTP_HEAD) {
        conn->state = CONN_COMPLETE;
        return;
    }

    if (!set_nonblocking(conn->sock_fd)) {
        log_perror(LOG_ERROR, "failed to change socket to nonblocking mode");
        conn->state = CONN_ERR_UNRECOVERABLE;
        abort_req();
    }

    if (conn->file_fd != -1) {
        conn->state = CONN_SENDING;
        process_request_write(conn);
    } else if (resp->body) {
        if (write(conn->sock_fd, resp->body, resp->body_size) != (ssize_t)resp->body_size) {
            log_perror(LOG_ERROR, "failed to write to socket");
            conn->state = CONN_ERR_UNRECOVERABLE;
            abort_req();
        }
        conn->state = CONN_COMPLETE;
    } else {
        conn->state = CONN_COMPLETE;
    }
}

void error_response(enum http_status_code code, struct active_connection *conn) {
    log_msg(LOG_INFO, "error response %d", http_status_code_int(code));
    struct http_response resp = {0};
    resp.req = NULL;
    resp.code = code;
    resp.headers = lappend(resp.headers, make_date_header());
    resp.headers = lappend(resp.headers, make_header("Content-Length", "0"));
    send_response(&resp, conn);
}

struct file_info {
    size_t size;
    enum http_content_type ct;
};

static bool get_file_info(const char *full_path, struct active_connection *conn, struct file_info *info) {
    struct stat st;
    if (stat(full_path, &st) < 0) {
        int err = errno;
        switch (err) {
        case EACCES: error_response(HTTP_FORBIDDEN, conn); return false;
        case ENOTDIR:
        case ENOENT: error_response(HTTP_NOT_FOUND, conn); return false;
        default: error_response(HTTP_INTERNAL_SERVER_ERROR, conn); return false;
        }
    }

    info->ct = http_conten_type_from_filename(full_path);
    info->size = st.st_size;
    return true;
}

static const char *resolve_path(const char *uri, struct worker *worker, struct active_connection *conn) {
    if (strcmp(uri, "/") == 0 || strcmp(uri, "") == 0) {
        uri = "index.html";
    }
    
    char path_buf[4096];
    snprintf(path_buf, sizeof(path_buf), "%s/%s", worker->settings->static_dir, uri);

    char *full_path = realpath(path_buf, NULL);
    if (full_path == NULL) {
        int err = errno;
        switch (err) {
        case EACCES: error_response(HTTP_FORBIDDEN, conn); return NULL;
        case ENOTDIR:
        case ENOENT: error_response(HTTP_NOT_FOUND, conn); return NULL;
        default: error_response(HTTP_INTERNAL_SERVER_ERROR, conn); return NULL;
        }
    }
    if (strncmp(full_path, worker->settings->static_dir, strlen(worker->settings->static_dir)) != 0) {
        log_msg(LOG_WARN, "attempt to access file outside of static directory");
        error_response(HTTP_FORBIDDEN, conn);
        free(full_path);
        return NULL;
    }

    size_t len = strlen(full_path);
    char *str = arena_alloc(g_memory_arena, len + 1);
    if (!str) {
        free(full_path);
        error_response(HTTP_INTERNAL_SERVER_ERROR, conn);
        return NULL;
    }
    memcpy(str, full_path, len);
    str[len] = '\0';
    free(full_path);
    return str;
}

static void serve_head_request(struct http_req *req, struct worker *worker, struct active_connection *conn) {
    const char *full_path = resolve_path(req->uri, worker, conn);
    if (!full_path)
        return;

    struct file_info info;
    if (!get_file_info(full_path, conn, &info))
        return;

    struct http_response resp = {0};
    resp.req = req;
    resp.code = HTTP_OK;
    resp.headers = lappend(resp.headers, make_date_header());
    resp.headers = lappend(resp.headers, make_header("Content-Length", server_memfmt("%zu", info.size)));
    resp.headers = lappend(resp.headers, make_header("Content-Type", http_content_type_str(info.ct)));
    resp.headers = lappend(resp.headers, make_header("Connection", "Close"));
    send_response(&resp, conn);
}

static void serve_get_request(struct http_req *req, struct worker *worker, struct active_connection *conn) {
    const char *full_path = resolve_path(req->uri, worker, conn);
    if (!full_path)
        return;

    struct file_info info;
    if (!get_file_info(full_path, conn, &info))
        return;

    int fd = open(full_path, O_RDONLY);
    if (fd == -1) {
        int err = errno;
        switch (err) {
        case EACCES: error_response(HTTP_FORBIDDEN, conn); return;
        case ENOTDIR:
        case ENOENT: error_response(HTTP_NOT_FOUND, conn); return;
        default: error_response(HTTP_INTERNAL_SERVER_ERROR, conn); return;
        }
    }
    assert(conn->file_fd == -1);
    conn->file_fd = fd;

    struct http_response resp = {0};
    resp.req = req;
    resp.code = HTTP_OK;
    resp.headers = lappend(resp.headers, make_date_header());
    resp.headers = lappend(resp.headers, make_header("Content-Length", server_memfmt("%zu", info.size)));
    resp.headers = lappend(resp.headers, make_header("Content-Type", http_content_type_str(info.ct)));
    resp.headers = lappend(resp.headers, make_header("Connection", "Close"));
    resp.body_size = info.size;
    send_response(&resp, conn);
}

static void serve_request(struct http_req *req, struct worker *worker, struct active_connection *conn) {
    switch (req->method) {
    case HTTP_GET: serve_get_request(req, worker, conn); break;
    case HTTP_HEAD: serve_head_request(req, worker, conn); break;
    }
}

void process_request(struct worker *worker, struct active_connection *conn) {
    char *req_data = NULL;
    enum read_req_data_result read_result = read_req_data(worker, conn, &req_data);
    switch (read_result) {
    case READ_REQ_DATA_OK: break;
    case READ_REQ_DATA_EMPTY: return;
    case READ_REQ_DATA_TOO_LARGE:
        log_msg(LOG_WARN, "request too large");
        error_response(HTTP_BAD_REQUEST, conn);
        break;
    }

    struct http_req req;
    enum parse_http_req_result parse_result = parse_http_req(worker, req_data, &req);
    switch (parse_result) {
    case PARSE_HTTP_OK: break;
    case PARSE_HTTP_INVALID_SYNTAX:
        log_msg(LOG_WARN, "invalid request syntax %s", req_data);
        error_response(HTTP_BAD_REQUEST, conn);
        return;
    case PARSE_HTTP_INVALID_VERSION:
        log_msg(LOG_WARN, "invalid request version");
        error_response(HTTP_VERSION_NO_SUPPORTED, conn);
        return;
    case PARSE_HTTP_URI_TOO_LONG:
        log_msg(LOG_WARN, "uri too long");
        error_response(HTTP_URI_TOO_LONG, conn);
        return;
    case PARSE_HTTP_INVALID_METHOD:
        log_msg(LOG_WARN, "invalid method");
        error_response(HTTP_METHOD_NOT_ALLOWED, conn);
        return;
    }

    serve_request(&req, worker, conn);
}
