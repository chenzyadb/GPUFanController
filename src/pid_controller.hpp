#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>

namespace fan {
struct Settings {
    int target = 75;
    int stop = 40;
    void normalize() noexcept {
        target = std::clamp(target, 50, 100);
        stop = std::clamp(stop, 0, 50);
    }
};

struct Temperatures {
    std::optional<double> gpu;
    std::optional<double> hotspot;
    static bool valid(double value) noexcept {
        return std::isfinite(value) && value > 0.0 && value <= 150.0;
    }
    std::optional<double> control() const noexcept {
        if (hotspot && valid(*hotspot)) return hotspot;
        if (gpu && valid(*gpu)) return gpu;
        return std::nullopt;
    }
    bool usesHotspot() const noexcept { return hotspot && valid(*hotspot); }
};

class PidController {
public:
    void configure(Settings settings, unsigned minimumSpeed = 30) noexcept {
        settings.normalize();
        minimumSpeed = std::clamp(minimumSpeed, 1u, 100u);
        if (settings.target != settings_.target || settings.stop != settings_.stop ||
            minimumSpeed != minimumSpeed_) reset();
        settings_ = settings;
        minimumSpeed_ = minimumSpeed;
    }
    void reset() noexcept {
        integral_ = 0.0;
        derivative_ = 0.0;
        previousTemperature_.reset();
        stopped_ = true;
    }
    unsigned update(double temperature, double seconds) {
        if (!Temperatures::valid(temperature) || !std::isfinite(seconds) || seconds <= 0.0)
            throw std::invalid_argument("Invalid PID sample.");
        // Do not integrate time spent suspended or without valid telemetry.
        seconds = std::clamp(seconds, 0.05, 3.0);
        const double restart = std::min(settings_.stop + 2, settings_.target);
        if (temperature <= settings_.stop || (stopped_ && temperature < restart)) {
            reset();
            return 0;
        }
        stopped_ = false;
        constexpr double kp = 4.0, ki = 0.25, kd = 2.0;
        constexpr double filterSeconds = 3.0;
        const double error = temperature - settings_.target;
        const double slope = previousTemperature_ ? (temperature - *previousTemperature_) / seconds : 0.0;
        derivative_ += seconds / (filterSeconds + seconds) * (slope - derivative_);
        previousTemperature_ = temperature;
        const double candidateIntegral = std::clamp(integral_ + ki * error * seconds, -100.0, 100.0);
        const double candidate = minimumSpeed_ + kp * error + candidateIntegral + kd * derivative_;
        // Conditional integration prevents windup at either actuator limit.
        if ((candidate >= minimumSpeed_ && candidate <= 100.0) ||
            (candidate > 100.0 && error < 0.0) || (candidate < minimumSpeed_ && error > 0.0))
            integral_ = candidateIntegral;
        const double output = minimumSpeed_ + kp * error + integral_ + kd * derivative_;
        return static_cast<unsigned>(std::lround(std::clamp(output, double(minimumSpeed_), 100.0)));
    }
private:
    Settings settings_;
    unsigned minimumSpeed_ = 30;
    double integral_ = 0.0, derivative_ = 0.0;
    std::optional<double> previousTemperature_;
    bool stopped_ = true;
};
} // namespace fan
