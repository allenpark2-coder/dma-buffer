/* ipc/vfr_registry.c — Stream Registry 實作
 *
 * Daemon 端：單執行緒 epoll event loop，無 mutex（所有操作序列化）。
 * Client 端（producer / consumer）：connect → send req → recv reply → close。
 *   每次操作建立新連線（無長連線），避免 fd 洩漏與複雜的狀態管理。
 */

#include "ipc/vfr_registry.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdatomic.h>

/* ─── 內部：建立 Unix abstract socket 連線 ──────────────────────────────── */
static int reg_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* abstract namespace：path[0] = '\0'，之後為名稱 */
    memcpy(addr.sun_path, VFR_REGISTRY_SOCKET_PATH,
           strlen(VFR_REGISTRY_SOCKET_PATH + 1) + 2);

    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                    + strlen(VFR_REGISTRY_SOCKET_PATH + 1) + 1);

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ─── 內部：send request + recv reply ──────────────────────────────────── */
static int reg_transact(const vfr_reg_req_t *req, vfr_reg_reply_t *reply)
{
    int fd = reg_connect();
    if (fd < 0) {
        /* daemon 未啟動 — 靜默失敗 */
        VFR_LOGW("registry: daemon not available");
        return -1;
    }

    ssize_t n = send(fd, req, sizeof(*req), MSG_NOSIGNAL);
    if (n != (ssize_t)sizeof(*req)) {
        VFR_LOGE("registry: send failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    n = recv(fd, reply, sizeof(*reply), 0);
    close(fd);

    if (n < (ssize_t)(offsetof(vfr_reg_reply_t, entries))) {
        VFR_LOGE("registry: short reply (%zd bytes)", n);
        return -1;
    }
    return reply->status;
}

/* ─── Producer API ───────────────────────────────────────────────────────── */

int vfr_registry_register(const vfr_stream_info_t *info)
{
    if (!info) return -1;

    vfr_reg_req_t req = { .opcode = VFR_REG_OP_REGISTER, .info = *info };
    vfr_reg_reply_t reply;
    int ret = reg_transact(&req, &reply);
    if (ret == 0)
        VFR_LOGI("registry: registered stream '%s'", info->stream_name);
    return ret;
}

int vfr_registry_unregister(const char *stream_name)
{
    if (!stream_name) return -1;

    vfr_reg_req_t req;
    memset(&req, 0, sizeof(req));
    req.opcode = VFR_REG_OP_UNREGISTER;
    size_t nlen = strlen(stream_name);
    if (nlen >= VFR_SOCKET_NAME_MAX) return -1;
    memcpy(req.info.stream_name, stream_name, nlen + 1);

    vfr_reg_reply_t reply;
    int ret = reg_transact(&req, &reply);
    if (ret == 0)
        VFR_LOGI("registry: unregistered stream '%s'", stream_name);
    return ret;
}

/* ─── Consumer API ───────────────────────────────────────────────────────── */

int vfr_registry_list(vfr_stream_info_t *entries, uint32_t max_count)
{
    if (!entries || max_count == 0) return -1;

    vfr_reg_req_t req;
    memset(&req, 0, sizeof(req));
    req.opcode = VFR_REG_OP_LIST;

    vfr_reg_reply_t reply;
    if (reg_transact(&req, &reply) < 0) return -1;

    uint32_t count = reply.count;
    if (count > max_count) count = max_count;
    if (count > VFR_REGISTRY_MAX_STREAMS) count = VFR_REGISTRY_MAX_STREAMS;

    memcpy(entries, reply.entries, count * sizeof(vfr_stream_info_t));
    VFR_LOGI("registry: list returned %u stream(s)", count);
    return (int)count;
}

/* ─── Daemon 內部狀態 ────────────────────────────────────────────────────── */

typedef struct {
    vfr_stream_info_t entries[VFR_REGISTRY_MAX_STREAMS];
    uint32_t          count;
} reg_state_t;

static volatile sig_atomic_t g_reg_running = 1;

static void reg_sig_handler(int sig)
{
    (void)sig;
    g_reg_running = 0;
}

/* ─── Daemon：處理一個 client 請求 ─────────────────────────────────────── */
static void reg_handle_client(reg_state_t *state, int client_fd)
{
    vfr_reg_req_t req;
    ssize_t n = recv(client_fd, &req, sizeof(req), 0);
    if (n != (ssize_t)sizeof(req)) {
        VFR_LOGW("registry daemon: bad request size %zd", n);
        close(client_fd);
        return;
    }

    vfr_reg_reply_t reply;
    memset(&reply, 0, sizeof(reply));
    reply.status = 0;

    switch (req.opcode) {
        case VFR_REG_OP_REGISTER: {
            /* 搜尋是否已存在同名 stream（更新）*/
            bool found = false;
            for (uint32_t i = 0; i < state->count; i++) {
                if (strncmp(state->entries[i].stream_name,
                            req.info.stream_name,
                            VFR_SOCKET_NAME_MAX) == 0) {
                    state->entries[i] = req.info;
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (state->count < VFR_REGISTRY_MAX_STREAMS) {
                    state->entries[state->count++] = req.info;
                } else {
                    VFR_LOGW("registry daemon: table full (%d)", VFR_REGISTRY_MAX_STREAMS);
                    reply.status = -1;
                }
            }
            VFR_LOGI("registry daemon: REGISTER '%s' (total=%u)",
                     req.info.stream_name, state->count);
            break;
        }

        case VFR_REG_OP_UNREGISTER: {
            for (uint32_t i = 0; i < state->count; i++) {
                if (strncmp(state->entries[i].stream_name,
                            req.info.stream_name,
                            VFR_SOCKET_NAME_MAX) == 0) {
                    /* 以最後一項填補（order 不重要）*/
                    state->entries[i] = state->entries[--state->count];
                    VFR_LOGI("registry daemon: UNREGISTER '%s' (total=%u)",
                             req.info.stream_name, state->count);
                    goto done_unreg;
                }
            }
            VFR_LOGW("registry daemon: UNREGISTER '%s' not found",
                     req.info.stream_name);
            reply.status = -1;
done_unreg:
            break;
        }

        case VFR_REG_OP_LIST: {
            reply.count = state->count;
            memcpy(reply.entries, state->entries,
                   state->count * sizeof(vfr_stream_info_t));
            VFR_LOGI("registry daemon: LIST → %u entry(s)", state->count);
            break;
        }

        default:
            VFR_LOGW("registry daemon: unknown opcode %u", req.opcode);
            reply.status = -1;
            break;
    }

    send(client_fd, &reply, sizeof(reply), MSG_NOSIGNAL);
    close(client_fd);
}

/* ─── Daemon：主迴圈 ─────────────────────────────────────────────────────── */
int vfr_registry_serve_forever(void)
{
    /* sigaction（禁止 signal()，原則四）*/
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reg_sig_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* listen socket */
    int listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        VFR_LOGE("registry daemon: socket: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, VFR_REGISTRY_SOCKET_PATH,
           strlen(VFR_REGISTRY_SOCKET_PATH + 1) + 2);

    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                    + strlen(VFR_REGISTRY_SOCKET_PATH + 1) + 1);

    if (bind(listen_fd, (struct sockaddr *)&addr, addrlen) < 0) {
        VFR_LOGE("registry daemon: bind: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 16) < 0) {
        VFR_LOGE("registry daemon: listen: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0) {
        VFR_LOGE("registry daemon: epoll_create1: %s", strerror(errno));
        close(listen_fd);
        return -1;
    }

    struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    reg_state_t state;
    memset(&state, 0, sizeof(state));

    VFR_LOGI("registry daemon started (socket: \\0/vfr/.registry)");

    struct epoll_event events[4];
    while (g_reg_running) {
        int nev = epoll_wait(epfd, events, 4, 500);
        if (nev < 0) {
            if (errno == EINTR) continue;
            VFR_LOGE("registry daemon: epoll_wait: %s", strerror(errno));
            break;
        }
        for (int i = 0; i < nev; i++) {
            if (events[i].data.fd == listen_fd) {
                int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
                if (client >= 0) {
                    reg_handle_client(&state, client);
                }
            }
        }
    }

    VFR_LOGI("registry daemon: shutting down");
    close(epfd);
    close(listen_fd);
    return 0;
}
