/* ipc/vfr_client.c — VFR IPC Client 實作（Consumer 端）
 *
 * 實作 vfr_client_connect / vfr_client_recv_frame /
 *       vfr_client_send_release / vfr_client_disconnect。
 *
 * Socket 型別：SOCK_SEQPACKET（可靠、保留訊息邊界、面向連線）
 *   - 確保 vfr_frame_msg_t + SCM_RIGHTS(dma_fd) 在一次 recvmsg 中原子接收
 *   - 避免 SOCK_STREAM partial read 時 SCM_RIGHTS 位置不確定的問題
 *
 * 握手協議（POOL_DESIGN.md §五）：
 *   1. client → server : vfr_client_hello_t
 *   2. server → client : vfr_handshake_t（版本合法）或 vfr_error_response_t（版本不符）
 *   3. server → client : vfr_shm_header_t（格式協商）
 *
 * 注意：所有 recv 使用迴圈處理 short read（SOCK_STREAM 語意下；
 *   SOCK_SEQPACKET 雖保有訊息邊界，但仍應處理被 signal 中斷的情況）。
 */

#include "ipc/vfr_client.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

/* ─── helper：可靠讀取（迴圈處理 short read / EINTR）───────────────────── */
static ssize_t recv_all(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = recv(fd, (char *)buf + done, len - done, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) {
            /* 對端已關閉 */
            errno = ECONNRESET;
            return -1;
        }
        done += (size_t)n;
    }
    return (ssize_t)done;
}

/* ─── helper：可靠發送（迴圈處理 short write / EINTR）─────────────────── */
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

