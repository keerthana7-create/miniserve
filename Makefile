CC ?= gcc
BASE_CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_GNU_SOURCE -Iinclude
CFLAGS = $(BASE_CFLAGS) -O2
LDFLAGS = -pthread

SRC_DIR = src
INC_DIR = include
OBJ_DIR = obj
BIN_DIR = bin
TEST_DIR = tests

# Core server implementation sources (shared with test harness)
CORE_SRCS = $(SRC_DIR)/logger.c $(SRC_DIR)/util.c $(SRC_DIR)/server.c $(SRC_DIR)/hashtable.c $(SRC_DIR)/protocol.c $(SRC_DIR)/threadpool.c
CORE_OBJS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(CORE_SRCS))

MAIN_SRC = $(SRC_DIR)/main.c
MAIN_OBJ = $(OBJ_DIR)/main.o

TARGET = $(BIN_DIR)/miniserve

# Unit and integration test executables
TEST_PHASE1 = $(BIN_DIR)/test_phase1
TEST_HASHTABLE = $(BIN_DIR)/test_hashtable
TEST_PROTOCOL = $(BIN_DIR)/test_protocol
TEST_THREADPOOL = $(BIN_DIR)/test_threadpool
TEST_SERVER = $(BIN_DIR)/test_server
TEST_ROBUSTNESS = $(BIN_DIR)/test_robustness
TEST_EPOLL = $(BIN_DIR)/test_epoll

# ThreadSanitizer-instrumented test executables
TEST_PHASE1_TSAN = $(BIN_DIR)/test_phase1_tsan
TEST_HT_TSAN = $(BIN_DIR)/test_hashtable_tsan
TEST_PROTO_TSAN = $(BIN_DIR)/test_protocol_tsan
TEST_THREADPOOL_TSAN = $(BIN_DIR)/test_threadpool_tsan
TEST_SERVER_TSAN = $(BIN_DIR)/test_server_tsan
TEST_ROBUSTNESS_TSAN = $(BIN_DIR)/test_robustness_tsan
TEST_EPOLL_TSAN = $(BIN_DIR)/test_epoll_tsan

# Phase 7 benchmark client executable
BENCH_SRC = bench/bench_client.c
BENCH_TARGET = $(BIN_DIR)/bench_client

# Test compilation flags: ALWAYS disable NDEBUG so assertions remain active
TEST_CFLAGS = $(BASE_CFLAGS) -g3 -O0 -UNDEBUG

# ThreadSanitizer flags: -no-pie avoids low-memory address collision on modern Linux kernels
TSAN_CFLAGS = $(BASE_CFLAGS) -g3 -O1 -fsanitize=thread -no-pie -DDEBUG -UNDEBUG
TSAN_LDFLAGS = -pthread -fsanitize=thread -no-pie

.PHONY: all release debug tsan valgrind test clean bench

all: release

release: CFLAGS = $(BASE_CFLAGS) -O2 -DNDEBUG
release: $(TARGET)

debug: CFLAGS = $(BASE_CFLAGS) -g3 -O0 -DDEBUG -UNDEBUG
debug: $(TARGET)

$(TARGET): $(MAIN_OBJ) $(CORE_OBJS) | $(BIN_DIR)
	$(CC) $(MAIN_OBJ) $(CORE_OBJS) -o $@ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Standard unit and integration test executables (-UNDEBUG)
$(TEST_PHASE1): $(TEST_DIR)/test_phase1.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_phase1.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_HASHTABLE): $(TEST_DIR)/test_hashtable.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_hashtable.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_PROTOCOL): $(TEST_DIR)/test_protocol.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_protocol.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_THREADPOOL): $(TEST_DIR)/test_threadpool.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_threadpool.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_SERVER): $(TEST_DIR)/test_server.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_server.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_ROBUSTNESS): $(TEST_DIR)/test_robustness.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_robustness.c $(CORE_SRCS) -o $@ $(LDFLAGS)

$(TEST_EPOLL): $(TEST_DIR)/test_epoll.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TEST_CFLAGS) $(TEST_DIR)/test_epoll.c $(CORE_SRCS) -o $@ $(LDFLAGS)

# ThreadSanitizer-instrumented test executables
$(TEST_PHASE1_TSAN): $(TEST_DIR)/test_phase1.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_phase1.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_HT_TSAN): $(TEST_DIR)/test_hashtable.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_hashtable.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_PROTO_TSAN): $(TEST_DIR)/test_protocol.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_protocol.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_THREADPOOL_TSAN): $(TEST_DIR)/test_threadpool.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_threadpool.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_SERVER_TSAN): $(TEST_DIR)/test_server.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_server.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_ROBUSTNESS_TSAN): $(TEST_DIR)/test_robustness.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_robustness.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

