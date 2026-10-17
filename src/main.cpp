#include <iostream>
#include <stdexcept>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
using namespace std;

int main(int argc, char** argv) {
    try {
        if (argc == 1 || (argc == 2 && string(argv[1]) == "--help")) {
            cout << "Kernoryx power module" << endl
                << "    kernoryx status | doctor | mode status | modes list" << endl
                << "    kernoryx mode set eco | balanced | performance | beast [--for 30m] [--replace] [--dry-run]" << endl
                << "    kernoryx mode restore       Restore the active lease's prior mode" << endl
                << "    kernoryx mode release       Restore startup snapshot and relinquish ownership" << endl
                << "    kernoryx --version" << endl
                << "    kernoryx --socket PATH COMMAND      Connect to simulation socket" << endl
                << "Beast defaults to 30m; all leases are at most 2h. Real mutations require sudo." << endl;
            return 0;
        }
        if (argc == 2 && string(argv[1]) == "--version") {
            cout << "Kernoryx 0.1.0" << endl;
            return 0;
        }
        string path = "/run/kernoryx/control.sock";
        string request;
        int begin = 1;
        if (argc >= 4 && string(argv[1]) == "--socket") {
            path = argv[2];
            begin = 3;
        }
        for (int i = begin; i < argc; i++) {
            if (!request.empty()) {
                request += ' ';
            }
            request += argv[i];
        }
        if (request.empty() || request.size() >= 2048) {
            throw runtime_error("Invalid command length");
        }
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            throw runtime_error("socket failed");
        }
        timeval timeout{15, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (path.size() >= sizeof(addr.sun_path)) {
            throw runtime_error("Socket path too long");
        }
        strcpy(addr.sun_path, path.c_str());
        if (connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr))) {
            throw runtime_error("Cannot reach kernoryxd; start the service or check --socket");
        }
        ucred cred{};
        socklen_t length = sizeof(cred);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &length) || (path == "/run/kernoryx/control.sock" && cred.uid != 0)) {
            throw runtime_error("Untrusted daemon credentials");
        }
        if (send(fd, request.data(), request.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(request.size())) {
            throw runtime_error("Request failed");
        }
        char response[131072];
        auto n = recv(fd, response, sizeof(response), MSG_TRUNC);
        close(fd);
        if (n <= 0 || n >= static_cast<ssize_t>(sizeof(response))) {
            throw runtime_error("Daemon response failed; inspect status before retrying");
        }
        string result(response, size_t(n));
        bool ok = result.starts_with("OK\n");
        (ok ? cout : cerr) << result.substr(result.find('\n') + 1);
        return ok ? 0 : 1;
    } catch (const exception &e) {
        cerr << "kernoryx: " << e.what() << '\n';
        return 1;
    }
}