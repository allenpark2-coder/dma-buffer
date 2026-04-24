/* sdk/vfr_metrics.h — Prometheus-compatible Metrics 收集與輸出
 *
 * 用途：供 producer 埋點，輸出 drop_count、frame latency histogram、
 *       slot usage histogram，以 Prometheus text format 呈現。
 *
 * 整合方式：
 *   vfr_metrics_t *m = vfr_metrics_create("cam0", slot_count);
 *   vfr_metrics_set_server(m, srv);                 // 串接 server drop count
 *   int lfd = vfr_metrics_listen(m, 9100);          // 取得 listen fd
 *   // 加入 epoll，EPOLLIN 時呼叫 vfr_metrics_serve_one(m, lfd)
 *   // 每幀：vfr_metrics_observe_latency(m, latency_ns);
 *   //        vfr_metrics_observe_slot_usage(m, used);
 *
 * log 等級：
 *   INFO  — metrics HTTP 連線事件
 *   DEBUG — 每幀 latency 數值（高 FPS 場景建議關閉）
 */
#ifndef VFR_METRICS_H
#define VFR_METRICS_H

#include "vfr_defs.h"

/* ─── 前向宣告（避免引入 vfr_server.h）──────────────────────────────────── */
typedef struct vfr_server vfr_server_t;

/* ─── Histogram bucket 定義 ─────────────────────────────────────────────── */

/* Latency buckets（nanoseconds 上界，最後一桶為 +Inf）*/
#define VFR_METRICS_LAT_NBUCKETS  7
static const uint64_t VFR_METRICS_LAT_BOUNDS[VFR_METRICS_LAT_NBUCKETS] = {
    1000000ULL,          /*  1 ms */
    2000000ULL,          /*  2 ms */
    5000000ULL,          /*  5 ms */
    10000000ULL,         /* 10 ms */
    33333333ULL,         /* 33.3 ms ≈ 1 frame @ 30fps */
    100000000ULL,        /* 100 ms */
    UINT64_MAX           /* +Inf   */
};
static const char *const VFR_METRICS_LAT_LE[VFR_METRICS_LAT_NBUCKETS] = {
    "0.001", "0.002", "0.005", "0.010", "0.0333", "0.100", "+Inf"
};

/* Slot usage buckets（ratio 上界，最後一桶為 +Inf）*/
#define VFR_METRICS_SLOT_NBUCKETS 5
static const double VFR_METRICS_SLOT_BOUNDS[VFR_METRICS_SLOT_NBUCKETS] = {
    0.25, 0.50, 0.75, 1.00, 1.01   /* 1.01 代表 +Inf（pool 100% 滿）*/
};
static const char *const VFR_METRICS_SLOT_LE[VFR_METRICS_SLOT_NBUCKETS] = {
    "0.25", "0.50", "0.75", "1.00", "+Inf"
};

/* ─── opaque handle ─────────────────────────────────────────────────────── */
typedef struct vfr_metrics vfr_metrics_t;

/* ─── 生命週期 ───────────────────────────────────────────────────────────── */

/*
 * vfr_metrics_create()：
 *   stream_name — 標籤名稱，嵌入 Prometheus label
 *   slot_total  — pool 總 slot 數，供 slot usage 比率計算
 * 回傳：成功回傳 handle；失敗回傳 NULL
 */
vfr_metrics_t *vfr_metrics_create(const char *stream_name, uint32_t slot_total);

/*
 * vfr_metrics_set_server()：
 *   綁定 server，metrics format 時自動讀取 drop_count / session_count。
 *   可選；未設定時 drop_count 從內部 counter 累計。
 */
void vfr_metrics_set_server(vfr_metrics_t *m, vfr_server_t *srv);

/*
 * vfr_metrics_destroy()：
 *   釋放資源，*m 設為 NULL；重複呼叫為 no-op。
 *   注意：呼叫前需先關閉 listen fd（若已呼叫 vfr_metrics_listen）。
 */
void vfr_metrics_destroy(vfr_metrics_t **m);

/* ─── 埋點 API ───────────────────────────────────────────────────────────── */

/*
 * vfr_metrics_observe_latency()：
 *   記錄一幀的 latency（從 timestamp_ns 到 dispatch 完成的差值）。
 *   thread-safe（原子累計）。
 *   log 等級：DEBUG（高 FPS 場景頻繁，不應在 INFO 輸出）。
 */
void vfr_metrics_observe_latency(vfr_metrics_t *m, uint64_t latency_ns);

/*
 * vfr_metrics_observe_slot_usage()：
 *   記錄目前 pool 使用量。
 *   used — 目前已配置的 slot 數
 * thread-safe（原子累計）。
 */
void vfr_metrics_observe_slot_usage(vfr_metrics_t *m, uint32_t used);

/* ─── 格式化輸出 ──────────────────────────────────────────────────────────── */

/*
 * vfr_metrics_format()：
 *   將目前所有 metrics 格式化為 Prometheus text format，寫入 buf。
 *   buflen — buf 容量（建議 4096 以上）
 * 回傳：寫入的 bytes 數（不含 NUL）；若 buf 不夠大回傳 -1。
 * log 等級：INFO（每次 scrape 事件）。
 */
int vfr_metrics_format(vfr_metrics_t *m, char *buf, size_t buflen);

/* ─── HTTP Endpoint（Prometheus scrape 用）───────────────────────────────── */

/*
 * vfr_metrics_listen()：
 *   在指定 TCP port 建立 listen socket，回傳 fd（非阻塞）。
 *   呼叫者負責關閉此 fd（在 vfr_metrics_destroy 之前）。
 * 回傳：listen fd；失敗回傳 -1。
 */
int vfr_metrics_listen(uint16_t port);

/*
 * vfr_metrics_serve_one()：
 *   在 listen_fd 上 accept 一個連線，回應 HTTP/1.0 200 OK + metrics body，
 *   然後關閉連線。非阻塞（accept 失敗時立即回傳）。
 * 回傳：0 = 成功服務一個請求；-1 = accept 失敗（EAGAIN 也算 -1）。
 * log 等級：INFO（連線事件）。
 */
int vfr_metrics_serve_one(vfr_metrics_t *m, int listen_fd);

#endif /* VFR_METRICS_H */
