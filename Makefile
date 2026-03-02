CC     = gcc
CFLAGS = -Wall -Wextra -g -D_GNU_SOURCE
TARGET = container-sim

SRCS = src/main.c src/container.c src/namespace.c src/filesystem.c src/logger.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	sudo ./$(TARGET)

clean:
	rm -f src/*.o $(TARGET) container.log

.PHONY: all run clean
