#include "system.hpp"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <fcntl.h>
#include <unistd.h>
using namespace std;

namespace kx {
    string read(const filesystem::path &path) {
        ifstream file(path);
        string s;
        if (!file || !getline(file, s)) {
            throw runtime_error("Failed to read from " + path.string());
        }
        while (!s.empty() && (s.back() == '\r' || s.back() == ' ')) {
            s.pop_back();
        }
        return s;
    }

    void write(const filesystem::path &path, const string &value) {
        ofstream file(path);
        if (!file || !(file << value << '\n') || !file.flush()) {
            throw runtime_error("Failed to write to " + path.string());
        }
    }

    vector<string> words(const string &value) {
        istringstream stream(value);
        vector<string> result;
        string word;
        while (stream >> word) {
            result.push_back(word);
        }
        return result;
    }

    double boottime() {
        timespec t;
        if (clock_gettime(CLOCK_BOOTTIME, &t)) {
            throw runtime_error("CLOCK_BOOTTIME failed");
        }
        return double(t.tv_sec) + double(t.tv_nsec) / 1e9;
    }

    long duration(const string &s) {
        if (!regex_match(s, regex("[1-9][0-9]{0,5}[smh]"))) {
            throw runtime_error("Duration must be 1s..2h (s, m, h suffix)");
        }
        long n = stol(s.substr(0, s.size() - 1));
        n *= s.back() == 'h' ? 3600 : s.back() == 'm' ? 60 : 1;

        if (n > 7200) {
            throw runtime_error("Maximum lease is 2h");
        }
        return n;
    }

    vector<filesystem::path> entries(const filesystem::path &p) {
        vector<filesystem::path> result;
        if (filesystem::exists(p)) {
            for (const auto &entry : filesystem::directory_iterator(p)) {
                result.push_back(entry.path());
            }
        }
        sort(result.begin(), result.end());
        return result;
    }

    Telemetry readTelemetry(const filesystem::path &root) {
        Telemetry t;
        for (const auto &p : entries(root / "class/power_supply")) {
            try {
                if (filesystem::exists(p / "online") && read(p / "type") != "Battery" && read(p / "online") == "1") {
                    t.ac = true;
                }
            } catch (...) {
            }
        }
        for (const auto &p : entries(root / "class/hwmon")) {
            try {
                const auto name = read(p / "name");
                if (name != "coretemp" && name != "k10temp") {
                    continue;
                }
                for (const auto &sensor : entries(p)) {
                    if (regex_match(sensor.filename().string(), regex("temp[0-9]+_input"))) {
                        try {
                            const auto raw = read(sensor);
                            size_t used = 0;
                            double v = stod(raw, &used) / 1000.0;
                            if (used == raw.size() && isfinite(v) && v > 0 && v < 150) {
                                t.temperature = max(t.temperature.value_or(v), v);
                            }
                        } catch (...) {
                        }
                    }
                }
            } catch (...) {
            }
        }
        return t;
    }

} // namespace kx

namespace kx {
    // Rename only after the complete new state is on disk.
    void saveState(const filesystem::path &path, const string &value) {
        auto temporary = path.string() + ".tmp";
        int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0) {
            throw runtime_error("Cannot create state file: " + temporary);
        }
        size_t done = 0;
        while (done < value.size()) {
            auto count = ::write(fd, value.data() + done, value.size() - done);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                close(fd);
                throw runtime_error("Cannot write state file");
            }
            done += size_t(count);
        }
        int result = fsync(fd);
        close(fd);
        if (result) {
            throw runtime_error("Cannot sync state file");
        }
        filesystem::rename(temporary, path);
        int dir = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) {
            throw runtime_error("Cannot open state directory");
        }
        result = fsync(dir);
        close(dir);
        if (result) {
            throw runtime_error("Cannot sync state directory");
        }
    }
} // namespace kx
