CC = gcc

# -D_GNU_SOURCE exposes sendmsg/recvmsg and POSIX extensions (needed by aa_video_output.c)
# -std=c99 enforces C99 compliance across all source files
CFLAGS  = -std=c99 -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS += -I/usr/include/libdrm

# FFmpeg (libavcodec for H.264/H.265 decode + H.264 encode, libswscale for scaling)
FFMPEG_CFLAGS := $(shell pkg-config --cflags libavcodec libavutil libswscale 2>/dev/null)
FFMPEG_LIBS   := $(shell pkg-config --libs libavcodec libavutil libswscale 2>/dev/null)
CFLAGS += $(FFMPEG_CFLAGS)

# NPU: VeriSilicon VIP9000 on Allwinner A733
# baby_ai.c uses the awnn wrapper library (libawnn.so) which wraps VIPLite.
# awnn is built from ai-sdk during setup.sh alongside VIPLite SDK.
ifneq ($(wildcard /usr/lib/libawnn.so),)
LDFLAGS = -ldrm -lpthread -lm -lawnn $(FFMPEG_LIBS)
else
CFLAGS += -DBABY_AI_STUB
LDFLAGS = -ldrm -lpthread -lm $(FFMPEG_LIBS)
endif

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
