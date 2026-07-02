# sim_particles — granular horde core (M1+M2)
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11 -pthread
LDLIBS   = -lm

# --- Platform / cross-compile ------------------------------------------------
# Native Linux by default. Windows cross-build via mingw-w64:
#     make vat_horde TARGET=win64
# Needs the SDL3 mingw-w64 dev package; set SDL3_MINGW to its prefix (the dir
# holding include/SDL3 and lib/libSDL3.dll.a). Override CROSS for a different
# toolchain triplet. Graphical exes get the .exe suffix. Ship SDL3.dll alongside.
TARGET ?= native

ifeq ($(TARGET),win64)
  CROSS      ?= x86_64-w64-mingw32-
  CC         := $(CROSS)gcc
  EXE        := .exe
  DL_LIB     :=                       # no dlopen on Windows (glad uses LoadLibrary)
  # win32 import libs an SDL3 OpenGL app pulls in
  PLAT_LIBS  := -lopengl32 -lgdi32 -lwinmm -lole32 -loleaut32 -limm32 \
                -lsetupapi -lversion -lcfgmgr32
  SDL3_MINGW ?= /usr/x86_64-w64-mingw32
  SDL_CFLAGS := -I$(SDL3_MINGW)/include
  SDL_LIBS   := -L$(SDL3_MINGW)/lib -lSDL3
else
  EXE        :=
  DL_LIB     := -ldl
  PLAT_LIBS  :=
  # SDL3 built from source lives in ~/.local (its .pc already carries the rpath)
  SDL_PC     := PKG_CONFIG_PATH=$(HOME)/.local/lib/pkgconfig:$(PKG_CONFIG_PATH) pkg-config
  SDL_CFLAGS := $(shell $(SDL_PC) --cflags sdl3 2>/dev/null)
  SDL_LIBS   := $(shell $(SDL_PC) --libs   sdl3 2>/dev/null)
endif

all: test_particles test_impulse test_dormant test_stun test_handles test_query test_corpses \
     test_corpse_pile \
     test_types test_density_route test_jam test_blood_fear test_scene test_siege test_turret \
     test_defense test_base test_director test_terrain test_pick test_editor \
     test_breakthrough test_props test_vat_layer test_turret_siege test_drag test_hybrid test_car \
     test_destruct test_place test_app test_anim

# audio backend (GAME_APP_DESIGN.md): se l'utente ha scaricato miniaudio.h in
# vat/, il target `game` suona; altrimenti backend nullo (muto), zero errori.
ifneq ($(wildcard vat/miniaudio.h),)
  AUDIO_DEF := -DHAVE_MINIAUDIO
else
  AUDIO_DEF :=
endif

test_particles: test_particles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_particles.c sim_particles.c $(LDLIBS)

test_breakthrough: test_breakthrough.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_breakthrough.c sim_particles.c $(LDLIBS)

# draggable objects + barricate sfondabili (DRAG_DESIGN.md)
test_drag: test_drag.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_drag.c sim_particles.c $(LDLIBS)

# cars = two draggable discs + a rigid rod joint (DRAG_DESIGN.md §8)
test_car: test_car.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_car.c sim_particles.c $(LDLIBS)

# destructible decor props (DESTRUCT_DESIGN.md): horde shatters them on contact
test_destruct: test_destruct.c destruct.c sim_particles.c scene.c props.c \
               destruct.h sim_particles.h scene.h props.h
	$(CC) $(CFLAGS) -o $@ test_destruct.c destruct.c sim_particles.c scene.c props.c $(LDLIBS)

# Hybrid barricade: struttura distruttibile -> detriti draggable al crollo
test_hybrid: test_hybrid.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_hybrid.c defense.c sim_particles.c $(LDLIBS)

test_place: test_place.c place.c defense.c sim_particles.c place.h defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_place.c place.c defense.c sim_particles.c $(LDLIBS)

test_impulse: test_impulse.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_impulse.c sim_particles.c $(LDLIBS)

test_dormant: test_dormant.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_dormant.c sim_particles.c $(LDLIBS)

test_stun: test_stun.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_stun.c sim_particles.c $(LDLIBS)

test_handles: test_handles.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_handles.c sim_particles.c $(LDLIBS)

test_query: test_query.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_query.c sim_particles.c $(LDLIBS)

test_corpses: test_corpses.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_corpses.c sim_particles.c $(LDLIBS)

