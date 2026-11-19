#include "mac/cursor_source.hpp"

#include <iostream>
#include <stdexcept>

namespace {
void expect(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        CursorRoute path(0);
        expect(!path.ready(0, false) && !path.initial_timeout(999), "First packet cannot establish the side route");
        path.ack(10);
        expect(path.ready(11, false), "Startup probe should activate without a one-second delay");
        expect(path.ready(2009, true) && !path.ready(2010, true), "Incorrect loss deadline");
        path.ack(2100);
        expect(!path.ready(2100, false), "One reply immediately disabled the video cursor");
        for (unsigned now : {2350, 2600, 2850, 3100}) path.ack(now);
        expect(path.ready(3100, false), "A sustained recovered route did not activate");
        CursorRoute blocked(9000);
        expect(blocked.initial_timeout(10000) && !blocked.ready(10000, false), "Missing startup fallback");
        blocked.ack(11000);
        blocked.ack(11700);
        blocked.ack(12200);
        expect(!blocked.ready(12400, false), "Flapping ACKs accumulated as healthy time");
        blocked.ack(12700);
        expect(blocked.ready(12700, false), "A stable route remained disabled");
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
