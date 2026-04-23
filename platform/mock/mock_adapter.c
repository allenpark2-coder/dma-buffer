/* platform/mock/mock_adapter.c
 *
 * Mock platform adapter — 在無 DSP 硬體的開發主機上模擬 frame 輸出。
 *
 * 實作方式：
 *   - 使用 memfd_create() 建立匿名記憶體，充當 "dma_buf"
 *   - 每幀填入 NV12 漸層 pattern（luma = seq_num & 0xFF，chroma = 128）
 *   - has_native_dma_buf = false → vfr_map() 走一般 mmap，不做 DMA_BUF_IOCTL_SYNC
 *
 * 若指定 VFR_MOCK_BINARY=<path>，則從該路徑讀取預錄 binary 逐幀循環播放。
 */

#include "../../include/vfr_defs.h"
#include "../../include/vfr.h"
#include "../platform_adapter.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>

/* ─── 預設解析度（可由 vfr_open cfg 覆蓋）────────────────────────────────── */
#define MOCK_DEFAULT_W      1920u
#define MOCK_DEFAULT_H      1080u
#define MOCK_FORMAT         0x3231564Eu   /* V4L2_PIX_FMT_NV12 = fourcc('N','V','1','2') */

/* ─── memfd_create wrapper（glibc < 2.27 fallback）─────────────────────── */
static int vfr_memfd_create(const char *name)
{
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, 0);
#else
    /* fallback：用 tmpfile + fileno（功能等價，無 exec-seal） */
    (void)name;
    char tmpl[] = "/tmp/vfr_mock_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd >= 0) unlink(tmpl);   /* 取 fd 後即刪除路徑，只保留 anonymous 語意 */
    return fd;
#endif
}

/* ─── Mock context ──────────────────────────────────────────────────────── */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t buf_size;
    uint64_t seq_num;

    /* 預錄 binary 模式 */
    int      binary_fd;    /* -1 = 不使用 */
    size_t   binary_size;
    uint8_t *binary_data;
    uint32_t binary_frame_size;
    uint32_t binary_frame_count;
    uint32_t binary_cur_frame;
} mock_ctx_t;

/* ─── NV12 pattern 產生（luma = (seq & 0xFF)，chroma = 128）───────────────── */
static void fill_nv12_pattern(uint8_t *buf, uint32_t width, uint32_t height,
                               uint32_t stride, uint64_t seq)
{
    uint8_t luma_val = (uint8_t)(seq & 0xFFu);

    /* Y plane */
    for (uint32_t row = 0; row < height; row++) {
        uint8_t *y = buf + row * stride;
        for (uint32_t col = 0; col < width; col++) {
            /* 漸層：左→右亮度 0→255，疊加幀序號 offset，形成動態 pattern */
            y[col] = (uint8_t)((luma_val + col * 255u / width) & 0xFFu);
        }
        /* stride padding */
        if (stride > width)
            memset(y + width, 0x10, stride - width);
    }

    /* UV plane（chroma = 128,128 → 灰色） */
    uint8_t *uv = buf + stride * height;
    uint32_t uv_rows = height / 2;
    for (uint32_t row = 0; row < uv_rows; row++) {
        uint8_t *uvp = uv + row * stride;
        for (uint32_t col = 0; col < width; col += 2) {
            uvp[col]     = 128;  /* Cb */
            uvp[col + 1] = 128;  /* Cr */
        }
        if (stride > width)
            memset(uvp + width, 128, stride - width);
    }
}

/* ─── init ──────────────────────────────────────────────────────────────── */
static int mock_init(void **ctx, const vfr_shm_header_t *cfg)
{
    mock_ctx_t *m = calloc(1, sizeof(*m));
    if (!m) {
        VFR_LOGE("calloc failed: %s", strerror(errno));
        return -1;
    }

    m->width  = (cfg && cfg->width)  ? cfg->width  : MOCK_DEFAULT_W;
    m->height = (cfg && cfg->height) ? cfg->height : MOCK_DEFAULT_H;
    m->stride = m->width;   /* mock：無 padding，stride == width */
    m->buf_size = m->stride * m->height * 3u / 2u;  /* NV12 */
    m->binary_fd = -1;

    /* 嘗試開啟預錄 binary（環境變數 VFR_MOCK_BINARY） */
    const char *bin_path = getenv("VFR_MOCK_BINARY");
    if (bin_path) {
        int bfd = open(bin_path, O_RDONLY);
        if (bfd < 0) {
            VFR_LOGW("VFR_MOCK_BINARY=%s open failed: %s (ignoring)", bin_path, strerror(errno));
        } else {
            off_t sz = lseek(bfd, 0, SEEK_END);
            lseek(bfd, 0, SEEK_SET);
            if (sz > 0 && (uint32_t)sz >= m->buf_size) {
                m->binary_fd          = bfd;
                m->binary_size        = (size_t)sz;
                m->binary_frame_size  = m->buf_size;
                m->binary_frame_count = (uint32_t)(sz / m->buf_size);
                m->binary_cur_frame   = 0;
                VFR_LOGI("mock_adapter: binary mode %s (%u frames)", bin_path, m->binary_frame_count);
            } else {
                VFR_LOGW("binary file too small (%lld < %u), ignoring", (long long)sz, m->buf_size);
                close(bfd);
            }
        }
    }

    *ctx = m;
    VFR_LOGI("mock_adapter: init %ux%u stride=%u buf_size=%u mode=%s",
             m->width, m->height, m->stride, m->buf_size,
             m->binary_fd >= 0 ? "binary" : "pattern");
    return 0;
}

