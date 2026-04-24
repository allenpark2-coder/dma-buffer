/* sdk/bridge_gstreamer.c — GStreamer Bridge 實作
 *
 * 編譯：需加 -DHAVE_GSTREAMER $(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0)
 * 連結：$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-allocators-1.0)
 */

#ifdef HAVE_GSTREAMER

#include "sdk/bridge_gstreamer.h"
#include "vfr_defs.h"

#include <gst/allocators/gstdmabuf.h>
#include <string.h>
#include <unistd.h>

/* V4L2 fourcc 定義（fallback）*/
#ifndef V4L2_PIX_FMT_NV12
#  define V4L2_PIX_FMT_NV12   0x3231564eu
#endif
#ifndef V4L2_PIX_FMT_RGB24
#  define V4L2_PIX_FMT_RGB24  0x33424752u
#endif
#ifndef V4L2_PIX_FMT_GREY
#  define V4L2_PIX_FMT_GREY   0x59455247u
#endif

/* ─── fourcc → GstVideoFormat ──────────────────────────────────────────── */
static const char *fourcc_to_gst_format(uint32_t fourcc)
{
    switch (fourcc) {
        case V4L2_PIX_FMT_NV12:  return "NV12";
        case V4L2_PIX_FMT_RGB24: return "RGB";
        case V4L2_PIX_FMT_GREY:  return "GRAY8";
        default:                  return "GRAY8";
    }
}

/* ─── vfr_gst_configure_appsrc ──────────────────────────────────────────── */
int vfr_gst_configure_appsrc(GstElement *appsrc, const vfr_frame_t *t)
{
    if (!appsrc || !t) return -1;

    const char *fmt = fourcc_to_gst_format(t->format);

    GstCaps *caps = gst_caps_new_simple(
        "video/x-raw",
        "format",    G_TYPE_STRING,  fmt,
        "width",     G_TYPE_INT,     (gint)t->width,
        "height",    G_TYPE_INT,     (gint)t->height,
        "framerate", GST_TYPE_FRACTION, 30, 1,   /* 預設 30fps，可外部覆蓋 */
        NULL);

    g_object_set(G_OBJECT(appsrc),
                 "caps",       caps,
                 "format",     GST_FORMAT_TIME,
                 "is-live",    TRUE,
                 "do-timestamp", TRUE,
                 NULL);
    gst_caps_unref(caps);

    VFR_LOGI("appsrc configured: %ux%u format=%s", t->width, t->height, fmt);
    return 0;
}

/* ─── GstBuffer notify callback（歸還 frame slot）───────────────────────── */
typedef struct {
    vfr_frame_t frame;   /* 複製 frame metadata（不複製像素）*/
    void       *mmap_ptr; /* 若有 mmap（降級路徑），此指標用於 vfr_unmap */
} vfr_gst_user_data_t;

static void vfr_gst_buffer_finalize(gpointer user_data)
{
    vfr_gst_user_data_t *ud = (vfr_gst_user_data_t *)user_data;

    if (ud->mmap_ptr) {
        /* 降級路徑：解除 mmap */
        vfr_unmap(&ud->frame, ud->mmap_ptr);
        ud->mmap_ptr = NULL;
    }

    /* 歸還 slot（關閉 dma_fd，遞減 refcount）*/
    vfr_put_frame(&ud->frame);

    VFR_LOGD("GstBuffer released: fd=%d seq=%llu",
             ud->frame.dma_fd, (unsigned long long)ud->frame.seq_num);
    g_free(ud);
}

/* ─── vfr_gst_appsrc_push ──────────────────────────────────────────────── */
GstFlowReturn vfr_gst_appsrc_push(GstElement *appsrc, vfr_frame_t *frame)
{
    if (!appsrc || !frame || frame->dma_fd < 0) return GST_FLOW_ERROR;

    vfr_gst_user_data_t *ud = g_new0(vfr_gst_user_data_t, 1);
    ud->frame    = *frame;   /* 複製 metadata（dma_fd、buf_size 等）*/
    ud->mmap_ptr = NULL;

    GstBuffer *buf = gst_buffer_new();

    /* timestamp 來自 vfr_frame_t */
    GST_BUFFER_PTS(buf)      = (GstClockTime)frame->timestamp_ns;
    GST_BUFFER_OFFSET(buf)   = frame->seq_num;

    GstMemory *mem = NULL;

    if (!(frame->flags & VFR_FLAG_NO_CPU_SYNC)) {
        /* 零複製路徑：GstDmaBufMemory wrap dma_fd */
        GstAllocator *dma_alloc = gst_dmabuf_allocator_new();
        if (dma_alloc) {
            mem = gst_dmabuf_allocator_alloc(dma_alloc, frame->dma_fd, frame->buf_size);
            gst_object_unref(dma_alloc);
        }
    }

    if (!mem) {
        /*
         * 降級路徑（mock memfd 或 dmabuf allocator 不可用）：
         * mmap → gst_memory_new_wrapped（data 指標包裝，不複製）
         */
        void *ptr = vfr_map(frame);
        if (!ptr) {
            gst_buffer_unref(buf);
            g_free(ud);
            return GST_FLOW_ERROR;
        }
        ud->mmap_ptr = ptr;
        mem = gst_memory_new_wrapped(
            GST_MEMORY_FLAG_READONLY,
            ptr,
            frame->buf_size,
            0,
            frame->buf_size,
            ud,
            vfr_gst_buffer_finalize);
    } else {
        /*
         * dmabuf 路徑：buffer finalize 由 GstDmaBufMemory 自動 close fd，
         * 但 vfr_put_frame 仍需呼叫以遞減 refcount。
         * 附加 mini_object qdata 以便在 buffer 析構時呼叫 vfr_put_frame。
         */
        gst_mini_object_set_qdata(
            GST_MINI_OBJECT_CAST(buf),
            g_quark_from_static_string("vfr-user-data"),
            ud,
            vfr_gst_buffer_finalize);
    }

    gst_buffer_append_memory(buf, mem);

    GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buf);
    /* gst_app_src_push_buffer 取得 buffer 所有權（不需 unref）*/

    if (ret != GST_FLOW_OK) {
        VFR_LOGW("appsrc push failed: %s (seq=%llu)",
                 gst_flow_get_name(ret), (unsigned long long)frame->seq_num);
    } else {
        VFR_LOGD("appsrc push OK: seq=%llu ts=%lluns",
                 (unsigned long long)frame->seq_num,
                 (unsigned long long)frame->timestamp_ns);
    }
    return ret;
}

#endif /* HAVE_GSTREAMER */
