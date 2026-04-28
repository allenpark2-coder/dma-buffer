/*
 * test/test_rec_full.c — Phase R4: full integration test for rec_engine
 *
 * Tests:
 *   T1  rec_schedule_query() unit tests (pure, no I/O)
 *   T2  CONTINUOUS mode: push frames → verify .ts file written
 *   T3  EVENT mode: pre-roll + AI trigger socket → clip written
 *   T4  SCHEDULED mode: slot OFF → no write; slot ON → write
 *   T5  rec_engine_destroy(): clean shutdown, no double-free
 *   T6  Write-queue overflow: dropped_frames increments correctly
 *   T7  Prometheus metrics: HTTP scrape returns correct metric names + values
 *
 * Build (no VFR dependency):
 *   gcc -std=c11 -D_GNU_SOURCE -Wall -I. -Irec -Iinclude \
 *       rec/rec_schedule.c rec/rec_metrics.c rec/rec_buf.c rec/rec_debounce.c \
 *       rec/rec_state.c rec/rec_trigger.c rec/rec_ts_mux.c rec/rec_segment.c \
 *       rec/rec_writer.c rec/rec_engine.c test/test_rec_full.c -lpthread -o test_rec_full
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <stddef.h>    /* offsetof */

#include "rec_engine.h"
#include "rec_defs.h"
#include "rec_schedule.h"

/* vfr_ipc_types.h for vfr_event_msg_t */
#include "../ipc/vfr_ipc_types.h"
/* vfr_defs.h for VFR_FMT_H264 */
#include "../include/vfr_defs.h"

/* ─── Test infrastructure ──────────────────────────────────────────────────── */
static int g_pass = 0;
static int g_fail = 0;

#define PASS(name) do { printf("  PASS  %s\n", (name)); g_pass++; } while (0)
#define FAIL(name) do { printf("  FAIL  %s\n", (name)); g_fail++; } while (0)
#define CHECK(name, cond) do { if (cond) PASS(name); else FAIL(name); } while (0)

/* ─── Test output directory ────────────────────────────────────────────────── */
#define OUT_DIR  "/tmp/test_rec_full_out"

static void mkdir_p(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* Remove all .ts files from a directory */
static void clean_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 3 && strcmp(e->d_name + len - 3, ".ts") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
        }
    }
    closedir(d);
}

/* Count .ts files in a directory; return total bytes written */
static uint64_t ts_file_bytes(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    uint64_t total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len > 3 && strcmp(e->d_name + len - 3, ".ts") == 0) {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            struct stat st;
            if (stat(path, &st) == 0) total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
    return total;
}

/* ─── Minimal H.264 fake-frame generator ──────────────────────────────────── */
/* IDR NALU start code + NALU type 0x65 (IDR slice) */
static const uint8_t IDR_FRAME[] = {
    0x00, 0x00, 0x00, 0x01, 0x65,  /* IDR slice NALU */
    0x88, 0x84, 0x00, 0x33, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};
