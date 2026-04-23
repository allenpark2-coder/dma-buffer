/* ipc/vfr_server.c — VFR IPC Server 實作（Producer 端）
 *
 * 功能：
 *   1. 建立 Unix abstract namespace socket 並接受 consumer 連線
 *   2. 握手協議：驗證 vfr_client_hello_t，發送 vfr_handshake_t + vfr_shm_header_t
 *   3. 透過 SCM_RIGHTS 將 dma_fd dispatch 給所有已連線的 consumer
 *   4. 接收 vfr_release_msg_t，驅動 pool refcount 遞減與 slot 回收
 *
 * 設計（POOL_DESIGN.md §七）：
 *   - 單執行緒 event loop（epoll 驅動），所有 IPC 事件序列化
 *   - Socket 型別：SOCK_SEQPACKET（保留訊息邊界，SCM_RIGHTS 原子傳遞）
 *   - Phase 4 watchdog thread 尚未實作（pidfd = -1）
 */

#include "ipc/vfr_server.h"
#include "ipc/vfr_ipc_types.h"
#include "core/vfr_pool.h"
#include "core/vfr_sync.h"
#include "platform/platform_adapter.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <poll.h>
#include <stdbool.h>

/* ─── Consumer Session（POOL_DESIGN.md §3.3）────────────────────────────── */
typedef struct {
    int      socket_fd;
    int      eventfd;     /* Phase 3：producer→consumer 新幀通知（EFD_SEMAPHORE）*/
    uint32_t session_id;
    uint32_t policy;      /* Phase 3：vfr_backpressure_t */
    uint32_t refslot[VFR_MAX_CONSUMER_SLOTS];  /* 此 consumer 持有的 slot id */
    uint32_t refslot_count;
    bool     active;
} consumer_session_t;

/* ─── Server 完整定義（opaque 給外部）──────────────────────────────────── */
struct vfr_server {
    char                 stream_name[VFR_SOCKET_NAME_MAX];
    int                  listen_fd;
    int                  epoll_fd;
    vfr_pool_t          *pool;
    vfr_shm_header_t     shm_hdr;
    uint32_t             next_session_id;
    consumer_session_t   sessions[VFR_MAX_CONSUMERS];
    uint32_t             session_count;
};

/* ─── Forward declarations ──────────────────────────────────────────────── */
static void handle_accepted_client(struct vfr_server *srv, int client_fd);
static void teardown_session(struct vfr_server *srv, consumer_session_t *sess);
static void handle_release_msg(struct vfr_server *srv, int client_fd);
static void force_release_slot(struct vfr_server *srv, consumer_session_t *sess, uint32_t slot_id);
static int  wait_for_consumer_free(struct vfr_server *srv, consumer_session_t *sess);
static int  dispatch_to_session(struct vfr_server *srv __attribute__((unused)),
                                 consumer_session_t *sess, uint32_t slot_idx,
                                 const vfr_frame_t *meta, int producer_dma_fd);

/* ─── helper：可靠發送（EINTR 安全）────────────────────────────────────── */
static ssize_t send_all(int fd, const void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = send(fd, (const char *)buf + done, len - done, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* ─── helper：找空閒 session slot ──────────────────────────────────────── */
static consumer_session_t *find_free_session(struct vfr_server *srv)
{
    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        if (!srv->sessions[i].active) return &srv->sessions[i];
    }
    return NULL;
}

/* ─── helper：根據 fd 找 session ───────────────────────────────────────── */
static consumer_session_t *find_session_by_fd(struct vfr_server *srv, int fd)
{
    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        if (srv->sessions[i].active && srv->sessions[i].socket_fd == fd)
            return &srv->sessions[i];
    }
    return NULL;
}

/* ─── helper：根據 session_id 找 session ─────────────────────────────── */
static consumer_session_t *find_session_by_id(struct vfr_server *srv, uint32_t sid)
{
    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        if (srv->sessions[i].active && srv->sessions[i].session_id == sid)
            return &srv->sessions[i];
    }
    return NULL;
}

