# VFR — Video Frame Reader
# Phase 1 + Phase 2 + Phase 3 + Phase 4 Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_GNU_SOURCE
# -I.       : allows #include "platform/platform_adapter.h", "core/vfr_pool.h", "ipc/..."
# -Iinclude : allows #include "vfr_defs.h", #include "vfr.h"
# -Icore    : allows #include "vfr_pool.h" (from vfr_ctx.c)
INCLUDES = -I. -Iinclude -Icore

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

# ─── targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean valgrind asan check check2 check3 check4

all: test_single_proc test_ipc_producer test_ipc_consumer test_multicast test_crash_recovery

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

clean:
	rm -f test_single_proc test_single_proc_asan
	rm -f test_ipc_producer test_ipc_consumer
	rm -f test_multicast test_multicast_asan
	rm -f test_crash_recovery test_crash_recovery_asan
	rm -f /tmp/frame_*.yuv
