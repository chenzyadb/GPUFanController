#include "pid_controller.hpp"
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
}

int main() {
    using namespace fan;
    Settings limits{120, -5};
    limits.normalize();
    expect(limits.target == 100 && limits.stop == 0, "upper target and lower stop bounds");
    limits = {10, 100};
    limits.normalize();
    expect(limits.target == 50 && limits.stop == 50, "lower target and upper stop bounds");

    Temperatures temperatures{60.0, 85.0};
    expect(temperatures.control() == 85.0 && temperatures.usesHotspot(), "hotspot must take precedence");
    temperatures.hotspot.reset();
    expect(temperatures.control() == 60.0 && !temperatures.usesHotspot(), "core fallback");
    temperatures.hotspot = 0.0;
    expect(temperatures.control() == 60.0, "zero hotspot is unavailable");
    temperatures.hotspot = std::numeric_limits<double>::quiet_NaN();
    expect(temperatures.control() == 60.0, "NaN hotspot fallback");
    temperatures.gpu.reset();
    expect(!temperatures.control(), "no stale or synthetic temperature");

    PidController pid;
    pid.configure({75, 40});
    expect(pid.update(40.0, 1) == 0, "stop threshold is inclusive");
    expect(pid.update(41.0, 1) == 0, "stop hysteresis");
    expect(pid.update(42.0, 1) >= 30, "restart at two degrees above stop");
    expect(pid.update(41.0, 1) >= 30, "running fan does not chatter");
    expect(pid.update(39.0, 1) == 0, "stop clears history");
    const unsigned initial = pid.update(80.0, 1);
    unsigned settled = initial;
    for (int i = 0; i < 10; ++i) settled = pid.update(80.0, 1);
    expect(settled > initial, "integral responds to sustained error");
    for (int i = 0; i < 600; ++i) expect(pid.update(120.0, 1) == 100, "upper saturation");
    for (int i = 0; i < 100; ++i) settled = pid.update(55.0, 1);
    expect(settled == 30, "anti-windup recovers after saturation");

    PidController rising, steady;
    rising.update(75, 1);
    steady.update(80, 1);
    expect(rising.update(80, 1) > steady.update(80, 1), "derivative anticipates rising heat");
    pid.configure({75, 0});
    expect(pid.update(25, 1) >= 30, "zero stop setting keeps fans running");
    pid.configure({50, 50});
    expect(pid.update(50, 1) == 0 && pid.update(50.5, 1) > 0, "equal endpoints remain reachable");
    pid.configure({100, 50}, 45);
    expect(pid.update(60, 1) >= 45, "hardware minimum is respected");
    expect(pid.update(120, 3600) <= 100, "suspend gap is bounded");
    bool rejected = false;
    try { pid.update(std::numeric_limits<double>::quiet_NaN(), 1); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "invalid sensor must not become a fan command");
    rejected = false;
    try { pid.update(70, 0); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "invalid sampling interval is rejected");
    std::cout << "PID and temperature-selection checks passed.\n";
}
