#include "health.hpp"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
using namespace std;

namespace kx {
    Health::Health(filesystem::path path, filesystem::path state) : root(move(path)), journal(move(state)), fan(root) {
        try {
            recover();
        } catch (const exception &error) {
            message = "Fan recovery blocked: " + string(error.what()) +
                      "; journal retained. Inspect hardware, then fan restore or fan release";
        }
    }

    bool Health::unsafe() const {
        auto telemetry = readTelemetry(root);
        if (!telemetry.temperature || *telemetry.temperature >= 85) {
            return true;
        }
        for (const auto &device : entries(root / "class/hwmon")) {
            for (const auto &sensor : entries(device)) {
                auto key = sensor.filename().string();
                if (!regex_match(key, regex("temp[0-9]+_crit"))) {
                    continue;
                }
                auto input = device / (key.substr(0, key.size() - 5) + "_input");
                try {
                    double limit = stod(read(sensor));
                    double value = stod(read(input));
                    if (!isfinite(limit) || !isfinite(value) || limit <= 0 || value >= limit) {
                        return true;
                    }
                } catch (...) {
                    return true;
                }
            }
        }
        return false;
    }

    bool Health::owns() const {
        return fan.identity() == deviceId && fan.owns() &&
               (fan.type() != FanKind::Ideapad || fan.profile() == expectedProfile);
    }

    void Health::persist() {
        ostringstream data;
        data << "KERNORYX-FAN2" << endl
             << quoted(fan.name()) << " " << quoted(deviceId) << " " << quoted(original) << " "
             << quoted(expectedProfile) << endl;
        saveState(journal, data.str());
    }

    void Health::recover() {
        if (!filesystem::exists(journal)) {
            return;
        }
        ifstream file(journal);
        string header, backend;
        getline(file, header);
        if (header != "KERNORYX-FAN2" ||
            !(file >> quoted(backend) >> quoted(deviceId) >> quoted(original) >> quoted(expectedProfile))) {
            throw runtime_error("Invalid fan recovery journal");
        }
        file >> ws;
        if (!file.eof() || backend != fan.name() || deviceId != fan.identity()) {
            throw runtime_error("Fan recovery device mismatch");
        }
        if ((fan.type() == FanKind::Ideapad && original != "1") ||
            (fan.type() == FanKind::Thinkpad && original != "2") || fan.type() == FanKind::None) {
            throw runtime_error("Invalid fan recovery target");
        }
        // A successful restoration may have occurred just before the daemon died.
        bool restored = fan.type() == FanKind::Ideapad ? fan.current() == original : fan.current().starts_with("2:");
        if (restored) {
            relinquish("Fan was already restored");
            return;
        }
        if (!owns()) {
            throw runtime_error("Fan/profile changed outside Kernoryx");
        }
        fan.checkManagers();
        fan.restore(original);
        relinquish("Recovered interrupted fan session");
    }

    void Health::relinquish(const string &reason) {
        active = false;
        deadline = 0;
        ++generation;
        message = reason;
        filesystem::remove(journal);
    }

    void Health::restore() {
        if (!active) {
            if (filesystem::exists(journal)) {
                recover();
                return;
            }
            throw runtime_error("No active fan session");
        }
        // Check ownership before expiry/thermal restoration, not afterwards.
        if (!owns()) {
            relinquish("Fan or platform profile changed externally; no restoration written");
            return;
        }
        fan.checkManagers();
        fan.restore(original);
        relinquish(fan.type() == FanKind::Ideapad ? "Restored IdeaPad Standard mode (1)"
                                                  : "Restored automatic firmware cooling");
    }

    void Health::release() {
        if (active) {
            restore();
        }
    }

    string Health::status() const {
        ostringstream out;
        out << fan.description();
        out << "Fan state: " << message << endl;
        out << "Fan generation: " << generation << endl;
        out << "Fan lease remaining: " << (active ? max(0L, long(ceil(deadline - boottime()))) : 0) << " s" << endl;
        bool any = false;
        for (const auto &device : entries(root / "class/hwmon")) {
            try {
                auto name = read(device / "name");
                for (const auto &sensor : entries(device)) {
                    auto key = sensor.filename().string();
                    if (regex_match(key, regex("temp[0-9]+_input"))) {
                        double temperature = stod(read(sensor)) / 1000;
                        if (!isfinite(temperature) || temperature < -20 || temperature > 150) {
                            continue;
                        }
                        any = true;
                        out << name << "/" << key << ": " << temperature << " C";
                        auto critical = device / (key.substr(0, key.size() - 6) + "_crit");
                        if (filesystem::exists(critical)) {
                            double limit = stod(read(critical)) / 1000;
                            out << " (critical " << limit << " C)";
                            if (temperature >= limit) {
                                out << " CRITICAL";
                            }
                        } else if (temperature >= 85) {
                            out << " HOT";
                        }
                        out << endl;
                    } else if (regex_match(key, regex("fan[0-9]+_input"))) {
                        out << name << "/" << key << ": " << read(sensor) << " RPM" << endl;
                    }
                }
            } catch (const exception &error) {
                out << "Sensor unavailable: " << error.what() << endl;
            }
        }
        if (!any) {
            out << "No readable temperature sensors" << endl;
        }
        return out.str();
    }

