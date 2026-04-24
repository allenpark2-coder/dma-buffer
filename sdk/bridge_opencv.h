/* sdk/bridge_opencv.h — OpenCV Bridge（C++ 介面）
 *
 * 提供 vfr_to_mat() / vfr_release_mat()，將 vfr_frame_t 零複製轉換為 cv::Mat。
 *
 * 使用條件：
 *   - 編譯器需支援 C++11（g++ / clang++）
 *   - 需連結 OpenCV：$(shell pkg-config --cflags --libs opencv4)
 *   - Makefile 目標：make WITH_OPENCV=1 test_opencv_bridge
 *
 * 零複製語意：
 *   vfr_to_mat()     — mmap dma_fd，以外部指標構造 cv::Mat（不複製像素）
 *   vfr_release_mat() — vfr_unmap()，mat.data 之後為 dangling，不得再存取
 *
 * 支援格式（frame->format fourcc）：
 *   V4L2_PIX_FMT_NV12  (0x3231564e) → CV_8UC1，rows = height * 3 / 2
 *   V4L2_PIX_FMT_RGB24 (0x33424752) → CV_8UC3，rows = height
 *   V4L2_PIX_FMT_GREY  (0x59455247) → CV_8UC1，rows = height（luma only）
 *   其他格式           → CV_8UC1，rows = height（raw luma plane）
 *
 * 使用範例：
 *   vfr_frame_t frame;
 *   vfr_get_frame(ctx, &frame, 0);
 *   cv::Mat mat;
 *   if (vfr_to_mat(&frame, mat, 0) == 0) {
 *       cv::Mat bgr;
 *       cv::cvtColor(mat, bgr, cv::COLOR_YUV2BGR_NV12);   // NV12 轉 BGR
 *       // ... AI inference ...
 *       vfr_release_mat(&frame, mat);
 *   }
 *   vfr_put_frame(&frame);
 */
#ifndef VFR_BRIDGE_OPENCV_H
#define VFR_BRIDGE_OPENCV_H

/* 此 bridge 僅在 C++ 環境下有效 */
#ifdef __cplusplus

#include "vfr.h"
#include <opencv2/core.hpp>

/* ─── Flags ─────────────────────────────────────────────────────────────── */
#define VFR_TO_MAT_FLAG_NONE  0u
/* 預留給未來擴充，目前 vfr_to_mat 固定使用 zero-copy wrap */

/* ─── API ───────────────────────────────────────────────────────────────── */

/*
 * vfr_to_mat()：
 *   零複製：mmap frame->dma_fd，以外部 data 指標構造 cv::Mat。
 *   mat 的 step（stride）取自 frame->stride。
 *   flags — 目前未使用，傳 0。
 *
 * 呼叫前置條件：
 *   - frame 必須有效（vfr_get_frame() 成功後、vfr_put_frame() 之前）
 *   - 不要在 vfr_release_mat() 前呼叫 vfr_put_frame()
 *
 * 回傳：0 = 成功；-1 = 失敗（見 stderr 輸出）
 */
extern "C" int vfr_to_mat(const vfr_frame_t *frame, cv::Mat &mat, uint32_t flags);

/*
 * vfr_release_mat()：
 *   執行 vfr_unmap() 解除 mmap，之後 mat.data 無效。
 *   呼叫 mat.release() 清除指標，防止誤用。
 */
extern "C" void vfr_release_mat(const vfr_frame_t *frame, cv::Mat &mat);

#endif /* __cplusplus */

#endif /* VFR_BRIDGE_OPENCV_H */
