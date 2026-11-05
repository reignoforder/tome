# SPDX-License-Identifier: GPL-3.0-or-later
# Shortcuts for the CMake build and the test scripts. `make help` lists them.
#
# Settings (make VAR=value, or in the environment):
#   BUILD       build folder (build)
#   BUILD_TYPE  CMake build type (Release)
#   DEV_TOOLS   build tome-mpq, which the e2e test's map needs (ON)
#   DEV         developer build: tome.mix reads the TOME_DEBUG_* variables the
#               e2e test needs (ON); OFF for a release build (CI). Given on the
#               command line, it also changes an existing build folder
#   RELEASE_BUILD  build folder of `make dist` (build-release; always DEV=OFF)
#   CMAKE_EXTRA more CMake flags for a new build folder (tools/coverage.sh's
#               --coverage)
#   INSTALL_DIR a Warcraft III 1.27b folder `make deploy` copies tome.mix
#               into (no default; never input/)
#   GAME_VERSION the game the tests and the game targets use: the folder
#               input/<version>/ (1.27b). Also a goal: `make test 1.26a`
#   EDITION     the game the e2e test and the game targets run as: tft (The
#               Frozen Throne, default) or roc (Reign of Chaos: war3.exe
#               -classic). Also a goal: `make e2e roc`, `make e2e 1.26a roc`
#   HEADLESS    e2e and game: 1 runs the game in its own X server (Xvfb),
#               0 on your screen (e2e: 1, game: 0)
#   ENGINE      container tool: podman or docker (the first one found)
#   IMAGE_READY 1: the container targets use the $(IMAGE) image as it is,
#               without rebuilding it first (CI builds it with a cache)
#   IMAGE_TARGET  `make container`'s stage: ci (build and unit tests only,
#               as CI's image) or the default, everything (the e2e test too)
#
# The tests read the game from input/<version>/ (input/README.md) and never
# write there.

BUILD      ?= build
BUILD_TYPE ?= Release
DEV_TOOLS  ?= ON
DEV        ?= ON
GENERATOR  ?= $(if $(shell command -v ninja),Ninja,Unix Makefiles)
ENGINE     ?= $(firstword $(foreach e,podman docker,$(if $(shell command -v $(e)),$(e))))
IMAGE      ?= tome-dev
IMAGE_TARGET ?=
RELEASE_BUILD ?= build-release
E2E_KEEP   ?= 0

