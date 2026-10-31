#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "pid_controller.hpp"

namespace nvapi {

struct FanReading {
    unsigned id = 0;
    std::optional<unsigned> rpm;
    std::optional<unsigned> percent;
};

// Optional, dynamically resolved NVAPI telemetry and fan control. Private
// structure layouts follow LibreHardwareMonitor's Interop/NvApi.cs.
class FanControl {
public:
    explicit FanControl(std::uint32_t busId, const std::string& name) {
        // Blackwell does not expose a hotspot at thermal channel 1: that
        // channel is the core temperature. Never relabel it as a hotspot.
        hotspotSupported_ = name.find("GeForce RTX 50") == std::string::npos &&
                            name.find("Blackwell") == std::string::npos;
#ifdef _WIN64
        library_ = LoadLibraryW(L"nvapi64.dll");
#else
        library_ = LoadLibraryW(L"nvapi.dll");
#endif
        if (!library_) throw std::runtime_error("Cannot load NVIDIA NVAPI.");
        try {
            query_ = reinterpret_cast<Query>(GetProcAddress(library_, "nvapi_QueryInterface"));
            if (!query_) throw std::runtime_error("nvapi_QueryInterface unavailable.");
            initialize_ = query<Initialize>(0x0150E828);
            unload_ = query<Initialize>(0xD22BDD7E);
            auto enumerate = query<Enumerate>(0xE5AC921F);
            auto getBus = query<GetUnsigned>(0x1BE0B8E5);
            if (!initialize_ || !enumerate || !getBus) throw std::runtime_error("NVAPI initialization unavailable.");
            check(initialize_(), "NVAPI initialization");
            initialized_ = true;
            Handle handles[64]{};
            unsigned count = 0;
            check(enumerate(handles, &count), "NVAPI enumeration");
            for (unsigned i = 0; i < std::min(count, 64u); ++i) {
                unsigned candidate = 0;
                if (getBus(handles[i], &candidate) == 0 && candidate == busId) { gpu_ = handles[i]; break; }
            }
            if (!gpu_) throw std::runtime_error("NVAPI could not match the GPU PCI bus.");
            getCoolers_ = query<GetCoolers>(0xDA141340);
            setLevels_ = query<SetLevels>(0x891FA0AE);
            getControl_ = query<GetControl>(0x814B209F);
            setControl_ = query<GetControl>(0xA58971A5);
            getStatus_ = query<GetStatus>(0x35AED5E8);
            getThermal_ = query<GetThermal>(0x65FE3AAD);
            getTach_ = query<GetUnsigned>(0x5F608315);
            detectCoolers();
        } catch (...) { cleanup(); throw; }
    }
    ~FanControl() { cleanup(); }
    FanControl(const FanControl&) = delete;
    FanControl& operator=(const FanControl&) = delete;

    unsigned fanCount() const noexcept { return count_; }
    unsigned minimumSpeed() const noexcept { return minimum_; }
    bool canControl() const noexcept { return mode_ != Mode::None; }

    std::optional<double> hotspot() const noexcept {
        if (!getThermal_ || !hotspotSupported_) return std::nullopt;
        Thermal sensors{};
        sensors.version = version<Thermal>();
        sensors.mask = 2; // Only channel 1 is needed, no unsupported-channel probing.
        if (getThermal_(gpu_, &sensors) != 0) return std::nullopt;
        const double value = sensors.temperatures[1] / 256.0;
        return fan::Temperatures::valid(value) ? std::optional<double>(value) : std::nullopt;
    }

    std::vector<FanReading> readings() const {
        std::vector<FanReading> result;
        Status status{};
        status.version = version<Status>();
        if (getStatus_ && getStatus_(gpu_, &status) == 0 && status.count > 0 && status.count <= 32) {
            for (unsigned i = 0; i < status.count; ++i) {
                const auto& item = status.items[i];
                FanReading reading{item.id, item.rpm, std::nullopt};
                if (item.level <= 100) reading.percent = item.level;
                result.push_back(reading);
            }
            return result;
        }
        Coolers coolers{};
        coolers.version = version<Coolers>();
        if (getCoolers_ && getCoolers_(gpu_, ~0u, &coolers) == 0 && coolers.count <= 20) {
            for (unsigned i = 0; i < coolers.count; ++i) {
                FanReading reading{i, std::nullopt, std::nullopt};
                if (coolers.items[i].currentLevel >= 0 && coolers.items[i].currentLevel <= 100)
                    reading.percent = unsigned(coolers.items[i].currentLevel);
                result.push_back(reading);
            }
        }
        unsigned rpm = 0;
        if (getTach_ && getTach_(gpu_, &rpm) == 0) {
            if (result.empty()) result.push_back({0, std::nullopt, std::nullopt});
            result.front().rpm = rpm; // A single tachometer is not all fans' RPM.
        }
        return result;
    }

    void setFanSpeed(unsigned percent) {
        if (!canControl()) throw std::runtime_error("GPU fan control is not exposed by the driver.");
        modified_ = true; // Restore even when a later fan rejects the write.
        if (mode_ == Mode::Client) {
            Control control = readControl();
            for (unsigned i = 0; i < control.count; ++i) {
                control.items[i].level = std::min(percent, 100u);
                control.items[i].mode = 1;
            }
            check(setControl_(gpu_, &control), "NVAPI fan control");
        } else {
            for (unsigned i = 0; i < count_; ++i) check(setClassic(i, percent, true), "NVAPI fan control");
        }
    }

