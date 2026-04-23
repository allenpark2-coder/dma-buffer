# VFR — Video Frame Reader
# Phase 1 Makefile

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_GNU_SOURCE
# -I.:       allows #include "platform/platform_adapter.h", #include "core/vfr_pool.h"
# -Iinclude: allows #include "vfr_defs.h", #include "vfr.h"
# -Icore:    allows #include "vfr_pool.h" (from vfr_ctx.c)
INCLUDES = -I. -Iinclude -Icore

ifdef DEBUG
  CFLAGS  += -g -O0 -DVFR_LOG_LEVEL=3
  $(info DEBUG build: VFR_LOG_LEVEL=DEBUG)
else
  CFLAGS  += -O2
endif

# ─── source files ────────────────────────────────────────────────────────────
SRCS_CORE = \
    core/vfr_ctx.c \
    core/vfr_pool.c \
    sdk/vfr_map.c

SRCS_MOCK = \
    platform/mock/mock_adapter.c

SRCS_TEST_SINGLE = \
    test/test_single_proc.c

# ─── targets ─────────────────────────────────────────────────────────────────
.PHONY: all clean valgrind asan check

all: test_single_proc

test_single_proc: $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_TEST_SINGLE)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^

# valgrind：無 leak、無 memory error
valgrind: test_single_proc
	valgrind \
	    --leak-check=full \
	    --show-leak-kinds=all \
	    --track-origins=yes \
	    --error-exitcode=1 \
	    ./test_single_proc --no-dump

# AddressSanitizer：Phase 3 規格項目
asan:
	$(CC) $(CFLAGS) $(INCLUDES) \
	    -fsanitize=address,undefined \
	    -fno-omit-frame-pointer \
	    -o test_single_proc_asan \
	    $(SRCS_CORE) $(SRCS_MOCK) $(SRCS_TEST_SINGLE)
	./test_single_proc_asan --no-dump

# memcpy 靜態驗證（應為 0）
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

clean:
	rm -f test_single_proc test_single_proc_asan
	rm -f /tmp/frame_*.yuv
