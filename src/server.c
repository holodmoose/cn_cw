#include "server.h"
#include "pg_list.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct memory_arena *g_memory_arena = NULL;
jmp_buf *g_err_jmpbuf = NULL;

struct master_state {
    const struct server_settings *settings;
    int sock_fd;
    size_t pid_count;
    pid_t *pids;
};

static bool validate_settings(const struct server_settings *settings) {
    if (settings->process_count == 0) {
        log_msg(LOG_FATAL, "invalid process count %zu", settings->process_count);
        return false;
    }
    if (settings->uri_length_limit == 0) {
        log_msg(LOG_FATAL, "invalid uri length limit (most be nonzero)");
        return false;
    }
    if (settings->listen_backlog == 0) {
        log_msg(LOG_FATAL, "listen backlog size too small");
        return false;
    }

    return true;
}

static bool init_socket(const struct server_settings *settings, int *sock_fd_p) {
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        log_perror(LOG_FATAL, "failed to create socket");
        return false;
    }
    if (!set_nonblocking(sock_fd)) {
        log_perror(LOG_FATAL, "failed to set socket nonblocking");
        close(sock_fd);
        return false;
    }

    int opt = 1;
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        log_perror(LOG_FATAL, "setsockopt error");
        close(sock_fd);
        return false;
    }
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        log_perror(LOG_FATAL, "setsockopt error");
        close(sock_fd);
        return false;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_len = sizeof(addr);
    addr.sin_port = htons(settings->port);
    addr.sin_addr.s_addr = inet_addr(settings->host);
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        log_perror(LOG_FATAL, "failed to bind socket to address");
        close(sock_fd);
        return false;
    }
    if (listen(sock_fd, settings->listen_backlog) == -1) {
        log_perror(LOG_FATAL, "listen failed");
        close(sock_fd);
        return false;
    }
    *sock_fd_p = sock_fd;
    return true;
}

// static bool init_logging(struct master_state *state) {
//     const struct server_settings *settings = state->settings;
// }

static bool init_master(const struct server_settings *settings, struct master_state *state) {
    state->settings = settings;
    state->pid_count = settings->process_count;
    state->pids = malloc(settings->process_count * sizeof(*state->pids));
    if (state->pids == NULL) {
        log_msg(LOG_FATAL, "failed to allocate memory");
        return false;
    }
    for (size_t i = 0; i < settings->process_count; ++i)
        state->pids[i] = -1;

    if (!init_socket(settings, &state->sock_fd)) {
        free(state->pids);
        return false;
    }
    return true;
}

static void kill_workers(struct master_state *state) {
    for (size_t i = 0; i < state->pid_count; ++i) {
        int pid = state->pids[i];
        if (pid != -1) {
            kill(pid, SIGKILL);
            int stat;
            pid_t waited = waitpid(pid, &stat, 0);
            if (waited == -1) {
                // ???
            }
        }
    }
}

static void conn_loop(struct worker *worker, struct master_state *master) {
    fd_set read_fset, write_fset;
    FD_ZERO(&read_fset);
    FD_ZERO(&write_fset);
    FD_SET(master->sock_fd, &read_fset);
    int max_fd = master->sock_fd;
    foreach (lc, worker->active_conns) {
        struct active_connection *conn = lfirst(lc);
        switch (conn->state) {
        case CONN_WAITING: FD_SET(conn->sock_fd, &read_fset); break;
        case CONN_SENDING: FD_SET(conn->sock_fd, &write_fset); break;
        case CONN_COMPLETE:
        case CONN_ERR_UNRECOVERABLE:
        case CONN_ERR_RECOVERABLE: assert(0); break;
        }
        if (conn->sock_fd > max_fd)
            max_fd = conn->sock_fd;
    }

    int ret = select(max_fd + 1, &read_fset, &write_fset, NULL, NULL);
    if (ret == -1 && errno == EINTR) {
        return;
    }
    if (ret == -1) {
        log_perror(LOG_FATAL, "select failed");
        exit(EXIT_FAILURE);
    }

    List *new_conns = NIL;
    if (FD_ISSET(master->sock_fd, &read_fset)) {
        socklen_t addrlen;
        struct sockaddr_in client_addr;
        int fd = accept(master->sock_fd, (struct sockaddr *)&client_addr, &addrlen);
        if (fd == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        } else if (fd == -1) {
            log_perror(LOG_FATAL, "accept failed");
            exit(EXIT_FAILURE);
        } else {
            struct active_connection *conn = server_alloc(sizeof(*conn));
            memset(conn, 0, sizeof(*conn));
            conn->sock_fd = fd;
            conn->state = CONN_WAITING;
            conn->file_fd = -1;
            new_conns = lappend(new_conns, conn);
        }
    }

    foreach (lc, worker->active_conns) {
        struct active_connection *conn = lfirst(lc);

        switch (conn->state) {
        case CONN_WAITING: {
            if (!FD_ISSET(conn->sock_fd, &read_fset)) {
                new_conns = lappend(new_conns, conn);
                continue;
            }

            g_memory_arena = &conn->arena;
            if (setjmp(worker->req_jmpbuf) == 0) {
                process_request(worker, conn);
            }
            g_memory_arena = NULL;
            break;
        }
        case CONN_SENDING: {
            if (!FD_ISSET(conn->sock_fd, &write_fset)) {
                new_conns = lappend(new_conns, conn);
                continue;
            }
            g_memory_arena = &conn->arena;
            if (setjmp(worker->req_jmpbuf) == 0) {
                process_request_write(conn);
            }
            g_memory_arena = NULL;
            break;
        }
        case CONN_COMPLETE:
        case CONN_ERR_UNRECOVERABLE:
        case CONN_ERR_RECOVERABLE: assert(0); break;
        }

        switch (conn->state) {
        case CONN_WAITING: assert(0); break;
        case CONN_SENDING:
            assert(g_memory_arena == NULL);
            new_conns = lappend(new_conns, conn);
            break;
        case CONN_COMPLETE: {
        complete:
            assert(g_memory_arena == NULL);
            if (conn->file_fd != -1)
                close(conn->file_fd);
            close(conn->sock_fd);
            arena_clear(&conn->arena);
            server_free(conn);
            break;
        }
        case CONN_ERR_RECOVERABLE: {
            g_memory_arena = &conn->arena;
            if (setjmp(worker->req_jmpbuf) == 0) {
                error_response(HTTP_INTERNAL_SERVER_ERROR, conn);
            }
            g_memory_arena = NULL;
            goto complete;
        }
        case CONN_ERR_UNRECOVERABLE:
            log_msg(LOG_ERROR, "uncrecoverable error occured, aborting connection");
            goto complete;
        }
    }
    list_free(worker->active_conns);
    worker->active_conns = new_conns;
}

