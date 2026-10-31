#include "fan_app.hpp"
#include "nvidia_gpu.hpp"
#include "resource.h"
#include <OpenGLGUI.hpp>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <commctrl.h>
#include <shellapi.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>

namespace {
using namespace OpenGLGUI;
using Clock = std::chrono::steady_clock;
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kSnapshotMessage = WM_APP + 2;
constexpr UINT kShowMessage = WM_APP + 3;
constexpr UINT kExitMessage = WM_APP + 4;
constexpr UINT kShowCommand = 1001, kExitCommand = 1002;
constexpr wchar_t kWindowClass[] = L"GPUFanController.TrayHost";

HICON loadApplicationIcon(int width, int height) {
    // Shared executable resources remain valid for both the tray and windows
    // throughout the process lifetime; Windows releases them at process exit.
    auto icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP_ICON),
                                            IMAGE_ICON, width, height, LR_DEFAULTCOLOR | LR_SHARED));
    if (!icon) throw std::runtime_error("Cannot load the embedded application icon.");
    return icon;
}

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0);
    std::wstring result(length, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), result.data(), length);
    return result;
}

std::string degrees(std::optional<double> value) {
    if (!value) return u8"不可用";
    std::ostringstream output;
    output << std::fixed << std::setprecision(1) << *value << u8" °C";
    return output.str();
}

struct GpuSnapshot {
    std::string name;
    fan::Temperatures temperatures;
    std::vector<nvapi::FanReading> fans;
    std::optional<unsigned> command;
    std::string status;
    bool error = false;
};

struct GpuState {
    std::unique_ptr<fan::NvidiaGpu> gpu;
    fan::PidController pid;
    std::optional<bool> usedHotspot;
};

class FanApplication {
public:
    FanApplication() {
        loadSettings();
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = windowProcedure;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = kWindowClass;
        cls.hIcon = largeIcon_;
        cls.hIconSm = smallIcon_;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Cannot register the tray window.");
        host_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"GPU Fan Controller",
                               WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, cls.hInstance, this);
        if (!host_) throw std::runtime_error("Cannot create the tray window.");
        taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    }
    ~FanApplication() {
        stopWorker();
        if (window_) {
            EventBus::Unsubscribe(window_, MainWindow::ON_DESTROYED, this);
            delete window_;
        }
        if (host_) { Shell_NotifyIconW(NIM_DELETE, &tray_); DestroyWindow(host_); }
        UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
    }
    int run() {
        worker_ = std::thread([this] { controlLoop(); });
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return startupFinished_; });
        }
        if (!startupError_.empty()) {
            stopWorker();
            MessageBoxW(host_, wide(startupError_).c_str(), L"GPU Fan Controller - 启动失败", MB_OK | MB_ICONERROR);
            return 1;
        }
        if (!saveSettings())
            throw std::runtime_error(u8"无法写入程序同目录下的 settings.ini，请检查该目录的写入权限。");
        if (!addTrayIcon()) throw std::runtime_error("Cannot create the tray icon.");
        // No OpenGL window is created until the first tray click: startup
        // cannot flash a window, take focus, or create a taskbar button.
        MSG message{};
        while (!window_ && !exiting_) {
            const int result = GetMessageW(&message, nullptr, 0, 0);
            if (result <= 0) { requestExit(); break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            // Initialize GLFW after returning from the native callback. Its
            // Win32 setup can dispatch more tray/activation messages itself.
            if (showRequested_ && !exiting_) {
                showRequested_ = false;
                showGui();
            }
        }
        if (window_ && !exiting_) application_.exec();
        stopWorker();
        return 0;
    }

