# VFR — Video Frame Reader
# Phase 1 + Phase 2 Makefile

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

SRCS_MOCK = \
    platform/mock/mock_adapter.c

SRCS_IPC_CLIENT = \
    ipc/vfr_client.c

SRCS_IPC_SERVER = \
    ipc/vfr_server.c

SRCS_TEST_SINGLE = \
    test/test_single_proc.c

SRCS_TEST_PRODUCER = \
    test/test_ipc_producer.c

SRCS_TEST_CONSUMER = \
    test/test_ipc_consumer.c

# ─── targets ───────────────────────────────────────────────────────────────────
.PHONY: all clean valgrind asan check check2

all: test_single_proc test_ipc_producer test_ipc_consumer

# ── Phase 1 ────────────────────────────────────────────────────────────────────
test_single_proc: $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_TEST_SINGLE)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# ── Phase 2 ────────────────────────────────────────────────────────────────────
# Producer：包含 server + pool + mock adapter（server 端需要 platform adapter）
test_ipc_producer: $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_IPC_SERVER) $(SRCS_TEST_PRODUCER)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# Consumer：包含 client + pool（vfr_ctx.c 引用 pool 型別）+ mock adapter（standalone fallback）
test_ipc_consumer: $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_IPC_CLIENT) $(SRCS_TEST_CONSUMER)
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

clean:
	rm -f test_single_proc test_single_proc_asan
	rm -f test_ipc_producer test_ipc_consumer
	rm -f /tmp/frame_*.yuv
