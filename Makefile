CC      = gcc
CFLAGS  = -Wall -Wextra -g -D_GNU_SOURCE
TARGET  = container-sim

SRCDIR  = src
SRCS    = $(SRCDIR)/main.c      \
          $(SRCDIR)/container.c \
          $(SRCDIR)/namespace.c \
          $(SRCDIR)/logger.c
OBJS    = $(SRCS:.c=.o)

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(SRCDIR)/%.o: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Convenience: build and run as root (namespace requires root in WSL2)
run: $(TARGET)
	sudo ./$(TARGET)

clean:
	rm -f $(SRCDIR)/*.o $(TARGET) container.log
