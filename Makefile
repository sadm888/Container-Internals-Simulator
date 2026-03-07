CC     = gcc
CFLAGS = -Wall -g -D_GNU_SOURCE
TARGET = container-sim

SRCS = src/main.c src/container.c src/logger.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f src/*.o $(TARGET)

.PHONY: all clean
