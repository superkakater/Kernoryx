#include "power.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include "test.hpp"
using namespace std;
using namespace testing;

int main() {
    Fixture fixture;
    filesystem::path base = fixture.base;
    filesystem::path root = fixture.root;
    filesystem::path journal = base / "rollback";
    auto put = [&](const string &path, const string &value) {
        fixture.put(path, value);
    };
    const string profile = "firmware/acpi/platform_profile", cpu = "devices/system/cpu/cpufreq/policy0/",
                 temp = "class/hwmon/hwmon0/temp1_input", ac = "class/power_supply/AC/online";
    try {
        put(profile, "balanced");
        put("firmware/acpi/platform_profile_choices", "low-power balanced performance");
        put(cpu + "scaling_driver", "intel_pstate");
        put(cpu + "scaling_governor", "powersave");
        put(cpu + "scaling_available_governors", "powersave performance");
        put(cpu + "energy_performance_preference", "balance_power");
        put(cpu + "energy_performance_available_preferences", "power balance_power balance_performance performance");
        put("devices/system/cpu/intel_pstate/status", "active");
        put("devices/system/cpu/intel_pstate/no_turbo", "0");
        put("class/hwmon/hwmon0/name", "coretemp");
        put(temp, "55000");
        put("class/power_supply/AC/type", "Mains");
        put(ac, "1");
        kx::Controller controller(root, journal, true);
        auto original = controller.backend.snapshot();
        controller.set("beast", 30, false, true);
        check(controller.backend.snapshot() == original && !filesystem::exists(journal), "dry run mutates");
        controller.set("beast", 2, false, false);
        check(controller.backend.get(cpu + "energy_performance_preference") == "performance", "beast EPP");
        fails(
            [&] {
                controller.set("eco", 0, false, false);
            },
            "active lease not protected");
        controller.tick(controller.deadline + 1);
        check(controller.requested == "unmanaged" && controller.backend.snapshot() == original, "expiry restoration");
        controller.set("balanced", 0, false, false);
        controller.set("beast", 10, false, false);
        double oldDeadline = controller.deadline;
        controller.set("eco", 0, true, false);
        controller.tick(oldDeadline + 1);
        check(controller.requested == "eco" && controller.backend.get(profile) == "low-power", "stale lease overwrite");
        controller.set("balanced", 0, false, false);
        controller.set("beast", 10, false, false);
        controller.set("performance", 20, true, false);
        controller.restore();
        check(controller.requested == "balanced", "replacement return target");
        controller.set("beast", 30, false, false);
        put(temp, "88000");
        controller.tick(kx::boottime());
        check(controller.effective == "balanced", "hot fallback");
        put(temp, "96000");
        controller.tick(kx::boottime());
        check(controller.requested == "eco" && controller.deadline == 0 && controller.policy.critical,
              "critical latch/cancel");
        fails(
            [&] {
                controller.set("beast", 0, false, false);
            },
            "hot latch bypass");
        put(temp, "55000");
        controller.set("performance", 0, false, false);
        put(ac, "0");
        controller.tick(kx::boottime());
        check(controller.effective == "balanced", "AC fallback");
        controller.set("eco", 5, false, false);
        put(ac, "1");
        controller.restore();
        check(controller.effective == "performance" &&
                  controller.backend.get(cpu + "energy_performance_preference") == "balance_performance",
              "restore must re-resolve previously throttled mode");
        filesystem::remove(root / temp);
        controller.tick(kx::boottime());
        check(controller.effective == "balanced", "missing sensor fallback");
        put(temp, "55000");
        controller.release();
        check(controller.backend.snapshot() == original, "release snapshot");
        controller.set("beast", 30, false, false);
        put(profile, "low-power");
        fails(
            [&] {
                controller.tick(kx::boottime());
            },
            "external write unnoticed");
        check(controller.requested == "external-change", "external state");
        controller.release();
        check(controller.backend.get(profile) == "low-power", "external write overwritten");
        controller.backend.apply(original);
        controller.set("beast", 30, false, false);
        kx::Controller restarted(root, journal, true);
        restarted.recover();
        check(restarted.backend.snapshot() == original && !filesystem::exists(journal), "crash recovery");
        restarted.set("beast", 30, false, false);
        put(profile, "low-power");
        kx::Controller conflict(root, journal, true);
        fails(
            [&] {
                conflict.recover();
            },
            "recovery conflict accepted");
        check(filesystem::exists(journal), "conflict journal erased");
        kx::write(
            journal,
            "KERNORYX1\n\"firmware/acpi/platform_profile\" \"balanced\" \"low-power\" \"low-power\"\n\"truncated");
        fails(
            [&] {
                conflict.recover();
            },
            "truncated journal accepted");
        check(conflict.backend.get(profile) == "low-power", "truncated recovery mutated");
        filesystem::remove(journal);
        conflict.backend.apply(original);
        conflict.set("eco", 0, false, false);
        filesystem::remove(root / (cpu + "energy_performance_preference"));
        fails(
            [&] {
                conflict.tick(kx::boottime());
            },
            "topology drift missed");
        check(conflict.requested == "external-change", "topology should suspend");
        cout << "Controller: " << checks << " assertions passed\n";
        return 0;
    } catch (const exception &e) {
        cerr << "Controller test failed: " << e.what() << '\n';
        return 1;
    }
}
