# MOTO-CTRL build/test/release automation.
#
# Firmware version bump + build + sign, and the fast local test suites
# (host ctest, Node itest, app typecheck/lint/jest). See docs/TESTING.md
# for what each test layer actually proves, and tools/README.md for the
# OTA signing key's own doctrine -- it lives outside this repo and is never
# read from a hardcoded path here, only from MOTO_CTRL_OTA_KEY.
#
# Usage:
#   make build v=0.8.1        bump firmware version to 0.8.1 and build it
#   make build                build whatever version is currently checked in
#   make sign v=0.8.1         sign firmware/build/moto_ctrl_firmware.bin
#   make release v=0.8.1      build + sign in one go
#   make publish v=0.8.1      release, then commit/tag/push and create the
#                              GitHub Release
#   make test                 host ctest + sim itest + app typecheck/lint/jest
#   make test-idf-build       compile-only check against the real ESP-IDF target
#   make clean                remove build directories
#
# The OTA release private key's path is needed by `make sign` / `make
# release` / `make publish`, either way:
#   make sign v=0.8.1 key=/path/outside/this/repo/moto-ctrl-ota.key
#   export MOTO_CTRL_OTA_KEY=/path/outside/this/repo/moto-ctrl-ota.key && make sign v=0.8.1
# key= wins if both are given. Never committed, never hardcoded here -- see
# tools/README.md.
#
# `make publish` additionally needs a GitHub token exported as GH_TOKEN
# (gh's own standard env var -- see `gh auth login --help`'s "Environment
# variables" section), so it can push the tag and create the release
# non-interactively instead of prompting for `gh auth login` each time.
#
# `make publish`'s commit happens LAST, after build+sign have already
# succeeded -- so a build or signing failure never leaves a "bumped the
# version but the release doesn't actually work" commit sitting on main.

SHELL := /bin/bash
.PHONY: help build bump-version require-version require-ota-key require-gh-token sign release publish test test-firmware test-app test-idf-build clean

# Overridable: export IDF_EXPORT=/other/path/export.sh if ESP-IDF isn't at
# the usual ~/esp/esp-idf install.
IDF_EXPORT ?= $(HOME)/esp/esp-idf/export.sh
FIRMWARE_DIR := firmware
BIN := $(FIRMWARE_DIR)/build/moto_ctrl_firmware.bin
BUNDLE_URL_BASE := https://github.com/silasboon/moto-ctrl/releases/download
VERSION_FILES := firmware/components/core/include/mc_status.h firmware/sim/itest/integration.test.mjs

# key=/path/to/key on the command line wins over an exported
# MOTO_CTRL_OTA_KEY if both are given.
OTA_KEY := $(if $(key),$(key),$(MOTO_CTRL_OTA_KEY))

help:
	@echo "make build v=X.Y.Z    bump firmware version and build (needs ESP-IDF)"
	@echo "make build            build the currently checked-in version, no bump"
	@echo "make sign v=X.Y.Z key=/path/to/key     sign the built binary"
	@echo "make release v=X.Y.Z  build + sign"
	@echo "make publish v=X.Y.Z  release, then commit/tag/push and create the GitHub Release"
	@echo "make test             host ctest + sim itest + app typecheck/lint/jest"
	@echo "make test-idf-build   compile-only check against the real ESP-IDF target"
	@echo "make clean            remove build directories"

# `build` gains the bump-version prerequisite only when v= was actually
# passed, so a plain `make build` (no v=) just rebuilds whatever version is
# currently checked in -- useful for iterating without bumping every time.
ifneq ($(strip $(v)),)
build: bump-version
endif
build:
	. $(IDF_EXPORT) && cd $(FIRMWARE_DIR) && idf.py set-target esp32s3 && idf.py build

bump-version:
	@if [ -z "$(v)" ]; then echo "Usage: make bump-version v=X.Y.Z" >&2; exit 1; fi
	python3 tools/bump_version.py $(v)

