# GPUFanController

Windows NVIDIA 显卡风扇控制程序，使用仓库内 `gui_lib` 的 OpenGLGUI 构建中文界面。

## 使用

- 启动时逐卡验证风扇实际控制权限：以不低于已读取当前转速百分比的统一输出测试所有风扇，并立即恢复自动策略。验证成功后才显示托盘图标；没有权限、驱动不支持控制或恢复失败时，弹窗报错并退出，不保留后台控制进程。单击托盘图标打开界面。
- 实时显示 GPU 温度、GPU 热点温度、风扇平均 RPM，以及每个风扇的 RPM 和实际百分比。温度和风扇状态同时用文字与进度条表示，每秒采样一次。
- 目标温度可设为 **50–100 °C**，停转温度可设为 **0–50 °C**，步长为 1 °C。默认分别为 **75 °C / 40 °C**。
- 温控始终优先使用有效的 **GPU 热点温度**，热点不可用才回退到 GPU 温度；两者都不可用时恢复驱动自动控制。界面明确标出正在使用的温度来源。
- 同一显卡的所有风扇接收相同的 PID 输出百分比，不提供固定转速功能。实际 RPM 由风扇硬件决定，可能略有差异。
- 多显卡分别运行独立 PID，使用同一组温度设置；通过显卡名称右侧箭头切换查看。
- 关闭或最小化主窗口会隐藏到托盘，控制继续运行；右键托盘图标选择“退出并恢复驱动自动控制”退出。
- 设置即时生效，全部保存到 **EXE 同目录的 `settings.ini`**；启动时自动创建默认配置，读取配置使用可执行文件的绝对路径，不受工作目录影响。目录不可写时会明确报错，不回退到 AppData。重复启动会显示已有窗口，避免多个控制循环争用风扇。

温度进度条范围为 0–120 °C，超过上限仍显示真实温度文字。风扇进度条使用驱动报告的实际百分比；PID 输出单独显示，未用指令值冒充转速。不可读取的数据标为“不可用”。

## PID 与停转

控制器包含比例、积分和微分三项，默认 `Kp=4.0`、`Ki=0.25`、`Kd=2.0`，输出单位为风扇百分比。使用条件积分防止饱和积累，并对温度微分进行 3 秒低通滤波。设置或温度来源变化时清除历史，休眠后的采样间隔限幅，避免异常积分跳变。

温度不高于停转阈值时输出 0%；升温达到停转阈值加 2 °C 时重新启动，重启阈值不高于目标温度。运行时最低输出为驱动报告的所有风扇最低值与 30% 的较大值。这个下限只保证运行区间，不影响 0% 停转请求。

## 构建

当前 `gui_lib` 是 Windows x64 MinGW ABI 的预编译库，使用兼容的 LLVM-MinGW 工具链、CMake 3.20+ 和 Ninja：

```powershell
cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build-gui -j 4
ctest --test-dir build-gui --output-on-failure
.\build-gui\GPUFanController.exe
```

主程序静态链接 GUI 与 MinGW C++ 运行库，不需要额外复制 `libOpenGLGUI.dll`、`libc++.dll`。NVIDIA 接口从已安装的显示驱动动态加载，无需 CUDA SDK。正式使用启动 `build-gui/GPUFanController.exe`；`fixtures` 和 `gui-smoke` 目录是隔离的模拟驱动测试环境。

## 应用图标

程序使用蓝色圆角底、白色三叶风扇与青色轴心图标。EXE 文件、窗口标题栏、任务栏和托盘使用同一图标；ICO 内含 16、20、24、32、48、64、128、256 像素版本，并直接嵌入可执行文件，无需在运行时附带图标文件。

源文件位于 assets/app-icon.svg，预览为 assets/app-icon.png，Windows 资源为 assets/app-icon.ico。修改 tools/generate_icon.py 后运行以下命令可重新生成全部尺寸；脚本仅使用 Python 标准库，正常构建不需要运行它：

    python tools/generate_icon.py

## 测试

- `controller_tests`：温度范围、热点优先及回退、停转回差、PID 的积分与微分、饱和恢复、传感器异常与休眠间隔。
- `nvidia_gpu_tests`：使用测试 DLL 检查接口结构、实际转速、NVML/NVAPI 回退、启动权限探测、同步输出、部分写入失败和恢复重试。
- `gui_smoke`：Python 标准库加 Win32 自动化，检查托盘默认启动、连续点击/双击及同步激活消息、滑块端点、EXE 同目录保存和重启读取、不同工作目录启动、最小化/关闭隐藏、单实例、同卡同步输出和退出恢复。覆盖首次创建窗口时的消息重入，防止 GLFW 重复注册窗口类。需要交互式 Windows 桌面；测试使用独立目录下的模拟驱动，不调整真实显卡。
- `startup_denied_10/11`：模拟全部风扇无权限、部分风扇无权限，检查中文错误弹窗、已写入风扇回滚和确认后退出。

## 驱动限制

不同显卡/驱动暴露的传感器、风扇控制和停转能力不同。程序会显示不可用或驱动错误；需要权限时应使用管理员权限运行。笔记本风扇可能由 OEM 固件管理。

热点与多风扇 NVAPI 私有接口的结构及查询编号参考 LibreHardwareMonitor 的原始实现：

- https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Interop/NvApi.cs
- https://github.com/LibreHardwareMonitor/LibreHardwareMonitor/blob/master/LibreHardwareMonitorLib/Hardware/Gpu/NvidiaGpu.cs
- NVML ABI：https://github.com/NVIDIA/go-nvml/blob/main/pkg/nvml/nvml.h

RTX 50 / Blackwell 的此 NVAPI 通道是核心温度，程序不会把它伪装成热点，而是标记热点不可用并采用 GPU 温度。未引入内核驱动读取额外传感器。

正常退出与系统会话结束时会恢复自动控制；系统强行终止进程时无法保证执行清理。
