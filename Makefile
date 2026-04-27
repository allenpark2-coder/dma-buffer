# VFR — Video Frame Reader
# Phase 1 + Phase 2 + Phase 3 + Phase 4 + Phase 5 + Recorder Phase R1 Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_GNU_SOURCE
# -I.       : allows #include "platform/platform_adapter.h", "core/vfr_pool.h", "ipc/..."
# -Iinclude : allows #include "vfr_defs.h", #include "vfr.h"
# -Icore    : allows #include "vfr_pool.h" (from vfr_ctx.c)
INCLUDES = -I. -Iinclude -Icore -Irec

ifdef DEBUG
  CFLAGS  += -g -O0 -DVFR_LOG_LEVEL=3
  $(info DEBUG build: VFR_LOG_LEVEL=DEBUG)
else
  CFLAGS  += -O2
endif

# ─── source files ──────────────────────────────────────────────────────────────
SRCS_CORE = \
    core/vfr_ctx.c \
    core/vfr_pool.c \
    sdk/vfr_map.c

SRCS_SYNC = \
    core/vfr_sync.c

SRCS_MOCK = \
    platform/mock/mock_adapter.c

SRCS_IPC_CLIENT = \
    ipc/vfr_client.c

SRCS_IPC_SERVER = \
    ipc/vfr_server.c \
    ipc/vfr_watchdog.c

SRCS_TEST_SINGLE = \
    test/test_single_proc.c

SRCS_TEST_PRODUCER = \
    test/test_ipc_producer.c

SRCS_TEST_CONSUMER = \
    test/test_ipc_consumer.c

SRCS_TEST_MULTICAST = \
    test/test_multicast.c

SRCS_TEST_CRASH = \
    test/test_crash_recovery.c

# ── Phase 5 ────────────────────────────────────────────────────────────────────
SRCS_METRICS = \
    sdk/vfr_metrics.c

SRCS_REGISTRY = \
    ipc/vfr_registry.c

SRCS_TEST_METRICS = \
    test/test_metrics.c

# ── Recorder Phase R1 ─────────────────────────────────────────────────────────
SRCS_REC_BUF = \
    rec/rec_buf.c

SRCS_REC_STATE = \
    rec/rec_buf.c \
    rec/rec_schedule.c \
    rec/rec_debounce.c \
    rec/rec_trigger.c \
    rec/rec_state.c

# ─── targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean valgrind asan check check2 check3 check4 check5 check5-serve asan5 \
        test_rec_buf check_r1 asan_r1 \
        test_rec_state check_r2 asan_r2

all: test_single_proc test_ipc_producer test_ipc_consumer test_multicast test_crash_recovery test_metrics test_rec_buf test_rec_state

# ── Phase 1 ────────────────────────────────────────────────────────────────────
test_single_proc: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_TEST_SINGLE)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 2 ────────────────────────────────────────────────────────────────────
# Producer：server 用到 vfr_sync.c（eventfd helper）
test_ipc_producer: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_PRODUCER)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# Consumer：client + pool + sync（vfr_ctx.c 引用 vfr_sync）
test_ipc_consumer: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_TEST_CONSUMER)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 3 ────────────────────────────────────────────────────────────────────
# test_multicast：單一 binary，--role producer|rtsp|recorder|motion
test_multicast: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_MULTICAST)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 4 ────────────────────────────────────────────────────────────────────
# test_crash_recovery：Consumer crash / watchdog recovery 驗證
test_crash_recovery: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_CRASH)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 1 驗證 ──────────────────────────────────────────────────────────────
valgrind: test_single_proc
	valgrind \
	    --leak-check=full \
	    --show-leak-kinds=all \
	    --track-origins=yes \
	    --error-exitcode=1 \
	    ./test_single_proc --no-dump

asan:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_single_proc_asan \
	    $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_TEST_SINGLE)
	./test_single_proc_asan --no-dump

check-no-memcpy: test_single_proc
	@echo "=== Checking for memcpy calls (should be 0) ==="
	@objdump -d test_single_proc | grep -c 'call.*memcpy' && \
	    echo "WARNING: memcpy found!" || echo "PASS: no memcpy"

check: test_single_proc
	@echo "=== Running Phase 1 Checklist ==="
	./test_single_proc --no-dump
	@echo "=== valgrind ==="
	$(MAKE) valgrind
	@echo "=== memcpy check ==="
	$(MAKE) check-no-memcpy

# ── Phase 2 驗證 ──────────────────────────────────────────────────────────────
# check2：producer 背景執行，consumer 連線後驗證，最後 kill producer
check2: test_ipc_producer test_ipc_consumer
	@echo "=== Running Phase 2 IPC Test ==="
	@./test_ipc_producer --frames 200 &
	@PROD_PID=$$!; \
	sleep 0.5; \
	VFR_MODE=client ./test_ipc_consumer --frames 20; \
	RESULT=$$?; \
	kill $$PROD_PID 2>/dev/null; \
	wait $$PROD_PID 2>/dev/null; \
	echo "=== Phase 2 Overall: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="

