# Shortcuts for the CMake build and the test scripts. `make help` lists them.
#
# Settings (make VAR=value, or in the environment):
#   BUILD       build folder (build)
#   BUILD_TYPE  CMake build type (Release)
#   DEV_TOOLS   build tome-mpq, which the e2e test's map needs (ON)
#   GAME_DIR    your Warcraft III 1.27b folder, only ever read by the tests
#               (./WarCraft III); `make deploy` copies tome.mix into it
#   HEADLESS    e2e and game: 1 runs the game in its own X server (Xvfb),
#               0 on your screen (e2e: 1, game: 0)
#   ENGINE      container tool: podman or docker (the first one found)

BUILD      ?= build
BUILD_TYPE ?= Release
DEV_TOOLS  ?= ON
GAME_DIR   ?= $(CURDIR)/WarCraft III
GENERATOR  ?= $(if $(shell command -v ninja),Ninja,Unix Makefiles)
ENGINE     ?= $(firstword $(foreach e,podman docker,$(if $(shell command -v $(e)),$(e))))
IMAGE      ?= tome-dev
E2E_KEEP   ?= 0
export GAME_DIR

CMAKE_FLAGS = -G "$(GENERATOR)" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
              -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DTOME_BUILD_DEV_TOOLS=$(DEV_TOOLS) \
              -DTOME_GAME_DIR="$(GAME_DIR)"

.PHONY: all configure build test e2e game game-stop fuzz sdk bindings deploy clean \
        container container-shell container-test container-e2e help

all: build

help:
	@echo "make build          configure (first time) and build into $(BUILD)/"
	@echo "make test           unit tests (ctest, under Wine)"
	@echo "make e2e            end-to-end test in the real game (needs GAME_DIR; Xvfb, about 6 min)"
	@echo "make game           start the build in the game (tools/game.sh start); make game-stop"
	@echo "make fuzz           libFuzzer targets (tools/fuzz.sh; FUZZ_SECONDS=60)"
	@echo "make sdk            the SDK zip for mod makers ($(BUILD)/tome-sdk-<version>.zip)"
	@echo "make bindings       regenerate the Rust bindings after changing tome.h"
	@echo "make deploy         copy tome.mix into GAME_DIR (your install; nothing else writes there)"
	@echo "make container      build the $(IMAGE) image: the whole toolchain, Wine and Xvfb"
	@echo "make container-test / container-e2e   build and test inside it (build-container/)"
	@echo "make clean          remove $(BUILD)/"

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

configure:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

build: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD)

test: build
	cd $(BUILD) && ctest --output-on-failure

e2e: build
	BUILD="$(abspath $(BUILD))" tools/e2e-test.sh "$(BUILD)"

game: build
	BUILD="$(abspath $(BUILD))" tools/game.sh start

game-stop:
	BUILD="$(abspath $(BUILD))" tools/game.sh stop

FUZZ_SECONDS ?= 60
fuzz:
	tools/fuzz.sh $(FUZZ_SECONDS)

sdk: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) --target sdk-dist

bindings:
	tools/gen-rust-bindings.sh

deploy: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) --target deploy

clean:
	rm -rf $(BUILD)

# --- the container ------------------------------------------------------------------
# The repository is mounted at /src and built into build-container/ (a CMake
# cache can't move between machines); GAME_DIR is mounted read-only at /game.
# SELinux labels are turned off for the container rather than changed on
# your files.

# Wine wants a HOME owned by the user, so it's a folder in the build; so is
# TMPDIR, so files E2E_KEEP=1 keeps outlive the container.
CONTAINER_BUILD = build-container
HAVE_GAME     = $(shell test -f "$(GAME_DIR)/war3.exe" && echo yes)
CONTAINER_RUN = $(ENGINE) run --rm -i $(if $(shell test -t 0 && echo yes),-t) \
                --security-opt label=disable -v "$(CURDIR):/src" -w /src -e HOME=/src/$(CONTAINER_BUILD)/home -e TMPDIR=/src/$(CONTAINER_BUILD)/tmp \
                $(if $(filter podman,$(ENGINE)),--userns=keep-id,--user $$(id -u):$$(id -g)) \
                $(if $(HAVE_GAME),-v "$(GAME_DIR):/game:ro" -e GAME_DIR=/game) $(IMAGE)

container:
	@test -n "$(ENGINE)" || { echo "needs podman or docker"; exit 1; }
	$(ENGINE) build -t $(IMAGE) -f tools/container/Containerfile tools/container

container-shell: container
	mkdir -p $(CONTAINER_BUILD)/home $(CONTAINER_BUILD)/tmp
	$(CONTAINER_RUN) bash

container-test: container
	mkdir -p $(CONTAINER_BUILD)/home $(CONTAINER_BUILD)/tmp
	$(CONTAINER_RUN) make test BUILD=$(CONTAINER_BUILD)

container-e2e: container
	mkdir -p $(CONTAINER_BUILD)/home $(CONTAINER_BUILD)/tmp
	$(CONTAINER_RUN) env E2E_KEEP=$(E2E_KEEP) make e2e BUILD=$(CONTAINER_BUILD)
