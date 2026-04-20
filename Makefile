CC     = gcc
CFLAGS = -Wall -g -D_GNU_SOURCE -pthread
TARGET = container-sim
BIN_DIR = bin

SRCS = src/main.c src/container.c src/logger.c src/namespace.c src/filesystem.c src/resource.c src/scheduler.c src/network.c src/monitor.c
OBJS = $(SRCS:.c=.o)

WORKLOADS = $(BIN_DIR)/workload-cpu $(BIN_DIR)/workload-mem $(BIN_DIR)/workload-fork $(BIN_DIR)/workload-netcheck

all: $(TARGET) workloads

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

workloads: $(WORKLOADS)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(BIN_DIR)/workload-cpu: src/workload_cpu.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/workload-mem: src/workload_mem.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/workload-fork: src/workload_fork.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

$(BIN_DIR)/workload-netcheck: src/workload_netcheck.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f src/*.o $(TARGET) $(WORKLOADS)

.PHONY: all clean workloads
