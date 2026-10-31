#pragma once
#include "system.hpp"
#include <map>
#include <string>
#include <vector>
using namespace std;

namespace kx {
    using string_map = map<string, string>;
    struct Decision {
        string mode;
        string reason;
    };

    // Policy here is used to decide how conditions affect the requested mode
    struct Policy {
        bool throttled = false, critical = false;
        double coolSince = -1;
        Decision evaluate(const string &requested, Telemetry t, double now);
    };
    // Backend is used for translating modes into linux controls
    class Backend {
        filesystem::path root;
        bool delegated = false;

    public:
        explicit Backend(filesystem::path root = "/sys");
        bool usesSystemService() const {
            return delegated;
        }
        vector<string> keys() const;
        string_map snapshot() const;
        string_map plan(const string &mode) const;
        Telemetry telemetry() const;
        string describe() const;
        string get(const string &key) const;
        void apply(const string_map &target) const;
    };
    struct Controller {
        Backend backend;
        filesystem::path journal;
        string_map baseline, expected;
        Policy policy;
        string requested = "unmanaged", effective = "unmanaged", reason = "No mode selected";
        string priorRequested = "unmanaged";
        double deadline = 0;
        unsigned long generation = 0;
        bool simulated;
        Controller(filesystem::path root, filesystem::path state, bool simulation);
        void recover();
        void persist(const string_map &intended);
        void transition(const string_map &target);
        void checkOwnership();
        void tick(double now);
        string set(const string &mode, long seconds, bool replace, bool dry);
        void restore();
        void release();
        string status() const;
        string conflicts() const;
    };
} // namespace kx