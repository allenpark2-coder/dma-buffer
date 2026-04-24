/* ipc/vfr_registry.h — Stream Registry Table（動態 stream 發現）
 *
 * 仿 ZeroMQ ServiceBinder 模式：
 *   - Registry daemon 監聽 Unix abstract socket `\0/vfr/.registry`
 *   - Producer 啟動時呼叫 vfr_registry_register()，關閉時呼叫 vfr_registry_unregister()
 *   - Consumer 呼叫 vfr_registry_list() 列舉所有已登記的 stream
 *
 * 協議（極簡 request/reply，單次連線）：
 *   Request：vfr_reg_req_t（含 opcode + payload）
 *   Reply：  vfr_reg_reply_t（含 status + entry 陣列）
 *
 * 注意：Registry daemon 需先啟動（vfr_registry_serve_forever()），
 *   producer / consumer 才能連線。若 daemon 未啟動，register / list 回傳 -1
 *   但不影響核心 IPC 功能。
 */
#ifndef VFR_REGISTRY_H
#define VFR_REGISTRY_H

#include "vfr_defs.h"

/* ─── 常數 ───────────────────────────────────────────────────────────────── */
#define VFR_REGISTRY_SOCKET_PATH  "\0/vfr/.registry"
#define VFR_REGISTRY_MAX_STREAMS  32

/* ─── Stream 資訊（可公開查詢的 metadata）──────────────────────────────── */
typedef struct {
    char     stream_name[VFR_SOCKET_NAME_MAX];
    uint32_t width;
    uint32_t height;
    uint32_t format;       /* fourcc */
    uint32_t slot_count;
    int32_t  producer_pid;
} vfr_stream_info_t;

/* ─── 協議訊息 ───────────────────────────────────────────────────────────── */
typedef enum {
    VFR_REG_OP_REGISTER   = 1,  /* producer → daemon：登記 stream */
    VFR_REG_OP_UNREGISTER = 2,  /* producer → daemon：取消登記 */
    VFR_REG_OP_LIST       = 3,  /* consumer → daemon：列舉所有 stream */
} vfr_reg_opcode_t;

typedef struct {
    uint32_t          opcode;    /* vfr_reg_opcode_t */
    vfr_stream_info_t info;      /* REGISTER / UNREGISTER 時有效 */
} vfr_reg_req_t;

typedef struct {
    int32_t           status;                             /* 0 = OK；-1 = error */
    uint32_t          count;                              /* LIST 回傳的 entry 數量 */
    vfr_stream_info_t entries[VFR_REGISTRY_MAX_STREAMS];
} vfr_reg_reply_t;

/* ─── Producer API ───────────────────────────────────────────────────────── */

/*
 * vfr_registry_register()：
 *   向 registry daemon 登記 stream。若 daemon 未啟動，靜默回傳 -1。
 *   應在 vfr_server_create() 成功後呼叫。
 * 回傳：0 = 成功；-1 = 失敗（daemon 未啟動 or 連線失敗）
 */
int vfr_registry_register(const vfr_stream_info_t *info);

/*
 * vfr_registry_unregister()：
 *   取消登記 stream_name。應在 vfr_server_destroy() 前呼叫。
 * 回傳：0 = 成功；-1 = 失敗
 */
int vfr_registry_unregister(const char *stream_name);

/* ─── Consumer API ───────────────────────────────────────────────────────── */

/*
 * vfr_registry_list()：
 *   列舉所有已登記的 stream，寫入 entries（最多 max_count 筆）。
 * 回傳：成功回傳 entry 數量（0..max_count）；失敗回傳 -1
 */
int vfr_registry_list(vfr_stream_info_t *entries, uint32_t max_count);

/* ─── Daemon API ─────────────────────────────────────────────────────────── */

/*
 * vfr_registry_serve_forever()：
 *   啟動 registry daemon（阻塞）。
 *   設計為在獨立 process 或 thread 中執行。
 *   收到 SIGTERM / SIGINT 後清理資源並回傳。
 * 回傳：0 = 正常結束；-1 = 初始化失敗
 */
int vfr_registry_serve_forever(void);

#endif /* VFR_REGISTRY_H */
