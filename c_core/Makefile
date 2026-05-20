# ─────────────────────────────────────────────────────────────────────
# EduOS Makefile
# Targets: all  clean  race  fixed  memcheck
# No hard-coded absolute paths.
# ─────────────────────────────────────────────────────────────────────

CC      := gcc
CFLAGS  := -Wall -Wextra -std=c11 -pthread -Iinclude
LDFLAGS := -pthread -lrt   # -lrt needed for shm_open on some Linux distros

SRCS    := process_manager.c thread_manager.c ipc_module.c main_sim.c
OBJS    := $(SRCS:.c=.o)
TARGET  := eduos

# ── Default target: compile everything, zero warnings ────────────────
.PHONY: all
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "Build complete: ./$@"

%.o: %.c include/eduos.h
	$(CC) $(CFLAGS) -c -o $@ $<

# ── make race: compile WITHOUT mutex protection ───────────────────────
# Run 5 times; results should be non-deterministic (data race).
.PHONY: race
race: CFLAGS += -DRACE_DEMO
race: clean $(TARGET)
	@echo "=== Running race demo 5 times (results may differ) ==="
	@for i in 1 2 3 4 5; do \
	    echo "--- Run $$i ---"; \
	    ./$(TARGET) 2>&1 | grep "shared_counter"; \
	done

# ── make fixed: compile WITH mutex protection ─────────────────────────
# Counter should always equal 1000000.
.PHONY: fixed
fixed: clean $(TARGET)
	@echo "=== Running fixed demo 5 times (must always be 1000000) ==="
	@for i in 1 2 3 4 5; do \
	    echo "--- Run $$i ---"; \
	    ./$(TARGET) 2>&1 | grep "shared_counter"; \
	done

# ── make memcheck: run under valgrind ────────────────────────────────
.PHONY: memcheck
memcheck: all
	valgrind --leak-check=full \
	         --show-leak-kinds=all \
	         --track-origins=yes \
	         --error-exitcode=1 \
	         ./$(TARGET) 2>&1 | tee valgrind_report.txt
	@echo "Valgrind complete — see valgrind_report.txt"

# ── Clean ─────────────────────────────────────────────────────────────
.PHONY: clean
clean:
	rm -f $(OBJS) $(TARGET) valgrind_report.txt pcb_snapshot.json
	@echo "Clean complete"