    bool restoreDefault() noexcept {
        if (!modified_) return true;
        bool ok = true;
        if (mode_ == Mode::Client) {
            try {
                Control control = readControl();
                for (unsigned i = 0; i < control.count; ++i) { control.items[i].level = 0; control.items[i].mode = 0; }
                ok = setControl_(gpu_, &control) == 0;
            } catch (...) { ok = false; }
        } else {
            // Try every fan even if restoring an earlier fan failed.
            for (unsigned i = 0; i < count_; ++i) if (setClassic(i, 0, false) != 0) ok = false;
        }
        if (ok) modified_ = false;
        return ok;
    }

private:
    using Handle = void*;
    using Query = void* (__cdecl*)(unsigned);
    using Initialize = int (__cdecl*)();
    using Enumerate = int (__cdecl*)(Handle*, unsigned*);
    using GetUnsigned = int (__cdecl*)(Handle, unsigned*);
    struct Cooler {
        int type, controller, defaultMin, defaultMax, currentMin, currentMax;
        int currentLevel, defaultPolicy, currentPolicy, target, controlType, active;
    };
    struct Coolers { unsigned version, count; Cooler items[20]; };
    struct Level { int level; unsigned policy; };
    struct Levels { unsigned version; Level items[20]; };
    struct ControlItem { unsigned id, level, mode, reserved[8]; };
    struct Control { unsigned version, reserved1, count, reserved2[8]; ControlItem items[32]; };
    struct StatusItem { unsigned id, rpm, minimum, maximum, level, reserved[8]; };
    struct Status { unsigned version, count; std::uint64_t reserved[4]; StatusItem items[32]; };
    struct Thermal { unsigned version, mask; int reserved[8], temperatures[32]; };
    static_assert(sizeof(Status) == 1704 && sizeof(Thermal) == 168 && sizeof(Control) == 1452);
    using GetCoolers = int (__cdecl*)(Handle, unsigned, Coolers*);
    using SetLevels = int (__cdecl*)(Handle, unsigned, Levels*);
    using GetControl = int (__cdecl*)(Handle, Control*);
    using GetStatus = int (__cdecl*)(Handle, Status*);
    using GetThermal = int (__cdecl*)(Handle, Thermal*);
    enum class Mode { None, Classic, Client };

    template <class T> static unsigned version() { return unsigned(sizeof(T)) | (1u << 16); }
    template <class T> T query(unsigned id) const { return reinterpret_cast<T>(query_(id)); }
    static void check(int status, const char* operation) {
        if (status != 0) throw std::runtime_error(std::string(operation) + " failed (NVAPI " + std::to_string(status) + ").");
    }
    Control readControl() const {
        Control control{};
        control.version = version<Control>();
        check(getControl_(gpu_, &control), "NVAPI read fan controllers");
        if (control.count == 0 || control.count > 32 || (count_ && count_ != control.count))
            throw std::runtime_error("NVAPI returned an invalid fan controller count.");
        return control;
    }
    void detectCoolers() {
        if (getControl_ && setControl_) {
            try { count_ = readControl().count; mode_ = Mode::Client; } catch (...) {}
        }
        Coolers settings{};
        settings.version = version<Coolers>();
        if (getCoolers_ && getCoolers_(gpu_, ~0u, &settings) == 0 && settings.count > 0 && settings.count <= 20) {
            if (mode_ == Mode::None && setLevels_) { mode_ = Mode::Classic; count_ = settings.count; }
            for (unsigned i = 0; i < settings.count; ++i)
                minimum_ = std::max(minimum_, unsigned(std::clamp(settings.items[i].currentMin, 0, 100)));
        }
        Status status{};
        status.version = version<Status>();
        if (getStatus_ && getStatus_(gpu_, &status) == 0 && status.count <= 32)
            for (unsigned i = 0; i < status.count; ++i)
                minimum_ = std::max(minimum_, std::min(status.items[i].minimum, 100u));
    }
    int setClassic(unsigned fan, unsigned percent, bool manual) const {
        Levels levels{};
        levels.version = version<Levels>();
        levels.items[0] = {int(std::min(percent, 100u)), manual ? 1u : 32u};
        return setLevels_(gpu_, fan, &levels);
    }
    void cleanup() noexcept {
        if (initialized_) { restoreDefault(); if (unload_) unload_(); }
        initialized_ = false;
        if (library_) { FreeLibrary(library_); library_ = nullptr; }
    }

    HMODULE library_ = nullptr;
    Handle gpu_ = nullptr;
    Query query_ = nullptr;
    Initialize initialize_ = nullptr, unload_ = nullptr;
    GetCoolers getCoolers_ = nullptr;
    SetLevels setLevels_ = nullptr;
    GetControl getControl_ = nullptr, setControl_ = nullptr;
    GetStatus getStatus_ = nullptr;
    GetThermal getThermal_ = nullptr;
    GetUnsigned getTach_ = nullptr;
    Mode mode_ = Mode::None;
    unsigned count_ = 0, minimum_ = 30;
    bool initialized_ = false, modified_ = false, hotspotSupported_ = true;
};
} // namespace nvapi