/* ─── vfr_client_connect ─────────────────────────────────────────────────── */
int vfr_client_connect(const char *stream_name, vfr_client_state_t *out_state,
                       uint32_t policy)
{
    if (!stream_name || !out_state) return -1;

    /* 建立 SOCK_SEQPACKET（保留訊息邊界，確保 SCM_RIGHTS 原子傳遞）*/
    int sock = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (sock < 0) {
        VFR_LOGE("socket() failed: %s", strerror(errno));
        return -1;
    }

    /* 組 abstract namespace 路徑（第一個 byte 為 '\0'）*/
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    int path_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "/vfr/%s", stream_name);
    if (path_len < 0 || path_len >= (int)(sizeof(addr.sun_path) - 1)) {
        VFR_LOGE("stream_name too long");
        close(sock);
        return -1;
    }
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + path_len);

    if (connect(sock, (struct sockaddr *)&addr, addrlen) < 0) {
        VFR_LOGE("connect(\"%s\") failed: %s", stream_name, strerror(errno));
        close(sock);
        return -1;
    }

    /* ── Step 1: 發送 vfr_client_hello_t ───────────────────────────────── */
    vfr_client_hello_t hello = {
        .magic         = VFR_SHM_MAGIC,
        .proto_version = VFR_PROTO_VERSION,
        .reserved      = 0,
        .consumer_pid  = (int32_t)getpid(),
        .consumer_uid  = (uint32_t)getuid(),
        .policy        = policy,
        ._pad2         = 0,
    };
    if (send_all(sock, &hello, sizeof(hello)) < 0) {
        VFR_LOGE("send hello failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    /* ── Step 2: 接收 vfr_handshake_t（或 vfr_error_response_t）────────── */
    vfr_handshake_t shake;
    if (recv_all(sock, &shake, sizeof(shake)) < 0) {
        VFR_LOGE("recv handshake failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    /* 先檢查 magic；若不符可能是 error_response */
    if (shake.magic != VFR_SHM_MAGIC) {
        VFR_LOGE("handshake magic mismatch: got 0x%08x expect 0x%08x",
                 shake.magic, VFR_SHM_MAGIC);
        close(sock);
        return -1;
    }

    /* error_response 的 layout：magic(4) + error(4)
     * handshake 的 layout：magic(4) + proto_version(2) + header_size(2) + session_id(4) + pad(4)
     * 用 proto_version == 0 判斷是否為 error_response（error_response 的對應欄位 = error code）*/
    if (shake.proto_version == 0) {
        /* 這其實是 vfr_error_response_t 的第二個 uint32_t（error code）*/
        vfr_error_response_t *err = (vfr_error_response_t *)&shake;
        VFR_LOGE("proto_version mismatch: server error code %u", err->error);
        close(sock);
        return -1;
    }

    if (shake.proto_version != VFR_PROTO_VERSION) {
        VFR_LOGE("proto_version mismatch: got %u, expect %u", shake.proto_version, VFR_PROTO_VERSION);
        close(sock);
        return -1;
    }

    /* ── Step 3: 接收 vfr_shm_header_t（格式協商）───────────────────────── */
    /* 用 header_size 決定讀取長度（向前相容：只讀已知部分）*/
    vfr_shm_header_t shm_hdr;
    memset(&shm_hdr, 0, sizeof(shm_hdr));
    uint16_t hdr_size = shake.header_size;
    if (hdr_size > (uint16_t)sizeof(shm_hdr)) {
        hdr_size = (uint16_t)sizeof(shm_hdr);   /* 只讀已知欄位 */
    }
    if (recv_all(sock, &shm_hdr, hdr_size) < 0) {
        VFR_LOGE("recv shm_header failed: %s", strerror(errno));
        close(sock);
        return -1;
    }
    if (shm_hdr.magic != VFR_SHM_MAGIC) {
        VFR_LOGE("shm_header magic mismatch");
        close(sock);
        return -1;
    }

    /* ── Step 4: 接收 vfr_eventfd_setup_t + eventfd（SCM_RIGHTS）────────── */
    /* Phase 3：server 在 shm_header 之後發送 eventfd，供 consumer 做 epoll_wait */
    vfr_eventfd_setup_t evsetup;
    union {
        char             buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr   align;
    } cmsg_buf;
    struct iovec iov_ev  = { .iov_base = &evsetup, .iov_len  = sizeof(evsetup) };
    struct msghdr mhdr_ev;
    memset(&mhdr_ev, 0, sizeof(mhdr_ev));
    mhdr_ev.msg_iov        = &iov_ev;
    mhdr_ev.msg_iovlen     = 1;
    mhdr_ev.msg_control    = cmsg_buf.buf;
    mhdr_ev.msg_controllen = sizeof(cmsg_buf.buf);

    ssize_t nr = recvmsg(sock, &mhdr_ev, MSG_WAITALL);
    if (nr < (ssize_t)sizeof(evsetup)) {
        VFR_LOGE("recv eventfd_setup failed (got %zd): %s", nr, strerror(errno));
        close(sock);
        return -1;
    }
    if (evsetup.magic != VFR_SHM_MAGIC) {
        VFR_LOGE("eventfd_setup magic mismatch 0x%08x", evsetup.magic);
        close(sock);
        return -1;
    }

    int evfd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr_ev);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS
            && cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
        memcpy(&evfd, CMSG_DATA(cmsg), sizeof(int));
    }
    if (evfd < 0) {
        VFR_LOGE("no eventfd received in SCM_RIGHTS");
        close(sock);
        return -1;
    }

    /* 成功：填入 out_state */
    memset(out_state, 0, sizeof(*out_state));
    out_state->socket_fd  = sock;
    out_state->eventfd    = evfd;
    out_state->session_id = shake.session_id;
    out_state->shm_hdr    = shm_hdr;

    VFR_LOGI("connected to stream '%s' session_id=%u %ux%u evfd=%d",
             stream_name, shake.session_id, shm_hdr.width, shm_hdr.height, evfd);
    return 0;
}

/* ─── vfr_client_recv_frame ──────────────────────────────────────────────── */
int vfr_client_recv_frame(vfr_client_state_t *state, vfr_frame_t *out_frame, int flags)
{
    vfr_frame_msg_t msg;

    /* 準備 iovec（payload）*/
    struct iovec iov = {
        .iov_base = &msg,
        .iov_len  = sizeof(msg),
    };

    /* 準備 ancillary data buffer（接收 SCM_RIGHTS dma_fd）*/
    union {
        char             buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr   align;
    } cmsg_buf;

    struct msghdr mhdr = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = cmsg_buf.buf,
        .msg_controllen = sizeof(cmsg_buf.buf),
    };

    int recv_flags = 0;
    if (flags & VFR_FLAG_NONBLOCK) recv_flags |= MSG_DONTWAIT;

    ssize_t n = recvmsg(state->socket_fd, &mhdr, recv_flags);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 1;  /* NONBLOCK: no frame */
        if (errno == EINTR) return 1;
        VFR_LOGE("recvmsg failed: %s", strerror(errno));
        return -1;
    }
    if (n == 0) {
        VFR_LOGW("server closed connection");
        return -1;
    }
    if (n < (ssize_t)sizeof(msg)) {
        VFR_LOGE("short frame_msg: got %zd expect %zu", n, sizeof(msg));
        return -1;
    }

    /* 驗證 magic */
    if (msg.magic != VFR_SHM_MAGIC) {
        VFR_LOGE("frame_msg magic mismatch 0x%08x", msg.magic);
        return -1;
    }

    /* 驗證 session_id */
    if (msg.session_id != state->session_id) {
        VFR_LOGW("session_id mismatch got=%u expect=%u", msg.session_id, state->session_id);
        return -1;
    }

    /* 取出 dma_fd from SCM_RIGHTS */
    int dma_fd = -1;
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&mhdr);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        memcpy(&dma_fd, CMSG_DATA(cmsg), sizeof(int));
    }
    if (dma_fd < 0) {
        VFR_LOGE("no SCM_RIGHTS fd received");
        return -1;
    }

    /* 填入 vfr_frame_t */
    out_frame->dma_fd          = dma_fd;
    out_frame->width           = msg.width;
    out_frame->height          = msg.height;
    out_frame->format          = msg.format;
    out_frame->stride          = msg.stride;
    out_frame->buf_size        = msg.buf_size;
    out_frame->flags           = msg.flags;
    out_frame->timestamp_ns    = msg.timestamp_ns;
    out_frame->seq_num         = msg.seq_num;
    out_frame->plane_offset[0] = msg.plane_offset[0];
    out_frame->plane_offset[1] = msg.plane_offset[1];
    out_frame->plane_offset[2] = msg.plane_offset[2];

    /* 設定 back-ref（供 vfr_put_frame() 發送 release_msg）*/
    state->slot_ref.mode       = VFR_PRIV_CLIENT;
    state->slot_ref.socket_fd  = state->socket_fd;
    state->slot_ref.session_id = state->session_id;
    state->slot_ref.slot_id    = msg.slot_id;
    state->slot_ref.seq_num    = msg.seq_num;
    out_frame->priv            = &state->slot_ref;

    VFR_LOGD("recv_frame: slot_id=%u seq=%llu dma_fd=%d %ux%u",
             msg.slot_id, (unsigned long long)msg.seq_num, dma_fd,
             msg.width, msg.height);
    return 0;
}

