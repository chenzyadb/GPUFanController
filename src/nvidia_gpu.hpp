#pragma once

#include "nvapi_fan.hpp"
#include <memory>
#include <utility>

namespace fan {

// The NVML runtime is shipped by the display driver; no CUDA SDK is required.
class Nvml {
public:
    using Device = void*;
    struct PciInfo {
        char legacyBusId[16];
        unsigned domain, bus, device, deviceId, subsystemId;
        char busId[32];
    };
    struct FanSpeed { unsigned version, fan, speed; };
    using Init = int (*)();
    using GetCount = int (*)(unsigned*);
    using GetHandle = int (*)(unsigned, Device*);
    using GetName = int (*)(Device, char*, unsigned);
    using GetUnsigned = int (*)(Device, unsigned*);
    using GetIndexed = int (*)(Device, unsigned, unsigned*);
    using GetPci = int (*)(Device, PciInfo*);
    using GetRpm = int (*)(Device, FanSpeed*);
    using SetFan = int (*)(Device, unsigned, unsigned);
    using RestoreFan = int (*)(Device, unsigned);
    using Error = const char* (*)(int);

    Nvml() {
        library_ = LoadLibraryW(L"nvml.dll");
        if (!library_) throw std::runtime_error("Cannot load nvml.dll. Install an NVIDIA display driver.");
        try {
            auto init = load<Init>("nvmlInit_v2");
            shutdown_ = load<Init>("nvmlShutdown");
            error_ = load<Error>("nvmlErrorString");
            count = load<GetCount>("nvmlDeviceGetCount_v2");
            handle = load<GetHandle>("nvmlDeviceGetHandleByIndex_v2");
            name = load<GetName>("nvmlDeviceGetName");
            pci = load<GetPci>("nvmlDeviceGetPciInfo_v3", false);
            temperature = load<GetIndexed>("nvmlDeviceGetTemperature");
            fanCount = load<GetUnsigned>("nvmlDeviceGetNumFans", false);
            fanPercent = load<GetIndexed>("nvmlDeviceGetFanSpeed_v2", false);
            fanRpm = load<GetRpm>("nvmlDeviceGetFanSpeedRPM", false);
            setFan = load<SetFan>("nvmlDeviceSetFanSpeed_v2", false);
            restoreFan = load<RestoreFan>("nvmlDeviceSetDefaultFanSpeed_v2", false);
            check(init(), "NVML initialization");
            initialized_ = true;
        } catch (...) { FreeLibrary(library_); throw; }
    }
    ~Nvml() { if (initialized_) shutdown_(); FreeLibrary(library_); }
    Nvml(const Nvml&) = delete;
    Nvml& operator=(const Nvml&) = delete;
    void check(int result, const char* operation) const {
        if (result == 0) return;
        const char* detail = error_ ? error_(result) : "unknown error";
        throw std::runtime_error(std::string(operation) + ": " + (detail ? detail : "unknown error") +
                                 (result == 4 ? " (Administrator privileges required.)" : ""));
    }
    GetCount count{};
    GetHandle handle{};
    GetName name{};
    GetPci pci{};
    GetIndexed temperature{}, fanPercent{};
    GetUnsigned fanCount{};
    GetRpm fanRpm{};
    SetFan setFan{};
    RestoreFan restoreFan{};
private:
    template <class T> T load(const char* symbol, bool required = true) {
        auto address = GetProcAddress(library_, symbol);
        if (!address && required) throw std::runtime_error(std::string("Missing NVML function: ") + symbol);
        return reinterpret_cast<T>(address);
    }
    HMODULE library_{};
    Init shutdown_{};
    Error error_{};
    bool initialized_ = false;
};

class NvidiaGpu {
public:
    NvidiaGpu(Nvml& nvml, unsigned index) : nvml_(nvml) {
        nvml_.check(nvml_.handle(index, &device_), "Get GPU handle");
        char name[128]{};
        nvml_.check(nvml_.name(device_, name, sizeof(name)), "Get GPU name");
        name_ = name;
        if (nvml_.fanCount && nvml_.fanCount(device_, &nvmlCount_) != 0) nvmlCount_ = 0;
        if (nvmlCount_ > 32) nvmlCount_ = 0;
        modifiedFans_.resize(nvmlCount_, false);
        if (nvml_.pci) {
            Nvml::PciInfo pci{};
            if (nvml_.pci(device_, &pci) == 0) {
                try { nvapi_ = std::make_unique<nvapi::FanControl>(pci.bus, name_); }
                catch (const std::exception&) { /* NVML remains usable without NVAPI. */ }
            }
        }
        useNvml_ = nvmlCount_ > 0 && nvml_.setFan && nvml_.restoreFan;
    }
    ~NvidiaGpu() { restoreDefault(); }
    const std::string& name() const noexcept { return name_; }
    bool controllable() const noexcept { return useNvml_ || (nvapi_ && nvapi_->canControl()); }
    unsigned minimumSpeed() const noexcept { return nvapi_ ? nvapi_->minimumSpeed() : 30; }
    unsigned fanCount() const noexcept { return useNvml_ ? nvmlCount_ : (nvapi_ ? nvapi_->fanCount() : 0); }

