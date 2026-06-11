# sim_particles — granular horde core (M1+M2)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDLIBS   = -lm

# SDL3 built from source lives in ~/.local (its .pc already carries the rpath)
SDL_PC     := PKG_CONFIG_PATH=$(HOME)/.local/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config
SDL_CFLAGS := $(shell $(SDL_PC) --cflags sdl3 2>/dev/null)
SDL_LIBS   := $(shell $(SDL_PC) --libs   sdl3 2>/dev/null)

all: test_particles test_impulse test_dormant test_handles test_query test_corpses \
     test_types test_density_route

test_particles: test_particles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_particles.c sim_particles.c $(LDLIBS)

test_impulse: test_impulse.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_impulse.c sim_particles.c $(LDLIBS)

test_dormant: test_dormant.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_dormant.c sim_particles.c $(LDLIBS)

test_handles: test_handles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_handles.c sim_particles.c $(LDLIBS)

test_query: test_query.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_query.c sim_particles.c $(LDLIBS)

test_corpses: test_corpses.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_corpses.c sim_particles.c $(LDLIBS)

test_types: test_types.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_types.c sim_particles.c $(LDLIBS)

test_density_route: test_density_route.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_density_route.c sim_particles.c $(LDLIBS)

sandbox: sandbox_particles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ sandbox_particles.c sim_particles.c $(SDL_LIBS) $(LDLIBS)

test: all
	./test_particles
	./test_impulse
	./test_dormant
	./test_handles
	./test_query
	./test_corpses
	./test_types
	./test_density_route

clean:
	rm -rf test_particles test_impulse test_dormant test_handles test_query \
	       test_corpses test_types test_density_route sandbox frames

.PHONY: all test clean
