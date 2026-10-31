#pragma once
#include <string>
#include <vector>
using namespace std;

namespace kx {
    struct Process {
        int code;
        string output;
    };
    bool serviceActive(const string &unit);
    Process execute(const vector<string> &args, int timeoutSeconds = 3);
} // namespace kx