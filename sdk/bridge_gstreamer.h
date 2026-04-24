/* sdk/bridge_gstreamer.h — GStreamer Bridge（C 介面）
 *
 * 將 vfr_frame_t 餵入 GStreamer appsrc element，達成零複製（dmabuf）。
 *
 * 使用條件：
 *   - 需連結 GStreamer：$(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-app-1.0)
 *   - Makefile 目標：make WITH_GSTREAMER=1 test_gstreamer_bridge
 *   - 需要 GStreamer >= 1.16（GstDmaBufAllocator 穩定版本）
 *
 * 零複製策略：
 *   若 frame->flags 不含 VFR_FLAG_NO_CPU_SYNC（即真實 DMA-BUF），
 *   使用 gst_dmabuf_allocator_alloc() wrap dma_fd，避免 memcpy。
 *   否則（mock memfd 降級）退回 gst_memory_new_wrapped()，仍透過 mmap 避免複製。
 *
 * 使用範例：
 *   GstElement *pipeline = gst_parse_launch("appsrc name=vfrsrc ! videoconvert ! autovideosink", NULL);
 *   GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "vfrsrc");
 *   vfr_gst_configure_appsrc(appsrc, &frame_template);
 *   gst_element_set_state(pipeline, GST_STATE_PLAYING);
 *
 *   vfr_frame_t frame;
 *   while (running) {
 *       vfr_get_frame(ctx, &frame, 0);
 *       vfr_gst_appsrc_push(appsrc, &frame);  // 零複製推入 pipeline
 *       // 不要 vfr_put_frame()！GStreamer GstBuffer destroy 時會自動 put_frame。
 *   }
 */
#ifndef VFR_BRIDGE_GSTREAMER_H
#define VFR_BRIDGE_GSTREAMER_H

#include "vfr.h"

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

/* ─── API ───────────────────────────────────────────────────────────────── */

/*
 * vfr_gst_configure_appsrc()：
 *   根據 frame_template 設定 appsrc 的 caps（format、width、height、framerate）。
 *   frame_template — 一個已填妥 width/height/format/stride 的 frame（通常是第一幀）。
 *   應在 pipeline 進入 PLAYING 狀態前呼叫。
 *
 * 回傳：0 = 成功；-1 = 失敗
 */
int vfr_gst_configure_appsrc(GstElement *appsrc, const vfr_frame_t *frame_template);

/*
 * vfr_gst_appsrc_push()：
 *   將 frame 包裝為 GstBuffer 並 push 到 appsrc。
 *
 *   零複製路徑（has_native_dma_buf = true）：
 *     wrap dma_fd 為 GstDmaBufMemory；GstBuffer release 時 close fd + vfr_put_frame()。
 *
 *   降級路徑（mock memfd）：
 *     mmap frame，以 gst_memory_new_wrapped() 包裝；GstBuffer release 時 vfr_unmap() + vfr_put_frame()。
 *
 *   注意：呼叫成功後，呼叫者不得再對此 frame 呼叫 vfr_put_frame()，
 *         由 GstBuffer 的 notify 回呼負責歸還 slot。
 *
 * 回傳：GST_FLOW_OK = 成功；其他 = 失敗
 */
GstFlowReturn vfr_gst_appsrc_push(GstElement *appsrc, vfr_frame_t *frame);

#endif /* HAVE_GSTREAMER */

#endif /* VFR_BRIDGE_GSTREAMER_H */
