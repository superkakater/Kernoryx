CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude
PREFIX ?= /usr/local
all: build/kernoryx build/kernoryxd
build:
	mkdir -p build
build/kernoryx: src/main.cpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@
build/kernoryxd: src/daemon.cpp src/sysfs.cpp src/sysfs.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/daemon.cpp src/sysfs.cpp -o $@
build/policy-test: tests/policy.cpp src/sysfs.cpp src/sysfs.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/policy.cpp src/sysfs.cpp -o $@
build/controller-test: tests/controller.cpp src/power.cpp src/sysfs.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/controller.cpp src/sysfs.cpp -o $@
test: all build/policy-test build/controller-test
	./build/policy-test
	./build/controller-test
	python3 tests/integration.py
install: all
	install -Dm755 build/kernoryx $(DESTDIR)$(PREFIX)/bin/kernoryx
	install -Dm755 build/kernoryxd $(DESTDIR)$(PREFIX)/sbin/kernoryxd
	install -d $(DESTDIR)/etc/systemd/system
	sed 's|/usr/local|$(PREFIX)|g' packaging/kernoryxd.service > $(DESTDIR)/etc/systemd/system/kernoryxd.service
clean:
	rm -rf build
.PHONY: all test install clean
