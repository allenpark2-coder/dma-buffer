/* sdk/bridge_opencv.cpp — OpenCV Bridge 實作
 *
 * 編譯：g++ -std=c++11 $(CFLAGS) $(INCLUDES) $(OPENCV_CFLAGS) -c -o bridge_opencv.o bridge_opencv.cpp
 * 連結：g++ ... $(OPENCV_LIBS)
 */

#include "sdk/bridge_opencv.h"
#include "vfr_defs.h"

#include <cerrno>
#include <cstdio>

/* V4L2 fourcc 定義（若無 linux/videodev2.h 則手動定義）*/
#ifndef V4L2_PIX_FMT_NV12
#  define V4L2_PIX_FMT_NV12   0x3231564eu  /* 'NV12' */
#endif
#ifndef V4L2_PIX_FMT_RGB24
#  define V4L2_PIX_FMT_RGB24  0x33424752u  /* 'RGB3' */
#endif
#ifndef V4L2_PIX_FMT_GREY
#  define V4L2_PIX_FMT_GREY   0x59455247u  /* 'GREY' */
#endif

/* ─── vfr_to_mat ─────────────────────────────────────────────────────────── */
extern "C" int vfr_to_mat(const vfr_frame_t *frame, cv::Mat &mat, uint32_t /*flags*/)
{
    if (!frame || frame->dma_fd < 0 || frame->buf_size == 0) {
        VFR_LOGE("vfr_to_mat: invalid frame");
        return -1;
    }

    /* mmap（交由 vfr_map 完成 DMA_BUF_IOCTL_SYNC）*/
    void *ptr = vfr_map(frame);
    if (!ptr) {
        VFR_LOGE("vfr_to_mat: vfr_map failed");
        return -1;
    }

    int mat_type = CV_8UC1;
    int mat_rows = static_cast<int>(frame->height);
    int mat_cols = static_cast<int>(frame->width);
    size_t step  = static_cast<size_t>(frame->stride);

    switch (frame->format) {
        case V4L2_PIX_FMT_NV12:
            /* NV12：Y plane（height rows）+ UV plane（height/2 rows）= height*3/2 rows */
            mat_rows = static_cast<int>(frame->height * 3 / 2);
            mat_type = CV_8UC1;
            /* step = luma stride（UV plane 與 Y plane stride 相同）*/
            break;

        case V4L2_PIX_FMT_RGB24:
            mat_type = CV_8UC3;
            step = static_cast<size_t>(frame->width) * 3;
            break;

        case V4L2_PIX_FMT_GREY:
            mat_type = CV_8UC1;
            /* mat_rows = height，step = stride，僅 luma */
            break;

        default:
            /* 未知格式：以 luma-only gray 呈現，讓呼叫者自行解析 */
            VFR_LOGW("vfr_to_mat: unknown format 0x%08x, treating as gray",
                     frame->format);
            mat_type = CV_8UC1;
            break;
    }

    /*
     * 零複製：cv::Mat 以外部 data 指標構造。
     * cv::Mat 析構時不會 free 外部記憶體（refcount 不接管外部 data）。
     * 必須在不再使用 mat 後呼叫 vfr_release_mat() 解除 mmap。
     */
    mat = cv::Mat(mat_rows, mat_cols, mat_type, ptr, step);

    VFR_LOGD("vfr_to_mat: fd=%d fmt=0x%08x rows=%d cols=%d step=%zu ptr=%p",
             frame->dma_fd, frame->format, mat_rows, mat_cols, step, ptr);
    return 0;
}

/* ─── vfr_release_mat ────────────────────────────────────────────────────── */
extern "C" void vfr_release_mat(const vfr_frame_t *frame, cv::Mat &mat)
{
    if (!frame) return;

    void *ptr = static_cast<void *>(mat.data);
    if (ptr) {
        vfr_unmap(frame, ptr);
        /*
         * 清除 mat 內部指標（不 free，因為是外部記憶體）。
         * mat = cv::Mat() 效果等同於 mat.release() + 重置 header。
         */
        mat = cv::Mat();
    }
}
