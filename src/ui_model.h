#pragma once
#include "metrics.h"
#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace pulse::ui {

enum class FullComponent : std::size_t {
    Hardware, Battery, PowerChart, Network, Processes, DiskIO, SystemDisk,
    SystemInfo, GpuMemory, Wifi, BatteryHealth, Fps, Count
};
enum class CompactComponent : std::size_t {
    Battery, Eta, Power, Cpu, Memory, Gpu, Network, Latency, DiskIO, CpuFrequency, Fps, Count
};
struct ComponentInfo {
    const wchar_t* key;
    const wchar_t* label;
    bool enabled;
    float extent; // Full: card height; compact: minimum cell width, in DIPs.
};
inline constexpr std::array<ComponentInfo, static_cast<std::size_t>(FullComponent::Count)> FullComponents{{
    {L"hardware", L"硬件四格", true, 96},
    {L"battery", L"电池与充电", true, 156},
    {L"power_chart", L"功耗走势图", false, 82},
    {L"network", L"网络", true, 142},
    {L"processes", L"进程前三", true, 98},
    {L"disk_io", L"磁盘读写速度", false, 78},
    {L"sys_disk", L"系统盘占用", false, 84},
    {L"system_info", L"系统信息", false, 114},
    {L"gpu_memory", L"显存", false, 84},
    {L"wifi", L"Wi-Fi", false, 80},
    {L"battery_health", L"电池健康", false, 78},
    {L"fps", L"帧数", false, 72}
}};
inline constexpr std::array<ComponentInfo, static_cast<std::size_t>(CompactComponent::Count)> CompactComponents{{
    {L"battery", L"电量 %", true, 68},
    {L"eta", L"续航/充满", true, 0},
    {L"power", L"总功耗", true, 89},
    {L"cpu", L"CPU %", true, 64},
    {L"memory", L"内存 %", true, 64},
    {L"gpu", L"GPU %", false, 64},
    {L"network", L"网速", true, 0},
    {L"latency", L"延迟", true, 75},
    {L"disk_io", L"磁盘读写", false, 0},
    {L"cpu_freq", L"CPU 频率", false, 89},
    {L"fps", L"FPS", false, 57}
}};
inline constexpr std::array<FullComponent, FullComponents.size()> FullOrder{{
    FullComponent::Hardware, FullComponent::Battery, FullComponent::PowerChart,
    FullComponent::Network, FullComponent::DiskIO, FullComponent::SystemDisk,
    FullComponent::SystemInfo, FullComponent::GpuMemory, FullComponent::Wifi,
    FullComponent::BatteryHealth, FullComponent::Fps, FullComponent::Processes
}};
struct Components {
    std::array<bool, FullComponents.size()> full{};
    std::array<bool, CompactComponents.size()> compact{};
    Components() { Reset(); }
    void Reset();
    bool Enabled(FullComponent c) const { return full[static_cast<std::size_t>(c)]; }
    bool Enabled(CompactComponent c) const { return compact[static_cast<std::size_t>(c)]; }
};
Wants CollectionWants(const Components& components, bool compact, PowerMode mode = PowerMode::Unknown);

struct Speed {
    std::wstring value, unit;
    bool operator==(const Speed& r) const { return value == r.value && unit == r.unit; }
};
struct TimePart {
    std::wstring value, unit;
    bool operator==(const TimePart& r) const { return value == r.value && unit == r.unit; }
};
struct Process {
    std::wstring name, percent;
    float fraction = 0;
    bool operator==(const Process& r) const {
        return name == r.name && percent == r.percent && fraction == r.fraction;
    }
};

// Canonical display values. Only enabled components are populated or compared.
struct View {
    PowerMode mode = PowerMode::Unknown;
    NetState netState = NetState::Checking;
    bool low = false;
    bool compactEtaVisible = false, compactEtaFromSystem = false;
    float battery = -1, cpu = -1, gpu = -1, memory = -1, disk = -1, wifi = -1;
    std::wstring clock, chip, scheme, percent, etaLabel, heroWord;
    std::vector<TimePart> hero;
    std::wstring wattLabel, watt, pphLabel, pph, eta20, foot;
    std::wstring totals, selfCpu, powerLabel, powerValue; // Shared by compact power and the chart.
    std::wstring compactEtaLabel, compactEtaValue;
    std::wstring cpuValue, memoryValue, packageW, cpuW, gpuW, memW, gpuUsage;
    std::wstring latency, netHint, compactUp, compactDown, diskRead, diskWrite;
    std::wstring diskPercent, diskFree, cpuFrequency, memoryDetail, processCount, uptime;
    std::wstring sharedMemory, dedicatedMemory, wifiSsid, wifiSignal, health, cycles, fps;
    bool fpsAdmin = false;
    Speed down, up, read, write;
    std::vector<Process> processes;
    std::vector<float> powerHistory; // Allocated only while the chart is visible.
    bool Equals(const View& r, bool compact, const Components& components) const;
};

class Model {
public:
    bool Update(const Snapshot& snapshot, bool compact, const Components& components);
    void BreakHistory();
    const View& Data() const { return view_; }
private:
    View view_;
    Components components_;
    bool compact_ = false, haveSample_ = false, historyBroken_ = false;
};

std::wstring Number(double value, int decimals = 1);
std::wstring Duration(double seconds);
std::wstring ShortDuration(double seconds);
} // namespace pulse::ui
