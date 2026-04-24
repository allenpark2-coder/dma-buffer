/* ipc/vfr_watchdog.h — Consumer Process Watchdog（Phase 4）
 *
 * 使用 pidfd_open() 監控 consumer PID。
 * 當 consumer process 死亡時，pidfd 變為可讀（EPOLLIN），
 * server event loop 可立即偵測並觸發 teardown_session()。
 *
 * 設計（POOL_DESIGN.md §七）：
 *   - 整合進 server epoll 而非獨立 thread，保持單執行緒 event loop 前提
 *   - pidfd 是 kernel 層面的精確偵測，比 heartbeat 更可靠
 *   - process 結束時 kernel 立即通知，不需等待逾時
 *
 * 依賴：Linux kernel 5.3+（pidfd_open syscall）
 */
#ifndef VFR_WATCHDOG_H
#define VFR_WATCHDOG_H

#include <stdbool.h>
#include <unistd.h>
#include <sys/types.h>

/*
 * vfr_watchdog_open()：
 *   呼叫 pidfd_open() 取得監控指定 PID 的 fd。
 *   當 PID 的 process 終止時，fd 變為可讀（POLLIN / EPOLLIN）。
 *
 *   用法：將回傳的 fd 加入 server 的 epoll，收到 EPOLLIN 後表示 consumer 已死。
 *
 * 回傳：fd（>= 0）= 成功；-1 = 失敗（ENOSYS 表示 kernel 不支援 pidfd_open）
 */
int vfr_watchdog_open(pid_t pid);

/*
 * vfr_watchdog_close()：
 *   close pidfd 並將 *fd 設為 -1。重複呼叫為 no-op。
 */
void vfr_watchdog_close(int *fd);

/*
 * vfr_watchdog_available()：
 *   回傳 true 若此平台支援 pidfd_open()（kernel 5.3+）。
 *   用於在握手時決定是否啟用 watchdog。
 */
bool vfr_watchdog_available(void);

#endif /* VFR_WATCHDOG_H */