/* P-frame NALU type 0x41 */
static const uint8_t P_FRAME[] = {
    0x00, 0x00, 0x00, 0x01, 0x41,
    0x9a, 0x3c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* SPS+PPS codec config */
static const uint8_t CODEC_CONFIG[] = {
    /* SPS NALU type 0x67 */
    0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0xc0, 0x1e,
    0xda, 0x01, 0x40, 0x16, 0xec,
    /* PPS NALU type 0x68 */
    0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x38, 0x80,
};

/* Push N frames (1 IDR then N-1 P-frames) at 33 ms intervals */
static void push_frames(rec_engine_t *eng, int n, uint64_t *ts_ns_inout,
                        uint64_t *seq_inout)
{
    for (int i = 0; i < n; i++) {
        bool is_idr = (i == 0);
        const uint8_t *data = is_idr ? IDR_FRAME : P_FRAME;
        uint32_t       size = is_idr ? (uint32_t)sizeof(IDR_FRAME)
                                     : (uint32_t)sizeof(P_FRAME);
        rec_engine_push_frame(eng, data, size,
                              *ts_ns_inout, *seq_inout,
                              is_idr, VFR_FMT_H264, false);
        *ts_ns_inout += 33333333ull;   /* ~30 fps */
        (*seq_inout)++;
    }
}

/* Push codec config to the engine */
static void push_codec_config(rec_engine_t *eng)
{
    rec_engine_push_frame(eng, CODEC_CONFIG, sizeof(CODEC_CONFIG),
                          0, 0, false, VFR_FMT_H264, true /*is_codec_config*/);
}

/* ─── Helper: send AI trigger via abstract Unix socket ────────────────────── */
static int send_trigger(const char *stream_name, rec_trigger_type_t type)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Abstract namespace: sun_path[0] = '\0', rest is the name */
    int name_len = snprintf(addr.sun_path + 1, sizeof(addr.sun_path) - 1,
                            "/vfr/event/%s", stream_name);
    socklen_t addrlen = (socklen_t)(offsetof(struct sockaddr_un, sun_path)
                                    + 1 + name_len);

    if (connect(fd, (struct sockaddr *)&addr, addrlen) < 0) {
        close(fd);
        return -1;
    }

    vfr_event_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.magic      = VFR_EVENT_MAGIC;
    msg.event_type = (uint32_t)type;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    msg.timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    msg.confidence = 0.9f;
    strncpy(msg.stream_name, stream_name, sizeof(msg.stream_name) - 1);
    strncpy(msg.label, "motion", sizeof(msg.label) - 1);

    ssize_t rc = write(fd, &msg, sizeof(msg));
    close(fd);
    return (rc == (ssize_t)sizeof(msg)) ? 0 : -1;
}

/* ─── T1: rec_schedule_query() unit tests ──────────────────────────────────── */
static void test_schedule_query(void)
{
    printf("\nT1: rec_schedule_query\n");

    rec_schedule_t sched;

    /* All OFF */
    rec_schedule_init_always(&sched, REC_SLOT_OFF);
    {
        rec_slot_mode_t s = rec_schedule_query(&sched, time(NULL));
        CHECK("all-OFF → REC_SLOT_OFF", s == REC_SLOT_OFF);
    }

    /* All CONTINUOUS */
    rec_schedule_init_always(&sched, REC_SLOT_CONTINUOUS);
    {
        rec_slot_mode_t s = rec_schedule_query(&sched, time(NULL));
        CHECK("all-CONTINUOUS → REC_SLOT_CONTINUOUS", s == REC_SLOT_CONTINUOUS);
    }

    /* Set Monday 08:00–08:15 (slot index 32) to EVENT, rest CONTINUOUS */
    rec_schedule_init_always(&sched, REC_SLOT_CONTINUOUS);
    rec_schedule_set_slot(&sched, 1 /*Monday*/, 32, REC_SLOT_EVENT);
    {
        /* Read Monday slot 32 back */
        rec_schedule_t *sp = &sched;
        int bit_offset = 32 * 2;
        uint8_t byte   = sp->days[1].slots[bit_offset / 8];
        rec_slot_mode_t v = (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3u);
        CHECK("set_slot: Monday slot32 == EVENT", v == REC_SLOT_EVENT);
    }
    {
        /* Neighbouring slot 31 should still be CONTINUOUS */
        int bit_offset = 31 * 2;
        uint8_t byte   = sched.days[1].slots[bit_offset / 8];
        rec_slot_mode_t v = (rec_slot_mode_t)((byte >> (bit_offset % 8)) & 0x3u);
        CHECK("set_slot: Monday slot31 still CONTINUOUS", v == REC_SLOT_CONTINUOUS);
    }

    /* Boundary: slot 0 and slot 95 */
    rec_schedule_init_always(&sched, REC_SLOT_OFF);
    rec_schedule_set_slot(&sched, 0, 0,  REC_SLOT_CONTINUOUS);
    rec_schedule_set_slot(&sched, 0, 95, REC_SLOT_EVENT);
    {
        int b0 = 0 * 2;
        uint8_t by0 = sched.days[0].slots[b0 / 8];
        CHECK("boundary: slot0 == CONTINUOUS",
              ((by0 >> (b0 % 8)) & 0x3u) == (uint8_t)REC_SLOT_CONTINUOUS);
        int b95 = 95 * 2;
        uint8_t by95 = sched.days[0].slots[b95 / 8];
        CHECK("boundary: slot95 == EVENT",
              ((by95 >> (b95 % 8)) & 0x3u) == (uint8_t)REC_SLOT_EVENT);
    }

    /* Out-of-range calls must not crash */
    rec_schedule_set_slot(&sched, -1, 0, REC_SLOT_CONTINUOUS);
    rec_schedule_set_slot(&sched, 7, 0, REC_SLOT_CONTINUOUS);
    rec_schedule_set_slot(&sched, 0, -1, REC_SLOT_CONTINUOUS);
    rec_schedule_set_slot(&sched, 0, 96, REC_SLOT_CONTINUOUS);
    PASS("out-of-range set_slot: no crash");
}

