#include "process.hpp"
#include <chrono>
#include <stdexcept>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace std;

namespace kx {
    Process execute(const vector<string> &args, int timeoutSeconds) {
        if (args.empty() || args[0].empty() || args[0][0] != '/' || timeoutSeconds <= 0) {
            throw runtime_error("Absolute executable and positive timeout required");
        }
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC)) {
            throw runtime_error("pipe failed");
        }
        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            throw runtime_error("fork failed");
        }
        if (pid == 0) {
            setpgid(0, 0);
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd < 0 || dup2(nullfd, STDIN_FILENO) < 0 || dup2(pipefd[1], STDOUT_FILENO) < 0 ||
                dup2(pipefd[1], STDERR_FILENO) < 0) {
                _exit(127);
            }
            close(nullfd);
            close(pipefd[0]);
            close(pipefd[1]);
            vector<char *> argv;
            for (const auto &arg : args) {
                argv.push_back(const_cast<char *>(arg.c_str()));
            }
            argv.push_back(nullptr);
            char path[] = "PATH=/usr/bin:/bin";
            char locale[] = "LC_ALL=C";
            char *env[] = {path, locale, nullptr};
            execve(argv[0], argv.data(), env);
            _exit(127);
        }
        setpgid(pid, pid);
        close(pipefd[1]);
        Process result{0, {}};
        int status = 0;
        bool reaped = false;
        try {
            if (fcntl(pipefd[0], F_SETFL, O_NONBLOCK) < 0) {
                throw runtime_error("Cannot set pipe nonblocking");
            }
            auto end = chrono::steady_clock::now() + chrono::seconds(timeoutSeconds);
            bool eof = false;
            while (!reaped || !eof) {
                char buffer[4096];
                auto count = ::read(pipefd[0], buffer, sizeof(buffer));
                if (count > 0) {
                    result.output.append(buffer, size_t(count));
                    if (result.output.size() > 65536) {
                        throw runtime_error("Command output limit: " + args[0]);
                    }
                } else if (count == 0) {
                    eof = true;
                } else if (errno != EAGAIN && errno != EINTR) {
                    throw runtime_error("Command output read failed");
                }
                if (!reaped) {
                    auto done = waitpid(pid, &status, WNOHANG);
                    reaped = done == pid;
                    if (done < 0 && errno != EINTR) {
                        throw runtime_error("waitpid failed");
                    }
                }
                if (reaped && eof) {
                    break;
                }
                if (chrono::steady_clock::now() >= end) {
                    throw runtime_error("Command timeout: " + args[0]);
                }
                if (count <= 0) {
                    pollfd poller{pipefd[0], POLLIN, 0};
                    poll(&poller, 1, 20);
                }
            }
        } catch (...) {
            kill(-pid, SIGKILL);
            if (!reaped) {
                kill(pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                }
            }
            close(pipefd[0]);
            throw;
        }
        close(pipefd[0]);
        result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        while (!result.output.empty() && (result.output.back() == '\n' || result.output.back() == '\r')) {
            result.output.pop_back();
        }
        return result;
    }

    bool serviceActive(const string &unit) {
        auto result = execute({"/usr/bin/systemctl", "is-active", unit});
        if (result.code == 0) {
            return true;
        }
        if (result.code != 3 && result.code != 4) {
            throw runtime_error("Cannot query systemd service: " + unit);
        }
        return false;
    }
} // namespace kx
