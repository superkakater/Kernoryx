#include "process.hpp"
#include "test.hpp"
using namespace std;
using namespace testing;

int main() {
    try {
        auto result = kx::execute({"/usr/bin/printf", "%s", "hello; $HOME"});
        check(result.code == 0 && result.output == "hello; $HOME", "Arguments were not passed literally");
        check(kx::execute({"/usr/bin/false"}).code != 0, "Exit status lost");
        check(kx::execute({"/does/not/exist"}).code == 127, "Missing executable not reported");
        fails(
            [] {
                kx::execute({"relative"});
            },
            "Relative executable accepted");
        fails(
            [] {
                kx::execute({"/usr/bin/sleep", "3"}, 1);
            },
            "Timeout was not enforced");
        fails(
            [] {
                kx::execute({"/usr/bin/yes"}, 2);
            },
            "Output limit was not enforced");
        cout << "Process: " << checks << " checks passed" << endl;
        return 0;
    } catch (const exception &error) {
        cerr << "Process test failed: " << error.what() << endl;
        return 1;
    }
}