/* ─── teardown_session（原則五順序）────────────────────────────────────── */
static void teardown_session(struct vfr_server *srv, consumer_session_t *sess)
{
    if (!sess || !sess->active) return;

    VFR_LOGI("teardown session_id=%u fd=%d (refslot_count=%u)",
             sess->session_id, sess->socket_fd, sess->refslot_count);

    /* 1. 從 epoll 登出（close(fd) 之前）*/
    epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, sess->socket_fd, NULL);

    /* 2. 回收此 consumer 持有的所有 slot */
    for (uint32_t i = 0; i < sess->refslot_count; i++) {
        vfr_pool_server_release(srv->pool, sess->refslot[i], 0 /* seq=0=force */);
    }
    sess->refslot_count = 0;

    /* 3. close client fd（觸發 consumer 端 EPOLLHUP）*/
    close(sess->socket_fd);
    sess->socket_fd = -1;

    /* 4. Phase 3：close eventfd */
    vfr_sync_close_eventfd(&sess->eventfd);

    sess->active = false;

    if (srv->session_count > 0) srv->session_count--;
}

/* ─── handle_accepted_client：握手 + 加入 epoll ─────────────────────── */
static void handle_accepted_client(struct vfr_server *srv, int client_fd)
{
    if (srv->session_count >= VFR_MAX_CONSUMERS) {
        VFR_LOGW("max consumers (%u) reached, rejecting fd=%d",
                 VFR_MAX_CONSUMERS, client_fd);
        close(client_fd);
        return;
    }

    /* Step 1: 接收 vfr_client_hello_t */
    vfr_client_hello_t hello;
    ssize_t n = recv(client_fd, &hello, sizeof(hello), MSG_WAITALL);
    if (n != (ssize_t)sizeof(hello)) {
        VFR_LOGW("short hello (got %zd expect %zu) fd=%d", n, sizeof(hello), client_fd);
        close(client_fd);
        return;
    }
    if (hello.magic != VFR_SHM_MAGIC) {
        VFR_LOGW("bad magic 0x%08x on fd=%d", hello.magic, client_fd);
        close(client_fd);
        return;
    }

    /* Step 2: 版本驗證 */
    if (hello.proto_version != VFR_PROTO_VERSION) {
        VFR_LOGE("proto_version mismatch: got %u, expect %u", hello.proto_version, VFR_PROTO_VERSION);
        vfr_error_response_t err = { .magic = VFR_SHM_MAGIC, .error = 1 };
        send_all(client_fd, &err, sizeof(err));
        close(client_fd);
        return;
    }

    /* Step 3: 分配 session */
    consumer_session_t *sess = find_free_session(srv);
    if (!sess) {
        VFR_LOGE("no free session slot");
        close(client_fd);
        return;
    }

    /* Phase 3：建立 eventfd（producer→consumer 新幀通知）*/
    int evfd = vfr_sync_create_eventfd();
    if (evfd < 0) {
        VFR_LOGE("vfr_sync_create_eventfd failed");
        close(client_fd);
        return;
    }

    uint32_t sid = ++srv->next_session_id;
    memset(sess, 0, sizeof(*sess));
    sess->socket_fd     = client_fd;
    sess->eventfd       = evfd;
    sess->session_id    = sid;
    sess->policy        = hello.policy;   /* Phase 3：記錄 backpressure policy */
    sess->refslot_count = 0;
    sess->active        = true;
    srv->session_count++;

    /* Step 4: 發送 vfr_handshake_t */
    vfr_handshake_t shake = {
        .magic         = VFR_SHM_MAGIC,
        .proto_version = VFR_PROTO_VERSION,
        .header_size   = (uint16_t)sizeof(vfr_shm_header_t),
        .session_id    = sid,
        ._pad          = 0,
    };
    if (send_all(client_fd, &shake, sizeof(shake)) < 0) {
        VFR_LOGE("send handshake: %s", strerror(errno));
        teardown_session(srv, sess);
        return;
    }

    /* Step 5: 發送 vfr_shm_header_t */
    if (send_all(client_fd, &srv->shm_hdr, sizeof(srv->shm_hdr)) < 0) {
        VFR_LOGE("send shm_header: %s", strerror(errno));
        teardown_session(srv, sess);
        return;
    }

    /* Step 5b: Phase 3 — 發送 vfr_eventfd_setup_t + eventfd（SCM_RIGHTS）*/
    {
        vfr_eventfd_setup_t evsetup = { .magic = VFR_SHM_MAGIC, ._pad = 0 };
        struct iovec iov_ev = { .iov_base = &evsetup, .iov_len = sizeof(evsetup) };
        union {
            char           buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsg_buf;
        memset(&cmsg_buf, 0, sizeof(cmsg_buf));

        struct msghdr mhdr_ev;
        memset(&mhdr_ev, 0, sizeof(mhdr_ev));
        mhdr_ev.msg_iov        = &iov_ev;
        mhdr_ev.msg_iovlen     = 1;
        mhdr_ev.msg_control    = cmsg_buf.buf;
        mhdr_ev.msg_controllen = sizeof(cmsg_buf.buf);

        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr_ev);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type  = SCM_RIGHTS;
        cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(cmsg), &evfd, sizeof(int));

        if (sendmsg(client_fd, &mhdr_ev, MSG_NOSIGNAL) < 0) {
            VFR_LOGE("send eventfd_setup: %s", strerror(errno));
            teardown_session(srv, sess);
            return;
        }
    }

    /* Step 6: 加入 epoll（EPOLLIN=release_msg, EPOLLHUP=斷線）*/
    struct epoll_event ev = {
        .events  = EPOLLIN | EPOLLHUP | EPOLLERR,
        .data.fd = client_fd,
    };
    if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) < 0) {
        VFR_LOGE("epoll_ctl ADD fd=%d: %s", client_fd, strerror(errno));
        teardown_session(srv, sess);
        return;
    }

    VFR_LOGI("new consumer: fd=%d session_id=%u pid=%d policy=%u evfd=%d (total=%u)",
             client_fd, sid, (int)hello.consumer_pid, hello.policy, evfd, srv->session_count);
}