# ── Phase 2 valgrind（consumer 側）────────────────────────────────────────────
valgrind2: test_ipc_producer test_ipc_consumer
	@echo "=== Phase 2 valgrind (consumer side) ==="
	./test_ipc_producer --frames 200 &
	PROD_PID=$$!; \
	sleep 0.5; \
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
	    --error-exitcode=1 \
	    env VFR_MODE=client ./test_ipc_consumer --frames 20 --no-reconnect; \
	RESULT=$$?; \
	kill $$PROD_PID; wait $$PROD_PID; \
	exit $$RESULT

# ── Phase 3 驗證（3 consumer 同時跑）────────────────────────────────────────
# check3：啟動 producer，再依序啟動 3 個 consumer（RTSP/Recorder/Motion）
# 全部完成後驗證整體結果
check3: test_multicast
	@echo "=== Running Phase 3 Multicast Test ==="
	@./test_multicast --role producer --frames 200 &
	@PROD_PID=$$!; \
	sleep 0.3; \
	VFR_MODE=client VFR_POLICY=drop_oldest    ./test_multicast --role rtsp     --frames 40 & RTSP_PID=$$!; \
	VFR_MODE=client VFR_POLICY=block_producer ./test_multicast --role recorder --frames 15 & REC_PID=$$!; \
	VFR_MODE=client VFR_POLICY=skip_self      ./test_multicast --role motion   --frames 25 & MOT_PID=$$!; \
	wait $$RTSP_PID;   RTSP_RET=$$?; \
	wait $$REC_PID;    REC_RET=$$?; \
	wait $$MOT_PID;    MOT_RET=$$?; \
	kill $$PROD_PID 2>/dev/null; wait $$PROD_PID 2>/dev/null; \
	echo "=== RTSP:     $$([ $$RTSP_RET -eq 0 ] && echo PASS || echo FAIL) ==="; \
	echo "=== RECORDER: $$([ $$REC_RET -eq 0 ]  && echo PASS || echo FAIL) ==="; \
	echo "=== MOTION:   $$([ $$MOT_RET -eq 0 ]  && echo PASS || echo FAIL) ==="; \
	OVERALL=$$(( $$RTSP_RET | $$REC_RET | $$MOT_RET )); \
	echo "=== Phase 3 Overall: $$([ $$OVERALL -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$OVERALL

# ── ASan 驗收（Phase 3 checklist 3.4）──────────────────────────────────────
asan3:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_multicast_asan \
	    $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_MULTICAST)
	@echo "=== ASan build OK. Run: make check3 with asan binary for full validation ==="

# ── Phase 4 驗證（fork-based self-test）────────────────────────────────────
# check4：單一 binary 自測，啟動 server 後 fork crasher child，
# 驗證 watchdog 觸發後 slot 回收、producer 繼續出幀
check4: test_crash_recovery
	@echo "=== Running Phase 4 Crash Recovery Test ==="
	./test_crash_recovery --self-test
	@RESULT=$$?; \
	echo "=== Phase 4 Overall: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$RESULT

# Phase 4 兩 binary 模式驗證（外部 kill -9）
check4-ext: test_crash_recovery
	@echo "=== Running Phase 4 External Kill Test ==="
	@./test_crash_recovery --role producer --frames 120 &
	@PROD_PID=$$!; \
	sleep 0.3; \
	VFR_MODE=client ./test_crash_recovery --role crasher --hold-ms 2000 & \
	CRASHER_PID=$$!; \
	sleep 0.4; \
	echo "[check4] kill -9 crasher (pid=$$CRASHER_PID)"; \
	kill -9 $$CRASHER_PID 2>/dev/null; \
	wait $$PROD_PID; RESULT=$$?; \
	echo "=== Phase 4 External: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$RESULT

# ── ASan 驗收（Phase 4 checklist 3.4）──────────────────────────────────────
asan4:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_crash_recovery_asan \
	    $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_CRASH)
	./test_crash_recovery_asan --self-test

# ── Phase 5 — Metrics + Bridges（純 C 部分無外部依賴）──────────────────────
# test_metrics：metrics unit tests + serve 模式 + registry daemon
test_metrics: $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
              $(SRCS_IPC_SERVER) $(SRCS_METRICS) $(SRCS_REGISTRY) $(SRCS_TEST_METRICS)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 5 — OpenCV Bridge（需 WITH_OPENCV=1）─────────────────────────────
