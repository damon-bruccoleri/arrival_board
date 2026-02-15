CC=gcc
CFLAGS=-D_GNU_SOURCE -O2 -Wall -Wextra -std=c11 \
  -I/usr/include/SDL2 -D_REENTRANT \
  -I/usr/include/harfbuzz -I/usr/include/freetype2 -I/usr/include/libpng16 \
  -I/usr/include/glib-2.0 -I/usr/lib/aarch64-linux-gnu/glib-2.0/include \
  -I/usr/include/aarch64-linux-gnu -I/usr/include/cjson

LDFLAGS=-lSDL2_ttf -lSDL2 -lcurl -lcjson -lm

all: arrival_board

arrival_board: arrival_board.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f arrival_board
