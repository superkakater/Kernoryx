#include "power.hpp"
#include "process.hpp"
#ifndef KX_POWERPROFILESCTL
#define KX_POWERPROFILESCTL "/usr/bin/powerprofilesctl"
#endif
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <regex>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
using namespace std;

namespace kx {

    // constexpr string to_string(Mode m) {
    //     switch (m) {
    //         case Mode::Eco: return "eco";
    //         case Mode::Balanced: return "balanced";
    //         case Mode::Performance: return "performance";
    //         case Mode::Beast: return "beast";
    //         default: return "unknown";
    //     }
    // }

    static bool has(const string &list, const string &item) {
        const auto w = words(list);
        return find(w.begin(), w.end(), item) != w.end();
    }

    Decision Policy::evaluate(const string &requested, Telemetry t, double now) {
        const bool high = requested == "performance" || requested == "beast";
        if (t.temperature && *t.temperature >= 95) {
            critical = true;
        }
        if (critical) {
            return {"eco", "critical temperature: latched eco; choose a mode after cooling below 80 C"};
        }
        if (!high) {
            return {requested, "requested policy"};
        }
        if (!t.temperature || !t.ac || *t.temperature >= 85) {
            throttled = true;
            coolSince = -1;
            return {"balanced", !t.temperature ? "CPU temperature unavailable"
                                : !t.ac        ? "AC not detected"
                                               : "CPU temperature is at or above 85 C"};
        }
        if (throttled) {
            if (*t.temperature <= 80) {
                if (coolSince < 0) {
                    coolSince = now;
                }
                if (now - coolSince >= 20) {
                    throttled = false;
                    coolSince = -1;
                }
            } else {
                coolSince = -1;
            }
            if (throttled) {
                return {"balanced", "CPU temperature is above 80 C; waiting for 20 seconds of cooling"};
            }
        }
        return {requested, "AC and thermal headroom available"};
    }

    Backend::Backend(filesystem::path path) : root(std::move(path)) {
        if (root == "/sys" && filesystem::exists(KX_POWERPROFILESCTL)) {
            auto probe = execute({KX_POWERPROFILESCTL, "get"});
            delegated = probe.code == 0 &&
                        (probe.output == "power-saver" || probe.output == "balanced" || probe.output == "performance");
        }
    }

    vector<string> Backend::keys() const {
        if (delegated) {
            return {"service/power-profile"};
        }
        vector<string> result;
        auto add = [&](const string &p) {
            if (filesystem::exists(root / p)) {
                result.push_back(p);
            }
        };
        add("firmware/acpi/platform_profile");
        for (const auto &p : entries(root / "devices/system/cpu/cpufreq")) {
            if (!regex_match(p.filename().string(), regex("policy[0-9]+"))) {
                continue;
            }
            if (!filesystem::exists(p / "scaling_driver") || read(p / "scaling_driver") != "intel_pstate") {
                continue;
            }
            if (!filesystem::exists(p / "energy_performance_preference")) {
                continue;
            }
            auto rel = p.lexically_relative(root).string();
            add(rel + "/scaling_governor");
            add(rel + "/energy_performance_preference");
        }
        if (filesystem::exists(root / "devices/system/cpu/intel_pstate/status") &&
            read(root / "devices/system/cpu/intel_pstate/status") == "active") {
            add("devices/system/cpu/intel_pstate/no_turbo");
        }
        return result;
    }

    string Backend::get(const string &key) const {
        if (delegated) {
            if (key != "service/power-profile") {
                throw runtime_error("Invalid service key");
            }
            auto r = execute({KX_POWERPROFILESCTL, "get"});
            if (r.code || (r.output != "power-saver" && r.output != "balanced" && r.output != "performance")) {
                throw runtime_error("Power profile service unavailable: " + r.output);
            }
            return r.output;
        }
        return read(root / key);
    }

    string_map Backend::snapshot() const {
        string_map result;
        for (const auto &k : keys()) {
            result[k] = get(k);
        }
        return result;
    }

