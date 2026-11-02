# Shortcuts for the CMake build and the test scripts. `make help` lists them.
#
# Settings (make VAR=value, or in the environment):
#   BUILD       build folder (build)
#   BUILD_TYPE  CMake build type (Release)
#   DEV_TOOLS   build tome-mpq, which the e2e test's map needs (ON)
#   DEV         developer build: tome.mix reads the TOME_DEBUG_* variables the
#               e2e test needs (ON); OFF for a release build (CI). Given on the
#               command line, it also changes an existing build folder
#   INSTALL_DIR a Warcraft III 1.27b folder `make deploy` copies tome.mix
#               into (no default; never input/)
#   HEADLESS    e2e and game: 1 runs the game in its own X server (Xvfb),
#               0 on your screen (e2e: 1, game: 0)
#   ENGINE      container tool: podman or docker (the first one found)
#   IMAGE_READY 1: the container targets use the $(IMAGE) image as it is,
#               without rebuilding it first (CI builds it with a cache)
#
# The tests read the game from input/ (input/README.md) and never write there.

BUILD      ?= build
BUILD_TYPE ?= Release
DEV_TOOLS  ?= ON
DEV        ?= ON
GENERATOR  ?= $(if $(shell command -v ninja),Ninja,Unix Makefiles)
ENGINE     ?= $(firstword $(foreach e,podman docker,$(if $(shell command -v $(e)),$(e))))
IMAGE      ?= tome-dev
E2E_KEEP   ?= 0

CMAKE_FLAGS = -G "$(GENERATOR)" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
              -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DTOME_BUILD_DEV_TOOLS=$(DEV_TOOLS) -DTOME_DEV=$(DEV)

.PHONY: all configure build test e2e game game-stop fuzz sdk bindings deploy clean \
        container container-shell container-test container-sdk container-e2e help

all: build

help:
	@echo "make build          configure (first time) and build into $(BUILD)/"
	@echo "make test           unit tests (ctest, under Wine)"
	@echo "make e2e            end-to-end test in the real game (needs the game in input/; Xvfb, about 6 min)"
	@echo "make game           start the build in the game (tools/game.sh start); make game-stop"
	@echo "make fuzz           libFuzzer targets (tools/fuzz.sh; FUZZ_SECONDS=60)"
	@echo "make sdk            the SDK zip for mod makers ($(BUILD)/tome-sdk-<version>.zip)"
	@echo "make bindings       regenerate the Rust bindings after changing tome.h"
	@echo "make deploy INSTALL_DIR=<game folder>   copy tome.mix there (nothing else writes into a game folder)"
	@echo "make container      build the $(IMAGE) image: the whole toolchain, Wine and Xvfb"
	@echo "make container-test / container-e2e   build and test inside it (build-container/)"
	@echo "make container-sdk  the SDK zip, built inside it"
	@echo "make clean          remove $(BUILD)/"

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

configure:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

# DEV given (make DEV=..., or the environment) sets it in the build folder;
# otherwise only a folder configured before TOME_DEV existed gets it, once.
DEV_GIVEN = $(filter command line environment,$(origin DEV))
build: $(BUILD)/CMakeCache.txt
	@grep -q$(if $(DEV_GIVEN),ix 'TOME_DEV:BOOL=$(DEV)', '^TOME_DEV:') $(BUILD)/CMakeCache.txt \
	    || cmake -S . -B $(BUILD) -DTOME_DEV=$(DEV)
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
	@test -n "$(INSTALL_DIR)" || { echo "make deploy INSTALL_DIR=<your Warcraft III 1.27b folder>"; exit 1; }
	@test -f "$(INSTALL_DIR)/war3.exe" || { echo "deploy: no war3.exe in $(INSTALL_DIR) (a Warcraft III 1.27b folder?)"; exit 1; }
	cmake -S . -B $(BUILD) -DTOME_INSTALL_DIR:PATH="$$(realpath -m "$(INSTALL_DIR)")"
	cmake --build $(BUILD) --target deploy

clean:
	rm -rf $(BUILD)

# --- the container ------------------------------------------------------------------
# The repository is mounted at /src and built into build-container/ (a CMake
# cache can't move between machines); input/ is mounted over itself
# read-only (also when it's a link to a folder outside the repository).
# SELinux labels are turned off for the container rather than changed on
# your files.

# Wine wants a HOME owned by the user, so it's a folder in the build; so is
# TMPDIR, so files E2E_KEEP=1 keeps outlive the container, and the runtime
# folder for the sound server (tools/test-env.sh).
CONTAINER_BUILD = build-container
HAVE_GAME     = $(shell test -f input/war3.exe && echo yes)
CONTAINER_RUN = $(ENGINE) run --rm -i $(if $(shell test -t 0 && echo yes),-t) \
                --security-opt label=disable -v "$(CURDIR):/src" -w /src -e HOME=/src/$(CONTAINER_BUILD)/home -e TMPDIR=/src/$(CONTAINER_BUILD)/tmp \
                -e XDG_RUNTIME_DIR=/src/$(CONTAINER_BUILD)/run \
                $(if $(filter podman,$(ENGINE)),--userns=keep-id,--user $$(id -u):$$(id -g)) \
                $(if $(HAVE_GAME),-v "$(realpath input):/src/input:ro") $(IMAGE)

container:
	@test -n "$(ENGINE)" || { echo "needs podman or docker"; exit 1; }
	$(ENGINE) build -t $(IMAGE) -f tools/container/Containerfile tools/container

# The image, unless IMAGE_READY=1 says it's already built.
CONTAINER_IMAGE = $(if $(filter 1,$(IMAGE_READY)),,container)

# Before each run (also with IMAGE_READY=1, which skips `container`): the
# engine, and the folders the run uses.
define CONTAINER_PREPARE
@test -n "$(ENGINE)" || { echo "needs podman or docker"; exit 1; }
mkdir -p $(CONTAINER_BUILD)/home $(CONTAINER_BUILD)/tmp
mkdir -p -m 700 $(CONTAINER_BUILD)/run
endef

container-shell: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) bash

container-test: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) make test BUILD=$(CONTAINER_BUILD) $(if $(DEV_GIVEN),DEV=$(DEV))

container-sdk: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) make sdk BUILD=$(CONTAINER_BUILD) $(if $(DEV_GIVEN),DEV=$(DEV))

container-e2e: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) env E2E_KEEP=$(E2E_KEEP) make e2e BUILD=$(CONTAINER_BUILD)
