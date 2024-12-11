#include "server.h"

#include <string.h>

static bool parse_http_method(const char *start, const char *end, enum http_method *method) {
    size_t len = end - start;
    if (len == 3 && memcmp(start, "GET", 3) == 0) {
        *method = HTTP_GET;
        return true;
    }
    if (len == 4 && memcmp(start, "HEAD", 4) == 0) {
        *method = HTTP_HEAD;
        return true;
    }
    return false;
}

enum parse_http_req_result parse_http_req(struct worker *worker, const char *str, struct http_req *req) {
    const char *line_end = strchr(str, '\r');
    if (line_end == NULL) {
        return PARSE_HTTP_INVALID_SYNTAX;
    }

    const char *method = str;
    const char *method_end = strpbrk(method, " ");
    if (method_end == NULL || method_end > line_end)
        return PARSE_HTTP_INVALID_SYNTAX;

    if (!parse_http_method(method, method_end, &req->method))
        return PARSE_HTTP_INVALID_METHOD;

    size_t skip_spaces = strspn(method_end, " ");
    if (skip_spaces == 0)
        return PARSE_HTTP_INVALID_SYNTAX;
    const char *uri = method_end + skip_spaces;
    const char *uri_end = strpbrk(uri, " ");
    if (uri_end == NULL || uri_end > line_end)
        return PARSE_HTTP_INVALID_SYNTAX;

    size_t uri_len = uri_end - uri;
    if (uri_len > worker->settings->uri_length_limit)
        return PARSE_HTTP_URI_TOO_LONG;

    char *uri_str = server_alloc(uri_len + 1);
    req->uri = uri_str;
    memcpy(uri_str, uri, uri_len);
    uri_str[uri_len] = '\0';

    skip_spaces = strspn(method_end, " ");
    if (skip_spaces == 0)
        return PARSE_HTTP_INVALID_SYNTAX;

    const char *protocol = uri_end + skip_spaces;
    const char *protocol_end = line_end;
    size_t protocol_len = protocol_end - protocol;
    if (protocol_len == 8 && memcmp(protocol, "HTTP/1.1", 8) == 0) {
        req->version = HTTP_11;
    } else if (protocol_len == 8 && memcmp(protocol, "HTTP/1.0", 8) == 0) {
        req->version = HTTP_10;
    } else {
        return PARSE_HTTP_INVALID_VERSION;
    }

    return PARSE_HTTP_OK;
}

const char *http_content_type_str(enum http_content_type type) {
    switch (type) {
    case HTTP_CT_BIN: return "application/octet-stream";
    case HTTP_CT_BMP: return "image/bmp";
    case HTTP_CT_CSS: return "text/css";
    case HTTP_CT_CSV: return "text/csv";
    case HTTP_CT_GIF: return "image/gif";
    case HTTP_CT_HTML: return "text/html";
    case HTTP_CT_JPEG: return "image/jpeg";
    case HTTP_CT_JS: return "text/javascript";
    case HTTP_CT_JSON: return "application/json";
    case HTTP_CT_MP3: return "audio/mpeg";
    case HTTP_CT_MP4: return "video/mp4";
    case HTTP_CT_OTF: return "font/otf";
    case HTTP_CT_PNG: return "image/png";
    case HTTP_CT_PDF: return "application/pdf";
    case HTTP_CT_SVG: return "image/svg+xml";
    case HTTP_CT_TTF: return "font/ttf";
    case HTTP_CT_TXT: return "text/plain";
    }
    __builtin_unreachable();
}

enum http_content_type http_conten_type_from_ext(const char *ext) {
    if (ext == NULL)
        return HTTP_CT_BIN;

    if (strcmp(ext, "bin") == 0)
        return HTTP_CT_BIN;
    if (strcmp(ext, "bmp") == 0)
        return HTTP_CT_BMP;
    if (strcmp(ext, "css") == 0)
        return HTTP_CT_CSS;
    if (strcmp(ext, "csv") == 0)
        return HTTP_CT_CSV;
    if (strcmp(ext, "gif") == 0)
        return HTTP_CT_GIF;
    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0)
        return HTTP_CT_HTML;
    if (strcmp(ext, "jpeg") == 0 || strcmp(ext, "jpg") == 0)
        return HTTP_CT_JPEG;
    if (strcmp(ext, "js") == 0 || strcmp(ext, "mjs") == 0)
        return HTTP_CT_JS;
    if (strcmp(ext, "json") == 0)
        return HTTP_CT_JSON;
    if (strcmp(ext, "mp3") == 0)
        return HTTP_CT_MP3;
    if (strcmp(ext, "mp4") == 0)
        return HTTP_CT_MP4;
    if (strcmp(ext, "otf") == 0)
        return HTTP_CT_OTF;
    if (strcmp(ext, "png") == 0)
        return HTTP_CT_PNG;
    if (strcmp(ext, "pdf") == 0)
        return HTTP_CT_PDF;
    if (strcmp(ext, "svg") == 0)
        return HTTP_CT_SVG;
    if (strcmp(ext, "ttf") == 0)
        return HTTP_CT_TTF;
    if (strcmp(ext, "txt") == 0)
        return HTTP_CT_TXT;

    return HTTP_CT_BIN;
}

enum http_content_type http_conten_type_from_filename(const char *name) {
    const char *last = strrchr(name, '.');
    if (last)
        ++last;
    return http_conten_type_from_ext(last);
}

const char *http_status_code_str(enum http_status_code code) {
    switch (code) {
    case HTTP_OK: return "OK";
    case HTTP_BAD_REQUEST: return "Bad Request";
    case HTTP_FORBIDDEN: return "Forbidden";
    case HTTP_NOT_FOUND: return "Not Found";
    case HTTP_METHOD_NOT_ALLOWED: return "Method Not Allowed";
    case HTTP_URI_TOO_LONG: return "URI Too Long";
    case HTTP_INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case HTTP_VERSION_NO_SUPPORTED: return "Version Not Supported";
    }
    __builtin_unreachable();
}

const char *http_method_str(enum http_method method) {
    switch (method) {
    case HTTP_GET: return "GET";
    case HTTP_HEAD: return "HEAD";
    }
    __builtin_unreachable();
}

int http_status_code_int(enum http_status_code code) {
    switch (code) {
    case HTTP_OK: return 200;
    case HTTP_BAD_REQUEST: return 400;
    case HTTP_FORBIDDEN: return 403;
    case HTTP_NOT_FOUND: return 404;
    case HTTP_METHOD_NOT_ALLOWED: return 405;
    case HTTP_URI_TOO_LONG: return 514;
    case HTTP_INTERNAL_SERVER_ERROR: return 500;
    case HTTP_VERSION_NO_SUPPORTED: return 505;
    }
    __builtin_unreachable();
}