/* ─── T2: CONTINUOUS mode ──────────────────────────────────────────────────── */
static void test_continuous_mode(void)
{
    printf("\nT2: CONTINUOUS mode\n");

    clean_dir(OUT_DIR);

    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.stream_name,  "cont_test",       sizeof(cfg.stream_name) - 1);
    strncpy(cfg.output_dir,   OUT_DIR,            sizeof(cfg.output_dir) - 1);
    cfg.default_mode          = REC_MODE_CONTINUOUS;
    cfg.segment_duration_sec  = 600;
    cfg.segment_size_max      = (uint64_t)REC_SEGMENT_SIZE_MAX;
    cfg.ring_buf_size         = REC_BUF_SIZE_MIN;
    rec_schedule_init_always(&cfg.schedule, REC_SLOT_CONTINUOUS);

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create CONTINUOUS engine", eng != NULL);
    if (!eng) return;

    push_codec_config(eng);

    uint64_t ts  = 1000000000ull;
    uint64_t seq = 1;
    /* Push 60 frames (2 seconds at 30 fps): first is IDR, rest P */
    push_frames(eng, 60, &ts, &seq);

    usleep(100000);  /* 100 ms: let writer thread drain the write queue */

    uint64_t written_before = rec_engine_get_written_bytes(eng);
    CHECK("written_bytes > 0 after 60 frames", written_before > 0);

    rec_engine_destroy(&eng);
    CHECK("engine ptr NULL after destroy", eng == NULL);

    uint64_t disk_bytes = ts_file_bytes(OUT_DIR);
    CHECK("at least one .ts file on disk with data", disk_bytes > 0);

    printf("    written_bytes=%llu  disk=%llu\n",
           (unsigned long long)written_before,
           (unsigned long long)disk_bytes);
}

