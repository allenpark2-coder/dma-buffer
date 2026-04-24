/* ipc/vfr_server.h — VFR IPC Server（Producer 端）API
 *
 * Producer process 建立 Unix abstract namespace socket，
 * 接受 consumer 連線，透過 SCM_RIGHTS 傳送 dma_fd。
 *
 * 設計原則（見 POOL_DESIGN.md §七）：
 *   - Server 為單執行緒 event loop（epoll 驅動）
 *   - Dispatch、連線管理、release msg 處理全部在同一個 thread
 *   - Phase 4 watchdog thread 是唯一例外（此版本尚未實作）
 */
#ifndef VFR_SERVER_H
#define VFR_SERVER_H

#include "vfr_defs.h"

/* opaque server handle；實際定義在 vfr_server.c */
typedef struct vfr_server vfr_server_t;

/*
 * vfr_server_create()：
 *   stream_name — 長度必須 < VFR_SOCKET_NAME_MAX，否則回傳 NULL
 *   slot_count  — 0 使用 VFR_DEFAULT_SLOTS；上限 VFR_MAX_SLOTS
 *
 * 回傳：成功回傳 server 指標；失敗回傳 NULL
 */
vfr_server_t *vfr_server_create(const char *stream_name, uint32_t slot_count);

/*
 * vfr_server_handle_events()：
 *   以 epoll 處理 IPC 事件（新連線、vfr_release_msg_t）。
 *   timeout_ms = 0  → 非阻塞（立即回傳）
 *   timeout_ms = -1 → 無限等待
 *
 * 回傳：0 = 正常；-1 = 不可恢復錯誤
 */
int vfr_server_handle_events(vfr_server_t *srv, int timeout_ms);

/*
 * vfr_server_produce()：
 *   從 platform adapter 取一幀，dispatch 給所有已連線的 consumer。
 *
 * 回傳：0 = 成功；1 = 無新幀（platform EAGAIN）；-1 = 不可恢復錯誤
 */
int vfr_server_produce(vfr_server_t *srv);

/*
 * vfr_server_destroy()：
 *   釋放所有資源，按原則五順序清理。
 *   *srv 設為 NULL；重複呼叫為 no-op。
 */
void vfr_server_destroy(vfr_server_t **srv);

/* ─── Phase 4：監控介面（供 metrics 系統讀取）─────────────────────────────── */

/*
 * vfr_server_get_drop_count()：
 *   讀取 SHM header 的 drop_count（原子操作，任何時刻可呼叫）。
 *   drop_count 在 DROP_OLDEST 觸發時遞增，代表被丟棄的幀數。
 */
uint32_t vfr_server_get_drop_count(const vfr_server_t *srv);

/*
 * vfr_server_get_session_count()：
 *   回傳目前已連線的 consumer 數量。
 */
uint32_t vfr_server_get_session_count(const vfr_server_t *srv);

#endif /* VFR_SERVER_H */
