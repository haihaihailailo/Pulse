// metrics.h —— Pulse 原生版：后端数据模块与界面之间的唯一接口。
// 后端（metrics.cpp / netprobe.cpp，由 Grok 实现）只负责采样，界面（Codex 实现）只读 Snapshot。
// 约定：数值未知时用 NaN（double）或 -1（整数），界面显示 "--"。
#pragma once
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace pulse {

enum class PowerMode { Unknown, NoBattery, Discharging, Charging, AcIdle };   // AcIdle = 插着电但未充放电（已充满等）
enum class NetState { Checking, Online, Slow, Offline };

struct ProcUsage {
    std::wstring name;     // 去掉 .exe
    double cpu_percent;    // 占全部逻辑核心的百分比（同任务管理器）
};

struct Snapshot {
    // —— 电池 ——
    PowerMode mode = PowerMode::Unknown;
    int percent = -1;                 // 0..100
    double power_w = NAN;             // 实时充/放电功率，正值
    double eta_s = NAN;               // 放电：预计剩余；充电：预计充满
    bool eta_from_system = false;     // eta 来自 Windows 估计（本程序平滑值尚未就绪时兜底）
    double eta20_s = NAN;             // 放电时到 20% 的时间
    double pct_per_hour = NAN;        // 每小时耗电 / 充入 %
    double on_battery_s = NAN;        // 拔电后经过的时间
    bool on_battery_since_start = false;
    double avg_w = NAN;               // 拔电后平均功率（只计实测秒数）
    double remaining_wh = NAN, full_wh = NAN;
    bool battery_saver = false;
    std::wstring power_scheme;        // "平衡" / "最佳性能" / "最佳能效" / "更长续航" / 空

    // —— 芯片功耗（Energy Meter，无需管理员）——
    double pkg_w = NAN, cpu_w = NAN, gpu_w = NAN;
    double mem_w = NAN;  // 内存功耗（RAPL DRAM，无此实例则 NaN）
    double gpu_percent = NAN;         // GPU 3D 引擎占用

    // —— 网络 ——
    double down_bps = 0, up_bps = 0;  // 字节/秒，只统计物理网卡
    uint64_t rx_total = 0, tx_total = 0;  // 本次启动以来
    NetState net_state = NetState::Checking;
    double latency_ms = NAN;          // 最近一次探测延迟（失败为 NaN）
    int64_t offline_since_unix = 0;   // 断网开始时间（0 = 未断网）
    bool offline_by_latency = false;  // 断网原因是延迟过高

    // —— CPU / 内存 / 进程 ——
    double cpu_percent = 0, mem_percent = 0;
    std::vector<ProcUsage> top_procs; // 最多 3 个，不含本程序
    double self_cpu_percent = NAN;    // 本程序自身

    // —— 可选指标（未启用或未知时为 NaN / -1 / 空）——
    double mem_used_gb = NAN, mem_total_gb = NAN;
    double disk_read_bps = NAN, disk_write_bps = NAN;   // 所有物理磁盘合计，字节/秒
    double sys_disk_used_percent = NAN, sys_disk_free_gb = NAN;  // 系统盘
    double cpu_mhz = NAN;                 // CPU 当前频率（MHz）
    int process_count = -1;
    double uptime_s = NAN;                // 开机时长
    double gpu_shared_mem_mb = NAN;       // 核显共享显存占用（GPU Adapter Memory）
    double gpu_dedicated_mem_mb = NAN;    // 独显专用显存占用（无独显则 NaN）
    std::wstring wifi_ssid;               // 未连 Wi-Fi 为空
    int wifi_signal = -1;                 // 0..100
    double battery_health_percent = NAN;  // 满充容量 / 设计容量 × 100
    int battery_cycles = -1;
    double fps = NAN;                     // 预留，本轮不实现，保持 NaN

    // 追加的诊断数据，仅完整模式的有效 package Energy 区间有值。
    // Energy 原始单位为 pWh；pkg_w = delta_pwh * 3.6e-9 / interval_s。
    bool pkg_power_from_energy = false;  // false 时为 Power 回退/迷你采样或未知
    double pkg_energy_delta_pwh = NAN, pkg_energy_interval_s = NAN;
    // 完整模式同次采集的 Power 对照值（mW / 1000），用于实测核验 Energy 单位。
    double pkg_power_counter_w = NAN;
};

// 网络探测事件（断网 / 恢复），由后端在探测线程里回调；界面负责弹提醒卡片、播放提示音。
struct NetEvent {
    bool recovered;                   // true = 恢复，false = 断开
    std::wstring message;             // 已格式化的中文文案
};

struct Settings {
    int probe_interval_s = 5, probe_timeout_s = 4, fail_threshold = 3;
    int slow_ms = 300, unusable_ms = 1000;
    std::vector<std::wstring> probe_urls;  // 空 = 默认三个地址
};

// 界面告诉后端需要哪些可选数据；不需要的完全不采集，以省电。
struct Wants {
    bool chip_power = true;   // Energy Meter：pkg/cpu/gpu/mem 功耗
    bool gpu_usage = true;    // GPU 占用（GPU Engine）
    bool processes = true;    // 进程前三 + process_count
    bool memory_detail = false;   // mem_used_gb / mem_total_gb
    bool disk_io = false;
    bool sys_disk = false;
    bool cpu_freq = false;
    bool uptime = false;
    bool gpu_memory = false;
    bool wifi = false;
    bool battery_health = false;
};

class Metrics {
public:
    // event_cb 在后台线程调用，界面需自行 PostMessage 回 UI 线程。
    using EventCallback = void (*)(const NetEvent&, void* user);

    Metrics();
    ~Metrics();                       // 必须能快速、安全地停止探测线程
    Metrics(const Metrics&) = delete;
    Metrics& operator=(const Metrics&) = delete;

    void Start(const Settings& s, EventCallback cb, void* user);
    // 界面每次刷新前调用一次（完整模式约 1 秒，迷你模式约 2 秒）。full=false 时跳过进程扫描、GPU 占用等较贵的项。
    // 可选指标由 SetWants 控制；未开启的项保持 NaN / -1 / 空。
    // 必须快（目标 < 1 ms，进程扫描那次 < 10 ms），不得阻塞在网络上。
    Snapshot Sample(bool full);
    // 线程安全，可随时调用。关掉的项从下一次 Sample 起变为 NaN / -1 / 空，并释放对应 PDH 计数器等资源。
    void SetWants(const Wants& w);
    void SetPaused(bool paused);      // 窗口最小化/隐藏时暂停采样（网络探测继续，否则断网检测失效）
    std::wstring LogPath() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace pulse
