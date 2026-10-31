#pragma once
#include "system.hpp"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
using namespace std;

namespace testing {
    inline int checks = 0;

    inline void check(bool condition, const string &message) {
        ++checks;
        if (!condition) {
            throw runtime_error(message);
        }
    }

    inline void fails(const function<void()> &action, const string &message) {
        bool failed = false;
        try {
            action();
        } catch (const exception &) {
            failed = true;
        }
        check(failed, message);
    }

    class Fixture {
    public:
        filesystem::path base, root;

        Fixture() {
            char pattern[] = "/tmp/kernoryx-test-XXXXXX";
            auto directory = mkdtemp(pattern);
            if (!directory) {
                throw runtime_error("Cannot create test fixture");
            }
            base = directory;
            root = base / "sys";
            filesystem::create_directories(root);
        }

        ~Fixture() {
            error_code error;
            filesystem::remove_all(base, error);
        }

        void put(const string &path, const string &value) const {
            filesystem::create_directories((root / path).parent_path());
            kx::write(root / path, value);
        }

        string get(const string &path) const {
            return kx::read(root / path);
        }
    };
} // namespace testing
