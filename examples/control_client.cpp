#include <iostream>
#include <rime_ime/control.hpp>

int main() {
    try {
        rime_ime::ControlClient control;
        std::cout << control.status().dump() << '\n';
        return 0;
    } catch (const rime_ime::ControlError &error) {
        std::cerr << error.code() << ": " << error.what() << '\n';
        return 1;
    }
}
