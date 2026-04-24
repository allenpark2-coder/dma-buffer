#define _GNU_SOURCE
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "rec_buf.h"
#include "rec_writer.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  FAIL (%s:%d): %s\n", __FILE__, __LINE__, (msg)); \
        g_fail++; \
    } else { \
        printf("  PASS: %s\n", (msg)); \
        g_pass++; \
    } \
} while (0)

#define TEST_BEGIN(name) do { printf("\n[TEST] %s\n", (name)); } while (0)

static void push_encoded_frames(rec_buf_t *buf,
                                uint32_t   count,
                                uint32_t   frame_size,
                                uint64_t   base_ts_ns,
                                uint32_t   keyframe_interval)
{
    uint8_t *frame = malloc(frame_size);
    for (uint32_t i = 0; i < count; ++i) {
        memset(frame, (int)(i & 0xff), frame_size);
        if (frame_size >= 5) {
            frame[0] = 0x00;
            frame[1] = 0x00;
            frame[2] = 0x00;
            frame[3] = 0x01;
            frame[4] = (i % keyframe_interval == 0) ? 0x65 : 0x41;
        }
        (void)rec_buf_push(buf,
                           frame,
                           frame_size,
                           base_ts_ns + (uint64_t)i * 33333333ULL,
                           i,
                           (i % keyframe_interval) == 0);
    }
    free(frame);
}

static int count_ts_files(const char *dir, char first_path[512])
{
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dp)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;
        if (strstr(ent->d_name, ".ts") == NULL)
            continue;

        count++;
        if (count == 1 && first_path) {
            snprintf(first_path, 512, "%s/%s", dir, ent->d_name);
        }
    }

    closedir(dp);
    return count;
}

static void wait_briefly(void)
{
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 120000000L };
    nanosleep(&ts, NULL);
}

static void test_preroll_drain_and_ts_headers(void)
{
    TEST_BEGIN("writer drains pre_queue and emits TS headers");

    char tmpdir[] = "/tmp/rec_writer_r3_XXXXXX";
    CHECK(mkdtemp(tmpdir) != NULL, "mkdtemp ok");

    rec_buf_t *buf = rec_buf_create(4 * 1024 * 1024);
    CHECK(buf != NULL, "rec_buf_create ok");
    push_encoded_frames(buf, 60, 512, 10ULL * 1000000000ULL, 30);

    rec_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", tmpdir);
    snprintf(cfg.stream_name, sizeof(cfg.stream_name), "%s", "cam0");
    cfg.mode = REC_MODE_EVENT;
    cfg.segment_duration_sec = REC_SEGMENT_DURATION_SEC;
    cfg.segment_size_max = REC_SEGMENT_SIZE_MAX;
    cfg.flush_interval_sec = 1;

    rec_writer_t *writer = rec_writer_create(&cfg, buf);
    CHECK(writer != NULL, "rec_writer_create ok");

    const uint8_t codec_cfg[] = {
        0x00,0x00,0x00,0x01,0x67,0x42,0xe0,0x1e,0x89,0x8b,0x60,
        0x00,0x00,0x00,0x01,0x68,0xce,0x38,0x80
    };
    CHECK(rec_writer_publish_codec_config(writer, VFR_FMT_H264,
                                          codec_cfg, sizeof(codec_cfg)) == 0,
          "publish codec config ok");

    uint32_t pre_gen = 0;
    CHECK(rec_buf_extract_from_keyframe(buf, 10ULL * 1000000000ULL, &pre_gen) == REC_OK,
          "extract pre-roll ok");

    uint64_t one = 1;
    CHECK(write(rec_writer_get_eventfd(writer), &one, sizeof(one)) == (ssize_t)sizeof(one),
          "wake writer for pre_queue drain");

    wait_briefly();

    const uint8_t idr[] = { 0x00,0x00,0x00,0x01,0x65,0x88,0x84,0x21,0xa0 };
    CHECK(rec_writer_enqueue_live(writer, idr, sizeof(idr),
                                  20ULL * 1000000000ULL, true, true) == 0,
          "enqueue live IDR ok");

    wait_briefly();
    rec_writer_destroy(&writer);
    CHECK(writer == NULL, "writer destroyed");

    char first_path[512] = {0};
    int file_count = count_ts_files(tmpdir, first_path);
    CHECK(file_count >= 1, "at least one TS file created");

    int fd = open(first_path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0, "open first TS file ok");

    uint8_t data[564];
    ssize_t n = read(fd, data, sizeof(data));
    close(fd);

    CHECK(n >= 564, "TS file has at least 3 packets");
    CHECK(data[0] == 0x47, "packet 1 sync byte");
    CHECK(data[188] == 0x47, "packet 2 sync byte");
    CHECK(data[376] == 0x47, "packet 3 sync byte");
    CHECK((((data[1] & 0x1f) << 8) | data[2]) == 0x0000, "packet 1 PID = PAT");
    CHECK((((data[189] & 0x1f) << 8) | data[190]) == 0x0100, "packet 2 PID = PMT");

    rec_buf_destroy(&buf);
}