/* ─── vfr_client_send_release ────────────────────────────────────────────── */
int vfr_client_send_release(vfr_client_ref_t *ref)
{
    if (!ref) return -1;

    vfr_release_msg_t rel = {
        .magic      = VFR_SHM_MAGIC,
        .session_id = ref->session_id,
        .slot_id    = ref->slot_id,
        .reserved   = 0,
        .seq_num    = ref->seq_num,
    };

    if (send_all(ref->socket_fd, &rel, sizeof(rel)) < 0) {
        VFR_LOGE("send release_msg failed: %s", strerror(errno));
        return -1;
    }

    VFR_LOGD("send_release: slot_id=%u seq=%llu",
             ref->slot_id, (unsigned long long)ref->seq_num);
    return 0;
}

/* ─── vfr_client_get_eventfd ─────────────────────────────────────────────── */
int vfr_client_get_eventfd(const vfr_client_state_t *state)
{
    if (!state) return -1;
    return state->eventfd;
}

/* ─── vfr_client_disconnect ──────────────────────────────────────────────── */
void vfr_client_disconnect(vfr_client_state_t *state)
{
    if (!state) return;

    /* Phase 3：呼叫者必須在此之前從自己的 epoll 登出 eventfd */
    if (state->eventfd >= 0) {
        close(state->eventfd);
        state->eventfd = -1;
    }
    if (state->socket_fd >= 0) {
        close(state->socket_fd);
        state->socket_fd = -1;
    }
    VFR_LOGI("client disconnected");
}
