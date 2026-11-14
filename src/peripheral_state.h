#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace pulse {
enum class BatteryStatus { Unknown, Unsupported, Disconnected, Sleeping, Unavailable };
struct PeripheralBattery {
    std::wstring id, name;
    int percent = -1;
    BatteryStatus status = BatteryStatus::Unknown;
    std::wstring source, detail;
    int left = -1, right = -1, chargingCase = -1;
    bool charging = false;
    bool operator==(const PeripheralBattery& r) const {
        return id == r.id && name == r.name && percent == r.percent && status == r.status &&
            source == r.source && detail == r.detail && left == r.left && right == r.right &&
            chargingCase == r.chargingCase && charging == r.charging;
    }
};
enum class PeripheralState { Reading, Ready, Unavailable };
struct PeripheralSnapshot {
    PeripheralState state = PeripheralState::Reading;
    std::vector<PeripheralBattery> devices;
    unsigned long long sampledAt = 0;
};
namespace peripheral {
inline int Percentage(unsigned value) { return value <= 100 ? static_cast<int>(value) : -1; }
inline const wchar_t* StatusText(BatteryStatus status, bool english) {
    switch(status) {
    case BatteryStatus::Unsupported: return english ? L"Unsupported" : L"暂不支持";
    case BatteryStatus::Disconnected: return english ? L"Offline" : L"未连接";
    case BatteryStatus::Sleeping: return english ? L"Asleep" : L"休眠";
    case BatteryStatus::Unavailable: return english ? L"Unavailable" : L"读取失败";
    default: return english ? L"No reading" : L"待获取";
    }
}
inline std::wstring ComponentsText(const PeripheralBattery& row, bool english) {
    if (row.left < 0 && row.right < 0 && row.chargingCase < 0) return {};
    const auto value=[](int n){return n < 0 ? std::wstring(L"--") : std::to_wstring(n)+L"%";};
    return std::wstring(english?L"L ":L"左 ")+value(row.left)+(english?L"  R ":L"  右 ")+
        value(row.right)+(english?L"  Case ":L"  盒 ")+value(row.chargingCase);
}
inline std::wstring CleanName(std::wstring name) {
    if (name.size() > 96) name.resize(96);
    for (auto& c : name) if (c < 32 || (c >= 0x202a && c <= 0x202e)) c = L' ';
    return name;
}
inline void Merge(std::vector<PeripheralBattery>& rows, PeripheralBattery row) {
    row.name = CleanName(std::move(row.name));
    if (row.id.empty() || row.name.empty()) return;
    auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& r) { return r.id == row.id; });
    if (it == rows.end()) { if (rows.size() < 32) rows.push_back(std::move(row)); return; }
    if ((it->percent < 0 && row.percent >= 0) || row.left >= 0 || row.right >= 0) {
        // Preserve the physical device name when a service node supplies data.
        auto name=it->name; *it=std::move(row); it->name=std::move(name);
    } else if (it->percent < 0 && !row.detail.empty()) {
        it->status=row.status; it->detail=std::move(row.detail); it->source=std::move(row.source);
    }
}
}
}