__attribute__((noreturn)) static void run_child(struct master_state *master) {
    struct worker worker = {0};
    worker.settings = master->settings;

    g_memory_arena = NULL;
    g_err_jmpbuf = &worker.req_jmpbuf;

    log_msg(LOG_INFO, "accepting connections on address %s:%d", worker.settings->host, worker.settings->port);

    for (;;) {
        conn_loop(&worker, master);
    }
}

static bool run_master(struct master_state *state) {
    log_msg(LOG_INFO, "creating %zu workers", state->settings->process_count);
    for (size_t i = 0; i < state->settings->process_count; ++i) {
        pid_t pid = fork();
        if (pid == -1) {
            log_perror(LOG_FATAL, "fork failed");
            kill_workers(state);
            return false;
        }
        if (pid == 0) {
            run_child(state);
        } else {
            log_msg(LOG_INFO, "created worker with pid %d", pid);
            state->pids[i] = pid;
        }
    }
    for (;;) {
        sleep(100);
    }
    return true;
}

bool run_server(const struct server_settings *settings) {
    log_msg(LOG_INFO, "initializing master");
    if (!validate_settings(settings))
        return false;

    log_msg(LOG_INFO, "validated settings");
    struct master_state state = {0};
    if (!init_master(settings, &state)) {
        return false;
    }
    log_msg(LOG_INFO, "initialized master");
    return run_master(&state);
}

__attribute__((noreturn)) void abort_req(void) {
    longjmp(*g_err_jmpbuf, 1);
}

static const char *log_level_str(enum log_level level) {
    switch (level) {
    case LOG_TRACE: return "trace";
    case LOG_INFO: return "info";
    case LOG_WARN: return "warn";
    case LOG_ERROR: return "error";
    case LOG_FATAL: return "fatal";
    }
    __builtin_unreachable();
}

static char *csstrerror(char *buf, size_t buf_size, int err) {
    char *err_msg;
#ifdef _GNU_SOURCE
    err_msg = strerror_r(err, buf, buf_size);
#else
    strerror_r(err, buf, buf_size);
    err_msg = buf;
#endif
    return err_msg;
}

__attribute__((format(printf, 2, 3))) void log_msg(enum log_level level, const char *fmt, ...) {
    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char buffer[4096];
    int len =
        snprintf(buffer, sizeof(buffer), "%d %d.%d.%d %02d:%02d:%02d [%s]: %s\n", getpid(), tm->tm_mday, tm->tm_mon + 1,
                 tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec, log_level_str(level), msg);
    write(STDERR_FILENO, buffer, len);
}

__attribute__((format(printf, 2, 3))) void log_perror(enum log_level level, const char *fmt, ...) {
    int err = errno;
    char err_buf[4096];
    char *err_str = csstrerror(err_buf, sizeof(err_buf), err);

    char msg[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char buffer[4096];
    int len =
        snprintf(buffer, sizeof(buffer), "%d %d.%d.%d %02d:%02d:%02d [%s]: %s: %s\n", getpid(), tm->tm_mday, tm->tm_mon,
                 tm->tm_year + 1900, tm->tm_hour, tm->tm_min, tm->tm_sec, log_level_str(level), msg, err_str);
    write(STDERR_FILENO, buffer, len);
}
