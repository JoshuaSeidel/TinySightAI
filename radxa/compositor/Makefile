CC = gcc

# -D_GNU_SOURCE exposes sendmsg/recvmsg and POSIX extensions (needed by aa_video_output.c)
# -std=c99 enforces C99 compliance across all source files
CFLAGS  = -std=c99 -O2 -Wall -Wextra -pthread -D_GNU_SOURCE
CFLAGS += -I/usr/include/libdrm

# Allwinner A733 (Cubie A7Z): CedarVE for H.264 encode/decode, G2D for 2D compositing
# TODO: Add Allwinner VPU/G2D include paths once vendor SDK is available on the board
# Candidate includes: /usr/include/cedarc, /usr/include/g2d
# For now, pipeline.c should use V4L2 M2M API or ffmpeg with hw accel

# NPU: check for VeriSilicon VIPLite runtime (VIP9000 on Allwinner A733)
ifneq ($(wildcard /usr/lib/libVIPlite.so /usr/lib/aarch64-linux-gnu/libVIPlite.so),)
CFLAGS += -I/usr/include/VIPLite
LDFLAGS = -ldrm -lpthread -lm -lVIPlite -lVIPuser
else
CFLAGS += -DBABY_AI_STUB
LDFLAGS = -ldrm -lpthread -lm
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
