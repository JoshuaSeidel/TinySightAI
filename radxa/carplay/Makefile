CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -pthread -I. -Imfi -Iiap2 -Iairplay \
          $(shell pkg-config --cflags libavcodec libavutil 2>/dev/null)

# Build-time dependencies on Debian (Radxa Cubie A7Z):
#   sudo apt install libssl-dev libavahi-client-dev libavahi-common-dev \
#                    libasound2-dev libavcodec-dev libavutil-dev \
#                    libbluetooth-dev libdbus-1-dev
LDFLAGS = -lpthread -lcrypto -lssl -lbluetooth -ldbus-1 \
          -lavahi-client -lavahi-common \
          -lasound \
          -lm \
          $(shell pkg-config --libs libavcodec libavutil 2>/dev/null)

SRC = main.c \
      mfi/mfi_auth.c \
      iap2/iap2_link.c \
      iap2/iap2_session.c \
      iap2/iap2_bt_sdp.c \
      iap2/iap2_bt_transport.c \
      iap2/iap2_auth.c \
      iap2/iap2_carplay.c \
      airplay/airplay_server.c \
      airplay/airplay_rtsp.c \
      airplay/airplay_pair.c \
      airplay/airplay_fairplay.c \
      airplay/airplay_mirror.c \
      airplay/airplay_mdns.c \
      airplay/airplay_audio.c

OBJ = $(SRC:.c=.o)
BIN = carplay-stack

.PHONY: all clean install

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJ) $(BIN)

install: $(BIN)
	install -m 755 $(BIN) /opt/aadongle/bin/carplay-stack
