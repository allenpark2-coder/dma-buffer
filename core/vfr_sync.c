/* core/vfr_sync.c — EventFD 同步原語實作（Phase 3）*/

#include "core/vfr_sync.h"
#include "vfr_defs.h"

#include <sys/eventfd.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

/* ─── vfr_sync_create_eventfd ────────────────────────────────────────────── */
int vfr_sync_create_eventfd(void)
{
    /* EFD_SEMAPHORE：每次 write +1，每次 read -1，EPOLLIN 在計數 > 0 時持續觸發 */
    int fd = eventfd(0, EFD_SEMAPHORE | EFD_CLOEXEC);
    if (fd < 0) {
        VFR_LOGE("eventfd(EFD_SEMAPHORE) failed: %s", strerror(errno));
        return -1;
    }
    return fd;
}

/* ─── vfr_sync_close_eventfd ─────────────────────────────────────────────── */
void vfr_sync_close_eventfd(int *fd)
{
    if (!fd || *fd < 0) return;
    close(*fd);
    *fd = -1;
}

/* ─── vfr_sync_notify ────────────────────────────────────────────────────── */
int vfr_sync_notify(int fd)
{
    if (fd < 0) return -1;

    uint64_t val = 1;
    ssize_t n = write(fd, &val, sizeof(val));
    if (n < 0) {
        /* EAGAIN：EFD_SEMAPHORE 計數達到 UINT64_MAX（幾乎不可能）*/
        if (errno == EAGAIN) {
            VFR_LOGW("eventfd semaphore overflow (fd=%d)", fd);
            return 0;
        }
        VFR_LOGE("eventfd write(fd=%d) failed: %s", fd, strerror(errno));
        return -1;
    }
    return 0;
}

/* ─── vfr_sync_drain ─────────────────────────────────────────────────────── */
int vfr_sync_drain(int fd)
{
    if (fd < 0) return -1;

    uint64_t val = 0;
    ssize_t n = read(fd, &val, sizeof(val));
    if (n < 0) {
        if (errno == EAGAIN) return 0;  /* 無積壓計數 */
        VFR_LOGE("eventfd read(fd=%d) failed: %s", fd, strerror(errno));
        return -1;
    }
    return (int)val;  /* EFD_SEMAPHORE 下始終為 1 */
}

/* ─── vfr_sync_wait ──────────────────────────────────────────────────────── */
int vfr_sync_wait(int fd, int timeout_ms)
{
    if (fd < 0) return -1;

    struct pollfd pfd = {
        .fd      = fd,
        .events  = POLLIN,
        .revents = 0,
    };

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret < 0) {
        if (errno == EINTR) return 0;   /* signal 中斷視為 timeout */
        VFR_LOGE("poll(fd=%d) failed: %s", fd, strerror(errno));
        return -1;
    }
    return (ret > 0) ? 1 : 0;   /* 1 = event, 0 = timeout */
}
