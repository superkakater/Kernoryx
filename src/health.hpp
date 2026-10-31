#pragma once
#include "fan.hpp"

namespace kx {
    class Health {
        filesystem::path root, journal;
        FanBackend fan;
        double deadline = 0;
        unsigned long generation = 0;
        string original, expectedProfile, deviceId;
        string message = "No fan override requested";
        bool active = false;

        bool unsafe() const;
        bool owns() const;
        void persist();
        void recover();
        void restore();
        void relinquish(const string &reason);

    public:
        Health(filesystem::path root, filesystem::path journal);
        string status() const;
        string command(const vector<string> &args, bool authorized);
        void tick(double now);
        void release();
    };
} // namespace kx
