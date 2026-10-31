CMAKE ?= cmake
CTEST ?= ctest
BUILD_DIR ?= build
BUILD_TYPE ?= Debug
PREFIX ?= /usr/local
JOBS ?= 2
.DEFAULT_GOAL := all
configure:
	$(CMAKE) -S . -B "$(BUILD_DIR)" -DCMAKE_BUILD_TYPE="$(BUILD_TYPE)" -DCMAKE_INSTALL_PREFIX="$(PREFIX)"
all: configure
	$(CMAKE) --build "$(BUILD_DIR)" --parallel "$(JOBS)"
test: all
	$(CTEST) --test-dir "$(BUILD_DIR)" --output-on-failure
install:
	@test -f "$(BUILD_DIR)/kernoryx" -a -f "$(BUILD_DIR)/kernoryxd" || { echo "Run make before installing."; exit 1; }
	$(CMAKE) --install "$(BUILD_DIR)"
uninstall:
	$(CMAKE) -P "$(BUILD_DIR)/uninstall.cmake"
clean:
	@test "$(BUILD_DIR)" != "/" -a "$(BUILD_DIR)" != "." -a -n "$(BUILD_DIR)"
	$(CMAKE) -E rm -rf "$(BUILD_DIR)"
help:
	@echo "make                  Configure and build (JOBS=2, BUILD_TYPE=Debug)"
	@echo "make test             Build and run all tests"
	@echo "sudo make install     Install already-built files; no compilation"
	@echo "sudo make uninstall   Remove installed files; preserve runtime state/workflows"
	@echo "make clean            Remove the selected build directory"
	@echo "Overrides: BUILD_DIR=build PREFIX=/usr/local JOBS=4 BUILD_TYPE=Release"
	@echo "Installation does not enable/reload services; see README.md."
.PHONY: configure all test install uninstall clean help
