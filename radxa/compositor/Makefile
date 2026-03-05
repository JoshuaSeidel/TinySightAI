CC = gcc

# -D_GNU_SOURCE exposes sendmsg/recvmsg and POSIX extensions (needed by aa_video_output.c)
# -std=c99 enforces C99 compliance across all source files
CFLAGS  = -std=c99 -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS += -I/usr/include/rockchip
CFLAGS += -I/usr/include/rga
CFLAGS += -I/usr/include/libdrm

LDFLAGS = -lrockchip_mpp -lrga -ldrm -lpthread -lm

SRC_DIR = src
OBJ_DIR = obj
BIN = compositor

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)

install: $(BIN)
	install -m 755 $(BIN) /opt/aadongle/bin/compositor
