CC       = gcc
CFLAGS   = -Wall -Wextra -g -D_GNU_SOURCE -pthread
TARGET   = container-sim
BIN_DIR  = bin
PREFIX   ?= /usr/local

SRCS = src/main.c src/container.c src/image.c src/logger.c src/namespace.c \
       src/filesystem.c src/resource.c src/scheduler.c src/network.c \
       src/monitor.c src/bridge.c src/security.c src/eventbus.c \
       src/metrics.c src/orchestrator.c src/webserver.c src/alert.c
OBJS = $(SRCS:.c=.o)

WORKLOADS = $(BIN_DIR)/workload-cpu $(BIN_DIR)/workload-mem \
            $(BIN_DIR)/workload-fork $(BIN_DIR)/workload-netcheck

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

# Bootstrap test rootfs directories (requires Linux/WSL2)
setup:
	bash scripts/setup-rootfs.sh

# Install binary and web assets to PREFIX (default /usr/local)
install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -d $(DESTDIR)$(PREFIX)/share/container-sim/web
	install -m 644 web/index.html web/style.css web/app.js \
	        $(DESTDIR)$(PREFIX)/share/container-sim/web/
	install -d $(DESTDIR)$(PREFIX)/share/container-sim/specs
	install -m 644 specs/*.json \
	        $(DESTDIR)$(PREFIX)/share/container-sim/specs/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -rf $(DESTDIR)$(PREFIX)/share/container-sim

clean:
	rm -f src/*.o $(TARGET) $(WORKLOADS)

# Remove everything including rootfs and runtime state
distclean: clean
	rm -rf rootfs/ logs/ containers.meta images.meta container.log

test: all setup
	sudo bash tests/run_all.sh

.PHONY: all clean distclean setup install uninstall test workloads
