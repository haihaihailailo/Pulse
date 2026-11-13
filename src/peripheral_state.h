#pragma once
#include <algorithm>
#include <string>
#include <vector>
namespace pulse {
struct PeripheralBattery {
    std::wstring id, name;
    int percent = -1;
    bool operator==(const PeripheralBattery& r) const { return id == r.id && name == r.name && percent == r.percent; }
};
enum class PeripheralState { Reading, Ready, Unavailable };
struct PeripheralSnapshot {
    PeripheralState state = PeripheralState::Reading;
    std::vector<PeripheralBattery> devices;
};
namespace peripheral {
inline int Percentage(unsigned value) { return value <= 100 ? static_cast<int>(value) : -1; }
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
    if (it->percent < 0 && row.percent >= 0) *it = std::move(row);
}
}
}