/* ─── handle_release_msg ─────────────────────────────────────────────────── */
static void handle_release_msg(struct vfr_server *srv, int client_fd)
{
    vfr_release_msg_t rel;
    ssize_t n = recv(client_fd, &rel, sizeof(rel), 0);
    if (n < 0) {
        if (errno == EINTR) return;
        VFR_LOGW("recv release_msg fd=%d: %s", client_fd, strerror(errno));
        consumer_session_t *sess = find_session_by_fd(srv, client_fd);
        if (sess) teardown_session(srv, sess);
        return;
    }
    if (n == 0) {
        VFR_LOGI("consumer fd=%d disconnected", client_fd);
        consumer_session_t *sess = find_session_by_fd(srv, client_fd);
        if (sess) teardown_session(srv, sess);
        return;
    }
    if (n < (ssize_t)sizeof(rel)) {
        VFR_LOGW("short release_msg got=%zd expect=%zu", n, sizeof(rel));
        return;
    }

    /* 驗證 magic */
    if (rel.magic != VFR_SHM_MAGIC) {
        VFR_LOGW("bad release_msg magic 0x%08x", rel.magic);
        return;
    }

    /* 驗證 session_id */
    consumer_session_t *sess = find_session_by_id(srv, rel.session_id);
    if (!sess) {
        VFR_LOGW("release_msg unknown session_id=%u (tombstone?)", rel.session_id);
        return;
    }

    /* 從 refslot 移除（把最後一個填到被移除的位置）*/
    bool found = false;
    for (uint32_t i = 0; i < sess->refslot_count; i++) {
        if (sess->refslot[i] == rel.slot_id) {
            sess->refslot[i] = sess->refslot[--sess->refslot_count];
            found = true;
            break;
        }
    }
    if (!found) {
        VFR_LOGW("release_msg slot_id=%u not in refslot[]", rel.slot_id);
        return;
    }

    /* 觸發 pool 回收 */
    vfr_pool_server_release(srv->pool, rel.slot_id, rel.seq_num);

    VFR_LOGD("release: session_id=%u slot_id=%u seq=%llu",
             rel.session_id, rel.slot_id, (unsigned long long)rel.seq_num);
}

