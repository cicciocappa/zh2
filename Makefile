# sim_particles — granular horde core (M1+M2)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -pthread
LDLIBS   = -lm

# SDL3 built from source lives in ~/.local (its .pc already carries the rpath)
SDL_PC     := PKG_CONFIG_PATH=$(HOME)/.local/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config
SDL_CFLAGS := $(shell $(SDL_PC) --cflags sdl3 2>/dev/null)
SDL_LIBS   := $(shell $(SDL_PC) --libs   sdl3 2>/dev/null)

all: test_particles test_impulse test_dormant test_handles test_query test_corpses \
     test_types test_density_route test_jam test_scene test_siege test_turret \
     test_defense

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

test_jam: test_jam.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_jam.c sim_particles.c $(LDLIBS)

test_scene: test_scene.c scene.c scene.h sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_scene.c scene.c sim_particles.c $(LDLIBS)

test_siege: test_siege.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_siege.c sim_particles.c $(LDLIBS)

test_turret: test_turret.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_turret.c sim_particles.c $(LDLIBS)

test_defense: test_defense.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_defense.c defense.c sim_particles.c $(LDLIBS)

# headless CPU benchmark del core (zero deps: niente SDL/GL/asset). Portatile.
bench_sim: bench_sim.c sim_particles.c scene.c sim_particles.h scene.h
	$(CC) $(CFLAGS) -o $@ bench_sim.c sim_particles.c scene.c $(LDLIBS)

sandbox: sandbox_particles.c sim_particles.c sim_particles.h scene.c scene.h \
         sprite_layer.c sprite_layer.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ sandbox_particles.c sim_particles.c scene.c sprite_layer.c $(SDL_LIBS) $(LDLIBS)

sprite_view: sprite_view.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ sprite_view.c $(SDL_LIBS) $(LDLIBS)

# VAT 3D previewer (migrazione_3d.md). glad.c e stb compilati con -w (codice di terzi).
vat_view: vat/vat_view.c vat/glad.c vat/stb_impl.c vat/vat.vs vat/vat.fs
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Ivat -o $@ vat/vat_view.c vat/glad.o vat/stb_impl.o $(SDL_LIBS) $(LDLIBS) -ldl

# Orda reale del core sim_particles resa in 3D VAT (vat_layer + vat_horde) su
# scena vettoriale (scene.c). Ostacoli estrusi via vat/flat.vs/.fs.
vat_horde: vat/vat_horde.c vat/vat_layer.c sim_particles.c scene.c defense.c vat/glad.c vat/stb_impl.c vat/vat.vs vat/vat.fs vat/flat.vs vat/flat.fs sim_particles.h vat/vat_layer.h vat/vat_gl.h scene.h defense.h
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Ivat -I. -o $@ vat/vat_horde.c vat/vat_layer.c sim_particles.c scene.c defense.c vat/glad.o vat/stb_impl.o $(SDL_LIBS) $(LDLIBS) -ldl

test: all
	./test_particles
	./test_impulse
	./test_dormant
	./test_handles
	./test_query
	./test_corpses
	./test_types
	./test_density_route
	./test_jam
	./test_scene
	./test_siege
	./test_turret
	./test_defense

clean:
	rm -rf test_particles test_impulse test_dormant test_handles test_query \
	       test_corpses test_types test_density_route test_jam test_scene \
	       test_siege test_turret test_defense bench_sim sandbox sprite_view frames

.PHONY: all test clean
