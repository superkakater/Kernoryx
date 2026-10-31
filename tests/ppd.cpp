#include "power.hpp"
#include "process.hpp"
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include "test.hpp"
using namespace std;
using namespace testing;

static string profile = "balanced";
static bool available = true, performance = true;
namespace kx {
    Process execute(const vector<string> &args, int) {
        if (args[0] == "/usr/bin/systemctl") {
            return {3, "inactive"};
        }
        if (!available) {
            return {1, "service offline"};
        }
        if (args.at(1) == "get") {
            return {0, profile};
        }
        if (args.at(1) == "list") {
            return {0, performance ? "power-saver:\nbalanced:\nperformance:" : "power-saver:\nbalanced:"};
        }
        if (args.at(1) == "set") {
            profile = args.at(2);
            return {0, ""};
        }
        throw runtime_error("Unexpected subprocess");
    }
} // namespace kx
int main() {
    Fixture fixture;
    filesystem::path base = fixture.base;
    try {
        // This target substitutes the service executable at build time; execute is a fake.
        kx::Backend backend("/sys");
        check(backend.usesSystemService(), "service not selected");
        check(backend.snapshot() == kx::string_map{{"service/power-profile", "balanced"}}, "direct sysfs leaked");
        check(backend.plan("beast").at("service/power-profile") == "performance", "beast mapping");
        backend.apply(backend.plan("eco"));
        check(profile == "power-saver", "eco delegation");
        profile = "balanced";
        kx::Controller controller("/sys", base / "rollback", true);
        controller.set("eco", 1, false, true);
        check(profile == "balanced", "service dry run mutated");
        controller.set("eco", 1, false, false);
        check(profile == "power-saver", "service mutation failed");
        controller.tick(controller.deadline + 1);
        check(profile == "balanced", "service lease restore");
        controller.set("eco", 60, false, false);
        kx::Controller restart("/sys", base / "rollback", true);
        restart.recover();
        check(profile == "balanced", "service crash restore");
        controller.baseline.clear();
        controller.expected.clear();
        controller.deadline = 0;
        controller.requested = "unmanaged";
        controller.set("eco", 60, false, false);
        profile = "performance";
        bool failed = false;
        try {
            controller.tick(kx::boottime());
        } catch (...) {
            failed = true;
        }
        check(failed && controller.requested == "external-change", "external service change missed");
        controller.release();
        check(profile == "performance", "external service change overwritten");
        performance = false;
        failed = false;
        try {
            backend.plan("beast");
        } catch (...) {
            failed = true;
        }
        check(failed, "unsupported service profile accepted");
        available = false;
        failed = false;
        try {
            backend.apply({{"service/power-profile", "balanced"}});
        } catch (...) {
            failed = true;
        }
        check(failed, "service outage silently fell back to sysfs");
        cout << "Delegated power tests passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << e.what() << '\n';
        return 1;
    }
}