    void verifyControlAccess() {
        if (!controllable()) throw std::runtime_error("This GPU/driver does not expose fan control.");
        // Exported APIs and elevation alone do not prove write permission.
        // Probe every fan, without lowering any reported current duty, then
        // restore automatic control before allowing startup to succeed.
        unsigned speed = minimumSpeed();
        for (const auto& reading : readings())
            if (reading.percent) speed = std::max(speed, *reading.percent);
        try { setFanSpeed(speed); }
        catch (...) { restoreDefault(); throw; }
        if (!restoreDefault())
            throw std::runtime_error("Fan control check could not restore the driver's automatic policy.");
    }

    Temperatures temperatures() const {
        Temperatures result;
        unsigned core = 0;
        if (nvml_.temperature(device_, 0, &core) == 0 && Temperatures::valid(core)) result.gpu = core;
        if (nvapi_) result.hotspot = nvapi_->hotspot();
        return result;
    }
    std::vector<nvapi::FanReading> readings() const {
        auto result = nvapi_ ? nvapi_->readings() : std::vector<nvapi::FanReading>{};
        // NVAPI cooler IDs need not equal NVML indices; keep its complete set
        // intact, and only use NVML enumeration when NVAPI has no readings.
        if (!result.empty()) return result;
        result.resize(std::max(nvmlCount_, fanCount()));
        for (unsigned i = 0; i < result.size(); ++i) {
            result[i].id = i;
            if (i >= nvmlCount_) continue;
            unsigned percent = 0;
            if (nvml_.fanPercent && nvml_.fanPercent(device_, i, &percent) == 0 && percent <= 100)
                result[i].percent = percent;
            Nvml::FanSpeed speed{unsigned(sizeof(Nvml::FanSpeed)) | (1u << 24), i, 0};
            if (nvml_.fanRpm && nvml_.fanRpm(device_, &speed) == 0) result[i].rpm = speed.speed;
        }
        return result;
    }
    void setFanSpeed(unsigned percent) {
        if (useNvml_) {
            for (unsigned i = 0; i < nvmlCount_; ++i) {
                const int status = nvml_.setFan(device_, i, percent);
                if (status == 0) modifiedFans_[i] = true;
                if (status != 0) {
                    const bool restored = restoreNvml();
                    // Some GeForce drivers export NVML setters but reject them.
                    if (restored && nvapi_ && nvapi_->canControl()) {
                        useNvml_ = false;
                        nvapi_->setFanSpeed(percent);
                        return;
                    }
                    nvml_.check(status, "Set GPU fan speed");
                }
            }
        } else if (nvapi_ && nvapi_->canControl()) nvapi_->setFanSpeed(percent);
        else throw std::runtime_error("This GPU/driver does not expose fan control.");
    }
    bool restoreDefault() noexcept {
        const bool nvmlOk = restoreNvml();
        const bool nvapiOk = !nvapi_ || nvapi_->restoreDefault();
        return nvmlOk && nvapiOk;
    }
private:
    bool restoreNvml() noexcept {
        bool ok = true;
        for (unsigned i = 0; i < nvmlCount_; ++i) {
            if (!modifiedFans_[i]) continue;
            if (nvml_.restoreFan(device_, i) == 0) modifiedFans_[i] = false;
            else ok = false;
        }
        return ok;
    }
    Nvml& nvml_;
    Nvml::Device device_{};
    std::string name_;
    unsigned nvmlCount_ = 0;
    bool useNvml_ = false;
    std::vector<bool> modifiedFans_;
    std::unique_ptr<nvapi::FanControl> nvapi_;
};
} // namespace fan