/* ─── vfr_server_create ──────────────────────────────────────────────────── */
vfr_server_t *vfr_server_create(const char *stream_name, uint32_t slot_count)
{
    if (!stream_name) {
        VFR_LOGE("stream_name is NULL");
        return NULL;
    }
    if (strlen(stream_name) >= VFR_SOCKET_NAME_MAX) {
        VFR_LOGE("stream_name too long (%zu >= %u)", strlen(stream_name), VFR_SOCKET_NAME_MAX);
        return NULL;
    }

    struct vfr_server *srv = calloc(1, sizeof(*srv));
    if (!srv) {
        VFR_LOGE("calloc failed: %s", strerror(errno));
        return NULL;
    }
    strncpy(srv->stream_name, stream_name, VFR_SOCKET_NAME_MAX - 1);
    srv->listen_fd = -1;
    srv->epoll_fd  = -1;

    /* 初始化所有 session 的 fd 為 -1（calloc 後為 0 = stdin，必須覆寫）*/
    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        srv->sessions[i].socket_fd = -1;
        srv->sessions[i].eventfd   = -1;
    }

    /* ── Platform + Pool ────────────────────────────────────────────────── */
    const vfr_platform_ops_t *ops;
    {
        const char *env = getenv("VFR_PLATFORM");
        if (!env || strcmp(env, "mock") == 0) ops = vfr_get_mock_ops();
#ifdef HAVE_IAV_IOCTL_H
        else if (strcmp(env, "amba") == 0) ops = vfr_get_amba_ops();
#endif
        else { VFR_LOGW("unknown VFR_PLATFORM='%s', using mock", env); ops = vfr_get_mock_ops(); }
    }

    srv->shm_hdr.magic      = VFR_SHM_MAGIC;
    srv->shm_hdr.slot_count = (slot_count == 0) ? VFR_DEFAULT_SLOTS : slot_count;

    srv->pool = vfr_pool_create(ops, slot_count, &srv->shm_hdr);
    if (!srv->pool) {
        VFR_LOGE("vfr_pool_create failed");
        free(srv);
        return NULL;
    }

    /* ── Unix SOCK_SEQPACKET + abstract namespace ────────────────────── */
    srv->listen_fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    if (srv->listen_fd < 0) {
        VFR_LOGE("socket() failed: %s", strerror(errno));
        goto fail;
    }

    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        int plen = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "/vfr/%s", stream_name);
        socklen_t alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + plen);

        if (bind(srv->listen_fd, (struct sockaddr *)&addr, alen) < 0) {
            VFR_LOGE("bind(\\0/vfr/%s) failed: %s", stream_name, strerror(errno));
            goto fail;
        }
    }

    if (listen(srv->listen_fd, 16) < 0) {
        VFR_LOGE("listen() failed: %s", strerror(errno));
        goto fail;
    }

    /* ── epoll ──────────────────────────────────────────────────────── */
    srv->epoll_fd = epoll_create1(0);
    if (srv->epoll_fd < 0) {
        VFR_LOGE("epoll_create1 failed: %s", strerror(errno));
        goto fail;
    }

    {
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv->listen_fd };
        if (epoll_ctl(srv->epoll_fd, EPOLL_CTL_ADD, srv->listen_fd, &ev) < 0) {
            VFR_LOGE("epoll_ctl ADD listen_fd: %s", strerror(errno));
            goto fail;
        }
    }

    VFR_LOGI("server created: stream='%s' platform=%s slots=%u",
             stream_name, ops->name, srv->shm_hdr.slot_count);
    return srv;