/* ─── T3: EVENT mode with AI trigger ──────────────────────────────────────── */
static void test_event_mode(void)
{
    printf("\nT3: EVENT mode with AI trigger\n");

    clean_dir(OUT_DIR);

    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.stream_name, "evt_test",  sizeof(cfg.stream_name) - 1);
    strncpy(cfg.output_dir,  OUT_DIR,     sizeof(cfg.output_dir) - 1);
    cfg.default_mode         = REC_MODE_EVENT;
    cfg.pre_record_sec       = 3;
    cfg.post_record_sec      = REC_POST_RECORD_SEC_DEFAULT;
    cfg.segment_duration_sec = 600;
    cfg.segment_size_max     = (uint64_t)REC_SEGMENT_SIZE_MAX;
    cfg.ring_buf_size        = REC_BUF_SIZE_MIN;
    rec_schedule_init_always(&cfg.schedule, REC_SLOT_EVENT);

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create EVENT engine", eng != NULL);
    if (!eng) return;

    /* Codec config must arrive before any IDR */
    push_codec_config(eng);

    uint64_t ts  = 1000000000ull;
    uint64_t seq = 1;

    /* Phase 1: push 30 pre-roll frames (fills ring buffer) */
    push_frames(eng, 30, &ts, &seq);
    CHECK("state IDLE before trigger", rec_engine_get_state(eng) == REC_STATE_IDLE);
    CHECK("no bytes written before trigger", rec_engine_get_written_bytes(eng) == 0);

    /* Phase 2: send TRIGGER_START via abstract Unix socket */
    int fds[REC_MAX_EPOLL_FDS];
    int nfds = rec_engine_get_epoll_fds(eng, fds, REC_MAX_EPOLL_FDS);
    CHECK("get_epoll_fds returns ≥ 1 fd", nfds >= 1);

    int rc = send_trigger("evt_test", REC_TRIGGER_START);
    CHECK("send TRIGGER_START succeeds", rc == 0);

    /* Call handle_event on each fd until the trigger is consumed */
    for (int i = 0; i < nfds; i++) {
        rec_engine_handle_event(eng, fds[i]);
        /* Small yield so writer can process pre-roll */
        usleep(10000);  /* 10 ms */
    }

    /* State should now be IN_EVENT or WAIT_KEYFRAME */
    rec_state_t state = rec_engine_get_state(eng);
    CHECK("state IN_EVENT or WAIT_KEYFRAME after trigger",
          state == REC_STATE_IN_EVENT || state == REC_STATE_WAIT_KEYFRAME);

    /* Phase 3: push live frames */
    push_frames(eng, 60, &ts, &seq);

    usleep(50000);  /* 50 ms: let Writer drain queues */

    uint64_t written = rec_engine_get_written_bytes(eng);
    CHECK("bytes written after live frames", written > 0);

    /* Phase 4: send TRIGGER_STOP */
    rc = send_trigger("evt_test", REC_TRIGGER_STOP);
    CHECK("send TRIGGER_STOP succeeds", rc == 0);
    for (int i = 0; i < nfds; i++)
        rec_engine_handle_event(eng, fds[i]);

    /* Push post-roll frames */
    push_frames(eng, 30, &ts, &seq);

    rec_engine_destroy(&eng);
    CHECK("destroy EVENT engine cleanly", eng == NULL);

    uint64_t disk_bytes = ts_file_bytes(OUT_DIR);
    CHECK("EVENT clip written to disk", disk_bytes > 0);

    printf("    written_bytes=%llu  disk=%llu\n",
           (unsigned long long)written,
           (unsigned long long)disk_bytes);
}

/* ─── T4: SCHEDULED mode ───────────────────────────────────────────────────── */
static void test_scheduled_mode(void)
{
    printf("\nT4: SCHEDULED mode\n");

    clean_dir(OUT_DIR);

    /* Schedule: all slots OFF */
    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.stream_name, "sched_test", sizeof(cfg.stream_name) - 1);
    strncpy(cfg.output_dir,  OUT_DIR,      sizeof(cfg.output_dir) - 1);
    cfg.default_mode         = REC_MODE_SCHEDULED;
    cfg.segment_duration_sec = 600;
    cfg.segment_size_max     = (uint64_t)REC_SEGMENT_SIZE_MAX;
    cfg.ring_buf_size        = REC_BUF_SIZE_MIN;
    rec_schedule_init_always(&cfg.schedule, REC_SLOT_OFF);

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create SCHEDULED engine (all-OFF)", eng != NULL);
    if (!eng) return;

    push_codec_config(eng);

    uint64_t ts  = 2000000000ull;
    uint64_t seq = 1;
    /* Push frames — schedule is OFF, nothing should be written */
    push_frames(eng, 30, &ts, &seq);
    usleep(20000);
    uint64_t written_off = rec_engine_get_written_bytes(eng);
    CHECK("no bytes written when schedule is OFF", written_off == 0);

    rec_engine_destroy(&eng);

    /* Re-create with schedule = all CONTINUOUS */
    clean_dir(OUT_DIR);
    rec_schedule_init_always(&cfg.schedule, REC_SLOT_CONTINUOUS);
    eng = rec_engine_create(&cfg);
    CHECK("create SCHEDULED engine (all-CONTINUOUS)", eng != NULL);
    if (!eng) return;

    push_codec_config(eng);

    ts  = 2000000000ull;
    seq = 1;
    push_frames(eng, 30, &ts, &seq);
    usleep(20000);
    uint64_t written_on = rec_engine_get_written_bytes(eng);
    CHECK("bytes written when schedule is CONTINUOUS", written_on > 0);

    rec_engine_destroy(&eng);
    CHECK("destroy SCHEDULED engine", eng == NULL);
}

