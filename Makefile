# Arrival Board: MTA bus arrivals + weather on full-screen display.
# Optional: make USE_SDL_IMAGE=1 for background image and logo (requires libsdl2-image-dev).

CC = cc

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || echo -I/usr/include/SDL2 -D_REENTRANT)
SDL_LIBS   := $(shell sdl2-config --libs 2>/dev/null || echo -lSDL2)

CFLAGS = -O2 -std=c11 -Wall -Wextra -Wshadow -Wformat=2 -D_GNU_SOURCE $(SDL_CFLAGS)
LDFLAGS =
LIBS = $(SDL_LIBS) -lSDL2_ttf -lcjson -lm

ifneq ($(USE_SDL_IMAGE),)
CFLAGS += -DUSE_SDL_IMAGE
LIBS += -lSDL2_image
endif

OBJS = main.o tile.o util.o mta.o weather.o

all: arrival_board

arrival_board: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

main.o: main.c tile.h types.h util.h mta.h weather.h
	$(CC) $(CFLAGS) -c -o $@ main.c

tile.o: tile.c tile.h util.h
	$(CC) $(CFLAGS) -c -o $@ tile.c

util.o: util.c util.h
	$(CC) $(CFLAGS) -c -o $@ util.c

mta.o: mta.c mta.h types.h util.h
	$(CC) $(CFLAGS) -c -o $@ mta.c

weather.o: weather.c weather.h types.h util.h
	$(CC) $(CFLAGS) -c -o $@ weather.c

clean:
	rm -f $(OBJS) arrival_board

.PHONY: all clean