test_corpse_pile: test_corpse_pile.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_corpse_pile.c sim_particles.c $(LDLIBS)

test_types: test_types.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_types.c sim_particles.c $(LDLIBS)

test_density_route: test_density_route.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_density_route.c sim_particles.c $(LDLIBS)

test_jam: test_jam.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_jam.c sim_particles.c $(LDLIBS)

test_blood_fear: test_blood_fear.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_blood_fear.c sim_particles.c $(LDLIBS)

test_scene: test_scene.c scene.c scene.h sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_scene.c scene.c sim_particles.c $(LDLIBS)

test_siege: test_siege.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_siege.c sim_particles.c $(LDLIBS)

test_turret: test_turret.c sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_turret.c sim_particles.c $(LDLIBS)

test_defense: test_defense.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_defense.c defense.c sim_particles.c $(LDLIBS)

test_turret_siege: test_turret_siege.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_turret_siege.c defense.c sim_particles.c $(LDLIBS)

test_base: test_base.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_base.c defense.c sim_particles.c $(LDLIBS)

test_director: test_director.c defense.c sim_particles.c defense.h sim_particles.h
	$(CC) $(CFLAGS) -o $@ test_director.c defense.c sim_particles.c $(LDLIBS)

test_terrain: test_terrain.c terrain.c terrain.h
	$(CC) $(CFLAGS) -o $@ test_terrain.c terrain.c $(LDLIBS)

# pure-decor prop catalog loader (headless, zero deps): EDITOR_DESIGN §10 stadio 5b
test_props: test_props.c props.c props.h
	$(CC) $(CFLAGS) -o $@ test_props.c props.c $(LDLIBS)

# VAT render-layer animation bookkeeping (headless, no GL): hit one-shot + death pool
test_vat_layer: test_vat_layer.c vat/vat_layer.c sim_particles.c vat/vat_layer.h sim_particles.h
	$(CC) $(CFLAGS) -I. -o $@ test_vat_layer.c vat/vat_layer.c sim_particles.c $(LDLIBS)

# editor picking math (headless, zero deps): EDITOR_DESIGN §6 verifica (b)
test_pick: test_pick.c vat/edit_pick.h
	$(CC) $(CFLAGS) -o $@ test_pick.c $(LDLIBS)

# editor mutation logic + save/reload roundtrip: EDITOR_DESIGN §6 verifica (a)
test_editor: test_editor.c vat/editor.h scene.c scene.h sim_particles.c sim_particles.h
	$(CC) $(CFLAGS) -I. -o $@ test_editor.c scene.c sim_particles.c $(LDLIBS)

# application flow della shell di gioco (GAME_APP_DESIGN.md): stati, campagna,
# progressi. Logica pura, zero deps.
test_app: test_app.c app.c app.h
	$(CC) $(CFLAGS) -o $@ test_app.c app.c $(LDLIBS)

# envelope one-shot dei meccanismi (rinculo torrette): pool fisso, zero deps.
test_anim: test_anim.c anim.c anim.h
	$(CC) $(CFLAGS) -o $@ test_anim.c anim.c $(LDLIBS)

# headless CPU benchmark del core (zero deps: niente SDL/GL/asset). Portatile.
bench_sim: bench_sim.c sim_particles.c scene.c sim_particles.h scene.h
	$(CC) $(CFLAGS) -o $@ bench_sim.c sim_particles.c scene.c $(LDLIBS)

sandbox: sandbox_particles.c sim_particles.c sim_particles.h scene.c scene.h \
         sprite_layer.c sprite_layer.h
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@$(EXE) sandbox_particles.c sim_particles.c scene.c sprite_layer.c $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS)

sprite_view: sprite_view.c
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@$(EXE) sprite_view.c $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS)

# VAT 3D previewer (migrazione_3d.md). glad.c e stb compilati con -w (codice di terzi).
vat_view: vat/vat_view.c vat/glad.c vat/stb_impl.c vat/vat.vs vat/vat.fs
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Ivat -o $@$(EXE) vat/vat_view.c vat/glad.o vat/stb_impl.o $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS) $(DL_LIB)