    string Health::command(const vector<string> &args, bool authorized) {
        if (args == vector<string>{"fan", "status"} || args == vector<string>{"thermal", "status"}) {
            return status();
        }
        if (args == vector<string>{"fan", "restore"} || args == vector<string>{"fan", "release"} ||
            args == vector<string>{"fan", "auto"}) {
            if (!authorized) {
                throw runtime_error("Fan changes require sudo");
            }
            if (args[1] == "release") {
                relinquish("Fan ownership released without a hardware write");
            } else if (args[1] == "auto") {
                if (fan.type() != FanKind::Thinkpad) {
                    throw runtime_error("IdeaPad has no documented auto mode; use fan restore for an active session");
                }
                fan.checkManagers();
                fan.restore("2");
                relinquish("Automatic firmware cooling requested");
            } else {
                restore();
            }
            return status();
        }
        if (args.size() < 2 || args[0] != "fan" || args[1] != "boost") {
            throw runtime_error("Use fan status | boost | restore | release | auto; see --help");
        }
        long seconds = 60;
        bool replace = false, dry = false, experimental = false, seenFor = false;
        for (size_t i = 2; i < args.size(); i++) {
            if (args[i] == "--for" && !seenFor && i + 1 < args.size()) {
                seconds = duration(args[++i]);
                seenFor = true;
            } else if (args[i] == "--replace" && !replace) {
                replace = true;
            } else if (args[i] == "--dry-run" && !dry) {
                dry = true;
            } else if (args[i] == "--experimental" && !experimental) {
                experimental = true;
            } else {
                throw runtime_error("Unknown or duplicate fan option");
            }
        }
        if (!authorized && !dry) {
            throw runtime_error("Fan changes require sudo");
        }
        fan.validate(experimental || dry);
        if (fan.type() == FanKind::Ideapad && seconds > 600) {
            throw runtime_error("Experimental IdeaPad sessions are limited to 10 minutes");
        }
        if (!active && filesystem::exists(journal)) {
            throw runtime_error("Unresolved fan journal; use fan restore or fan release after inspection");
        }
        if (active && !owns()) {
            if (!dry) {
                relinquish("External fan/profile change; no further writes");
            }
            throw runtime_error("Fan ownership changed; inspect status before retrying");
        }
        if (active && !replace) {
            throw runtime_error("Fan session active; use --replace");
        }
        auto before = active ? original : fan.baseline();
        fan.checkManagers();
        if (unsafe()) {
            throw runtime_error("Fan boost requires readable sensors below critical limits and CPU below 85 C");
        }
        if (dry) {
            return "DRY RUN: " + fan.name() + " temporary boost for " + to_string(seconds) + " s; return target " +
                   before + "\n" +
                   (fan.type() == FanKind::Ideapad
                        ? "Firmware mode 4; hardware behavior unverified; writes require --experimental\n"
                        : "");
        }
        if (!active) {
            original = before;
            expectedProfile = fan.profile();
            deviceId = fan.identity();
            persist();
            try {
                fan.boost();
            } catch (...) {
                message = "Fan write failed; recovery journal retained";
                try {
                    // Restore only a recognized state still owned by this session.
                    if (owns()) {
                        fan.restore(original);
                        relinquish("Failed fan transition rolled back");
                    }
                } catch (...) {
                }
                throw;
            }
        }
        deadline = boottime() + seconds;
        active = true;
        ++generation;
        message = fan.type() == FanKind::Ideapad ? "Experimental firmware mode 4; restoration depends on daemon/restart"
                                                 : "Normal maximum fan level; kernel watchdog 15 s";
        return status();
    }

    void Health::tick(double now) {
        if (!active) {
            return;
        }
        if (!owns()) {
            relinquish("External fan/profile change detected; writes suspended");
            return;
        }
        if (now >= deadline || unsafe()) {
            restore();
            return;
        }
        fan.refresh();
    }
} // namespace kx