ifdef WITH_OPENCV
  CXX           ?= g++
  OPENCV_CFLAGS := $(shell pkg-config --cflags opencv4 2>/dev/null || \
                           pkg-config --cflags opencv  2>/dev/null)
  OPENCV_LIBS   := $(shell pkg-config --libs   opencv4 2>/dev/null || \
                           pkg-config --libs   opencv  2>/dev/null)

bridge_opencv.o: sdk/bridge_opencv.cpp sdk/bridge_opencv.h include/vfr.h
	$(CXX) -std=c++11 $(CFLAGS) $(INCLUDES) $(OPENCV_CFLAGS) -c -o $@ $<

test_opencv_bridge: sdk/vfr_map.c platform/mock/mock_adapter.c bridge_opencv.o
	$(CXX) -std=c++11 $(CFLAGS) $(INCLUDES) $(OPENCV_CFLAGS) \
	    -o $@ $^ $(OPENCV_LIBS)
	@echo "[Phase 5] OpenCV bridge built OK"
endif

# ── Phase 5 — GStreamer Bridge（需 WITH_GSTREAMER=1）─────────────────────────
ifdef WITH_GSTREAMER
  GST_CFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 \
                         gstreamer-allocators-1.0 2>/dev/null)
  GST_LIBS   := $(shell pkg-config --libs   gstreamer-1.0 gstreamer-app-1.0 \
                         gstreamer-allocators-1.0 2>/dev/null)

bridge_gstreamer.o: sdk/bridge_gstreamer.c sdk/bridge_gstreamer.h include/vfr.h
	$(CC) $(CFLAGS) $(INCLUDES) $(GST_CFLAGS) -DHAVE_GSTREAMER -c -o $@ $<

test_gstreamer_bridge: sdk/vfr_map.c platform/mock/mock_adapter.c bridge_gstreamer.o
	$(CC) $(CFLAGS) $(INCLUDES) $(GST_CFLAGS) -DHAVE_GSTREAMER \
	    -o $@ $^ $(GST_LIBS)
	@echo "[Phase 5] GStreamer bridge built OK"
endif

# ── Phase 5 驗收（單元測試）──────────────────────────────────────────────
check5: test_metrics
	@echo "=== Running Phase 5 Metrics Unit Tests ==="
	./test_metrics
	@RESULT=$$?; \
	echo "=== Phase 5 Overall: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$RESULT

# ── Phase 5 HTTP scrape 驗收（互動式）───────────────────────────────────
# 啟動 metrics HTTP server，用 curl 手動驗證：
#   curl http://127.0.0.1:9100/metrics
check5-serve: test_metrics
	@echo "=== Phase 5 Metrics HTTP Server ==="
	@echo "=== In another terminal: curl http://127.0.0.1:9100/metrics ==="
	./test_metrics --serve

# ── ASan 驗收（Phase 5）─────────────────────────────────────────────────
asan5:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_metrics_asan \
	    $(SRCS_CORE) $(SRCS_SYNC) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) \
	    $(SRCS_IPC_SERVER) $(SRCS_METRICS) $(SRCS_REGISTRY) $(SRCS_TEST_METRICS)
	./test_metrics_asan

# ── Recorder Phase R1 targets ─────────────────────────────────────────────────
test_rec_buf: $(SRCS_REC_BUF) test/test_rec_buf.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

test_rec_state: $(SRCS_REC_STATE) test/test_rec_state.c
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase R1 驗收 ─────────────────────────────────────────────────────────────
check_r1: test_rec_buf
	@echo "=== Running Phase R1 rec_buf Tests ==="
	./test_rec_buf
	@RESULT=$$?; \
	echo "=== Phase R1 Overall: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$RESULT

# ── Phase R1 ASan 驗收 ────────────────────────────────────────────────────────
asan_r1:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_rec_buf_asan \
	    $(SRCS_REC_BUF) test/test_rec_buf.c
	./test_rec_buf_asan

check_r2: test_rec_state
	@echo "=== Running Phase R2 rec_state Tests ==="
	./test_rec_state
	@RESULT=$$?; \
	echo "=== Phase R2 Overall: $$([ $$RESULT -eq 0 ] && echo PASS || echo FAIL) ==="; \
	exit $$RESULT

asan_r2:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_rec_state_asan \
	    $(SRCS_REC_STATE) test/test_rec_state.c
	./test_rec_state_asan

clean:
	rm -f test_single_proc test_single_proc_asan
	rm -f test_ipc_producer test_ipc_consumer
	rm -f test_multicast test_multicast_asan
	rm -f test_crash_recovery test_crash_recovery_asan
	rm -f test_metrics test_metrics_asan
	rm -f bridge_opencv.o test_opencv_bridge
	rm -f bridge_gstreamer.o test_gstreamer_bridge
	rm -f test_rec_buf test_rec_buf_asan
	rm -f test_rec_state test_rec_state_asan
	rm -f /tmp/frame_*.yuv
