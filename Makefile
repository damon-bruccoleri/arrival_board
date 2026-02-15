CC=cc

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)

CFLAGS = -O2 -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -D_GNU_SOURCE $(SDL_CFLAGS)
LDFLAGS =
LIBS = $(SDL_LIBS) -lSDL2_ttf -lcjson -lm
# Optional: make USE_SDL_IMAGE=1 to enable Steampunk bus background (requires libsdl2-image-dev)
ifneq ($(USE_SDL_IMAGE),)
CFLAGS += -DUSE_SDL_IMAGE
LIBS += -lSDL2_image
endif

OBJS = main.o tile.o

all: arrival_board

arrival_board: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

main.o: main.c tile.h
	$(CC) $(CFLAGS) -c -o $@ main.c

tile.o: tile.c tile.h
	$(CC) $(CFLAGS) -c -o $@ tile.c

clean:
	rm -f $(OBJS) arrival_board

.PHONY: all clean
