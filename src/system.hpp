#pragma once
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
using namespace std;

namespace kx {
    struct Telemetry {
        optional<double> temperature;
        bool ac = false;
    };

    string read(const filesystem::path &path);
    void write(const filesystem::path &path, const string &value);
    void saveState(const filesystem::path &path, const string &value);
    vector<filesystem::path> entries(const filesystem::path &path);
    vector<string> words(const string &value);
    double boottime();
    long duration(const string &value);
    Telemetry readTelemetry(const filesystem::path &root);
} // namespace kx
