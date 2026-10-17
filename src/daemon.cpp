#include "sysfs.hpp"
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
using namespace std;

static volatile __sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }
static string dispatch(kx::Controller &c, const string &line, bool authorized) {
    const auto args = kx::words(line);
    if (args == vector<string>{"status"} || args == vector<string>{"mode", "status"}) {
        return c.status();
    }
    if (args == vector<string>{"doctor"}) {
        return c.status() + c.backend.describe() + "Competing services: " + c.conflicts() + "\n";
    }
    if (args == vector<string>{"mode", "list"}) {
        string out;
        for (const auto &m : {"eco", "balanced", "performance", "beast"}) {
            out += string(m) + ":\n";
            try {
                for (const auto &[k, v] : c.backend.plan(m)) {
                    out += " " + k + " = " + v + "\n";
                }
            } catch (const exception &e) {
                out += string(" unavailable: ") + e.what() + "\n";
            }
        }
        return out;
    }
    if (args.size() >= 3 && args[0] == "mode" && args[1] == "set") {
        bool replace = false, dry = false, seenFor = false;
        long seconds = 0;
        for (size_t i = 3; i < args.size(); i++) {
            if (args[i] == "--replace" && !replace) {
                replace = true;
            } else if (args[i] == "--dry-run" && !dry) {
                dry = true;
            } else if (args[i] == "--for" && !seenFor && i + 1 < args.size()) {
                seconds = kx::duration(args[++i]);
                seenFor = true;
            } else {
                throw runtime_error("Unknown/duplicate option: " + args[i]);
            }
        }
        if (!authorized && !dry) {
            throw runtime_error("Changing modes requires sudo");
        }
        return c.set(args[2], seconds, replace, dry);
    }
    if (!authorized) {
        throw runtime_error("Changing modes requires sudo");
    }
    if (args == vector<string>{"mode", "restore"}) {
        c.restore();
        return c.status();
    }
    if (args == vector<string>{"mode", "release"}) {
        c.release();
        return c.status();
    }
    throw runtime_error("Unknown command; run kernoryx --help");
}

int main(int argc, char **argv) {
    try {
        bool simulated = false;
        filesystem::path root = "/sys", run = "run/kernoryx";
        if (argc == 5 && string(argv[1]) == "--simulate" && string(argv[3]) == "--runtime") {
            simulated = true;
            root = filesystem::canonical(argv[2]);
            run = filesystem::absolute(argv[4]);
            if (root == "/" || root.string().starts_with("/") || root.string().starts_with("/proc") || root.string().starts_with("/dev")) {
                throw runtime_error("Simulation needs a regular fixture directory");
            }
            for (auto const &e : filesystem::recursive_directory_iterator(root)) {
                if (e.is_symlink()) {
                    throw runtime_error("Simulation fixture must not contain symlinks");
                }
            }
        } else if (argc != 1) {
            throw runtime_error("Usage: kernoryxd [--simulate FIXTURE --runtime DIRECTORY]");
        }
        if (!simulated && geteuid() != 0) {
            throw runtime_error("Real daemon must run as root");
        }
        umask(0077);
        if (!filesystem::exists(run)) {
            filesystem::create_directory(run);
        }
        struct stat st{};
        if (lstat(run.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022)) {
            throw runtime_error("Runtime directory must be owned by daemon user and not group/world-writable");
        }
        chmod(run.c_str(), simulated ? 0700 : 0755);
        int lock = open((run / "lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
            throw runtime_error("Another daemon holds the runtime lock");
        }
        kx::Controller controller(root, run / "rollback", simulated);
        controller.recover();
        int server = socket(AF_UNIX, SOCK_SEQPACKET |  SOCK_CLOEXEC, 0);
        if (server < 0) {
            throw runtime_error(string("socket failed: ") + strerror(errno));
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const auto path = (run / "control.sock").string();
        if (path.size() >= sizeof(addr.sun_path)) {
            throw runtime_error("Socket path too long");
        }
        strcpy(addr.sun_path, path.c_str());
        unlink(path.c_str());
        if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) || listen(server, 16)) {
            throw runtime_error("Socket path too long");
        }
        chmod(path.c_str(), simulated ? 0600 : 0666);
        signal(SIGINT, stop);
        signal(SIGTERM, stop);
        signal(SIGPIPE, SIG_IGN);
        cerr << "Kernoryx daemon ready: " << path << endl;
        double nextTick = 0;
        string lastError;
        while (!stopping) {
            const auto now = kx::boottime();
            if (now >= nextTick) {
                try {
                    controller.tick(now);
                    lastError.clear();
                } catch (const exception &e) {
                    if (lastError != e.what()) {
                        cerr << "Policy error: " << e.what() << endl;
                    }
                    lastError = e.what();
                    controller.reason = "Policy error: " + lastError;
                }
                nextTick = now + 1;
            }
            pollfd p{server, POLLIN, 0};
            int result = poll(&p, 1, 100);
            if (result < 0 && errno != EINTR) {
                throw runtime_error("poll failed");
            }
            if (result < 0 || !(p.revents & POLLIN)) {
                continue;
            }
            int client = accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
            if (client < 0) {
                continue;
            }
            timeval timeout{0, 100000};
            setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
            ucred cred{};
            socklen_t len = sizeof(cred);
            if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &cred, &len)) {
                close(client);
                continue;
            }
            char buffer[2048];
            auto n = recv(client, buffer, sizeof(buffer), MSG_TRUNC);
            string response;
            try {
                if (n <= 0 || n >= static_cast<ssize_t>(sizeof(buffer))) {
                    throw runtime_error("Invalid request size");
                }
                const string request(buffer, size_t(n));
                for (unsigned char b : request) {
                    if (b < 32 || b > 126) {
                        throw runtime_error("Invalid request character");
                    }
                }
                response = "OK\n" + dispatch(controller, request, cred.uid == 0 || (simulated && cred.uid == geteuid()));
            } catch (const exception &e) {
                response = string("ERR\n") + e.what() + '\n';
            }
            if (response.size() > 120000) {
                response = "ERR\nResponse too large\n";
            }
            send(client, response.data(), response.size(), MSG_NOSIGNAL);
            close(client);
        }
        controller.release();
        close(server);
        unlink(path.c_str());
        close(lock);
        return 0;
    } catch (const exception &e) {
        cerr << "kernoryxd: " << e.what() << endl;
        return 1;
    }
}