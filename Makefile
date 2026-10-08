.DEFAULT_GOAL := all
.PHONY: all configure build test install clean help

BUILD_DIR ?= build
BUILD_TYPE ?= Release
JOBS ?= $(shell nproc)
PREFIX ?= /usr/local
BUILD_TESTING ?= OFF
STUDIO_TARGETS := aurora_studio aurora_studio_runtime aurora_studio_mcp aurora_studio_control_mcp

all: build

configure:
	cmake -S . -B "$(BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" \
		-DBUILD_TESTING="$(BUILD_TESTING)"

build: configure
	cmake --build "$(BUILD_DIR)" --target $(STUDIO_TARGETS) --parallel "$(JOBS)"

test:
	cmake -S . -B "$(BUILD_DIR)" -G Ninja \
		-DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
	cmake --build "$(BUILD_DIR)" --parallel "$(JOBS)"
	ctest --test-dir "$(BUILD_DIR)" --output-on-failure --parallel "$(JOBS)"

install: build
	cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

clean:
	rm -rf "$(BUILD_DIR)"

help:
	@printf '%s\n' 'Aurora Studio targets:' '  make build   Configure and build the Studio launcher' \
		'  make test    Build and run Studio tests' \
		'  make install Install the local build'