/* ─── T5: Double destroy is safe (idempotent) ──────────────────────────────── */
static void test_destroy_idempotent(void)
{
    printf("\nT5: destroy idempotent\n");

    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.stream_name, "destroy_test", sizeof(cfg.stream_name) - 1);
    strncpy(cfg.output_dir,  OUT_DIR,        sizeof(cfg.output_dir) - 1);
    cfg.default_mode = REC_MODE_CONTINUOUS;
    cfg.ring_buf_size = REC_BUF_SIZE_MIN;

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create engine for destroy test", eng != NULL);
    if (!eng) return;

    push_codec_config(eng);

    uint64_t ts = 0, seq = 0;
    push_frames(eng, 5, &ts, &seq);

    rec_engine_destroy(&eng);
    CHECK("first destroy: ptr NULL", eng == NULL);
    rec_engine_destroy(&eng);   /* must not crash */
    PASS("second destroy on NULL: no crash");
}

/* ─── T6: Write-queue overflow ──────────────────────────────────────────────── */
static void test_write_queue_overflow(void)
{
    printf("\nT6: write-queue overflow → dropped_frames\n");

    /*
     * We flood the write queue faster than the Writer thread can drain it.
     * Use REC_WRITE_QUEUE_DEPTH + extra frames to guarantee overflow.
     */
    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strncpy(cfg.stream_name, "overflow_test", sizeof(cfg.stream_name) - 1);
    strncpy(cfg.output_dir,  OUT_DIR,         sizeof(cfg.output_dir) - 1);
    cfg.default_mode  = REC_MODE_CONTINUOUS;
    cfg.ring_buf_size = REC_BUF_SIZE_MAX;   /* large ring so push never fails */

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create engine for overflow test", eng != NULL);
    if (!eng) return;

    push_codec_config(eng);

    /*
     * Push REC_WRITE_QUEUE_DEPTH * 2 frames without sleeping.
     * The Writer thread won't be able to drain them all immediately.
     */
    uint64_t ts  = 3000000000ull;
    uint64_t seq = 1;
    int flood = REC_WRITE_QUEUE_DEPTH * 2;
    for (int i = 0; i < flood; i++) {
        bool is_idr = (i == 0);
        const uint8_t *d = is_idr ? IDR_FRAME : P_FRAME;
        uint32_t sz      = is_idr ? (uint32_t)sizeof(IDR_FRAME)
                                  : (uint32_t)sizeof(P_FRAME);
        rec_engine_push_frame(eng, d, sz, ts, seq, is_idr, VFR_FMT_H264, false);
        ts  += 33333333ull;
        seq++;
    }

    uint32_t drops = rec_engine_get_dropped_frames(eng);
    CHECK("dropped_frames > 0 on overflow", drops > 0);
    printf("    flooded %d frames → dropped %u\n", flood, drops);

    rec_engine_destroy(&eng);
}

/* ─── T7: Prometheus metrics HTTP scrape ───────────────────────────────────── */

#define METRICS_PORT 19200   /* distinct from vfr_metrics default (9100) */

