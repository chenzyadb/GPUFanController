#include <windows.h>
#include <exception>
#include <string>
#include "fan_app.hpp"

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    try {
        return runFanApplication();
    } catch (const std::exception& error) {
        const int count = MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, nullptr, 0);
        std::wstring message(count > 0 ? count : 1, L'\0');
        if (count > 0) MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, message.data(), count);
        MessageBoxW(nullptr, message.c_str(), L"GPU Fan Controller", MB_OK | MB_ICONERROR);
        return 1;
    }
}
