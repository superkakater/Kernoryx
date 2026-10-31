#pragma once
#include "system.hpp"

namespace kx {
    // Driver operations live here; Health owns the session and deadline.
    enum class FanKind { None, Ideapad, Thinkpad };

    class FanBackend {
        filesystem::path root, device, watchdog;
        FanKind kind = FanKind::None;
        bool targetModel = false;
        void verifiedWrite(const filesystem::path &path, const string &value) const;

    public:
        explicit FanBackend(filesystem::path root);
        FanKind type() const;
        string name() const;
        string identity() const;
        string current() const;
        string profile() const;
        string baseline() const;
        string description() const;
        void validate(bool experimental) const;
        void checkManagers() const;
        bool owns() const;
        void boost() const;
        void restore(const string &value) const;
        void refresh() const;
    };
} // namespace kx
