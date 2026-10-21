#include "ui_model.h"
#include "netprobe.h"
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cwchar>
#include <tuple>
#include <utility>

namespace pulse::ui {
namespace {
bool Known(double v) { return std::isfinite(v) && v >= 0; }
float Fraction(double v) {
    return Known(v) ? static_cast<float>(std::round(std::clamp(v, 0.0, 100.0)) / 100.0) : -1.0f;
}
std::vector<TimePart> Parts(double seconds) {
    if (!Known(seconds)) return {};
    const auto minutes = static_cast<long long>(std::round(std::min(seconds, 360000.0) / 60));
    if (minutes >= 6000) return {{L"99+", L"小时"}};
    if (minutes >= 60) return {{std::to_wstring(minutes / 60), L"小时"},
                             {std::to_wstring(minutes % 60), L"分"}};
    return {{std::to_wstring(minutes), L"分钟"}};
}
Speed FormatSpeed(double bps) {
    if (!Known(bps)) return {L"--", L"KB/s"};
    const wchar_t* units[] = {L"KB/s", L"MB/s", L"GB/s"};
    double n = bps / 1024;
    int i = 0;
    while (n >= 1000 && i < 2) { n /= 1024; ++i; }
    return {Number(n, n >= 100 ? 0 : n >= 10 ? 1 : i ? 2 : 1), units[i]};
}
std::wstring Bytes(uint64_t bytes) {
    const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    double n = static_cast<double>(bytes);
    int i = 0;
    while (n >= 1024 && i < 4) { n /= 1024; ++i; }
    return Number(n, i == 0 || n >= 100 ? 0 : 1) + L" " + units[i];
}
std::wstring Percent(double v, int decimals) {
    return Known(v) ? Number(std::clamp(v, 0.0, 100.0), decimals) + L"%" : L"--";
}
float ChartValue(double v) {
    return Known(v) ? static_cast<float>(std::min(v, 1.0e15)) : -1.0f;
}
} // namespace

std::wstring Number(double value, int decimals) {
    if (!Known(value)) return L"--";
    wchar_t buffer[96]{};
    std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%.*f", decimals, value);
    return buffer;
}
std::wstring Duration(double seconds) {
    auto parts = Parts(seconds);
    if (parts.empty()) return L"--";
    std::wstring result;
    for (const auto& p : parts) {
        if (!result.empty()) result += L" ";
        result += p.value + L" " + p.unit;
    }
    return result;
}
std::wstring ShortDuration(double seconds) {
    if (!Known(seconds)) return L"--";
    const auto m = static_cast<int>(std::round(std::min(seconds, 360000.0) / 60));
    if (m >= 6000) return L"99+";
    wchar_t buffer[32]{};
    std::swprintf(buffer, 32, L"%d:%02d", m / 60, m % 60);
    return buffer;
}

void Components::Reset() {
    for (size_t i = 0; i < full.size(); ++i) full[i] = FullComponents[i].enabled;
    for (size_t i = 0; i < compact.size(); ++i) compact[i] = CompactComponents[i].enabled;
}
Wants CollectionWants(const Components& c, bool compact, PowerMode mode) {
    Wants w;
    w.chip_power = compact ? c.Enabled(CompactComponent::Power) && mode != PowerMode::Discharging :
        c.Enabled(FullComponent::Hardware) || (c.Enabled(FullComponent::PowerChart) && mode != PowerMode::Discharging);
    w.gpu_usage = compact ? c.Enabled(CompactComponent::Gpu) : c.Enabled(FullComponent::Hardware);
    w.processes = !compact && (c.Enabled(FullComponent::Processes) || c.Enabled(FullComponent::SystemInfo));
    w.memory_detail = !compact && c.Enabled(FullComponent::SystemInfo);
    w.disk_io = compact ? c.Enabled(CompactComponent::DiskIO) : c.Enabled(FullComponent::DiskIO);
    w.sys_disk = !compact && c.Enabled(FullComponent::SystemDisk);
    w.cpu_freq = compact ? c.Enabled(CompactComponent::CpuFrequency) : c.Enabled(FullComponent::SystemInfo);
    w.uptime = !compact && c.Enabled(FullComponent::SystemInfo);
    w.gpu_memory = !compact && c.Enabled(FullComponent::GpuMemory);
    w.wifi = !compact && c.Enabled(FullComponent::Wifi);
    w.battery_health = !compact && c.Enabled(FullComponent::BatteryHealth);
    return w;
}

bool View::Equals(const View& r, bool compact, const Components& c) const {
    if (compact) {
        if (c.Enabled(CompactComponent::Battery) &&
            std::tie(battery, percent) != std::tie(r.battery, r.percent)) return false;
        if (c.Enabled(CompactComponent::Power) &&
            std::tie(mode, low, powerLabel, powerValue) !=
            std::tie(r.mode, r.low, r.powerLabel, r.powerValue)) return false;
        if (c.Enabled(CompactComponent::Cpu) && cpuValue != r.cpuValue) return false;
        if (c.Enabled(CompactComponent::Memory) && memoryValue != r.memoryValue) return false;
        if (c.Enabled(CompactComponent::Gpu) && gpuUsage != r.gpuUsage) return false;
        if (c.Enabled(CompactComponent::Network) &&
            std::tie(compactUp, compactDown) != std::tie(r.compactUp, r.compactDown)) return false;
        if (c.Enabled(CompactComponent::Latency) &&
            std::tie(netState, latency) != std::tie(r.netState, r.latency)) return false;
        if (c.Enabled(CompactComponent::DiskIO) &&
            std::tie(diskRead, diskWrite) != std::tie(r.diskRead, r.diskWrite)) return false;
        if (c.Enabled(CompactComponent::CpuFrequency) && cpuFrequency != r.cpuFrequency) return false;
        if (c.Enabled(CompactComponent::Fps) && fps != r.fps) return false;
        return true;
    }
    if (clock != r.clock) return false; // Always-visible title clock.
    if (c.Enabled(FullComponent::Hardware) &&
        std::tie(cpu, gpu, memory, cpuValue, gpuUsage, memoryValue, packageW, cpuW, gpuW, memW) !=
        std::tie(r.cpu, r.gpu, r.memory, r.cpuValue, r.gpuUsage, r.memoryValue, r.packageW, r.cpuW, r.gpuW, r.memW)) return false;
    if (c.Enabled(FullComponent::Battery) &&
        std::tie(mode, low, battery, percent, chip, scheme, etaLabel, heroWord, hero, wattLabel,
                 watt, pphLabel, pph, eta20, foot) !=
        std::tie(r.mode, r.low, r.battery, r.percent, r.chip, r.scheme, r.etaLabel, r.heroWord, r.hero, r.wattLabel,
                 r.watt, r.pphLabel, r.pph, r.eta20, r.foot)) return false;
    if (c.Enabled(FullComponent::PowerChart) &&
        std::tie(mode, powerLabel, powerValue, powerHistory) !=
        std::tie(r.mode, r.powerLabel, r.powerValue, r.powerHistory)) return false;
    if (c.Enabled(FullComponent::Network) &&
        std::tie(netState, down, up, totals, latency, netHint) !=
        std::tie(r.netState, r.down, r.up, r.totals, r.latency, r.netHint)) return false;
    if (c.Enabled(FullComponent::Processes) &&
        std::tie(processes, selfCpu) != std::tie(r.processes, r.selfCpu)) return false;
    if (c.Enabled(FullComponent::DiskIO) && std::tie(read, write) != std::tie(r.read, r.write)) return false;
    if (c.Enabled(FullComponent::SystemDisk) &&
        std::tie(disk, diskPercent, diskFree) != std::tie(r.disk, r.diskPercent, r.diskFree)) return false;
    if (c.Enabled(FullComponent::SystemInfo) &&
        std::tie(cpuFrequency, memoryDetail, processCount, uptime) !=
        std::tie(r.cpuFrequency, r.memoryDetail, r.processCount, r.uptime)) return false;
    if (c.Enabled(FullComponent::GpuMemory) &&
        std::tie(sharedMemory, dedicatedMemory) != std::tie(r.sharedMemory, r.dedicatedMemory)) return false;
    if (c.Enabled(FullComponent::Wifi) &&
        std::tie(wifi, wifiSsid, wifiSignal) != std::tie(r.wifi, r.wifiSsid, r.wifiSignal)) return false;
    if (c.Enabled(FullComponent::BatteryHealth) &&
        std::tie(health, cycles) != std::tie(r.health, r.cycles)) return false;
    return true;
}

namespace {
std::wstring CompactSpeed(double bps) {
    if (!Known(bps)) return L"--";
    if (bps == 0) return L"0";
    double value = bps / 1024;
    const wchar_t* suffix = L"K";
    if (value >= 1024) { value /= 1024; suffix = L"M"; }
    if (value >= 1024) { value /= 1024; suffix = L"G"; }
    return Number(value, value < 100 ? 1 : 0) + suffix;
}
std::wstring Uptime(double seconds) {
    if (!Known(seconds)) return L"--";
    const auto minutes = static_cast<unsigned long long>(std::min(seconds, 3.0e11) / 60);
    std::wstring value;
    if (minutes >= 1440) value = std::to_wstring(minutes / 1440) + L" 天 ";
    return value + std::to_wstring(minutes / 60 % 24) + L" 时 " + std::to_wstring(minutes % 60) + L" 分";
}
}
void Model::BreakHistory() {
    std::fill(view_.powerHistory.begin(), view_.powerHistory.end(), -1);
    historyBroken_ = true;
}
bool Model::Update(const Snapshot& s, bool compact, const Components& c) {
    View v;
    const bool battery = compact ? c.Enabled(CompactComponent::Battery) : c.Enabled(FullComponent::Battery);
    const bool power = compact && c.Enabled(CompactComponent::Power);
    const bool chart = !compact && c.Enabled(FullComponent::PowerChart);
    const bool hardware = !compact && c.Enabled(FullComponent::Hardware);
    const bool network = !compact && c.Enabled(FullComponent::Network);
    const bool latency = network || (compact && c.Enabled(CompactComponent::Latency));
    const bool discharging = s.mode == PowerMode::Discharging;
    const bool charging = s.mode == PowerMode::Charging;
    if (battery || power || chart) v.mode = s.mode;
    if (battery || power) v.low = discharging && s.percent >= 0 && s.percent <= 20;
    if (battery) {
        v.battery = s.percent < 0 || s.mode == PowerMode::NoBattery ? -1 : Fraction(s.percent);
        v.percent = v.battery < 0 ? L"--" : std::to_wstring(std::clamp(s.percent, 0, 100));
    }
    if (power || chart) {
        const double watts = discharging ? s.power_w : s.pkg_w;
        v.powerLabel = discharging ? L"整机" : L"处理器";
        v.powerValue = Number(watts, watts >= 100 ? 0 : 1);
    }
    if (hardware || (compact && c.Enabled(CompactComponent::Cpu))) v.cpuValue = Percent(s.cpu_percent, 0);
    if (hardware || (compact && c.Enabled(CompactComponent::Memory))) v.memoryValue = Percent(s.mem_percent, 0);
    if (hardware || (compact && c.Enabled(CompactComponent::Gpu))) v.gpuUsage = Percent(s.gpu_percent, 0);
    if (latency) {
        v.netState = s.net_state;
        v.latency = s.net_state == NetState::Offline ? L"断网" :
                    s.net_state == NetState::Checking ? L"检测中" :
                    Known(s.latency_ms) ? Number(s.latency_ms, 0) + L" ms" : L"-- ms";
    }
    if (compact) {
        if (c.Enabled(CompactComponent::Network)) {
            v.compactUp = L"↑ " + CompactSpeed(s.up_bps);
            v.compactDown = L"↓ " + CompactSpeed(s.down_bps);
        }
        if (c.Enabled(CompactComponent::DiskIO)) {
            v.diskRead = L"读 " + CompactSpeed(s.disk_read_bps);
            v.diskWrite = L"写 " + CompactSpeed(s.disk_write_bps);
        }
        if (c.Enabled(CompactComponent::CpuFrequency)) v.cpuFrequency = Number(s.cpu_mhz / 1000, 2) + L" GHz";
        if (c.Enabled(CompactComponent::Fps)) v.fps = Number(s.fps, 0);
    } else {
        if (hardware) {
            v.cpu = Fraction(s.cpu_percent); v.gpu = Fraction(s.gpu_percent); v.memory = Fraction(s.mem_percent);
            v.packageW = Number(s.pkg_w); v.cpuW = Number(s.cpu_w); v.gpuW = Number(s.gpu_w); v.memW = Number(s.mem_w);
        }
        if (battery) {
            v.chip = v.low ? L"电量低" : discharging ? L"放电中" : charging ? L"充电中" :
                     s.mode == PowerMode::AcIdle ? L"外接电源" : s.mode == PowerMode::NoBattery ? L"无电池" : L"读取中…";
            v.scheme = s.battery_saver ? L"节电模式已开启" : s.power_scheme.empty() ? L"" : L"电源模式 · " + s.power_scheme;
            v.etaLabel = discharging ? L"预计可用" : charging ? L"预计充满" : L"电池状态";
            if (s.eta_from_system && discharging) v.etaLabel += L"（系统估计）";
            if (discharging || charging) {
                v.hero = Parts(s.eta_s);
                if (v.hero.empty()) v.heroWord = L"计算中…";
            } else if (s.mode == PowerMode::AcIdle) v.heroWord = s.percent >= 99 ? L"已充满" : L"电源供电";
            else v.heroWord = s.mode == PowerMode::NoBattery ? L"未检测到电池" : L"读取中…";
            v.wattLabel = charging ? L"充电功率" : discharging ? L"实时功耗" :
                          s.mode == PowerMode::NoBattery ? L"功耗" : L"电池功率";
            v.watt = Number(s.power_w, s.power_w >= 100 ? 0 : 1);
            v.pphLabel = charging ? L"每小时充入" : discharging ? L"每小时耗电" : L"每小时";
            v.pph = Number(s.pct_per_hour, s.pct_per_hour >= 10 ? 0 : 1);
            v.eta20 = discharging && Known(s.eta20_s) ? ShortDuration(s.eta20_s) : L"";
            if (discharging && Known(s.on_battery_s)) {
                v.foot = (s.on_battery_since_start ? L"组件启动后已用 " : L"拔电后已用 ") + Duration(s.on_battery_s);
                if (Known(s.avg_w)) v.foot += L" · 平均 " + Number(s.avg_w) + L" W";
            } else if (Known(s.remaining_wh) && Known(s.full_wh) && s.full_wh > 0)
                v.foot = L"剩余 " + Number(s.remaining_wh) + L" Wh / 满电 " + Number(s.full_wh) + L" Wh";
        }
        if (chart) {
            const bool sameSource = (view_.mode == PowerMode::Discharging) == discharging;
            if (components_.Enabled(FullComponent::PowerChart) && !compact_ && sameSource && view_.powerHistory.size() == 120)
                v.powerHistory = view_.powerHistory;
            else v.powerHistory.assign(120, -1);
            std::move(v.powerHistory.begin() + 1, v.powerHistory.end(), v.powerHistory.begin());
            v.powerHistory.back() = ChartValue(discharging ? s.power_w : s.pkg_w);
        }
        if (network) {
            v.down = FormatSpeed(s.down_bps); v.up = FormatSpeed(s.up_bps);
            v.totals = L"本次 ↓ " + Bytes(s.rx_total) + L" · ↑ " + Bytes(s.tx_total);
            if (s.net_state == NetState::Offline) {
                v.netHint = s.offline_by_latency ? L"延迟过高，网络不可用" : L"网络连接已断开";
                if (s.offline_since_unix > 0) {
                    const auto elapsed = std::max(0.0, std::difftime(std::time(nullptr), static_cast<std::time_t>(s.offline_since_unix)));
                    v.netHint = L"已断开 " + FormatOutageDuration(static_cast<std::uint64_t>(elapsed));
                    if (s.offline_by_latency) v.netHint += L" · 延迟过高";
                }
            } else if (s.net_state == NetState::Slow) v.netHint = L"网络较慢 · 连接可用";
            else if (s.net_state == NetState::Online) v.netHint = L"网络正常";
            else v.netHint = L"正在检测连接…";
        }
        if (c.Enabled(FullComponent::Processes)) {
            v.selfCpu = Known(s.self_cpu_percent) ? L"本组件 " + Percent(s.self_cpu_percent, 1) : L"";
            const double top = !s.top_procs.empty() && Known(s.top_procs.front().cpu_percent)
                                   ? std::max(s.top_procs.front().cpu_percent, 5.0) : 5.0;
            for (size_t i = 0; i < std::min(size_t(3), s.top_procs.size()); ++i) {
                const auto& p = s.top_procs[i];
                v.processes.push_back({p.name.empty() ? L"--" : p.name, Percent(p.cpu_percent, 1),
                    Known(p.cpu_percent) ? static_cast<float>(std::clamp(p.cpu_percent / top, 0.0, 1.0)) : 0.0f});
            }
        }
        if (c.Enabled(FullComponent::DiskIO)) { v.read = FormatSpeed(s.disk_read_bps); v.write = FormatSpeed(s.disk_write_bps); }
        if (c.Enabled(FullComponent::SystemDisk)) {
            v.disk = Fraction(s.sys_disk_used_percent); v.diskPercent = Percent(s.sys_disk_used_percent, 0);
            v.diskFree = Number(s.sys_disk_free_gb, 1) + L" GB";
        }
        if (c.Enabled(FullComponent::SystemInfo)) {
            v.cpuFrequency = Number(s.cpu_mhz / 1000, 2) + L" GHz";
            v.memoryDetail = Number(s.mem_used_gb, 1) + L" / " + Number(s.mem_total_gb, 1) + L" GB";
            v.processCount = s.process_count < 0 ? L"--" : std::to_wstring(s.process_count);
            v.uptime = Uptime(s.uptime_s);
        }
        if (c.Enabled(FullComponent::GpuMemory)) {
            v.sharedMemory = Number(s.gpu_shared_mem_mb / 1024, 2) + L" GB";
            v.dedicatedMemory = Number(s.gpu_dedicated_mem_mb / 1024, 2) + L" GB";
        }
        if (c.Enabled(FullComponent::Wifi)) {
            v.wifiSsid = s.wifi_ssid.empty() ? L"--" : s.wifi_ssid;
            v.wifi = s.wifi_signal < 0 ? -1 : Fraction(s.wifi_signal);
            v.wifiSignal = s.wifi_signal < 0 ? L"--" : Percent(s.wifi_signal, 0);
        }
        if (c.Enabled(FullComponent::BatteryHealth)) {
            v.health = Known(s.battery_health_percent) ? Number(s.battery_health_percent, 1) + L"%" : L"--";
            v.cycles = s.battery_cycles < 0 ? L"--" : std::to_wstring(s.battery_cycles);
        }
        std::time_t now = std::time(nullptr);
        std::tm local{};
#if defined(_MSC_VER)
        localtime_s(&local, &now);
#else
        if (const auto* t = std::localtime(&now)) local = *t;
#endif
        wchar_t clock[16]{};
        std::swprintf(clock, 16, L"%02d:%02d", local.tm_hour, local.tm_min);
        v.clock = clock;
    }
    const bool selectionChanged = compact_ != compact ||
        (compact ? components_.compact != c.compact : components_.full != c.full);
    const bool changed = !haveSample_ || historyBroken_ || selectionChanged || !v.Equals(view_, compact, c);
    view_ = std::move(v); components_ = c; compact_ = compact;
    haveSample_ = true; historyBroken_ = false;
    return changed;
}
} // namespace pulse::ui
