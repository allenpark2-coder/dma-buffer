/* test/test_metrics.c — Phase 5 Metrics 驗收測試
 *
 * 測試項目：
 *   5.1 vfr_metrics_create / vfr_metrics_destroy（資源不洩漏）
 *   5.2 observe_latency：histogram bucket 累計正確
 *   5.3 observe_slot_usage：slot usage histogram 正確
 *   5.4 vfr_metrics_format：輸出符合 Prometheus text format
 *   5.5 HTTP scrape：啟動 listen，curl 驗收（需手動執行 --serve 模式）
 *   5.6 Registry：register → list → unregister（daemon 需在背景執行）
 *
 * 使用方式：
 *   ./test_metrics                  # 執行 5.1~5.4 自動測試
 *   ./test_metrics --serve          # 啟動 producer + HTTP metrics，Ctrl-C 結束
 *   ./test_metrics --registry-daemon  # 啟動 registry daemon（背景服務）
 *
 * 環境變數：
 *   VFR_METRICS_PORT  — HTTP listen port（預設 9100）
 */

#include "sdk/vfr_metrics.h"
#include "ipc/vfr_registry.h"
#include "ipc/vfr_server.h"
#include "vfr_defs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/epoll.h>
#include <time.h>

/* ─── Signal handler ─────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_running = 1;
static void sig_handler(int s) { (void)s; g_running = 0; }

/* ─── PASS / FAIL ─────────────────────────────────────────────────────────── */
static int g_pass = 0, g_fail = 0;
#define CHECK(label, cond) \
    do { \
        if (cond) { \
            fprintf(stderr, "  PASS [%s]\n", label); \
            g_pass++; \
        } else { \
            fprintf(stderr, "  FAIL [%s]  (line %d)\n", label, __LINE__); \
            g_fail++; \
        } \
    } while (0)

/* ─── 5.1 Create / Destroy ───────────────────────────────────────────────── */
static void test_lifecycle(void)
{
    fprintf(stderr, "\n=== 5.1 Lifecycle ===\n");

    vfr_metrics_t *m = vfr_metrics_create("test_stream", 8);
    CHECK("create returns non-NULL", m != NULL);

    vfr_metrics_destroy(&m);
    CHECK("destroy sets ptr to NULL", m == NULL);

    /* 重複呼叫為 no-op */
    vfr_metrics_destroy(&m);
    CHECK("double destroy is no-op", m == NULL);

    /* 太長的 stream_name */
    char long_name[VFR_SOCKET_NAME_MAX + 8];
    memset(long_name, 'x', sizeof(long_name) - 1);
    long_name[sizeof(long_name) - 1] = '\0';
    vfr_metrics_t *m2 = vfr_metrics_create(long_name, 8);
    CHECK("long stream_name returns NULL", m2 == NULL);
}

/* ─── 5.2 Latency Histogram ──────────────────────────────────────────────── */
static void test_latency_histogram(void)
{
    fprintf(stderr, "\n=== 5.2 Latency Histogram ===\n");

    vfr_metrics_t *m = vfr_metrics_create("lat_test", 8);

    /* 觀察 10 個 0.5 ms 樣本（落入 le="0.001" bucket）*/
    for (int i = 0; i < 10; i++)
        vfr_metrics_observe_latency(m, 500000ULL);  /* 0.5 ms */

    /* 觀察 5 個 3 ms 樣本（落入 le="0.005" bucket，但不在 le="0.002"）*/
    for (int i = 0; i < 5; i++)
        vfr_metrics_observe_latency(m, 3000000ULL); /* 3 ms */

    char buf[4096];
    int len = vfr_metrics_format(m, buf, sizeof(buf));
    CHECK("format returns positive length", len > 0);

    /* le="0.001" bucket 應 = 10（0.5ms 樣本）*/
    CHECK("le=0.001 bucket contains 0.5ms samples",
          strstr(buf, "le=\"0.001\"} 10") != NULL);

    /* le="0.005" bucket 應 = 15（10 + 5，累積直方圖）*/
    CHECK("le=0.005 bucket is cumulative",
          strstr(buf, "le=\"0.005\"} 15") != NULL);

    /* +Inf bucket 應 = 15 */
    CHECK("+Inf bucket equals total count",
          strstr(buf, "le=\"+Inf\"} 15") != NULL);

    /* count 應 = 15 */
    CHECK("latency count = 15",
          strstr(buf, "vfr_latency_seconds_count{stream=\"lat_test\"} 15") != NULL);

    vfr_metrics_destroy(&m);
}

/* ─── 5.3 Slot Usage Histogram ───────────────────────────────────────────── */
static void test_slot_usage_histogram(void)
{
    fprintf(stderr, "\n=== 5.3 Slot Usage Histogram ===\n");

    vfr_metrics_t *m = vfr_metrics_create("slot_test", 8);

    /* 2/8 = 0.25 → 落入 le="0.25" bucket（邊界值）*/
    vfr_metrics_observe_slot_usage(m, 2);

    /* 4/8 = 0.50 → 落入 le="0.50" */
    vfr_metrics_observe_slot_usage(m, 4);

    /* 8/8 = 1.00 → 落入 le="1.00" */
    vfr_metrics_observe_slot_usage(m, 8);

    char buf[4096];
    int len = vfr_metrics_format(m, buf, sizeof(buf));
    CHECK("slot format OK", len > 0);

    /* le="0.25" bucket：累計 = 1（只有第一個 2/8=0.25 的樣本 ≤ 0.25）*/
    CHECK("le=0.25 bucket = 1",
          strstr(buf, "le=\"0.25\"} 1") != NULL);

    /* le="0.50" bucket：累計 = 2（0.25 + 0.50）*/
    CHECK("le=0.50 bucket = 2",
          strstr(buf, "le=\"0.50\"} 2") != NULL);

    /* le="+Inf" bucket = 3 */
    CHECK("slot +Inf = 3",
          strstr(buf, "le=\"+Inf\"} 3") != NULL);

    vfr_metrics_destroy(&m);
}

