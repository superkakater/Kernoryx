#include "sysfs.hpp"
#include <iostream>
#include <stdexcept>
static int checks = 0;
static void require(bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); }
int main() {
    kx::Policy p;
    require(p.evaluate("beast", {60, true}, 0).mode == "beast", "cool AC boost");
    require(p.evaluate("beast", {86, true}, 1).mode == "balanced", "hot fallback");
    require(p.evaluate("beast", {79, true}, 2).mode == "balanced", "start cooldown");
    require(p.evaluate("beast", {81, true}, 21).mode == "balanced", "reset cooldown");
    require(p.evaluate("beast", {79, true}, 22).mode == "balanced", "restart cooldown");
    require(p.evaluate("beast", {79, true}, 41).mode == "balanced", "hold cooldown");
    require(p.evaluate("beast", {79, true}, 42).mode == "beast", "cooldown recovery");
    require(p.evaluate("performance", {70, false}, 43).mode == "balanced", "battery fallback");
    require(p.evaluate("beast", {std::nullopt, true}, 44).mode == "balanced", "missing temperature");
    require(p.evaluate("balanced", {95, true}, 45).mode == "eco", "critical fallback");
    require(p.evaluate("beast", {60, true}, 100).mode == "eco", "critical latch");
    require(kx::duration("2h") == 7200 && kx::duration("30m") == 1800, "duration conversion");
    for (const auto* invalid : {"0s", "-1m", "3h", "30", "2x", "100000000000h", "1m junk"}) {
        bool threw = false; try { kx::duration(invalid); } catch (...) { threw = true; }
        require(threw, "invalid duration accepted");
    }
    std::cout << "Policy: " << checks << " assertions passed\n";
}