fail:
    if (srv->epoll_fd  >= 0) { close(srv->epoll_fd);  srv->epoll_fd  = -1; }
    if (srv->listen_fd >= 0) { close(srv->listen_fd); srv->listen_fd = -1; }
    vfr_pool_destroy(&srv->pool);
    free(srv);
    return NULL;
}

/* ─── vfr_server_handle_events ───────────────────────────────────────────── */
int vfr_server_handle_events(vfr_server_t *srv, int timeout_ms)
{
    if (!srv) return -1;

    struct epoll_event events[VFR_MAX_CONSUMERS + 1];
    int nfds = epoll_wait(srv->epoll_fd, events,
                          (int)(sizeof(events)/sizeof(events[0])), timeout_ms);
    if (nfds < 0) {
        if (errno == EINTR) return 0;
        VFR_LOGE("epoll_wait: %s", strerror(errno));
        return -1;
    }

    for (int i = 0; i < nfds; i++) {
        int fd = events[i].data.fd;

        if (fd == srv->listen_fd) {
            /* 新連線（SOCK_NONBLOCK，需迴圈 accept）*/
            for (;;) {
                int cfd = accept(srv->listen_fd, NULL, NULL);
                if (cfd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    VFR_LOGW("accept: %s", strerror(errno));
                    break;
                }
                handle_accepted_client(srv, cfd);
            }
        } else {
            /* Client fd：release_msg 或斷線 */
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                VFR_LOGI("client fd=%d HUP/ERR", fd);
                consumer_session_t *sess = find_session_by_fd(srv, fd);
                if (sess) teardown_session(srv, sess);
            } else if (events[i].events & EPOLLIN) {
                handle_release_msg(srv, fd);
            }
        }
    }
    return 0;
}

/* ─── helper：從 refslot[] 移除並呼叫 force_release（DROP_OLDEST 用）────── */
static void force_release_slot(struct vfr_server *srv, consumer_session_t *sess,
                                uint32_t slot_id)
{
    /* 先從 refslot[] 移除，確保後續 handle_release_msg 不會再次觸發 server_release */
    bool found = false;
    for (uint32_t i = 0; i < sess->refslot_count; i++) {
        if (sess->refslot[i] == slot_id) {
            sess->refslot[i] = sess->refslot[--sess->refslot_count];
            found = true;
            break;
        }
    }
    if (!found) {
        VFR_LOGW("force_release_slot: slot_id=%u not in refslot[]", slot_id);
        return;
    }

    /* 遞減 refcount（pool 會在最後一個持有者釋放時 free slot）*/
    vfr_pool_force_release(srv->pool, slot_id);
    VFR_LOGD("force_release_slot: session_id=%u slot_id=%u", sess->session_id, slot_id);
}

/* ─── helper：等待 BLOCK_PRODUCER consumer 釋放 slot（輪詢 socket）─────── */
/* 單執行緒 server 不能 sleep；直接在 consumer socket 上 poll，接收 release_msg */
static int wait_for_consumer_free(struct vfr_server *srv, consumer_session_t *sess)
{
    if (sess->refslot_count == 0) return 1;

    struct pollfd pfd = { .fd = sess->socket_fd, .events = POLLIN };
    int elapsed_ms    = 0;
    const int STEP_MS = 2;

    while (elapsed_ms < VFR_BLOCK_PRODUCER_TIMEOUT_MS && sess->refslot_count > 0) {
        int ret = poll(&pfd, 1, STEP_MS);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            /* 接收 release_msg，更新 refslot_count */
            handle_release_msg(srv, sess->socket_fd);
        } else if (ret < 0 && errno != EINTR) {
            return -1;
        }
        if (pfd.revents & (POLLHUP | POLLERR)) return -1;
        elapsed_ms += STEP_MS;
    }

    return (sess->refslot_count == 0) ? 1 : 0;
}

