// Driver fixture used by backend and GUI tests. This DLL never accesses hardware.
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace {
int scenario = [] {
    char value[16]{};
    return GetEnvironmentVariableA("GPU_FAN_FIXTURE_SCENARIO", value, sizeof(value)) ? std::atoi(value) : 0;
}();
unsigned commands[2]{}, calls[2]{}, restores[2]{};
unsigned nvapiCommands[2]{}, nvapiCalls = 0, nvapiRestores = 0;
struct Pci { char oldId[16]; unsigned domain, bus, device, deviceId, subsystemId; char id[32]; };
struct Speed { unsigned version, fan, speed; };
struct ControlItem { unsigned id, level, mode, reserved[8]; };
struct Control { unsigned version, reserved1, count, reserved2[8]; ControlItem items[32]; };
struct StatusItem { unsigned id, rpm, minimum, maximum, level, reserved[8]; };
struct Status { unsigned version, count; std::uint64_t reserved[4]; StatusItem items[32]; };
struct Thermal { unsigned version, mask; int reserved[8], temperatures[32]; };
void logCommand(char operation, unsigned fan, unsigned speed) {
    char path[2048]{};
    if (!GetEnvironmentVariableA("GPU_FAN_FIXTURE_LOG", path, sizeof(path))) return;
    if (FILE* file = std::fopen(path, "a")) {
        std::fprintf(file, "%c %u %u\n", operation, fan, speed);
        std::fclose(file);
    }
}
int init() { return 0; }
int enumerate(void** handles, unsigned* count) { handles[0] = reinterpret_cast<void*>(1); *count = 1; return 0; }
int bus(void*, unsigned* id) { *id = 1; return 0; }
int control(void*, Control* output) {
    if (output->version != (sizeof(Control) | (1u << 16))) return -9;
    output->count = 2;
    output->items[0].id = 7; output->items[1].id = 12;
    return 0;
}
int setControl(void*, Control* input) {
    if (scenario == 10 || scenario == 11) return -137; // NVAPI_INSUFFICIENT_PERMISSIONS
    if (scenario == 5) return -1;
    if (input->count != 2 || input->items[0].id != 7 || input->items[1].id != 12) return -5;
    if (input->items[0].mode == 0 && input->items[1].mode == 0) ++nvapiRestores;
    else { ++nvapiCalls; nvapiCommands[0] = input->items[0].level; nvapiCommands[1] = input->items[1].level; }
    return 0;
}
int status(void*, Status* output) {
    if (output->version != (sizeof(Status) | (1u << 16))) return -9;
    if (scenario == 4) return -3;
    output->count = 2;
    output->items[0] = {7, 1234, 30, 100, 42, {}};
    output->items[1] = {12, 1280, 35, 100, 42, {}};
    return 0;
}
int thermal(void*, Thermal* output) {
    if (output->version != (sizeof(Thermal) | (1u << 16)) || output->mask != 2) return -9;
    if (scenario == 1 || scenario == 2) return -3;
    output->temperatures[1] = 85 * 256 + 128;
    return 0;
}
}

extern "C" {
__declspec(dllexport) void fixtureReset(int value) {
    scenario = value;
    std::fill_n(commands, 2, 0); std::fill_n(calls, 2, 0); std::fill_n(restores, 2, 0);
    std::fill_n(nvapiCommands, 2, 0); nvapiCalls = nvapiRestores = 0;
}
__declspec(dllexport) unsigned fixtureCommand(unsigned fan) { return commands[fan]; }
__declspec(dllexport) unsigned fixtureCalls(unsigned fan) { return calls[fan]; }
__declspec(dllexport) unsigned fixtureRestores(unsigned fan) { return restores[fan]; }
__declspec(dllexport) unsigned fixtureNvapiCommand(unsigned fan) { return nvapiCommands[fan]; }
__declspec(dllexport) unsigned fixtureNvapiCalls() { return nvapiCalls; }
__declspec(dllexport) unsigned fixtureNvapiRestores() { return nvapiRestores; }

__declspec(dllexport) int nvmlInit_v2() { return 0; }
__declspec(dllexport) int nvmlShutdown() { return 0; }
__declspec(dllexport) const char* nvmlErrorString(int status) { return status == 4 ? "Insufficient Permissions" : "fixture failure"; }
__declspec(dllexport) int nvmlDeviceGetCount_v2(unsigned* count) { *count = 1; return 0; }
__declspec(dllexport) int nvmlDeviceGetHandleByIndex_v2(unsigned, void** handle) { *handle = reinterpret_cast<void*>(1); return 0; }
__declspec(dllexport) int nvmlDeviceGetName(void*, char* name, unsigned length) {
    const char* model = scenario == 6 ? "NVIDIA GeForce RTX 5090" :
                        scenario == 9 ? "NVIDIA RTX 5000 Ada Generation" : "NVIDIA GeForce RTX 4090 (test)";
    strncpy_s(name, length, model, _TRUNCATE); return 0;
}
__declspec(dllexport) int nvmlDeviceGetPciInfo_v3(void*, Pci* output) { output->bus = 1; return 0; }
__declspec(dllexport) int nvmlDeviceGetTemperature(void*, unsigned, unsigned* output) { *output = 61; return scenario == 2 ? 3 : 0; }
__declspec(dllexport) int nvmlDeviceGetNumFans(void*, unsigned* output) { *output = 2; return 0; }
__declspec(dllexport) int nvmlDeviceGetFanSpeed_v2(void*, unsigned, unsigned* output) { *output = 44; return 0; }
__declspec(dllexport) int nvmlDeviceGetFanSpeedRPM(void*, Speed* output) {
    if (output->version != (sizeof(Speed) | (1u << 24))) return 25;
    output->speed = 1400 + output->fan * 100; return 0;
}
__declspec(dllexport) int nvmlDeviceSetFanSpeed_v2(void*, unsigned fan, unsigned percent) {
    if (scenario == 10 || (scenario == 11 && fan == 1)) { logCommand('D', fan, percent); return 4; }
    if (scenario == 3 || scenario == 5 || (scenario == 7 && fan == 1)) return 3;
    commands[fan] = percent; ++calls[fan]; logCommand('S', fan, percent); return 0;
}
__declspec(dllexport) int nvmlDeviceSetDefaultFanSpeed_v2(void*, unsigned fan) {
    ++restores[fan]; logCommand('R', fan, 0); return scenario == 8 && fan == 0 ? 3 : 0;
}
__declspec(dllexport) void* nvapi_QueryInterface(unsigned id) {
    switch (id) {
    case 0x0150E828: case 0xD22BDD7E: return reinterpret_cast<void*>(init);
    case 0xE5AC921F: return reinterpret_cast<void*>(enumerate);
    case 0x1BE0B8E5: return reinterpret_cast<void*>(bus);
    case 0x814B209F: return reinterpret_cast<void*>(control);
    case 0xA58971A5: return reinterpret_cast<void*>(setControl);
    case 0x35AED5E8: return reinterpret_cast<void*>(status);
    case 0x65FE3AAD: return reinterpret_cast<void*>(thermal);
    default: return nullptr;
    }
}
}