    string_map Backend::plan(const string &mode) const {
        if (mode != "eco" && mode != "balanced" && mode != "performance" && mode != "beast") {
            throw runtime_error("Invalid mode: " + mode);
        }
        if (delegated) {
            const string profile = mode == "eco" ? "power-saver" : mode == "balanced" ? "balanced" : "performance";
            auto r = execute({KX_POWERPROFILESCTL, "list"});
            if (r.code || r.output.find(profile + ":") == string::npos) {
                throw runtime_error("System service does not offer profile: " + profile);
            }
            return {{"service/power-profile", profile}};
        }
        string_map result;
        for (const auto &k : keys()) {
            const auto p = root / k;
            if (p.filename() == "platform_profile") {
                const auto available = read(root / "firmware/acpi/platform_profile_choices");
                const vector<string> candidates =
                    mode == "eco"        ? vector<string>{"low-power", "quiet", "balanced"}
                    : mode == "balanced" ? vector<string>{"balanced"}
                                         : vector<string>{"performance", "balanced-performance", "balanced"};
                for (const auto &v : candidates) {
                    if (has(available, v)) {
                        result[k] = v;
                        break;
                    }
                }
            } else if (p.filename() == "scaling_governor") {
                if (!has(read(p.parent_path() / "scaling_available_governors"), "powersave")) {
                    throw runtime_error("CPU scaling governor does not support powersave");
                }
                result[k] = "powersave";
            } else if (p.filename() == "energy_performance_preference") {
                const string v = mode == "eco"           ? "power"
                                 : mode == "balanced"    ? "balance_power"
                                 : mode == "performance" ? "balance_performance"
                                                         : "performance";
                if (!has(read(p.parent_path() / "energy_performance_available_preferences"), v)) {
                    throw runtime_error("Required EPP unavailable: " + v);
                }
                result[k] = v;
            } else if (p.filename() == "no_turbo") {
                result[k] = mode == "eco" ? "1" : "0";
            }
        }
        if (result.empty()) {
            throw runtime_error("No supported power controls; inspect doctor");
        }
        return result;
    }

    static int priority(const string &k) {
        if (k.ends_with("platform_profile")) {
            return 0;
        }
        if (k.ends_with("scaling_governor")) {
            return 1;
        }
        if (k.ends_with("energy_performance_preference")) {
            return 2;
        }
        return 3;
    }

