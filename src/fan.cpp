#include "fan.hpp"
#include "process.hpp"
#include <sstream>
using namespace std;

namespace kx {
    FanBackend::FanBackend(filesystem::path path) : root(move(path)) {
        // IdeaPad exposes firmware fan modes outside hwmon.
        for (const auto &entry : entries(root / "bus/platform/drivers/ideapad_acpi")) {
            if (entry.filename().string().starts_with("VPC2004:") && filesystem::exists(entry / "fan_mode")) {
                if (!device.empty()) {
                    throw runtime_error("Multiple IdeaPad fan interfaces; selection is ambiguous");
                }
                device = entry;
                kind = FanKind::Ideapad;
            }
        }
        if (kind == FanKind::Ideapad) {
            try {
                targetModel = read(root / "class/dmi/id/sys_vendor") == "LENOVO" &&
                              read(root / "class/dmi/id/product_name") == "83AL";
            } catch (...) {
            }
            return;
        }
        for (const auto &entry : entries(root / "class/hwmon")) {
            try {
                if (read(entry / "name") == "thinkpad" && filesystem::exists(entry / "pwm1_enable") &&
                    filesystem::exists(entry / "pwm1")) {
                    device = entry;
                    kind = FanKind::Thinkpad;
                    break;
                }
            } catch (...) {
            }
        }
        for (const auto &path :
             {"bus/platform/drivers/thinkpad_hwmon/fan_watchdog", "bus/platform/drivers/thinkpad_acpi/fan_watchdog"}) {
            if (filesystem::exists(root / path)) {
                watchdog = root / path;
                break;
            }
        }
    }

    FanKind FanBackend::type() const {
        return kind;
    }

    string FanBackend::name() const {
        if (kind == FanKind::Ideapad) {
            return "ideapad-laptop";
        }
        if (kind == FanKind::Thinkpad) {
            return "thinkpad-acpi";
        }
        return "unavailable";
    }

    string FanBackend::identity() const {
        if (kind == FanKind::None) {
            return "unavailable";
        }
        // Canonical path identifies the device, independent of hwmon numbering.
        auto result = filesystem::canonical(device).lexically_relative(filesystem::canonical(root)).string();
        if (result.starts_with("../")) {
            throw runtime_error("Fan device escaped sysfs root");
        }
        if (kind == FanKind::Ideapad) {
            result += ":" + read(root / "class/dmi/id/sys_vendor") + ":" + read(root / "class/dmi/id/product_name");
        }
        return result;
    }

    string FanBackend::current() const {
        if (kind == FanKind::Ideapad) {
            return read(device / "fan_mode");
        }
        if (kind == FanKind::Thinkpad) {
            return read(device / "pwm1_enable") + ":" + read(device / "pwm1");
        }
        return "unavailable";
    }

    string FanBackend::profile() const {
        auto path = root / "firmware/acpi/platform_profile";
        return filesystem::exists(path) ? read(path) : "unavailable";
    }

    string FanBackend::baseline() const {
        auto value = current();
        if (kind == FanKind::Ideapad && value == "1") {
            return "1";
        }
        if (kind == FanKind::Thinkpad && value.starts_with("2:")) {
            return "2";
        }
        throw runtime_error("Boost requires IdeaPad Standard mode (1) or ThinkPad automatic mode (2)");
    }

    string FanBackend::description() const {
        ostringstream out;
        out << "Fan backend: " << name() << endl;
        out << "Fan control state: " << current() << endl;
        if (kind == FanKind::Ideapad) {
            out << "Model: " << (targetModel ? "Lenovo 83AL / XiaoXinPro 14 IRH8" : "not enabled for writes") << endl;
            out << "Firmware fan modes: 1 = Standard; 4 = Efficient Thermal Dissipation" << endl;
            out << "Experimental mode selection, not PWM/RPM control; no kernel fan watchdog" << endl;
            out << "Platform profile: " << profile() << endl;
        } else if (kind == FanKind::Thinkpad) {
            out << "Kernel fan watchdog: " << (watchdog.empty() ? "unavailable" : "available") << endl;
        } else {
            out << "No supported write interface; firmware retains cooling control" << endl;
        }
        return out.str();
    }

    void FanBackend::validate(bool experimental) const {
        if (kind == FanKind::None) {
            throw runtime_error("Fan writes unavailable on this machine");
        }
        if (kind == FanKind::Ideapad && (!targetModel || !experimental)) {
            throw runtime_error("IdeaPad writes target Lenovo 83AL and require --experimental; mode behavior needs "
                                "hardware validation");
        }
        if (kind == FanKind::Thinkpad && watchdog.empty()) {
            throw runtime_error("ThinkPad boost requires the kernel fan watchdog");
        }
    }

    void FanBackend::checkManagers() const {
        if (root != "/sys") {
            return;
        }
        for (const auto &unit : {"fancontrol.service", "thinkfan.service", "nbfc.service", "nbfc_service.service"}) {
            if (serviceActive(unit)) {
                throw runtime_error("Competing fan manager: " + string(unit));
            }
        }
    }

    bool FanBackend::owns() const {
        return kind == FanKind::Ideapad ? current() == "4" : kind == FanKind::Thinkpad && current() == "1:255";
    }

    void FanBackend::verifiedWrite(const filesystem::path &path, const string &value) const {
        if (root != "/sys" &&
            !filesystem::canonical(path).string().starts_with(filesystem::canonical(root).string() + "/")) {
            throw runtime_error("Fan control escaped fixture");
        }
        write(path, value);
        if (read(path) != value) {
            throw runtime_error("Fan control readback mismatch: " + path.string());
        }
    }

    void FanBackend::boost() const {
        if (kind == FanKind::Ideapad) {
            verifiedWrite(device / "fan_mode", "4");
        } else if (kind == FanKind::Thinkpad) {
            verifiedWrite(watchdog, "15");
            // Set a normal maximum before enabling manual operation.
            write(device / "pwm1", "255");
            verifiedWrite(device / "pwm1_enable", "1");
            verifiedWrite(device / "pwm1", "255");
        } else {
            throw runtime_error("No fan backend");
        }
    }

    void FanBackend::restore(const string &value) const {
        if (kind == FanKind::Ideapad && value == "1") {
            verifiedWrite(device / "fan_mode", value);
        } else if (kind == FanKind::Thinkpad && value == "2") {
            verifiedWrite(device / "pwm1_enable", value);
        } else {
            throw runtime_error("Invalid fan restoration target");
        }
    }

    void FanBackend::refresh() const {
        if (kind == FanKind::Thinkpad) {
            verifiedWrite(device / "pwm1", "255");
        }
        // IdeaPad mode selection is not a watchdog; do not repeatedly write firmware.
    }
} // namespace kx