static void test_segment_boundary_rotates_files(void)
{
    TEST_BEGIN("writer rotates files at keyframe segment boundary");

    char tmpdir[] = "/tmp/rec_writer_r3_rot_XXXXXX";
    CHECK(mkdtemp(tmpdir) != NULL, "mkdtemp ok");

    rec_buf_t *buf = rec_buf_create(4 * 1024 * 1024);
    CHECK(buf != NULL, "rec_buf_create ok");

    rec_writer_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", tmpdir);
    snprintf(cfg.stream_name, sizeof(cfg.stream_name), "%s", "cam1");
    cfg.mode = REC_MODE_EVENT;
    cfg.segment_duration_sec = REC_SEGMENT_DURATION_SEC;
    cfg.segment_size_max = REC_SEGMENT_SIZE_MAX;
    cfg.flush_interval_sec = 1;

    rec_writer_t *writer = rec_writer_create(&cfg, buf);
    CHECK(writer != NULL, "rec_writer_create ok");

    const uint8_t codec_cfg[] = {
        0x00,0x00,0x00,0x01,0x67,0x4d,0x40,0x1f,0xec,0xa0,0x50,
        0x00,0x00,0x00,0x01,0x68,0xee,0x3c,0x80
    };
    CHECK(rec_writer_publish_codec_config(writer, VFR_FMT_H264,
                                          codec_cfg, sizeof(codec_cfg)) == 0,
          "publish codec config ok");

    const uint8_t idr1[] = { 0x00,0x00,0x00,0x01,0x65,0x11,0x22,0x33 };
    const uint8_t p1[]   = { 0x00,0x00,0x00,0x01,0x41,0x44,0x55,0x66 };
    const uint8_t idr2[] = { 0x00,0x00,0x00,0x01,0x65,0xaa,0xbb,0xcc };

    CHECK(rec_writer_enqueue_live(writer, idr1, sizeof(idr1), 1ULL * 1000000000ULL, true, true) == 0,
          "enqueue first boundary IDR");
    CHECK(rec_writer_enqueue_live(writer, p1, sizeof(p1), 11ULL * 1000000000ULL / 10ULL, false, false) == 0,
          "enqueue P-frame");
    wait_briefly();
    CHECK(rec_writer_enqueue_live(writer, idr2, sizeof(idr2), 2ULL * 1000000000ULL, true, true) == 0,
          "enqueue second boundary IDR");

    wait_briefly();
    rec_writer_destroy(&writer);

    int file_count = count_ts_files(tmpdir, NULL);
    CHECK(file_count >= 2, "segment boundary created at least two TS files");

    rec_buf_destroy(&buf);
}

static void test_codec_config_snapshot_latest_blob(void)
{
    TEST_BEGIN("codec config snapshot returns a stable latest blob");

    rec_codec_config_t cfg;
    rec_codec_blob_t snapshot;
    uint32_t version = 0;

    rec_codec_config_init(&cfg);

    const uint8_t blob_a[] = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t blob_b[] = { 0xaa, 0xbb, 0xcc, 0xdd, 0xee };

    CHECK(rec_codec_config_publish(&cfg, VFR_FMT_H264, blob_a, sizeof(blob_a)) == 0,
          "publish blob A ok");
    CHECK(rec_codec_config_snapshot(&cfg, &snapshot, &version), "snapshot after blob A");
    CHECK(snapshot.size == sizeof(blob_a), "snapshot A size matches");
    CHECK(memcmp(snapshot.data, blob_a, sizeof(blob_a)) == 0, "snapshot A bytes match");

    CHECK(rec_codec_config_publish(&cfg, VFR_FMT_H265, blob_b, sizeof(blob_b)) == 0,
          "publish blob B ok");
    CHECK(rec_codec_config_snapshot(&cfg, &snapshot, &version), "snapshot after blob B");
    CHECK(snapshot.format == VFR_FMT_H265, "snapshot B format matches");
    CHECK(snapshot.size == sizeof(blob_b), "snapshot B size matches");
    CHECK(memcmp(snapshot.data, blob_b, sizeof(blob_b)) == 0, "snapshot B bytes match");
}

int main(void)
{
    printf("=== Phase R3: rec_writer unit tests ===\n");

    test_preroll_drain_and_ts_headers();
    test_segment_boundary_rotates_files();
    test_codec_config_snapshot_latest_blob();

    printf("\n=== Results: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
