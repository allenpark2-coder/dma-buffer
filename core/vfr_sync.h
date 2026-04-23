/* core/vfr_sync.h — EventFD 同步原語（Phase 3）
 *
 * 提供 producer→consumer 新幀通知的 eventfd 建立、寫入、等待介面。
 *
 * 設計（POOL_DESIGN.md §3.1）：
 *   - 每個 consumer session 各有一個獨立的 eventfd（不共用）
 *   - producer dispatch 後對每個 consumer 各呼叫一次 vfr_sync_notify()
 *   - consumer 在自己的 epoll 中等待 eventfd，收到通知後呼叫 vfr_get_frame()
 *
 * 使用 EFD_SEMAPHORE 語意：每次 notify 累加 1，每次 drain 減去 1，
 * 確保多幀積壓時 consumer 可逐一取幀（level-triggered 下 EPOLLIN 持續觸發）。
 */

#ifndef VFR_SYNC_H
#define VFR_SYNC_H

/*
 * vfr_sync_create_eventfd()：
 *   建立 EFD_SEMAPHORE | EFD_CLOEXEC eventfd。
 *
 * 回傳：fd（>= 0）；失敗回傳 -1
 */
int vfr_sync_create_eventfd(void);

/*
 * vfr_sync_close_eventfd()：
 *   close eventfd，並將 *fd 設為 -1；重複呼叫為 no-op。
 */
void vfr_sync_close_eventfd(int *fd);

/*
 * vfr_sync_notify()：
 *   向 eventfd 寫入 1（通知 consumer 有新幀）。
 *   producer dispatch 後呼叫。
 *
 * 回傳：0 = 成功；-1 = 失敗
 */
int vfr_sync_notify(int fd);

/*
 * vfr_sync_drain()：
 *   讀出 eventfd 計數值（EFD_SEMAPHORE 下每次讀出 1）。
 *   consumer 呼叫 vfr_get_frame() 之前應先 drain 以重置 EPOLLIN。
 *
 * 回傳：讀出的計數值（通常為 1）；EAGAIN（無資料）回傳 0；失敗回傳 -1
 */
int vfr_sync_drain(int fd);

/*
 * vfr_sync_wait()：
 *   使用 poll() 等待 eventfd 有資料（POLLIN）。
 *   timeout_ms < 0 = 無限等待；0 = 立即返回（非阻塞）。
 *
 * 回傳：1 = 有事件；0 = 逾時；-1 = 錯誤
 */
int vfr_sync_wait(int fd, int timeout_ms);

#endif /* VFR_SYNC_H */
