# sim_particles — granular horde core (M1+M2)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = -lm

# SDL3 built from source lives in ~/.local (its .pc already carries the rpath)
SDL_PC     := PKG_CONFIG_PATH=$(HOME)/.local/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config
SDL_CFLAGS := $(shell $(SDL_PC) --cflags sdl3 2>/dev/null)
SDL_LIBS   := $(shell $(SDL_PC) --libs   sdl3 2>/dev/null)

all: test_particles test_impulse

test_particles: test_particles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_particles.c sim_particles.c $(LDLIBS)

test_impulse: test_impulse.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_impulse.c sim_particles.c $(LDLIBS)

sandbox: sandbox_particles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ sandbox_particles.c sim_particles.c $(SDL_LIBS) $(LDLIBS)

test: all
	./test_particles
	./test_impulse

clean:
	rm -rf test_particles test_impulse sandbox frames

.PHONY: all test clean