# Orda reale del core sim_particles resa in 3D VAT (vat_layer + vat_horde) su
# scena vettoriale (scene.c). Ostacoli estrusi via vat/flat.vs/.fs.
VAT_HORDE_SRC = vat/vat_horde.c vat/vat_layer.c sim_particles.c fx_particles.c scene.c defense.c place.c terrain.c props.c destruct.c anim.c
VAT_HORDE_DEP = $(VAT_HORDE_SRC) vat/glad.c vat/stb_impl.c vat/cgltf_impl.c vat/vat.vs vat/vat.fs vat/flat.vs vat/flat.fs vat/ground.vs vat/ground.fs vat/shadow.vs vat/shadow.fs vat/decal.vs vat/decal.fs vat/corpse_decal.vs vat/corpse_decal.fs vat/corpsebake.fs vat/particle.vs vat/particle.fs vat/mesh.vs vat/mesh.fs sim_particles.h fx_particles.h vat/vat_layer.h vat/vat_gl.h vat/cgltf.h terrain.h scene.h defense.h place.h props.h destruct.h anim.h

vat_horde: $(VAT_HORDE_DEP)
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) -O2 -w -Ivat -c vat/cgltf_impl.c -o vat/cgltf_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Ivat -I. -o $@$(EXE) $(VAT_HORDE_SRC) vat/glad.o vat/stb_impl.o vat/cgltf_impl.o $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS) $(DL_LIB)

# L'ESEGUIBILE DEL GIOCO (GAME_APP_DESIGN.md): vat_horde + shell applicativa
# (-DGAME_SHELL): title/menu/briefing/prep/assalto/debrief, campagna, audio.
#     ./game [campaign.txt]
game: $(VAT_HORDE_DEP) app.c app.h audio.c audio.h vat/font8.h vat/ui.fs campaign.txt
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) -O2 -w -Ivat -c vat/cgltf_impl.c -o vat/cgltf_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -DGAME_SHELL $(AUDIO_DEF) -Ivat -I. -o $@$(EXE) $(VAT_HORDE_SRC) app.c audio.c vat/glad.o vat/stb_impl.o vat/cgltf_impl.o $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS) $(DL_LIB)

fxlab: vat/fxlab.c vat/vat_layer.c sim_particles.c fx_particles.c terrain.c vat/glad.c vat/stb_impl.c vat/cgltf_impl.c vat/vat.vs vat/vat.fs vat/flat.vs vat/flat.fs vat/ground.vs vat/ground.fs vat/shadow.vs vat/shadow.fs vat/decal.vs vat/decal.fs vat/corpse_decal.vs vat/corpse_decal.fs vat/particle.vs vat/particle.fs vat/mesh.vs vat/mesh.fs sim_particles.h fx_particles.h vat/vat_layer.h vat/vat_gl.h vat/cgltf.h vat/nuklear.h terrain.h
	$(CC) -O2 -w -Ivat $(SDL_CFLAGS) -c vat/glad.c -o vat/glad.o
	$(CC) -O2 -w -Ivat -c vat/stb_impl.c -o vat/stb_impl.o
	$(CC) -O2 -w -Ivat -c vat/cgltf_impl.c -o vat/cgltf_impl.o
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -Ivat -I. -o $@$(EXE) vat/fxlab.c vat/vat_layer.c sim_particles.c fx_particles.c terrain.c vat/glad.o vat/stb_impl.o vat/cgltf_impl.o $(SDL_LIBS) $(PLAT_LIBS) $(LDLIBS) $(DL_LIB)

test: all
	./test_particles
	./test_impulse
	./test_dormant
	./test_stun
	./test_handles
	./test_query
	./test_corpses
	./test_corpse_pile
	./test_types
	./test_density_route
	./test_jam
	./test_blood_fear
	./test_scene
	./test_siege
	./test_turret
	./test_defense
	./test_base
	./test_director
	./test_terrain
	./test_pick
	./test_editor
	./test_breakthrough
	./test_props
	./test_vat_layer
	./test_drag
	./test_hybrid
	./test_place
	./test_app
	./test_anim

clean:
	rm -rf test_particles test_impulse test_dormant test_handles test_query \
	       test_corpses test_corpse_pile test_types test_density_route test_jam test_blood_fear test_scene \
	       test_siege test_turret test_defense test_base test_director test_terrain bench_sim test_pick test_editor test_breakthrough test_props test_vat_layer test_drag test_hybrid test_car test_destruct test_place \
	       test_app test_anim game \
	       sandbox sprite_view vat_view vat_horde frames *.exe vat/*.o

.PHONY: all test clean