sign:
	@if [ -z "$(v)" ]; then echo "Usage: make sign v=X.Y.Z key=/path/to/key" >&2; exit 1; fi
	@if [ -z "$(OTA_KEY)" ]; then \
		echo "No signing key -- pass key=/path/to/key or export MOTO_CTRL_OTA_KEY (see tools/README.md)" >&2; \
		exit 1; \
	fi
	@if [ ! -f "$(BIN)" ]; then \
		echo "$(BIN) doesn't exist yet -- run 'make build v=$(v)' first" >&2; \
		exit 1; \
	fi
	python3 tools/sign-firmware.py sign \
	  --input $(BIN) \
	  --key $(OTA_KEY) \
	  --output moto-ctrl-$(v).mcota \
	  --manifest manifest.json \
	  --version $(v) \
	  --bundle-url $(BUNDLE_URL_BASE)/v$(v)/moto-ctrl-$(v).mcota
	@echo ""
	@echo "Ready: moto-ctrl-$(v).mcota + manifest.json"
	@echo "Next: create a GitHub Release tagged v$(v) (not a pre-release/draft) and attach both files."

# All three fail fast, before build/sign do any real (slow) work --
# without this, `make release`/`make publish` missing v=, or the OTA key,
# or a GitHub token, would run a full multi-minute idf.py build (build
# itself doesn't require any of these, see above) only to then fail at
# sign's or publish's own check. release/publish need these; build alone
# doesn't, since it can rebuild whatever's already checked in.
require-version:
	@if [ -z "$(v)" ]; then echo "Usage: v=X.Y.Z required here" >&2; exit 1; fi

require-ota-key:
	@if [ -z "$(OTA_KEY)" ]; then \
		echo "No signing key -- pass key=/path/to/key or export MOTO_CTRL_OTA_KEY (see tools/README.md)" >&2; \
		exit 1; \
	fi

require-gh-token:
	@if [ -z "$(GH_TOKEN)$(GITHUB_TOKEN)" ]; then \
		echo "GH_TOKEN (or GITHUB_TOKEN) is not set -- export a GitHub token first" >&2; \
		exit 1; \
	fi

release: require-version require-ota-key build sign

# `build` only edits the version-bump files, it doesn't commit them -- tag
# first and the tag would point at a commit that doesn't actually contain
# the version it's named after. So: commit that bump if there is one (only
# once release has already succeeded, per the note up top), push main, then
# tag and push the tag, then create the release from it. Commit message and
# release title both say "firmware" explicitly -- this repo also has the
# app, which is versioned/released completely separately (Xcode/TestFlight,
# not this Makefile), so a bare "vX.Y.Z" would be ambiguous about which one
# it is.
publish: require-version require-gh-token release
	@if ! git diff --quiet -- $(VERSION_FILES); then \
		git add $(VERSION_FILES); \
		git commit -m "Firmware release v$(v)"; \
	fi
	git push origin HEAD:main
	git tag v$(v)
	git push origin v$(v)
	gh release create v$(v) moto-ctrl-$(v).mcota manifest.json \
	  --title "Firmware v$(v)" --notes "MOTO-CTRL firmware v$(v)."

test: test-firmware test-app

# Host-only: no ESP-IDF needed, matches CI's sim-build-test job.
test-firmware:
	cmake -S firmware/sim -B firmware/sim/build
	cmake --build firmware/sim/build
	ctest --test-dir firmware/sim/build --output-on-failure
	cd firmware/sim/itest && npm ci && npm test

test-app:
	cd app && npm ci && npm run typecheck && npm run lint && npm test -- --ci

# Separate from `test`: needs ESP-IDF and is slow. Matches CI's
# firmware-build job -- compiles for the real esp32s3 target, doesn't run
# anything.
test-idf-build:
	. $(IDF_EXPORT) && cd $(FIRMWARE_DIR) && idf.py set-target esp32s3 && idf.py build

clean:
	rm -rf firmware/build firmware/sim/build