/* ─── helper：向 consumer 發送一幀（SCM_RIGHTS + eventfd notify）────────── */
/* 回傳：0 = 成功；-1 = consumer 斷線（已 teardown）*/
static int dispatch_to_session(struct vfr_server *srv __attribute__((unused)),
                                consumer_session_t *sess,
                                uint32_t slot_idx, const vfr_frame_t *meta,
                                int producer_dma_fd)
{
    vfr_frame_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic           = VFR_SHM_MAGIC;
    msg.session_id      = sess->session_id;
    msg.slot_id         = slot_idx;
    msg.width           = meta->width;
    msg.height          = meta->height;
    msg.format          = meta->format;
    msg.stride          = meta->stride;
    msg.buf_size        = meta->buf_size;
    msg.flags           = meta->flags;
    msg.timestamp_ns    = meta->timestamp_ns;
    msg.seq_num         = meta->seq_num;
    msg.plane_offset[0] = meta->plane_offset[0];
    msg.plane_offset[1] = meta->plane_offset[1];
    msg.plane_offset[2] = meta->plane_offset[2];

    struct iovec iov = { .iov_base = &msg, .iov_len = sizeof(msg) };
    union {
        char           buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg_buf;
    memset(&cmsg_buf, 0, sizeof(cmsg_buf));

    struct msghdr mhdr;
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_iov        = &iov;
    mhdr.msg_iovlen     = 1;
    mhdr.msg_control    = cmsg_buf.buf;
    mhdr.msg_controllen = sizeof(cmsg_buf.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &producer_dma_fd, sizeof(int));

    ssize_t sent = sendmsg(sess->socket_fd, &mhdr, MSG_NOSIGNAL);
    if (sent < 0) {
        VFR_LOGE("sendmsg session_id=%u fd=%d: %s",
                 sess->session_id, sess->socket_fd, strerror(errno));
        return -1;   /* 呼叫方負責 refcount-- 和 teardown */
    }

    /* Phase 3：通知 consumer 有新幀（eventfd write）
     * fd 送出後才通知，確保 consumer recvmsg 能取到 fd（POOL_DESIGN.md §3.2 Phase C）*/
    if (sess->eventfd >= 0) {
        vfr_sync_notify(sess->eventfd);
    }

    /* 記錄 refslot（供斷線時 force release）*/
    if (sess->refslot_count < VFR_MAX_CONSUMER_SLOTS) {
        sess->refslot[sess->refslot_count++] = slot_idx;
    }

    VFR_LOGD("dispatch slot[%u] seq=%llu → session_id=%u",
             slot_idx, (unsigned long long)meta->seq_num, sess->session_id);
    return 0;
}

/* ─── vfr_server_produce（Phase 3 Backpressure Dispatch）────────────────── */
int vfr_server_produce(vfr_server_t *srv)
{
    if (!srv) return -1;

    /* 從 platform 取一幀（FILLING → READY）*/
    uint32_t slot_idx;
    int ret = vfr_pool_acquire(srv->pool, &slot_idx);
    if (ret != 0) return ret;

    const vfr_frame_t *meta = vfr_pool_slot_meta(srv->pool, slot_idx);
    if (!meta) {
        VFR_LOGE("slot_meta slot[%u] NULL", slot_idx);
        return -1;
    }

    /* ── Phase A：決策（POOL_DESIGN.md §3.2）──────────────────────────────
     * 不傳送，只決定哪些 consumer 本次要接收，並按 policy 處理 backpressure */
    uint32_t receiver_idx[VFR_MAX_CONSUMERS];
    uint32_t n = 0;

    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        consumer_session_t *sess = &srv->sessions[i];
        if (!sess->active) continue;

        switch (sess->policy) {
        case VFR_POLICY_DROP_OLDEST:
            /* 若有舊幀未消化：force_release 最舊 slot，確保有空間接收新幀 */
            if (sess->refslot_count > 0) {
                force_release_slot(srv, sess, sess->refslot[0]);
                atomic_fetch_add_explicit(
                    &srv->shm_hdr.drop_count, 1u, memory_order_relaxed);
            }
            receiver_idx[n++] = (uint32_t)i;
            break;

        case VFR_POLICY_BLOCK_PRODUCER:
            /* 若有舊幀：等待 consumer 釋放（最多 1 frame 時間）*/
            if (sess->refslot_count > 0) {
                int ok = wait_for_consumer_free(srv, sess);
                if (ok <= 0) {
                    VFR_LOGW("BLOCK_PRODUCER session_id=%u timeout, downgrade to DROP_OLDEST",
                             sess->session_id);
                    if (sess->refslot_count > 0) {
                        force_release_slot(srv, sess, sess->refslot[0]);
                        atomic_fetch_add_explicit(
                            &srv->shm_hdr.drop_count, 1u, memory_order_relaxed);
                    }
                }
            }
            receiver_idx[n++] = (uint32_t)i;
            break;

        case VFR_POLICY_SKIP_SELF:
            /* 若有舊幀未消化：跳過本幀 */
            if (sess->refslot_count > 0) {
                VFR_LOGD("SKIP_SELF session_id=%u: has slot, skipping frame", sess->session_id);
                continue;
            }
            receiver_idx[n++] = (uint32_t)i;
            break;

        default:
            /* 未知 policy：當作 DROP_OLDEST 處理 */
            receiver_idx[n++] = (uint32_t)i;
            break;
        }
    }

    if (n == 0) {
        vfr_pool_cancel_acquire(srv->pool, slot_idx);
        VFR_LOGD("no receivers for slot[%u], cancelled", slot_idx);
        return 0;
    }

    /* ── Phase B：先設 refcount，再開始任何 sendmsg ─────────────────────
     * 必須在第一個 write(eventfd) 之前完成，防止 consumer 提前 put_frame 導致
     * refcount wrap（POOL_DESIGN.md §3.2 "關鍵約束"）*/
    if (vfr_pool_begin_dispatch(srv->pool, slot_idx, n) < 0) {
        VFR_LOGE("begin_dispatch slot[%u] failed", slot_idx);
        return -1;
    }

    int producer_dma_fd = vfr_pool_slot_dma_fd(srv->pool, slot_idx);

    /* ── Phase C：逐一發送（sendmsg + eventfd notify）──────────────────── */
    for (uint32_t r = 0; r < n; r++) {
        consumer_session_t *sess = &srv->sessions[receiver_idx[r]];
        if (!sess->active) {
            /* 在 Phase A→C 之間 consumer 已死亡（tombstone=true 不適用，直接 dec）*/
            vfr_pool_server_release(srv->pool, slot_idx, meta->seq_num);
            continue;
        }

        if (dispatch_to_session(srv, sess, slot_idx, meta, producer_dma_fd) < 0) {
            /* sendmsg 失敗：EPIPE，consumer 在 Phase B→C 之間死亡 */
            vfr_pool_server_release(srv->pool, slot_idx, meta->seq_num);
            teardown_session(srv, sess);
        }
    }

    return 0;
}

/* ─── vfr_server_destroy（原則五順序）──────────────────────────────────── */
void vfr_server_destroy(vfr_server_t **srv_ptr)
{
    if (!srv_ptr || !*srv_ptr) return;
    struct vfr_server *srv = *srv_ptr;

    VFR_LOGI("server destroy: stream='%s'", srv->stream_name);

    /* 2. Teardown 所有 consumer sessions（從 epoll 登出 + close fd）*/
    for (int i = 0; i < VFR_MAX_CONSUMERS; i++) {
        if (srv->sessions[i].active) teardown_session(srv, &srv->sessions[i]);
    }

    /* 3+4. close listen_fd、epoll_fd */
    if (srv->listen_fd >= 0) {
        epoll_ctl(srv->epoll_fd, EPOLL_CTL_DEL, srv->listen_fd, NULL);
        close(srv->listen_fd);
        srv->listen_fd = -1;
    }
    if (srv->epoll_fd >= 0) {
        close(srv->epoll_fd);
        srv->epoll_fd = -1;
    }

    /* 5. 銷毀 pool（close dma_fd）*/
    vfr_pool_destroy(&srv->pool);

    /* 6. free */
    free(srv);
    *srv_ptr = NULL;
    VFR_LOGI("server destroyed");
}