    void Backend::apply(const string_map &target) const {
        if (delegated) {
            for (const auto &[k, v] : target) {
                if (k != "service/power-profile" || (v != "power-saver" && v != "balanced" && v != "performance")) {
                    throw runtime_error("Invalid service profile");
                }
                if (get(k) != v) {
                    auto r = execute({KX_POWERPROFILESCTL, "set", v});
                    if (r.code) {
                        throw runtime_error("System profile change failed: " + r.output);
                    }
                }
                if (get(k) != v) {
                    throw runtime_error("System profile readback mismatch");
                }
            }
            return;
        }
        const auto supported = keys();
        vector<pair<string, string>> sorted(target.begin(), target.end());
        stable_sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
            return priority(a.first) < priority(b.first);
        });
        for (const auto &[k, v] : sorted) {
            if (find(supported.begin(), supported.end(), k) == supported.end()) {
                throw runtime_error("Control disappeared: " + k);
            }
            if (root != "/sys") {
                auto resolved = filesystem::canonical(root / k).string();
                if (!resolved.starts_with(filesystem::canonical(root).string() + "/")) {
                    throw runtime_error("Simulation path escaped fixture");
                }
            }
            if (get(k) != v) {
                write(root / k, v);
            }
            if (get(k) != v) {
                throw runtime_error("Readback mismatch: " + k);
            }
        }
        for (const auto &[k, v] : target) {
            if (get(k) != v) {
                throw runtime_error("Final verification failed: " + k);
            }
        }
    }

    Telemetry Backend::telemetry() const {
        return readTelemetry(root);
    }

    string Backend::describe() const {
        ostringstream s;
        s << "Power backend: "
          << (delegated ? "system power-profiles API via powerprofilesctl" : "direct sysfs fallback") << "\n";
        s << "Supported control values (missing controls are not emulated):\n";
        for (const auto &[k, v] : snapshot()) {
            s << " " << k << " = " << v << '\n';
        }
        for (const auto &mode : {"eco", "balanced", "performance", "beast"}) {
            try {
                auto p = plan(mode);
                s << mode << ": " << p.size() << " controls\n";
            } catch (const exception &e) {
                s << mode << ": unavailable: " << e.what() << "\n";
            }
        }
        s << "CPU backend: active intel_pstate + EPP only; other drivers use platform profile only.\n"
          << "Power backend does not write fan controls; see fan status.\n";
        return s.str();
    }

    Controller::Controller(filesystem::path root, filesystem::path state, bool simulation)
        : backend(move(root)), journal(move(state)), simulated(simulation) {
    }

    string Controller::conflicts() const {
        // simulation is for testing
        if (simulated) {
            return {};
        }
        string result;
        vector<string> active;
        for (const char *unit : {"power-profiles-daemon.service", "tlp.service", "auto-cpufreq.service",
                                 "tuned.service", "tuned-ppd.service", "system76-power.service"}) {
            auto r = execute({"/usr/bin/systemctl", "is-active", unit});
            if (r.code == 0) {
                active.emplace_back(unit);
            } else if (r.code != 3 && r.code != 4) {
                throw runtime_error("Cannot query systemd; refusing hardware ownership");
            }
        }
        bool tunedCompat = find(active.begin(), active.end(), "tuned-ppd.service") != active.end();
        for (const auto &unit : active) {
            if (backend.usesSystemService() &&
                (unit == "power-profiles-daemon.service" ||
                 (tunedCompat && (unit == "tuned.service" || unit == "tuned-ppd.service")))) {
                continue;
            }
            result += unit + " ";
        }
        return result;
    }

    void Controller::persist(const string_map &intended) {
        ostringstream s;
        s << "KERNORYX1\n";
        for (const auto &[k, v] : baseline) {
            auto old = expected.find(k);
            auto next = intended.find(k);
            s << quoted(k) << ' ' << quoted(v) << ' ' << quoted(old == expected.end() ? v : old->second) << ' '
              << quoted(next == intended.end() ? v : next->second) << endl;
        }
        saveState(journal, s.str());
    }

    void Controller::recover() {
        if (!filesystem::exists(journal)) {
            return;
        }
        ifstream in(journal);
        string header;
        getline(in, header);
        if (header != "KERNORYX1") {
            throw runtime_error("Invalid recovery journal");
        }
        string k, v, old, next, line;
        string_map restore;
        while (getline(in, line)) {
            istringstream row(line);
            if (!(row >> quoted(k) >> quoted(v) >> quoted(old) >> quoted(next))) {
                throw runtime_error("Malformed recovery journal");
            }
            row >> ws;
            if (!row.eof() || restore.contains(k)) {
                throw runtime_error("Malformed recovery journal");
            }
            const auto allowed = backend.keys();
            if (find(allowed.begin(), allowed.end(), k) == allowed.end()) {
                throw runtime_error("Recovery control missing: " + k);
            }
            auto actual = backend.get(k);
            if (actual != old && actual != next && actual != v) {
                throw runtime_error("Recovery conflict; journal retained: " + k);
            }
            restore[k] = v;
        }
        if (!in.eof() || restore.empty()) {
            throw runtime_error("Malformed recovery journal");
        }
        if (!conflicts().empty()) {
            throw runtime_error("Competing manager prevents crash recovery; journal retained");
        }
        backend.apply(restore);
        filesystem::remove(journal);
        cerr << "Recovered original settings after interrupted session" << endl;
    }

    void Controller::transition(const string_map &target) {
        const auto before = backend.snapshot();
        persist(target);
        try {
            backend.apply(target);
            expected = backend.snapshot();
            persist(expected);
        } catch (...) {
            try {
                backend.apply(before);
                expected = before;
                persist(expected);
            } catch (const exception &e) {
                cerr << "Rollback incomplete: " << e.what() << endl;
            }
            throw;
        }
    }

    void Controller::checkOwnership() {
        if (baseline.empty()) {
            return;
        }
        if (backend.snapshot() != expected || !conflicts().empty()) {
            requested = effective = "external-change";
            deadline = 0;
            reason = "Hardware changed externally; automatic writes suspended. mode release to relinquish.";
            throw runtime_error(reason);
        }
    }

    void Controller::tick(double now) {
        if (baseline.empty() || requested == "external-change") {
            return;
        }
        checkOwnership();
        if (deadline && now >= deadline) {
            restore();
            return;
        }
        auto decision = policy.evaluate(requested, backend.telemetry(), now);
        if (policy.critical) {
            requested = "eco";
            deadline = 0;
        }
        if (effective != decision.mode) {
            transition(backend.plan(decision.mode));
            effective = decision.mode;
        }
        reason = decision.reason;
    }

    string Controller::set(const string &mode, long seconds, bool replace, bool dry) {
        if (seconds < 0 || seconds > 7200) {
            throw runtime_error("Invalid lease duration");
        }
        backend.plan(mode);
        if (mode == "beast" && !seconds) {
            seconds = 1800;
        }
        if (deadline && !replace) {
            throw runtime_error("Lease active; use --replace to supersede it");
        }
        checkOwnership();
        if (requested == "external-change") {
            throw runtime_error("Use mode release before a new mode");
        }
        const auto competing = conflicts();
        if (!competing.empty()) {
            throw runtime_error("Competing manager active: " + competing);
        }
        const auto telemetry = backend.telemetry();
        if (policy.critical && (!telemetry.temperature || *telemetry.temperature > 80)) {
            throw runtime_error("Critical latch: wait until CPU <= 80 C");
        }
        Policy candidate;
        auto decision = candidate.evaluate(mode, telemetry, boottime());
        auto target = backend.plan(decision.mode);
        ostringstream out;
        out << (dry ? "DRY RUN: " : "APPLIED: ") << mode << " ->" << decision.mode << " (" << decision.reason << ")\n";
        for (const auto &[k, v] : target) {
            out << " " << k << " = " << v << endl;
        }
        if (dry) {
            return out.str();
        }
        const auto before = backend.snapshot();
        const bool first = baseline.empty();
        if (first) {
            baseline = before;
            expected = before;
        }
        try {
            transition(target);
        } catch (...) {
            if (first && backend.snapshot() == before) {
                baseline.clear();
                expected.clear();
                filesystem::remove(journal);
            }
            throw;
        }
        if (seconds && !deadline) {
            priorRequested = requested;
        }
        requested = candidate.critical ? "eco" : mode;
        effective = decision.mode;
        policy = candidate;
        reason = decision.reason;
        deadline = seconds && !candidate.critical ? boottime() + double(seconds) : 0;
        ++generation;
        return out.str();
    }

    void Controller::restore() {
        if (!deadline) {
            throw runtime_error("No active lease");
        }
        checkOwnership();
        if (priorRequested == "unmanaged") {
            release();
            return;
        }
        const auto telemetry = backend.telemetry();
        Policy nextPolicy;
        auto decision = nextPolicy.evaluate(priorRequested, telemetry, boottime());
        auto target = backend.plan(decision.mode);
        transition(target);
        requested = nextPolicy.critical ? "eco" : priorRequested;
        effective = decision.mode;
        policy = nextPolicy;
        reason = "Lease restored: " + decision.reason;
        deadline = 0;
        ++generation;
    }

    void Controller::release() {
        if (baseline.empty()) {
            return;
        }
        if (requested == "external-change" || backend.snapshot() != expected || !conflicts().empty()) {
            reason = "Relinquished without restoring: external manager/change detected";
        } else {
            transition(baseline);
            reason = "Original hardware settings restored";
        }
        baseline.clear();
        expected.clear();
        filesystem::remove(journal);
        requested = effective = "unmanaged";
        deadline = 0;
        policy = {};
        ++generation;
    }

    string Controller::status() const {
        auto t = backend.telemetry();
        ostringstream s;
        s << "Kernoryx 0.3.0" << (simulated ? " [SIMULATION]" : "") << '\n'
          << "Requested: " << requested << "\nEffective policy: " << effective << "\nReason: " << reason
          << "\nAC: " << (t.ac ? "online" : "absent/unknown") << "\nCPU hottest: ";
        if (t.temperature) {
            s << *t.temperature << " C";
        } else {
            s << "unavailable";
        }
        s << "\nLease remaining: " << (deadline ? std::max(0L, long(std::ceil(deadline - boottime()))) : 0)
          << " s\nGeneration: " << generation << "\n";
        for (const auto &[k, v] : backend.snapshot()) {
            s << k << " = " << v << '\n';
        }
        return s.str();
    }
} // namespace kx