/* ─── 5.4 Prometheus Format ──────────────────────────────────────────────── */
static void test_prometheus_format(void)
{
    fprintf(stderr, "\n=== 5.4 Prometheus Format ===\n");

    vfr_metrics_t *m = vfr_metrics_create("fmt_test", 8);

    char buf[4096];
    int len = vfr_metrics_format(m, buf, sizeof(buf));
    CHECK("format non-empty", len > 0);

    /* 必須含有標準 HELP / TYPE 標頭 */
    CHECK("has HELP vfr_drop_total",
          strstr(buf, "# HELP vfr_drop_total") != NULL);
    CHECK("has TYPE counter",
          strstr(buf, "# TYPE vfr_drop_total counter") != NULL);
    CHECK("has HELP latency histogram",
          strstr(buf, "# HELP vfr_latency_seconds") != NULL);
    CHECK("has TYPE histogram",
          strstr(buf, "# TYPE vfr_latency_seconds histogram") != NULL);
    CHECK("has slot usage histogram",
          strstr(buf, "# HELP vfr_slot_usage_ratio") != NULL);

    /* 小 buffer 應回傳 -1 */
    char tiny[32];
    int ret = vfr_metrics_format(m, tiny, sizeof(tiny));
    CHECK("tiny buf returns -1", ret == -1);

    vfr_metrics_destroy(&m);
}

/* ─── 5.5 HTTP Serve（--serve 模式）─────────────────────────────────────── */
static void run_serve_mode(void)
{
    const char *port_env = getenv("VFR_METRICS_PORT");
    uint16_t port = port_env ? (uint16_t)atoi(port_env) : 9100;

    fprintf(stderr, "[metrics-serve] Starting producer simulation + metrics HTTP on port %u\n", port);
    fprintf(stderr, "[metrics-serve] Try: curl http://127.0.0.1:%u/metrics\n", port);
    fprintf(stderr, "[metrics-serve] Press Ctrl-C to stop.\n");

    struct sigaction sa = { .sa_handler = sig_handler };
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    /* SIGPIPE 忽略（HTTP client 可能提前關連線）*/
    struct sigaction sa_pipe = { .sa_handler = SIG_IGN };
    sigaction(SIGPIPE, &sa_pipe, NULL);

    /* 建立 metrics 物件 */
    vfr_metrics_t *m = vfr_metrics_create("cam0", 8);
    if (!m) { fprintf(stderr, "vfr_metrics_create failed\n"); return; }

    /* 建立 VFR server（producer 角色）*/
    vfr_server_t *srv = vfr_server_create("cam0", 8);
    if (!srv) {
        fprintf(stderr, "vfr_server_create failed\n");
        vfr_metrics_destroy(&m);
        return;
    }
    vfr_metrics_set_server(m, srv);

    /* 登記 registry（若 daemon 有啟動）*/
    vfr_stream_info_t sinfo = {
        .stream_name  = "cam0",
        .width        = 1920,
        .height       = 1080,
        .format       = 0x3231564eu,  /* NV12 */
        .slot_count   = 8,
        .producer_pid = (int32_t)getpid(),
    };
    vfr_registry_register(&sinfo);

    /* Listen */
    int listen_fd = vfr_metrics_listen(port);
    if (listen_fd < 0) {
        fprintf(stderr, "vfr_metrics_listen failed\n");
        vfr_server_destroy(&srv);
        vfr_metrics_destroy(&m);
        return;
    }

    int epfd = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = listen_fd };
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

    uint64_t frame_count = 0;
    struct epoll_event events[4];

    while (g_running) {
        /* 模擬 produce（20 fps，50ms/frame）*/
        int ret = vfr_server_produce(srv);
        if (ret == 0) {
            /* 模擬 latency：1~5 ms */
            uint64_t lat_ns = (uint64_t)(1 + (frame_count % 5)) * 1000000ULL;
            vfr_metrics_observe_latency(m, lat_ns);
            vfr_metrics_observe_slot_usage(m, (frame_count % 8) + 1);
            frame_count++;
        }

        /* 處理 IPC 事件 */
        vfr_server_handle_events(srv, 0);

        /* 處理 HTTP scrape */
        int nev = epoll_wait(epfd, events, 4, 50 /*ms*/);
        for (int i = 0; i < nev; i++) {
            if (events[i].data.fd == listen_fd)
                vfr_metrics_serve_one(m, listen_fd);
        }
    }

    fprintf(stderr, "[metrics-serve] Stopping after %llu frames.\n",
            (unsigned long long)frame_count);

    vfr_registry_unregister("cam0");
    close(listen_fd);
    close(epfd);
    vfr_server_destroy(&srv);
    vfr_metrics_destroy(&m);
}

/* ─── 5.6 Registry Daemon（--registry-daemon 模式）─────────────────────── */
static void run_registry_daemon(void)
{
    fprintf(stderr, "[registry-daemon] Starting. Ctrl-C to stop.\n");
    vfr_registry_serve_forever();
}

/* ─── main ───────────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "--serve") == 0) {
        run_serve_mode();
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--registry-daemon") == 0) {
        run_registry_daemon();
        return 0;
    }

    /* 自動測試（5.1 ~ 5.4）*/
    fprintf(stderr, "=== Phase 5 Metrics Unit Tests ===\n");

    test_lifecycle();
    test_latency_histogram();
    test_slot_usage_histogram();
    test_prometheus_format();

    fprintf(stderr, "\n=== Summary: %d PASS, %d FAIL ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
