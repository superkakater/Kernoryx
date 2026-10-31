#include "power.hpp"
#include <iostream>
#include <stdexcept>
#include "test.hpp"
using namespace std;
using namespace testing;

int main() {
    kx::Policy policy;
    check(policy.evaluate("beast", {60, true}, 0).mode == "beast", "cool AC boost");
    check(policy.evaluate("beast", {86, true}, 1).mode == "balanced", "hot fallback");
    check(policy.evaluate("beast", {79, true}, 2).mode == "balanced", "start cooldown");
    check(policy.evaluate("beast", {81, true}, 21).mode == "balanced", "reset cooldown");
    check(policy.evaluate("beast", {79, true}, 22).mode == "balanced", "restart cooldown");
    check(policy.evaluate("beast", {79, true}, 41).mode == "balanced", "hold cooldown");
    check(policy.evaluate("beast", {79, true}, 42).mode == "beast", "cooldown recovery");
    check(policy.evaluate("performance", {70, false}, 43).mode == "balanced", "battery fallback");
    check(policy.evaluate("beast", {nullopt, true}, 44).mode == "balanced", "missing temperature");
    check(policy.evaluate("balanced", {95, true}, 45).mode == "eco", "critical fallback");
    check(policy.evaluate("beast", {60, true}, 100).mode == "eco", "critical latch");
    check(kx::duration("2h") == 7200 && kx::duration("30m") == 1800, "duration conversion");
    for (const auto *invalid : {"0s", "-1m", "3h", "30", "2x", "100000000000h", "1m junk"}) {
        bool threw = false;
        try {
            kx::duration(invalid);
        } catch (...) {
            threw = true;
        }
        check(threw, "invalid duration accepted");
    }
    cout << "Policy: " << checks << " assertions passed\n";
}