static void test_prometheus_metrics(void)
{
    printf("\nT7: Prometheus metrics HTTP scrape\n");

    clean_dir(OUT_DIR);

    rec_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.stream_name, sizeof(cfg.stream_name), "%s", "met_test");
    snprintf(cfg.output_dir,  sizeof(cfg.output_dir),  "%s", OUT_DIR);
    cfg.default_mode  = REC_MODE_CONTINUOUS;
    cfg.ring_buf_size = REC_BUF_SIZE_MIN;
    cfg.metrics_port  = METRICS_PORT;

    rec_engine_t *eng = rec_engine_create(&cfg);
    CHECK("create engine with metrics_port", eng != NULL);
    if (!eng) return;

    /* Metrics listen fd must appear in get_epoll_fds */
    int fds[REC_MAX_EPOLL_FDS];
    int nfds = rec_engine_get_epoll_fds(eng, fds, REC_MAX_EPOLL_FDS);
    /* Expect: timerfd + trigger_fd + metrics_fd = at least 2 */
    CHECK("get_epoll_fds returns ≥ 2 fds (timer + metrics)", nfds >= 2);

    /* Push frames so written_bytes > 0 */
    push_codec_config(eng);
    uint64_t ts = 5000000000ull, seq = 1;
    push_frames(eng, 30, &ts, &seq);
    usleep(100000);   /* let writer drain */
    CHECK("written_bytes > 0 before scrape",
          rec_engine_get_written_bytes(eng) > 0);

    /*
     * Synchronous scrape without a second thread:
     *
     *   1. Client socket: connect (kernel accepts TCP SYN immediately via
     *      the listen backlog, so connect() returns before handle_event).
     *   2. Client: send the GET request into the kernel TX buffer.
     *   3. Engine:  handle_event → accept4 + recv(GET) + send(response) + close.
     *      The GET data is already in the RX buffer, so recv() returns immediately.
     *      The response lands in the client's RX buffer before close().
     *   4. Client: recv reads the response that the kernel already buffered.
     */
    int cfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK("open client socket for scrape", cfd >= 0);
    if (cfd < 0) { rec_engine_destroy(&eng); return; }

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_port        = htons(METRICS_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    int conn_ok = (connect(cfd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    CHECK("connect to metrics port", conn_ok);

    if (conn_ok) {
        /* Send GET — data lands in kernel buffers immediately */
        const char *req = "GET /metrics HTTP/1.0\r\nHost: localhost\r\n\r\n";
        send(cfd, req, strlen(req), MSG_NOSIGNAL);

        /* Engine serves the queued connection.
         * Call handle_event on every fd — only the matching one reacts. */
        for (int i = 0; i < nfds; i++)
            rec_engine_handle_event(eng, fds[i]);

        /* Server has already sent the response and closed its side.
         * Read until EOF so we get the full body. */
        char rbuf[4096];
        ssize_t n = 0, r;
        while ((r = recv(cfd, rbuf + n, sizeof(rbuf) - 1 - (size_t)n, 0)) > 0)
            n += r;
        rbuf[n] = '\0';

        CHECK("response received (> 0 bytes)", n > 0);
        CHECK("HTTP 200 OK in response",
              strstr(rbuf, "200 OK") != NULL);
        CHECK("metric rec_written_bytes_total present",
              strstr(rbuf, "rec_written_bytes_total") != NULL);
        CHECK("metric rec_drop_frames_total present",
              strstr(rbuf, "rec_drop_frames_total") != NULL);
        CHECK("metric rec_state present",
              strstr(rbuf, "rec_state{") != NULL);
        CHECK("stream label 'met_test' in output",
              strstr(rbuf, "met_test") != NULL);
        /* written_bytes must be non-zero: the line must NOT end with "} 0\n" */
        CHECK("rec_written_bytes_total value > 0",
              strstr(rbuf, "rec_written_bytes_total{stream=\"met_test\"} 0\n") == NULL
              && strstr(rbuf, "rec_written_bytes_total") != NULL);

        const char *body = strstr(rbuf, "\r\n\r\n");
        if (body) printf("    --- metrics body ---\n%s\n", body + 4);
    }
    close(cfd);

    rec_engine_destroy(&eng);
    CHECK("destroy metrics engine", eng == NULL);
}

/* ─── main ─────────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("=== Phase R4: rec_engine full integration tests ===\n");

    mkdir_p(OUT_DIR);

    test_schedule_query();
    test_continuous_mode();
    test_event_mode();
    test_scheduled_mode();
    test_destroy_idempotent();
    test_write_queue_overflow();
    test_prometheus_metrics();

    printf("\n=== Results: %d PASS / %d FAIL ===\n", g_pass, g_fail);

    /* Clean up output */
    clean_dir(OUT_DIR);

    return g_fail ? 1 : 0;
}
