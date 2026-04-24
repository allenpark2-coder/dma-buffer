/* sdk/vfr_metrics.c — Prometheus Metrics 實作
 *
 * 收集三類指標：
 *   1. vfr_drop_total        — counter（從 server SHM 讀取，或內部累計）
 *   2. vfr_latency_seconds   — histogram（frame capture → dispatch latency）
 *   3. vfr_slot_usage_ratio  — histogram（slot used / slot total）
 *   4. vfr_session_count     — gauge（目前連線 consumer 數，若有 server）
 *
 * HTTP server：極簡 TCP socket，只支援 GET /metrics（HTTP/1.0）。
 *   不支援 keep-alive；不支援 path routing；不支援 chunked transfer。
 *
 * Thread safety：
 *   observe_* 函數使用 _Atomic uint64_t bucket 累計，無 mutex。
 *   vfr_metrics_format 讀取 snapshot，在 scrape 期間 metrics 可能仍在更新——
 *   這對 Prometheus 收集而言是可接受的 eventual consistency。
 */

#include "sdk/vfr_metrics.h"
#include "ipc/vfr_server.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

/* TCP listen */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ─── 內部結構 ───────────────────────────────────────────────────────────── */
struct vfr_metrics {
    char           stream_name[VFR_SOCKET_NAME_MAX];
    uint32_t       slot_total;
    vfr_server_t  *srv;      /* 可為 NULL */

    /* latency histogram buckets（累計次數，最後 index = +Inf）*/
    _Atomic uint64_t lat_bucket[VFR_METRICS_LAT_NBUCKETS];
    _Atomic uint64_t lat_sum_ns;   /* 所有樣本 latency 總和（ns）*/
    _Atomic uint64_t lat_count;    /* 樣本總數 */

    /* slot usage histogram buckets */
    _Atomic uint64_t slot_bucket[VFR_METRICS_SLOT_NBUCKETS];
    _Atomic uint64_t slot_count;   /* 樣本總數 */

    /* 內部 drop counter（無 server 時使用）*/
    _Atomic uint64_t drop_total;
};

/* ─── 生命週期 ───────────────────────────────────────────────────────────── */

vfr_metrics_t *vfr_metrics_create(const char *stream_name, uint32_t slot_total)
{
    if (!stream_name) return NULL;

    vfr_metrics_t *m = calloc(1, sizeof(*m));
    if (!m) return NULL;

    size_t nlen = strlen(stream_name);
    if (nlen >= VFR_SOCKET_NAME_MAX) {
        VFR_LOGE("vfr_metrics_create: stream_name too long (%zu >= %d)",
                 nlen, VFR_SOCKET_NAME_MAX);
        free(m);
        return NULL;
    }
    memcpy(m->stream_name, stream_name, nlen + 1);
    m->slot_total = slot_total ? slot_total : 1;   /* 防止除以零 */

    VFR_LOGI("metrics created: stream=%s slot_total=%u", stream_name, m->slot_total);
    return m;
}

void vfr_metrics_set_server(vfr_metrics_t *m, vfr_server_t *srv)
{
    if (m) m->srv = srv;
}

void vfr_metrics_destroy(vfr_metrics_t **m)
{
    if (!m || !*m) return;
    VFR_LOGI("metrics destroyed: stream=%s", (*m)->stream_name);
    free(*m);
    *m = NULL;
}

/* ─── 埋點 ───────────────────────────────────────────────────────────────── */

void vfr_metrics_observe_latency(vfr_metrics_t *m, uint64_t latency_ns)
{
    if (!m) return;

    /* 找到對應 bucket（從小到大，第一個 >= latency_ns 的上界）*/
    for (int i = 0; i < VFR_METRICS_LAT_NBUCKETS; i++) {
        if (latency_ns <= VFR_METRICS_LAT_BOUNDS[i]) {
            /* Prometheus 累積直方圖：所有 le >= latency_ns 的 bucket 都 +1 */
            for (int j = i; j < VFR_METRICS_LAT_NBUCKETS; j++) {
                atomic_fetch_add_explicit(&m->lat_bucket[j], 1, memory_order_relaxed);
            }
            break;
        }
    }
    atomic_fetch_add_explicit(&m->lat_sum_ns, latency_ns, memory_order_relaxed);
    atomic_fetch_add_explicit(&m->lat_count,  1,          memory_order_relaxed);

    VFR_LOGD("latency %.3f ms", (double)latency_ns / 1e6);
}

void vfr_metrics_observe_slot_usage(vfr_metrics_t *m, uint32_t used)
{
    if (!m) return;

    double ratio = (m->slot_total > 0)
                   ? (double)used / (double)m->slot_total
                   : 0.0;

    for (int i = 0; i < VFR_METRICS_SLOT_NBUCKETS; i++) {
        if (ratio <= VFR_METRICS_SLOT_BOUNDS[i]) {
            for (int j = i; j < VFR_METRICS_SLOT_NBUCKETS; j++) {
                atomic_fetch_add_explicit(&m->slot_bucket[j], 1, memory_order_relaxed);
            }
            break;
        }
    }
    atomic_fetch_add_explicit(&m->slot_count, 1, memory_order_relaxed);
}

/* ─── Prometheus 格式化 ──────────────────────────────────────────────────── */

