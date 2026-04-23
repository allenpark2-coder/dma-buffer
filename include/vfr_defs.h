/* include/vfr_defs.h — 唯一常數定義點（原則二）
 * 所有跨模組共用的 macro 常數集中於此，其他所有 .h 以此為第一個 include。
 * 禁止在其他 .h / .c 重複定義這些值。
 */
#ifndef VFR_DEFS_H
#define VFR_DEFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ─── 版本與識別 ────────────────────────────────────────────────────────── */
#define VFR_SHM_MAGIC           0x56465231u   /* "VFR1" */
#define VFR_PROTO_VERSION       2u            /* IPC 握手協議版本；v2 新增 policy 欄位、eventfd 握手 */

/* ─── Buffer Pool ───────────────────────────────────────────────────────── */
#define VFR_DEFAULT_SLOTS       8             /* vfr_open() slot_count 預設值 */
#define VFR_MAX_SLOTS           64            /* slot_count 上限，防止異常配置 */

/* ─── Consumer 限制 ──────────────────────────────────────────────────────── */
#define VFR_MAX_CONSUMERS       16
#define VFR_MAX_CONSUMER_SLOTS  4             /* 單一 consumer 最多同時持有的 slot 數（當前幀 + 預取）*/

/* ─── Frame 格式 ─────────────────────────────────────────────────────────── */
#define VFR_MAX_PLANES          3

/* ─── Socket / IPC ──────────────────────────────────────────────────────── */
#define VFR_SOCKET_PATH_FMT     "\0/vfr/%s"   /* abstract namespace */
#define VFR_SOCKET_NAME_MAX     64            /* stream name 最大長度（bytes），防止 path overflow */

/* ─── Watchdog ──────────────────────────────────────────────────────────── */
#define VFR_WATCHDOG_TIMEOUT_MS     2000      /* Phase 4 watchdog：偵測 consumer process 死亡的超時 */
#define VFR_BLOCK_PRODUCER_TIMEOUT_MS 33     /* BLOCK_PRODUCER slot wait timeout（1 frame @ 30fps）*/

/* ─── vfr_get_frame() flags ─────────────────────────────────────────────── */
#define VFR_FLAG_NONBLOCK    (1u << 0)
#define VFR_FLAG_NO_CPU_SYNC (1u << 1)

/* ─── Backpressure policy values（與 vfr_backpressure_t enum 值一致）──────── */
#define VFR_POLICY_DROP_OLDEST    0
#define VFR_POLICY_BLOCK_PRODUCER 1
#define VFR_POLICY_SKIP_SELF      2

/* ─── Log 等級（編譯期靜態過濾，不影響 release build）─────────────────────── */
typedef enum {
    VFR_LOG_ERROR = 0,  /* 不可恢復錯誤，永遠輸出 */
    VFR_LOG_WARN  = 1,  /* 可恢復異常（drop、重連） */
    VFR_LOG_INFO  = 2,  /* 生命週期事件（open/close/連線） */
    VFR_LOG_DEBUG = 3,  /* 每幀細節，高頻，僅 debug build 啟用 */
} vfr_log_level_t;

#ifndef VFR_LOG_LEVEL
#  define VFR_LOG_LEVEL VFR_LOG_INFO   /* release 預設：INFO；debug build 傳 -DVFR_LOG_LEVEL=3 */
#endif

/* ─── Log macro（編譯期靜態過濾）────────────────────────────────────────── */
#include <stdio.h>

#define VFR_LOG(level, fmt, ...) \
    do { \
        if ((level) <= VFR_LOG_LEVEL) { \
            static const char *_level_str[] = {"ERROR", "WARN", "INFO", "DEBUG"}; \
            fprintf(stderr, "[VFR %s] %s:%d " fmt "\n", \
                    _level_str[(level) < 4 ? (level) : 3], \
                    __func__, __LINE__, ##__VA_ARGS__); \
        } \
    } while (0)

#define VFR_LOGE(fmt, ...) VFR_LOG(VFR_LOG_ERROR, fmt, ##__VA_ARGS__)
#define VFR_LOGW(fmt, ...) VFR_LOG(VFR_LOG_WARN,  fmt, ##__VA_ARGS__)
#define VFR_LOGI(fmt, ...) VFR_LOG(VFR_LOG_INFO,  fmt, ##__VA_ARGS__)
#define VFR_LOGD(fmt, ...) VFR_LOG(VFR_LOG_DEBUG, fmt, ##__VA_ARGS__)

/* ─── SHM State Header ───────────────────────────────────────────────────── */
/* 置於每個 stream 的 SHM 起始處，消費者可在不建立 socket 的情況下讀取 seq/drop_count */
typedef struct {
    uint32_t         magic;                         /* VFR_SHM_MAGIC，版本校驗 */
    uint32_t         width;
    uint32_t         height;
    uint32_t         format;
    uint32_t         stride;                        /* 初始協商值；以 vfr_frame_t 內的每幀值為準 */
    uint32_t         plane_offset[VFR_MAX_PLANES];  /* 初始協商值；以 vfr_frame_t 內的每幀值為準 */
    uint32_t         slot_count;
    uint64_t         producer_boot_ns;              /* producer 啟動時的 CLOCK_REALTIME（nanoseconds）；
                                                     * 預留欄位，供跨 SoC / 虛擬化場景做時鐘域 offset 校正 */
    _Atomic uint64_t seq;
    _Atomic uint32_t drop_count;
} vfr_shm_header_t;

/* ─── Backpressure Policy ────────────────────────────────────────────────── */
/* 直接用整數值，與上方 #define 一致 */
typedef enum {
    VFR_BP_DROP_OLDEST    = VFR_POLICY_DROP_OLDEST,    /* 即時性優先（RTSP、Preview） */
    VFR_BP_BLOCK_PRODUCER = VFR_POLICY_BLOCK_PRODUCER, /* 完整性優先（Recorder） */
    VFR_BP_SKIP_SELF      = VFR_POLICY_SKIP_SELF,      /* 自己跳過（AI inference） */
} vfr_backpressure_t;

#endif /* VFR_DEFS_H */
