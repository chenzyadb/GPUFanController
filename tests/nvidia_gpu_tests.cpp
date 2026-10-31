#include "nvidia_gpu.hpp"
#include <cstdlib>
#include <iostream>

void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}

template<class T> T symbol(HMODULE dll, const char* name) {
    auto address = GetProcAddress(dll, name);
    expect(address != nullptr, "test must use the fixture DLL");
    return reinterpret_cast<T>(address);
}

int main() {
    HMODULE nvmlDll = LoadLibraryW(L"nvml.dll"), nvapiDll = LoadLibraryW(L"nvapi64.dll");
    expect(nvmlDll && nvapiDll, "load driver fixtures");
    auto reset = symbol<void (*)(int)>(nvmlDll, "fixtureReset");
    auto resetApi = symbol<void (*)(int)>(nvapiDll, "fixtureReset");
    auto command = symbol<unsigned (*)(unsigned)>(nvmlDll, "fixtureCommand");
    auto restores = symbol<unsigned (*)(unsigned)>(nvmlDll, "fixtureRestores");
    auto apiCommand = symbol<unsigned (*)(unsigned)>(nvapiDll, "fixtureNvapiCommand");
    auto apiRestores = symbol<unsigned (*)()>(nvapiDll, "fixtureNvapiRestores");
    fan::Nvml nvml;
    reset(0); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        expect(gpu.controllable() && gpu.fanCount() == 2, "two controllable fans");
        const auto temp = gpu.temperatures();
        expect(temp.gpu == 61 && temp.hotspot == 85.5 && temp.control() == 85.5, "hotspot telemetry ABI");
        const auto fans = gpu.readings();
        expect(fans.size() == 2 && fans[0].id == 7 && fans[1].id == 12, "preserve NVAPI cooler identity");
        expect(fans[0].rpm == 1234 && fans[1].rpm == 1280 && fans[0].percent == 42, "actual per-fan telemetry");
        expect(gpu.minimumSpeed() == 35, "largest per-fan minimum");
        gpu.setFanSpeed(63);
        expect(command(0) == 63 && command(1) == 63, "all fans share one duty");
    }
    expect(restores(0) == 1 && restores(1) == 1, "RAII restores every fan");
    reset(0); resetApi(1);
    { fan::NvidiaGpu gpu(nvml, 0); expect(gpu.temperatures().control() == 61, "NVAPI unavailable core fallback"); }
    reset(2); resetApi(2);
    { fan::NvidiaGpu gpu(nvml, 0); expect(!gpu.temperatures().control(), "both sensors unavailable"); }
    reset(6); resetApi(0);
    { fan::NvidiaGpu gpu(nvml, 0); expect(!gpu.temperatures().hotspot, "Blackwell core is not misreported as hotspot"); }
    reset(9); resetApi(0);
    { fan::NvidiaGpu gpu(nvml, 0); expect(gpu.temperatures().hotspot == 85.5, "RTX 5000 Ada is not a GeForce RTX 50"); }
    reset(0); resetApi(4);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        const auto fans = gpu.readings();
        expect(fans.size() == 2 && fans[0].rpm == 1400 && fans[1].rpm == 1500 && fans[0].percent == 44, "NVML RPM ABI fallback");
    }
    reset(3); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        gpu.setFanSpeed(57);
        expect(apiCommand(0) == 57 && apiCommand(1) == 57, "NVML setter rejected: switch to NVAPI");
    }
    expect(apiRestores() == 1, "NVAPI restoration");
    reset(7); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0); gpu.setFanSpeed(65);
        expect(restores(0) == 1 && apiCommand(0) == 65 && apiCommand(1) == 65, "partial NVML write restored before fallback");
    }
    reset(5); resetApi(5);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        bool failed = false;
        try { gpu.setFanSpeed(60); } catch (const std::exception&) { failed = true; }
        expect(failed, "surface rejected control instead of fabricating success");
    }
    reset(8); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0); gpu.setFanSpeed(60);
        expect(!gpu.restoreDefault() && restores(0) == 1 && restores(1) == 1, "restore tries later fans after failure");
        reset(0);
        expect(gpu.restoreDefault(), "failed restoration is retried");
    }
    reset(0); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        gpu.verifyControlAccess();
        expect(command(0) == 42 && command(1) == 42, "startup probes every fan without lowering current duty");
        expect(restores(0) == 1 && restores(1) == 1, "startup probe restores automatic control");
    }
    reset(10); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0); gpu.verifyControlAccess();
        expect(apiCommand(0) == 42 && apiCommand(1) == 42 && apiRestores() == 1, "NVAPI access is sufficient when NVML denies permission");
    }
    for (int denied : {10, 11}) {
        reset(denied); resetApi(denied);
        fan::NvidiaGpu gpu(nvml, 0);
        bool failed = false;
        try { gpu.verifyControlAccess(); } catch (const std::exception&) { failed = true; }
        expect(failed, "startup must reject denied fan writes even when interfaces are present");
        if (denied == 11) expect(restores(0) == 1, "startup rolls back a successful earlier fan when the next fan denies access");
    }
    reset(8); resetApi(0);
    {
        fan::NvidiaGpu gpu(nvml, 0);
        bool failed = false;
        try { gpu.verifyControlAccess(); } catch (const std::exception&) { failed = true; }
        expect(failed, "startup fails if probe cannot restore automatic control");
        reset(0);
    }
    std::cout << "NVIDIA backend checks passed with simulated drivers.\n";
}