private:
    void loadSettings() {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(), DWORD(executable.size()));
        if (length == 0 || length >= executable.size())
            throw std::runtime_error(u8"无法获取可执行文件路径。");
        executable.resize(length);
        settingsPath_ = std::filesystem::path(executable).parent_path() / L"settings.ini";
        settings_.target = int(GetPrivateProfileIntW(L"Temperature", L"Target", 75, settingsPath_.c_str()));
        settings_.stop = int(GetPrivateProfileIntW(L"Temperature", L"Stop", 40, settingsPath_.c_str()));
        settings_.normalize();
    }
    bool saveSettings() {
        // Write the complete settings section together, using the executable's
        // absolute directory even when a shortcut specifies another CWD.
        std::wstring section = L"Target=" + std::to_wstring(settings_.target);
        section.push_back(L'\0');
        section += L"Stop=" + std::to_wstring(settings_.stop);
        section.push_back(L'\0');
        const bool saved = WritePrivateProfileSectionW(L"Temperature", section.c_str(), settingsPath_.c_str()) != FALSE;
        if (settingsNote_) settingsNote_->text(saved ? u8"设置已保存到程序目录的 settings.ini" :
                                                      u8"保存失败：请检查程序目录的写入权限。");
        return saved;
    }
    void settingsChanged() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            settings_.target = int(targetSlider_->getValue());
            settings_.stop = int(stopSlider_->getValue());
        }
        targetValue_->text(std::to_string(settings_.target) + u8" °C");
        stopValue_->text(std::to_string(settings_.stop) + u8" °C");
        saveSettings();
        window_->commit();
    }
    void publish(std::vector<GpuSnapshot> snapshots) {
        { std::lock_guard<std::mutex> lock(mutex_); pending_ = std::move(snapshots); }
        PostMessageW(host_, kSnapshotMessage, 0, 0);
    }
    void completeStartup(std::string error = {}) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (startupFinished_) return;
            startupError_ = std::move(error);
            startupFinished_ = true;
        }
        wake_.notify_all();
    }
    void controlLoop() noexcept {
        try {
            fan::Nvml nvml;
            unsigned count = 0;
            nvml.check(nvml.count(&count), "Enumerate NVIDIA GPUs");
            if (count == 0 || count > 64) throw std::runtime_error("No NVIDIA GPU is available.");
            std::vector<GpuState> devices(count);
            for (unsigned i = 0; i < count; ++i) {
                devices[i].gpu = std::make_unique<fan::NvidiaGpu>(nvml, i);
                try { devices[i].gpu->verifyControlAccess(); }
                catch (const std::exception& error) {
                    throw std::runtime_error(devices[i].gpu->name() + u8"\n无法取得风扇控制权限或驱动不支持控制。\n"
                        u8"请尝试以管理员身份运行，并确认驱动支持风扇控制。\n\n" + error.what());
                }
            }
            completeStartup();
            auto previous = Clock::now() - std::chrono::seconds(1);
            while (!stopping_.load()) {
                const auto now = Clock::now();
                const double elapsed = std::chrono::duration<double>(now - previous).count();
                previous = now;
                fan::Settings settings;
                { std::lock_guard<std::mutex> lock(mutex_); settings = settings_; }
                std::vector<GpuSnapshot> snapshots;
                for (unsigned i = 0; i < count; ++i) {
                    auto& device = devices[i];
                    GpuSnapshot snapshot;
                    snapshot.name = "GPU " + std::to_string(i + 1);
                    {
                        auto& gpu = *device.gpu;
                        snapshot.name = gpu.name();
                        try {
                            snapshot.temperatures = gpu.temperatures();
                            snapshot.fans = gpu.readings();
                            const auto temperature = snapshot.temperatures.control();
                            if (!gpu.controllable()) {
                                snapshot.status = u8"此显卡/驱动不开放风扇控制；当前由显卡固件管理。";
                                snapshot.error = true;
                            } else if (!temperature) {
                                const bool restored = gpu.restoreDefault();
                                device.pid.reset();
                                device.usedHotspot.reset();
                                snapshot.status = restored ? u8"温度读取失败，已恢复驱动自动控制。" :
                                                             u8"温度读取失败，恢复自动控制也失败；正在重试。";
                                snapshot.error = true;
                            } else {
                                const bool hotspot = snapshot.temperatures.usesHotspot();
                                if (device.usedHotspot && *device.usedHotspot != hotspot) device.pid.reset();
                                device.usedHotspot = hotspot;
                                device.pid.configure(settings, gpu.minimumSpeed());
                                const unsigned speed = device.pid.update(*temperature, elapsed);
                                gpu.setFanSpeed(speed);
                                snapshot.command = speed;
                                snapshot.status = speed == 0 ? u8"低温停转 · 同一卡所有风扇统一控制" :
                                                              u8"PID 自动调速 · 同一卡所有风扇统一控制";
                            }
                        } catch (const std::exception& error) {
                            const bool restored = gpu.restoreDefault();
                            device.pid.reset();
                            device.usedHotspot.reset();
                            snapshot.command.reset();
                            snapshot.status = (restored ? u8"已恢复自动控制：" : u8"恢复自动控制失败：") + std::string(error.what());
                            snapshot.error = true;
                        }
                    }
                    snapshots.push_back(std::move(snapshot));
                }
                publish(std::move(snapshots));
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_until(lock, now + std::chrono::seconds(1), [this] { return stopping_.load(); });
            }
            // Restore while both NVML and NVAPI are still loaded. Each device
            // destructor retries any restoration that failed here.
            for (auto& device : devices) if (device.gpu) device.gpu->restoreDefault();
        } catch (const std::exception& error) {
            completeStartup(error.what());
            GpuSnapshot snapshot;
            snapshot.name = u8"NVIDIA GPU 不可用";
            snapshot.status = error.what();
            snapshot.error = true;
            publish({snapshot});
        }
    }
    void stopWorker() noexcept {
        stopping_.store(true);
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    Label* label(Widget* parent, int x, int y, int width, int height, const std::string& text, float size = 15,
                 const Color& color = Color{35, 48, 67}) {
        auto* item = new Label(parent);
        item->place({x, y}).size({width, height}).text(text).fontSize(size).textColor(color)
            .textAlignment(ALIGN_LEFT | ALIGN_VERTICAL_CENTER);
        return item;
    }
    Button* button(Widget* parent, int x, int y, int width, const std::string& text, Callback callback) {
        auto* item = new Button(parent);
        item->place({x, y}).size({width, 36}).text(text).fontSize(14).textColor(Color{35, 48, 67})
            .backgroundColor(Color{226, 233, 242}).borderRadius(8);
        EventBus::Subscribe(item, Widget::ON_CLICKED, this, [callback](const EventData& data) {
            const auto* mouse = data.get<MouseButton>();
            if (!mouse || *mouse == MouseButton::LEFT) callback();
        });
        return item;
    }
    struct Meter { Label* value{}; ProgressBar* bar{}; };
    Meter meter(Widget* parent, int x, int y, int width, const std::string& title) {
        auto* card = new Spacer(parent);
        card->place({x, y}).size({width, 116}).backgroundColor(Color{255, 255, 255}).borderRadius(12);
        label(card, 16, 10, width - 32, 24, title, 13, Color{91, 108, 130});
        auto* value = label(card, 16, 36, width - 32, 40, u8"等待采样", 27);
        auto* bar = new ProgressBar(card);
        bar->place({16, 87}).size({width - 32, 10}).backgroundColor(Color{222, 230, 240}).borderRadius(5);
        return {value, bar};
    }
    void createGui() {
        // The bundled renderer supports TrueType collections; use a Chinese
        // font when available and retain its platform fallback otherwise.
        wchar_t windows[MAX_PATH]{};
        GetWindowsDirectoryW(windows, MAX_PATH);
        try {
            const auto path = std::filesystem::path(windows) / L"Fonts" / L"msyh.ttc";
            const auto family = FontManager::addFont(path.u8string());
            FontManager::setDefaultFontFamily(family);
        } catch (const std::exception&) {}
        auto window = std::make_unique<MainWindow>();
        window->title(u8"GPU 风扇控制").size({720, 640}).decorated(true).resizable(false).backgroundColor(Color{243, 246, 250});
        auto* root = new Spacer;
        root->size({720, 640}).backgroundColor(Color{243, 246, 250});
        window->setWidget(root);
        gpuName_ = label(root, 28, 20, 570, 30, u8"正在读取 NVIDIA GPU…", 17);
        previousGpu_ = button(root, 610, 18, 34, "<", [this] { selectGpu(-1); });
        nextGpu_ = button(root, 654, 18, 34, ">", [this] { selectGpu(1); });
        core_ = meter(root, 28, 68, 208, u8"GPU 温度");
        hotspot_ = meter(root, 252, 68, 208, u8"GPU 热点温度");
        fan_ = meter(root, 476, 68, 212, u8"当前风扇转速（平均）");
        source_ = label(root, 28, 194, 660, 28, u8"控制温度：等待采样", 14, Color{32, 104, 174});
        auto* fanScroll = new ScrollArea(root);
        fanScroll->place({28, 231}).size({660, 103}).backgroundColor(Color{255, 255, 255}).borderRadius(10);
        fanContent_ = new Spacer;
        fanContent_->size({640, 100});
        fanScroll->setWidget(fanContent_);
        label(root, 28, 350, 580, 30, u8"温度设置", 18)->fontWeight(600);
        label(root, 28, 392, 520, 28, u8"GPU 目标温度（50–100 °C）");
        targetValue_ = label(root, 598, 392, 90, 28, std::to_string(settings_.target) + u8" °C", 18);
        targetSlider_ = new Slider(root);
        targetSlider_->place({28, 425}).size({660, 30}).min(50).max(100).precision(1).value(settings_.target);
        label(root, 28, 470, 520, 28, u8"风扇停转温度（0–50 °C）");
        stopValue_ = label(root, 598, 470, 90, 28, std::to_string(settings_.stop) + u8" °C", 18);
        stopSlider_ = new Slider(root);
        stopSlider_->place({28, 503}).size({660, 30}).min(0).max(50).precision(1).value(settings_.stop);
        settingsNote_ = label(root, 28, 541, 660, 25, u8"设置已保存到程序目录的 settings.ini", 12, Color{91, 108, 130});
        label(root, 28, 567, 660, 22, u8"温度进度条：0–120 °C；风扇进度条：驱动报告的实际百分比。", 12, Color{91, 108, 130});
        status_ = label(root, 28, 597, 660, 28, u8"正在初始化…", 13);
        EventBus::Subscribe(targetSlider_, Slider::ON_VALUE_CHANGED, this, [this](const EventData&) { settingsChanged(); });
        EventBus::Subscribe(stopSlider_, Slider::ON_VALUE_CHANGED, this, [this](const EventData&) { settingsChanged(); });
        window->show();
        HWND hwnd = glfwGetWin32Window(window->windowId());
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon_));
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(largeIcon_));
        if (!SetWindowSubclass(hwnd, guiProcedure, 1, reinterpret_cast<DWORD_PTR>(this)))
            throw std::runtime_error("Cannot install the GUI window callback.");
        window_ = window.release();
        EventBus::Subscribe(window_, MainWindow::ON_DESTROYED, this, [this](const EventData&) { window_ = nullptr; });
        renderSnapshot();
    }
    void requestShowGui() {
        if (exiting_ || guiOpening_ || showRequested_) return;
        showRequested_ = true;
        if (window_) {
            Application::RunOnMainThread([this] {
                if (!showRequested_ || exiting_) return;
                showRequested_ = false;
                showGui();
            });
        }
        // Before the first window exists, run() consumes this flag after
        // DispatchMessageW instead of recursively entering GLFW from WndProc.
    }
    void showGui() {
        if (exiting_ || guiOpening_) return;
        guiOpening_ = true;
        struct OpeningGuard {
            bool& opening;
            ~OpeningGuard() { opening = false; }
        } openingGuard{guiOpening_};
        if (!window_) createGui();
        else {
            glfwShowWindow(window_->windowId());
            window_->restore();
            glfwFocusWindow(window_->windowId());
            renderSnapshot();
        }
        if (exiting_) { window_->close(); return; }
        SetForegroundWindow(glfwGetWin32Window(window_->windowId()));
    }
    void hideGui() { if (window_) glfwHideWindow(window_->windowId()); }
    void selectGpu(int direction) {
        if (snapshots_.empty()) return;
        selected_ = (selected_ + snapshots_.size() + direction) % snapshots_.size();
        renderSnapshot();
    }
    void receiveSnapshot() {
        { std::lock_guard<std::mutex> lock(mutex_); snapshots_ = pending_; }
        if (!snapshots_.empty()) selected_ = std::min(selected_, snapshots_.size() - 1);
        if (window_ && IsWindowVisible(glfwGetWin32Window(window_->windowId()))) renderSnapshot();
        if (!snapshots_.empty()) {
            const auto& snapshot = snapshots_[selected_];
            const std::wstring tip = L"GPU Fan Controller\n" + wide(snapshot.name + " | " + degrees(snapshot.temperatures.control()) +
                                      (snapshot.command ? " | PID " + std::to_string(*snapshot.command) + "%" : ""));
            wcsncpy_s(tray_.szTip, tip.c_str(), _TRUNCATE);
            tray_.uFlags = NIF_TIP;
            Shell_NotifyIconW(NIM_MODIFY, &tray_);
        }
    }
    static void updateTemperature(Meter meter, std::optional<double> value) {
        meter.value->text(degrees(value));
        meter.bar->progress(value ? std::clamp(int(std::lround(*value / 120.0 * 100.0)), 0, 100) : 0);
        meter.bar->alpha(value ? 1.0f : 0.3f);
    }
    void renderSnapshot() {
        if (!window_ || snapshots_.empty()) return;
        const auto& snapshot = snapshots_[selected_];
        gpuName_->text(std::to_string(selected_ + 1) + " / " + std::to_string(snapshots_.size()) + "   " + snapshot.name);
        previousGpu_->visible(snapshots_.size() > 1);
        nextGpu_->visible(snapshots_.size() > 1);
        updateTemperature(core_, snapshot.temperatures.gpu);
        updateTemperature(hotspot_, snapshot.temperatures.hotspot);
        const auto control = snapshot.temperatures.control();
        source_->text(control ? (snapshot.temperatures.usesHotspot() ? u8"控制温度：GPU 热点  " :
                                u8"控制温度：GPU 温度（热点不可用）  ") + degrees(control) : u8"控制温度：不可用");
        unsigned rpmCount = 0, percentCount = 0;
        double rpmSum = 0, percentSum = 0;
        for (const auto& reading : snapshot.fans) {
            if (reading.rpm) { ++rpmCount; rpmSum += *reading.rpm; }
            if (reading.percent) { ++percentCount; percentSum += *reading.percent; }
        }
        const int percent = percentCount ? int(std::lround(percentSum / percentCount)) : 0;
        if (rpmCount) fan_.value->text(std::to_string(int(std::lround(rpmSum / rpmCount))) + " RPM").fontSize(23);
        else fan_.value->text(percentCount ? std::to_string(percent) + "%" : u8"不可用").fontSize(27);
        fan_.bar->progress(std::clamp(percent, 0, 100)).alpha(percentCount ? 1.0f : 0.3f);
        const std::size_t rows = std::max(std::size_t(1), snapshot.fans.size());
        while (fanLabels_.size() < rows)
            fanLabels_.push_back(label(fanContent_, 14, int(fanLabels_.size()) * 30 + 8, 606, 28, "", 13));
        fanContent_->size({640, int(rows) * 30 + 16});
        for (std::size_t i = 0; i < fanLabels_.size(); ++i) {
            fanLabels_[i]->visible(i < rows);
            if (i >= rows) continue;
            if (snapshot.fans.empty()) { fanLabels_[i]->text(u8"驱动未提供风扇转速信息"); continue; }
            const auto& reading = snapshot.fans[i];
            fanLabels_[i]->text(u8"风扇 " + std::to_string(i + 1) + "     " +
                (reading.rpm ? std::to_string(*reading.rpm) + " RPM" : u8"RPM 不可用") + "     " +
                (reading.percent ? std::to_string(*reading.percent) + "%" : u8"百分比不可用"));
        }
        status_->text(snapshot.status + (snapshot.command ? u8"  | 输出 " + std::to_string(*snapshot.command) + "%" : ""))
            .textColor(snapshot.error ? Color{178, 62, 35} : Color{27, 119, 85});
        window_->commit();
    }

    bool addTrayIcon() {
        tray_ = {};
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = host_;
        tray_.uID = 1;
        tray_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = smallIcon_;
        wcscpy_s(tray_.szTip, L"GPU Fan Controller — 单击打开");
        if (!Shell_NotifyIconW(NIM_ADD, &tray_)) return false;
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
        return true;
    }
    void trayMenu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, kShowCommand, L"显示 GPU 风扇控制");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kExitCommand, L"退出并恢复驱动自动控制");
        POINT point{};
        GetCursorPos(&point);
        SetForegroundWindow(host_);
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, host_, nullptr);
        DestroyMenu(menu);
        PostMessageW(host_, WM_NULL, 0, 0);
        if (command == kShowCommand) requestShowGui();
        if (command == kExitCommand) requestExit();
    }
    void requestExit() {
        if (exiting_) return;
        exiting_ = true;
        stopWorker();
        Shell_NotifyIconW(NIM_DELETE, &tray_);
        if (window_) window_->close();
    }
    LRESULT handleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
        if (taskbarCreated_ != 0 && message == taskbarCreated_) {
            if (!addTrayIcon()) requestShowGui();
            return 0;
        }
        switch (message) {
        case kTrayMessage:
            switch (LOWORD(lparam)) {
            case NIN_SELECT: case NIN_KEYSELECT: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: requestShowGui(); break;
            case WM_CONTEXTMENU: case WM_RBUTTONUP: trayMenu(); break;
            }
            return 0;
        case kSnapshotMessage: receiveSnapshot(); return 0;
        case kShowMessage: requestShowGui(); return 0;
        case kExitMessage: case WM_CLOSE: requestExit(); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wparam) requestExit(); return 0;
        default: return DefWindowProcW(host_, message, wparam, lparam);
        }
    }
    static LRESULT CALLBACK windowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<FanApplication*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<FanApplication*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            self->host_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(hwnd, message, wparam, lparam);
        try { return self->handleMessage(message, wparam, lparam); }
        catch (const std::exception& error) {
            self->requestExit();
            MessageBoxW(hwnd, wide(error.what()).c_str(), L"GPU Fan Controller", MB_OK | MB_ICONERROR);
            return 0;
        }
    }
    static LRESULT CALLBACK guiProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
        auto* self = reinterpret_cast<FanApplication*>(data);
        if (message == WM_CLOSE || (message == WM_SYSCOMMAND && (wparam & 0xFFF0) == SC_MINIMIZE)) {
            self->hideGui();
            return 0;
        }
        if (message == WM_QUERYENDSESSION) return TRUE;
        if (message == WM_ENDSESSION && wparam) { self->requestExit(); return 0; }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, guiProcedure, 1);
        return DefSubclassProc(hwnd, message, wparam, lparam);
    }

    Application application_;
    HICON smallIcon_ = loadApplicationIcon(GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    HICON largeIcon_ = loadApplicationIcon(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    MainWindow* window_ = nullptr; // OpenGLGUI deletes a closed MainWindow.
    HWND host_ = nullptr;
    NOTIFYICONDATAW tray_{};
    UINT taskbarCreated_ = 0;
    bool exiting_ = false;
    bool showRequested_ = false, guiOpening_ = false;
    std::filesystem::path settingsPath_;
    fan::Settings settings_;
    std::mutex mutex_;
    bool startupFinished_ = false; // Protected by mutex_; checked before exposing the tray.
    std::string startupError_;
    std::condition_variable wake_;
    std::atomic<bool> stopping_{false};
    std::thread worker_;
    std::vector<GpuSnapshot> pending_, snapshots_;
    std::size_t selected_ = 0;
    Label *gpuName_{}, *source_{}, *targetValue_{}, *stopValue_{}, *settingsNote_{}, *status_{};
    Button *previousGpu_{}, *nextGpu_{};
    Slider *targetSlider_{}, *stopSlider_{};
    Spacer* fanContent_{};
    std::vector<Label*> fanLabels_;
    Meter core_, hotspot_, fan_;
};
} // namespace

int runFanApplication() {
    // Prevent two PID loops from fighting over the same physical fans.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\GPUFanController.SingleInstance");
    if (!instance) throw std::runtime_error("Cannot create the application instance lock.");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) PostMessageW(existing, kShowMessage, 0, 0);
        CloseHandle(instance);
        return 0;
    }
    try {
        FanApplication application;
        const int result = application.run();
        CloseHandle(instance);
        return result;
    } catch (...) { CloseHandle(instance); throw; }
}
