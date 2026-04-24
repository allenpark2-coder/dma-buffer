/* ipc/vfr_watchdog.c — Consumer Process Watchdog 實作（Phase 4）
 *
 * 使用 pidfd_open() syscall（Linux 5.3+）監控 consumer PID。
 * 整合進 server 的 epoll event loop（不使用獨立 thread）：
 *   - 呼叫 vfr_watchdog_open(pid) 取得 pidfd
 *   - 將 pidfd 加入 server epoll（EPOLLIN）
 *   - epoll_wait 收到 EPOLLIN 時表示 consumer process 已終止
 *   - server event loop 呼叫 teardown_session() 清理資源
 *
 * Fallback：若 kernel 不支援 pidfd_open（< 5.3），回傳 -1 並 log WARN；
 *   server 仍透過 socket EPOLLHUP 偵測斷線，只是精確度略低。
 */

#include "ipc/vfr_watchdog.h"
#include "vfr_defs.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <stdbool.h>

/* ─── pidfd_open syscall wrapper ─────────────────────────────────────────── */

#ifdef __NR_pidfd_open
#  define HAVE_PIDFD_OPEN 1
#else
#  define HAVE_PIDFD_OPEN 0
#endif

/* ─── vfr_watchdog_available ─────────────────────────────────────────────── */
bool vfr_watchdog_available(void)
{
#if HAVE_PIDFD_OPEN
    return true;
#else
    return false;
#endif
}

/* ─── vfr_watchdog_open ──────────────────────────────────────────────────── */
int vfr_watchdog_open(pid_t pid)
{
#if HAVE_PIDFD_OPEN
    int fd = (int)syscall(__NR_pidfd_open, (long)pid, 0L);
    if (fd < 0) {
        /* ESRCH = process already dead（也算一種死亡通知）*/
        if (errno == ESRCH) {
            VFR_LOGW("pidfd_open(pid=%d): process already dead", (int)pid);
        } else {
            VFR_LOGW("pidfd_open(pid=%d) failed: %s", (int)pid, strerror(errno));
        }
        return -1;
    }
    VFR_LOGD("pidfd_open(pid=%d) → fd=%d", (int)pid, fd);
    return fd;
#else
    (void)pid;
    VFR_LOGW("pidfd_open not available on this kernel (need Linux 5.3+)");
    errno = ENOSYS;
    return -1;
#endif
}

/* ─── vfr_watchdog_close ─────────────────────────────────────────────────── */
void vfr_watchdog_close(int *fd)
{
    if (!fd || *fd < 0) return;
    close(*fd);
    *fd = -1;
}