$(TEST_EPOLL_TSAN): $(TEST_DIR)/test_epoll.c $(CORE_SRCS) $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(TSAN_CFLAGS) $(TEST_DIR)/test_epoll.c $(CORE_SRCS) -o $@ $(TSAN_LDFLAGS)

test: $(TEST_PHASE1) $(TEST_HASHTABLE) $(TEST_PROTOCOL) $(TEST_THREADPOOL) $(TEST_SERVER) $(TEST_ROBUSTNESS) $(TEST_EPOLL)
	@echo "--- Running Phase 1 Unit Tests ---"
	./$(TEST_PHASE1)
	@echo "--- Running Hash Table Unit Tests ---"
	./$(TEST_HASHTABLE)
	@echo "--- Running Protocol Parser Unit Tests ---"
	./$(TEST_PROTOCOL)
	@echo "--- Running Thread Pool & Queue Unit Tests ---"
	./$(TEST_THREADPOOL)
	@echo "--- Running TCP Server Integration Tests ---"
	./$(TEST_SERVER)
	@echo "--- Running Phase 5 Robustness & Signal Tests ---"
	./$(TEST_ROBUSTNESS)
	@echo "--- Running Phase 6 Linux Epoll Integration Tests ---"
	./$(TEST_EPOLL)

# Compile both server and all test harnesses with TSan and run the instrumented test suite.
# Strictly fails with nonzero exit status whenever an instrumented test fails, crashes, or reports a race.
# A retry with setarch -R is permitted ONLY if the initial process crashed at startup before any test ran
# and no data race was reported.
tsan: CFLAGS = $(TSAN_CFLAGS)
tsan: LDFLAGS = $(TSAN_LDFLAGS)
tsan: clean $(TARGET) $(TEST_PHASE1_TSAN) $(TEST_HT_TSAN) $(TEST_PROTO_TSAN) $(TEST_THREADPOOL_TSAN) $(TEST_SERVER_TSAN) $(TEST_ROBUSTNESS_TSAN) $(TEST_EPOLL_TSAN)
	@echo "--- Running ThreadSanitizer Test Suite ---"
	@for test_bin in $(TEST_PHASE1_TSAN) $(TEST_HT_TSAN) $(TEST_PROTO_TSAN) $(TEST_THREADPOOL_TSAN) $(TEST_SERVER_TSAN) $(TEST_ROBUSTNESS_TSAN) $(TEST_EPOLL_TSAN); do \
		echo "Executing $$test_bin..."; \
		out=$$(./$$test_bin 2>&1); \
		status=$$?; \
		if [ $$status -eq 0 ]; then \
			echo "$$out"; \
			echo "[PASS] $$test_bin passed TSan verification"; \
		else \
			echo "$$out"; \
			if echo "$$out" | grep -q "WARNING: ThreadSanitizer: data race"; then \
				echo "FAIL: Data race detected by ThreadSanitizer in $$test_bin (exit status $$status)"; \
				exit 1; \
			elif echo "$$out" | grep -q "Running "; then \
				echo "FAIL: Test failed or crashed during execution in $$test_bin (exit status $$status)"; \
				exit 1; \
			elif which setarch > /dev/null 2>&1; then \
				echo "Notice: $$test_bin crashed at startup before tests executed (exit status $$status)."; \
				echo "Retrying under setarch $$(uname -m) -R to check ASLR address collision..."; \
				retry_out=$$(setarch $$(uname -m) -R ./$$test_bin 2>&1); \
				retry_status=$$?; \
				if [ $$retry_status -eq 0 ]; then \
					echo "$$retry_out"; \
					echo "[PASS] $$test_bin passed TSan verification under setarch -R"; \
				else \
					echo "$$retry_out"; \
					if echo "$$retry_out" | grep -q "WARNING: ThreadSanitizer: data race"; then \
						echo "FAIL: Data race detected by ThreadSanitizer under setarch -R in $$test_bin"; \
					else \
						echo "FAIL: TSan runtime/environment failure in $$test_bin (exit status $$retry_status). TSan cannot run under host kernel memory layout."; \
					fi; \
					exit 1; \
				fi; \
			else \
				echo "FAIL: $$test_bin crashed before execution (exit status $$status). TSan environment failure."; \
				exit 1; \
			fi; \
		fi; \
	done

valgrind: debug
	@which valgrind > /dev/null 2>&1 || (echo "Error: valgrind is not installed." && exit 1)
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes --error-exitcode=1 $(TARGET) --help

$(BENCH_TARGET): $(BENCH_SRC) $(SRC_DIR)/util.c $(wildcard $(INC_DIR)/*.h) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(BENCH_SRC) $(SRC_DIR)/util.c -o $@ $(LDFLAGS)

bench: $(BENCH_TARGET)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR) *.log