int vfr_metrics_format(vfr_metrics_t *m, char *buf, size_t buflen)
{
    if (!m || !buf || buflen == 0) return -1;

    /* 取 snapshot（從 atomic 讀，不做跨 bucket 原子 snapshot，可接受 eventual）*/
    uint64_t lat_bucket[VFR_METRICS_LAT_NBUCKETS];
    for (int i = 0; i < VFR_METRICS_LAT_NBUCKETS; i++)
        lat_bucket[i] = atomic_load_explicit(&m->lat_bucket[i], memory_order_relaxed);
    uint64_t lat_sum_ns = atomic_load_explicit(&m->lat_sum_ns, memory_order_relaxed);
    uint64_t lat_count  = atomic_load_explicit(&m->lat_count,  memory_order_relaxed);

    uint64_t slot_bucket[VFR_METRICS_SLOT_NBUCKETS];
    for (int i = 0; i < VFR_METRICS_SLOT_NBUCKETS; i++)
        slot_bucket[i] = atomic_load_explicit(&m->slot_bucket[i], memory_order_relaxed);
    uint64_t slot_count = atomic_load_explicit(&m->slot_count, memory_order_relaxed);

    /* drop_count：優先讀 server SHM（精確），否則讀內部 counter */
    uint64_t drop_total = m->srv
        ? (uint64_t)vfr_server_get_drop_count(m->srv)
        : atomic_load_explicit(&m->drop_total, memory_order_relaxed);

    /* session_count（需要 server）*/
    uint32_t sess_count = m->srv ? vfr_server_get_session_count(m->srv) : 0;

    const char *s = m->stream_name;
    int n = 0;
    size_t left = buflen;
    char *p = buf;

#define APPEND(fmt, ...) \
    do { \
        int _r = snprintf(p, left, fmt, ##__VA_ARGS__); \
        if (_r < 0 || (size_t)_r >= left) return -1; \
        p += _r; left -= (size_t)_r; n += _r; \
    } while (0)

    /* ── drop_total ─────────────────────────────────────────────────────── */
    APPEND("# HELP vfr_drop_total Total dropped frames (DROP_OLDEST triggered)\n");
    APPEND("# TYPE vfr_drop_total counter\n");
    APPEND("vfr_drop_total{stream=\"%s\"} %llu\n\n", s, (unsigned long long)drop_total);

    /* ── session_count ──────────────────────────────────────────────────── */
    if (m->srv) {
        APPEND("# HELP vfr_session_count Currently connected consumers\n");
        APPEND("# TYPE vfr_session_count gauge\n");
        APPEND("vfr_session_count{stream=\"%s\"} %u\n\n", s, sess_count);
    }

    /* ── latency histogram ──────────────────────────────────────────────── */
    APPEND("# HELP vfr_latency_seconds"
           " Frame latency from capture timestamp to consumer dispatch\n");
    APPEND("# TYPE vfr_latency_seconds histogram\n");
    for (int i = 0; i < VFR_METRICS_LAT_NBUCKETS; i++) {
        APPEND("vfr_latency_seconds_bucket{stream=\"%s\",le=\"%s\"} %llu\n",
               s, VFR_METRICS_LAT_LE[i], (unsigned long long)lat_bucket[i]);
    }
    /* sum in seconds */
    APPEND("vfr_latency_seconds_sum{stream=\"%s\"} %.9f\n",
           s, (double)lat_sum_ns / 1e9);
    APPEND("vfr_latency_seconds_count{stream=\"%s\"} %llu\n\n",
           s, (unsigned long long)lat_count);

    /* ── slot usage histogram ────────────────────────────────────────────── */
    APPEND("# HELP vfr_slot_usage_ratio"
           " Slot pool usage ratio (used/total) per observation\n");
    APPEND("# TYPE vfr_slot_usage_ratio histogram\n");
    for (int i = 0; i < VFR_METRICS_SLOT_NBUCKETS; i++) {
        APPEND("vfr_slot_usage_ratio_bucket{stream=\"%s\",le=\"%s\"} %llu\n",
               s, VFR_METRICS_SLOT_LE[i], (unsigned long long)slot_bucket[i]);
    }
    APPEND("vfr_slot_usage_ratio_count{stream=\"%s\"} %llu\n\n",
           s, (unsigned long long)slot_count);

#undef APPEND

    VFR_LOGI("metrics formatted: %d bytes (drop=%llu lat_count=%llu)",
             n, (unsigned long long)drop_total, (unsigned long long)lat_count);
    return n;
}

/* ─── HTTP Endpoint ──────────────────────────────────────────────────────── */

int vfr_metrics_listen(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        VFR_LOGE("socket: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        VFR_LOGE("bind port %u: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        VFR_LOGE("listen: %s", strerror(errno));
        close(fd);
        return -1;
    }

    VFR_LOGI("metrics HTTP listening on 127.0.0.1:%u", port);
    return fd;
}

int vfr_metrics_serve_one(vfr_metrics_t *m, int listen_fd)
{
    int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
    if (client < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            VFR_LOGW("accept: %s", strerror(errno));
        return -1;
    }

    VFR_LOGI("metrics scrape request accepted (fd=%d)", client);

    /* 格式化 Prometheus body */
    char body[8192];
    int body_len = vfr_metrics_format(m, body, sizeof(body));
    if (body_len < 0) {
        /* buffer 太小：截斷並告知 */
        snprintf(body, sizeof(body), "# ERROR: metrics buffer overflow\n");
        body_len = (int)strlen(body);
    }

    /* HTTP/1.0 response（簡單，不做 keep-alive）*/
    char header[256];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    /* 發送（忽略 SIGPIPE：呼叫者應 sigaction SIG_IGN 或 MSG_NOSIGNAL）*/
    ssize_t sent = send(client, header, (size_t)hlen, MSG_NOSIGNAL);
    if (sent > 0)
        send(client, body, (size_t)body_len, MSG_NOSIGNAL);

    close(client);
    return 0;
}