/* ─── get_frame ─────────────────────────────────────────────────────────── */
static int mock_get_frame(void *ctx, vfr_frame_t *out)
{
    mock_ctx_t *m = ctx;

    /* 建立 memfd 充當 "dma_buf" */
    int fd = vfr_memfd_create("vfr_mock_frame");
    if (fd < 0) {
        VFR_LOGE("memfd_create failed: %s", strerror(errno));
        return -1;
    }

    if (ftruncate(fd, m->buf_size) < 0) {
        VFR_LOGE("ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    /* mmap 填入 pattern 或 binary 資料 */
    uint8_t *buf = mmap(NULL, m->buf_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (buf == MAP_FAILED) {
        VFR_LOGE("mmap failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    if (m->binary_fd >= 0) {
        /* 從預錄 binary 讀取對應幀 */
        off_t offset = (off_t)m->binary_cur_frame * m->binary_frame_size;
        ssize_t n = pread(m->binary_fd, buf, m->binary_frame_size, offset);
        if (n < (ssize_t)m->binary_frame_size) {
            VFR_LOGW("pread only %zd/%u bytes", n, m->binary_frame_size);
        }
        m->binary_cur_frame = (m->binary_cur_frame + 1) % m->binary_frame_count;
    } else {
        fill_nv12_pattern(buf, m->width, m->height, m->stride, m->seq_num);
    }

    munmap(buf, m->buf_size);

    /* 取 timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    /* 填 frame metadata */
    out->dma_fd          = fd;
    out->width           = m->width;
    out->height          = m->height;
    out->format          = MOCK_FORMAT;
    out->stride          = m->stride;
    out->buf_size        = m->buf_size;
    out->flags           = VFR_FLAG_NO_CPU_SYNC;   /* memfd 不需要 DMA sync */
    out->plane_offset[0] = 0;
    out->plane_offset[1] = m->stride * m->height;
    out->plane_offset[2] = 0;
    out->timestamp_ns    = now_ns;
    out->seq_num         = ++m->seq_num;
    out->priv            = NULL;

    VFR_LOGD("get_frame: seq=%llu dma_fd=%d %ux%u",
             (unsigned long long)out->seq_num, out->dma_fd, out->width, out->height);
    return 0;
}

/* ─── put_frame ─────────────────────────────────────────────────────────── */
static void mock_put_frame(void *ctx, vfr_frame_t *frame)
{
    (void)ctx;
    /* mock：producer 端直接 close，no-op if -1 */
    if (frame && frame->dma_fd >= 0) {
        close(frame->dma_fd);
        frame->dma_fd = -1;
    }
}

/* ─── destroy ───────────────────────────────────────────────────────────── */
static void mock_destroy(void **ctx)
{
    if (!ctx || !*ctx) return;
    mock_ctx_t *m = *ctx;
    if (m->binary_fd >= 0) {
        close(m->binary_fd);
        m->binary_fd = -1;
    }
    free(m);
    *ctx = NULL;
    VFR_LOGI("mock_adapter: destroyed");
}

/* ─── ops table ─────────────────────────────────────────────────────────── */
static const vfr_platform_ops_t s_mock_ops = {
    .name               = "mock",
    .init               = mock_init,
    .get_frame          = mock_get_frame,
    .put_frame          = mock_put_frame,
    .destroy            = mock_destroy,
    .has_native_dma_buf = false,   /* memfd 不是真正的 DMA-BUF */
};

const vfr_platform_ops_t *vfr_get_mock_ops(void)
{
    return &s_mock_ops;
}
