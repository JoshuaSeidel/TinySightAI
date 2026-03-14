CC = gcc

# -D_GNU_SOURCE exposes sendmsg/recvmsg and POSIX extensions (needed by aa_video_output.c)
CFLAGS  = -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS += -I/usr/include/libdrm

# FFmpeg (libavcodec for H.264/H.265 decode + H.264 encode, libswscale for scaling)
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil libswscale 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs libavcodec libavutil libswscale 2>/dev/null)
CFLAGS += $(FFMPEG_CFLAGS)

SRC_DIR = src
OBJ_DIR = obj
BIN = compositor

SRCS = $(wildcard $(SRC_DIR)/*.c)

# NPU: VeriSilicon VIP9000 on Allwinner A733
# awnn_*.c are vendored from ai-sdk — they wrap VIPLite and require its headers/libs.
# When VIPLite is not installed, exclude awnn files and build baby_ai.c as stub.
ifneq ($(wildcard /usr/lib/libVIPhal.so /usr/lib/aarch64-linux-gnu/libVIPhal.so),)
CFLAGS += -I/usr/include/VIPLite
LDFLAGS = -ldrm -lpthread -lm -lVIPhal -lNBGlinker $(FFMPEG_LIBS)
else ifneq ($(wildcard /usr/lib/libVIPlite.so /usr/lib/aarch64-linux-gnu/libVIPlite.so),)
CFLAGS += -I/usr/include/VIPLite
LDFLAGS = -ldrm -lpthread -lm -lVIPlite -lVIPuser $(FFMPEG_LIBS)
else
CFLAGS += -DBABY_AI_STUB
SRCS := $(filter-out $(SRC_DIR)/awnn_%.c, $(SRCS))
LDFLAGS = -ldrm -lpthread -lm $(FFMPEG_LIBS)
endif

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
