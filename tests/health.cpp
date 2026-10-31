#include "health.hpp"
#include "test.hpp"
using namespace std;
using namespace testing;

static const string fanPath = "bus/platform/drivers/ideapad_acpi/VPC2004:00/fan_mode";
static const string tempPath = "class/hwmon/hwmon0/temp1_input";
static const string profilePath = "firmware/acpi/platform_profile";

static void prepareIdeapad(const Fixture &fixture) {
    fixture.put("class/dmi/id/sys_vendor", "LENOVO");
    fixture.put("class/dmi/id/product_name", "83AL");
    fixture.put("class/dmi/id/product_version", "XiaoXinPro 14 IRH8");
    fixture.put("class/hwmon/hwmon0/name", "coretemp");
    fixture.put(tempPath, "55000");
    fixture.put(profilePath, "performance");
    fixture.put(fanPath, "1");
}

static void ideapadSessions() {
    Fixture fixture;
    prepareIdeapad(fixture);
    auto journal = fixture.base / "fan-rollback";
    kx::Health health(fixture.root, journal);

    check(health.status().find("ideapad-laptop") != string::npos, "IdeaPad backend not discovered");
    check(health.status().find("83AL") != string::npos, "Target model not identified");
    health.command({"fan", "boost", "--dry-run"}, false);
    check(fixture.get(fanPath) == "1" && !filesystem::exists(journal), "Dry-run changed fan state");
    fails(
        [&] {
            health.command({"fan", "boost"}, true);
        },
        "Experimental flag was not required");
    fails(
        [&] {
            health.command({"fan", "boost", "--experimental"}, false);
        },
        "Unprivileged write allowed");
    fails(
        [&] {
            health.command({"fan", "auto"}, true);
        },
        "IdeaPad auto mode was invented");
    fails(
        [&] {
            health.command({"fan", "boost", "--for", "11m", "--experimental"}, true);
        },
        "IdeaPad duration cap missed");

    health.command({"fan", "boost", "--for", "1m", "--experimental"}, true);
    check(fixture.get(fanPath) == "4", "Efficient thermal mode not selected");
    fails(
        [&] {
            health.command({"fan", "boost", "--experimental"}, true);
        },
        "Active lease not protected");
    health.command({"fan", "boost", "--for", "2m", "--replace", "--experimental"}, true);
    health.tick(kx::boottime() + 70);
    check(fixture.get(fanPath) == "4", "Old deadline cancelled replacement");
    health.tick(kx::boottime() + 130);
    check(fixture.get(fanPath) == "1" && !filesystem::exists(journal), "Expiry did not restore Standard");

    health.command({"fan", "boost", "--experimental"}, true);
    fixture.put(tempPath, "86000");
    health.tick(kx::boottime());
    check(fixture.get(fanPath) == "1", "Thermal cutoff did not restore Standard");

    fixture.put(tempPath, "55000");
    health.command({"fan", "boost", "--experimental"}, true);
    filesystem::remove(fixture.root / tempPath);
    health.tick(kx::boottime());
    check(fixture.get(fanPath) == "1", "Sensor loss did not end session");

    fixture.put(tempPath, "55000");
    health.command({"fan", "boost", "--experimental"}, true);
    fixture.put(fanPath, "0");
    health.tick(kx::boottime() + 700);
    check(fixture.get(fanPath) == "0", "Expiry overwrote external fan change");

    fixture.put(fanPath, "1");
    health.command({"fan", "boost", "--experimental"}, true);
    fixture.put(profilePath, "balanced");
    health.tick(kx::boottime());
    check(fixture.get(fanPath) == "4" && !filesystem::exists(journal), "External profile change was overwritten");

    fixture.put(fanPath, "1");
    health.command({"fan", "boost", "--experimental"}, true);
    health.release();
    check(fixture.get(fanPath) == "1", "Clean shutdown did not restore");
}

static void ideapadRecovery() {
    Fixture fixture;
    prepareIdeapad(fixture);
    auto journal = fixture.base / "fan-rollback";
    {
        kx::Health interrupted(fixture.root, journal);
        interrupted.command({"fan", "boost", "--experimental"}, true);
    }
    kx::Health recovered(fixture.root, journal);
    check(fixture.get(fanPath) == "1" && !filesystem::exists(journal), "Crash recovery failed");

    recovered.command({"fan", "boost", "--experimental"}, true);
    fixture.put(fanPath, "2");
    kx::Health conflict(fixture.root, journal);
    check(fixture.get(fanPath) == "2" && filesystem::exists(journal), "Recovery overwrote external mode");
    fails(
        [&] {
            conflict.command({"fan", "boost", "--experimental"}, true);
        },
        "Recovery conflict bypassed");
    conflict.command({"fan", "release"}, true);
    check(fixture.get(fanPath) == "2" && !filesystem::exists(journal), "Release wrote hardware");

    kx::write(journal, "KERNORYX-FAN2\ntruncated");
    kx::Health corrupt(fixture.root, journal);
    check(filesystem::exists(journal), "Malformed journal erased");
    corrupt.command({"fan", "release"}, true);
    fixture.put(fanPath, "1");
    fixture.put("class/dmi/id/product_name", "OTHER");
    kx::Health other(fixture.root, journal);
    fails(
        [&] {
            other.command({"fan", "boost", "--experimental"}, true);
        },
        "Unknown IdeaPad model enabled");
}

static void thinkpadSessions() {
    Fixture fixture;
    auto journal = fixture.base / "fan-rollback";
    const string fan = "class/hwmon/hwmon1/";
    fixture.put("class/hwmon/hwmon0/name", "coretemp");
    fixture.put(tempPath, "55000");
    fixture.put(fan + "name", "thinkpad");
    fixture.put(fan + "pwm1_enable", "2");
    fixture.put(fan + "pwm1", "128");
    fixture.put("bus/platform/drivers/thinkpad_hwmon/fan_watchdog", "0");
    kx::Health health(fixture.root, journal);
    health.command({"fan", "boost", "--for", "1s"}, true);
    check(fixture.get(fan + "pwm1") == "255", "ThinkPad boost failed");
    check(fixture.get("bus/platform/drivers/thinkpad_hwmon/fan_watchdog") == "15", "Watchdog not armed");
    health.tick(kx::boottime() + 2);
    check(fixture.get(fan + "pwm1_enable") == "2", "ThinkPad auto restoration failed");

    health.command({"fan", "boost"}, true);
    kx::Health recovered(fixture.root, journal);
    check(fixture.get(fan + "pwm1_enable") == "2", "ThinkPad crash recovery failed");
}

int main() {
    try {
        ideapadSessions();
        ideapadRecovery();
        thinkpadSessions();
        Fixture unsupported;
        kx::Health health(unsupported.root, unsupported.base / "journal");
        check(health.status().find("unavailable") != string::npos, "Missing backend not reported");
        fails(
            [&] {
                health.command({"fan", "boost"}, true);
            },
            "Unsupported backend accepted writes");
        cout << "Fan/thermal: " << checks << " checks passed" << endl;
        return 0;
    } catch (const exception &error) {
        cerr << "Fan test failed: " << error.what() << endl;
        return 1;
    }
}