# The game version: a goal like `1.26a` (make test 1.26a) or GAME_VERSION.
# One named either way must be in input/; the default (1.27b) may be missing
# (the tests skip what needs the game).
GAME_VERSIONS = $(patsubst input/%/war3.exe,%,$(wildcard input/*/war3.exe))
VERSION_GOALS := $(filter 1.%,$(MAKECMDGOALS))
ifneq ($(word 2,$(VERSION_GOALS)),)
$(error one game version at a time, not $(VERSION_GOALS))
endif
ifneq ($(VERSION_GOALS),)
GAME_VERSION := $(VERSION_GOALS)
endif
ifneq ($(VERSION_GOALS)$(filter command line,$(origin GAME_VERSION)),)
ifeq ($(wildcard input/$(GAME_VERSION)/war3.exe),)
$(error no game for $(GAME_VERSION): input/$(GAME_VERSION)/war3.exe isn't there (input/ has: $(or $(GAME_VERSIONS),nothing); input/README.md))
endif
endif
GAME_VERSION ?= 1.27b
export GAME_VERSION
# The edition: a goal `roc` or `tft` (make e2e roc), or EDITION.
EDITION_GOALS := $(filter roc tft,$(MAKECMDGOALS))
ifneq ($(word 2,$(EDITION_GOALS)),)
$(error one edition at a time, not $(EDITION_GOALS))
endif
ifneq ($(EDITION_GOALS),)
EDITION := $(EDITION_GOALS)
endif
EDITION ?= tft
export EDITION
# For the tests (tests/game_dir.h, tests/tools/test_mpq.cmake).
export TOME_GAME_DIR = $(CURDIR)/input/$(GAME_VERSION)

CMAKE_FLAGS = -G "$(GENERATOR)" -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-i686-mingw.cmake \
              -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DTOME_BUILD_DEV_TOOLS=$(DEV_TOOLS) -DTOME_DEV=$(DEV) $(CMAKE_EXTRA)

.PHONY: all configure build-folder build test e2e e2e-multiplayer coverage game game-stop fuzz sdk dist bindings deploy clean \
        container container-shell container-test container-sdk container-dist container-fuzz container-e2e help

all: build

# A version or edition goal only names the game version or edition (above).
ifneq ($(VERSION_GOALS)$(EDITION_GOALS),)
.PHONY: $(VERSION_GOALS) $(EDITION_GOALS)
$(VERSION_GOALS) $(EDITION_GOALS):
	@:
endif

help:
	@echo "make build          configure (first time) and build into $(BUILD)/"
	@echo "make test           unit tests (ctest, under Wine)"
	@echo "make e2e            end-to-end test in the real game (needs the game in input/<version>/; Xvfb, about 4 min):"
	@echo "                    tools/e2e-test.sh, then tools/e2e-multiplayer.sh"
	@echo "make e2e-multiplayer  only the two-instance LAN game test (about 1.5 min)"
	@echo "make coverage       line coverage of the tests and e2e tests, by section (tools/coverage.sh; build-cov/,"
	@echo "                    COVERAGE_MIN=90 fails below that)"
	@echo "make game           start the build in the game (tools/game.sh start); make game-stop"
	@echo "make fuzz           libFuzzer targets (tools/fuzz.sh; FUZZ_SECONDS=60)"
	@echo "make sdk            the SDK zip for mod makers ($(BUILD)/tome-sdk-<version>.zip)"
	@echo "make dist           the player download, a release build ($(RELEASE_BUILD)/tome-<version>.zip and .sha256)"
	@echo "make bindings       regenerate the Rust bindings after changing tome.h"
	@echo "make deploy INSTALL_DIR=<game folder>   copy tome.mix there (nothing else writes into a game folder)"
	@echo "make container      build the $(IMAGE) image: the whole toolchain, Wine and Xvfb"
	@echo "make container-test / container-e2e   build and test inside it (build-container/)"
	@echo "make container-sdk  the SDK zip, built inside it"
	@echo "make container-fuzz the fuzz targets, built and run inside it (build-container-fuzz/; FUZZ_SECONDS)"
	@echo "make container-dist the player download, built inside it ($(CONTAINER_RELEASE_BUILD)/)"
	@echo "make clean          remove $(BUILD)/"
	@echo ""
	@echo "Game versions: input/<version>/ (input/README.md); name one after the target,"
	@echo "e.g. make test 1.27b or make e2e 1.26a. Default 1.27b. In input/: $(or $(GAME_VERSIONS),nothing)"
	@echo "Editions: make e2e roc runs Reign of Chaos (war3.exe -classic), tft (default) The Frozen Throne"

$(BUILD)/CMakeCache.txt:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

configure:
	cmake -S . -B $(BUILD) $(CMAKE_FLAGS)

# DEV given (make DEV=..., or the environment) sets it in the build folder;
# otherwise only a folder configured before TOME_DEV existed gets it, once.
DEV_GIVEN = $(filter command line environment,$(origin DEV))
build-folder: $(BUILD)/CMakeCache.txt
	@grep -q$(if $(DEV_GIVEN),ix 'TOME_DEV:BOOL=$(DEV)', '^TOME_DEV:') $(BUILD)/CMakeCache.txt \
	    || cmake -S . -B $(BUILD) -DTOME_DEV=$(DEV)

build: build-folder
	cmake --build $(BUILD)

# In the container without a display (CI, `make container-test`), Wine has
# no driver to make windows with, and tests that need one fail
# (host_background_mute_round_trip): its own prefix gets Wine's null display
# driver. Only there (TOME_CONTAINER, set by the image), never a host prefix.
test: build
	@if [ "$${TOME_CONTAINER:-}" = 1 ] && [ -z "$${DISPLAY:-}$${WAYLAND_DISPLAY:-}" ]; then \
	    wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f >/dev/null 2>&1 \
	        || echo "warning: couldn't select Wine's null display driver"; \
	fi
	cd $(BUILD) && ctest --output-on-failure

# The multiplayer test runs even when the first one failed (both report).
e2e: build
	BUILD="$(abspath $(BUILD))" tools/e2e-test.sh "$(BUILD)"; first=$$?; \
	BUILD="$(abspath $(BUILD))" tools/e2e-multiplayer.sh "$(BUILD)" && exit $$first

e2e-multiplayer: build
	BUILD="$(abspath $(BUILD))" tools/e2e-multiplayer.sh "$(BUILD)"

COVERAGE_MIN ?= 90
coverage:
	COVERAGE_MIN=$(COVERAGE_MIN) tools/coverage.sh

game: build
	BUILD="$(abspath $(BUILD))" tools/game.sh start

game-stop:
	BUILD="$(abspath $(BUILD))" tools/game.sh stop

FUZZ_SECONDS ?= 60
fuzz:
	tools/fuzz.sh $(FUZZ_SECONDS)

sdk: $(BUILD)/CMakeCache.txt
	cmake --build $(BUILD) --target sdk-dist

# A release build of its own, so the developer build in $(BUILD) stays as it is.
# Only what the download needs (tome.mix): not the tests and tools.
dist:
	$(MAKE) build-folder BUILD=$(RELEASE_BUILD) DEV=OFF
	cmake --build $(RELEASE_BUILD) --target dist

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
# container-dist's: a folder of its own, or $(CONTAINER_BUILD) when that's
# already a release build (CI: DEV=OFF), so it isn't built twice.
CONTAINER_RELEASE_BUILD ?= $(CONTAINER_BUILD)-release
HAVE_GAME     = $(if $(GAME_VERSIONS),yes)
# A version folder that's a link (input/README.md) points outside the mounts:
# its folder is also mounted where the link points, so it resolves inside.
game_link_target = $(if $(filter /%,$(1)),$(1),$(abspath /src/input/$(1)))
GAME_LINK_MOUNTS = $(foreach v,$(GAME_VERSIONS),$(if $(shell test -L "input/$(v)" && echo link), \
                   -v "$(realpath input/$(v)):$(call game_link_target,$(shell readlink "input/$(v)")):ro"))
CONTAINER_RUN = $(ENGINE) run --rm -i $(if $(shell test -t 0 && echo yes),-t) \
                --security-opt label=disable -v "$(CURDIR):/src" -w /src -e HOME=/src/$(CONTAINER_BUILD)/home -e TMPDIR=/src/$(CONTAINER_BUILD)/tmp \
                -e XDG_RUNTIME_DIR=/src/$(CONTAINER_BUILD)/run \
                $(if $(filter podman,$(ENGINE)),--userns=keep-id,--user $$(id -u):$$(id -g)) \
                -e GAME_VERSION=$(GAME_VERSION) -e EDITION=$(EDITION) \
                $(if $(HAVE_GAME),-v "$(realpath input):/src/input:ro" $(GAME_LINK_MOUNTS)) $(IMAGE)

container:
	@test -n "$(ENGINE)" || { echo "needs podman or docker"; exit 1; }
	$(ENGINE) build -t $(IMAGE) $(if $(IMAGE_TARGET),--target $(IMAGE_TARGET)) -f tools/container/Containerfile tools/container

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

container-dist: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) make dist RELEASE_BUILD=$(CONTAINER_RELEASE_BUILD)

container-fuzz: $(CONTAINER_IMAGE)
	$(CONTAINER_PREPARE)
	$(CONTAINER_RUN) env FUZZ_BUILD=$(CONTAINER_BUILD)-fuzz tools/fuzz.sh $(FUZZ_SECONDS)
