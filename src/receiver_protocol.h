#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace pulse::peripheral {
// Nordic receiver battery command used by the ATK/VGN Hub. Only confirmed
// receiver IDs are eligible; sharing a vendor ID does not imply compatibility.
inline const wchar_t* ReceiverName(unsigned vid, unsigned pid) {
    if (vid == 0x3554) {
        switch (pid) {
        case 0xf503: case 0xf505: case 0xf50b: return L"VGN 无线鼠标";
        case 0xf58a: case 0xf58d: case 0xf58e: return L"VXE 无线鼠标";
        }
    }
    if (vid == 0x373b) {
        switch (pid) {
        case 0x1031: case 0x11d9: return L"ATK 无线鼠标";
        case 0x104d: case 0x1040: return L"VXE MAD R";
        }
    }
    return nullptr;
}
inline constexpr std::array<uint8_t,17> BatteryRequest() {
    return {8,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x49};
}
inline int ReceiverPercentage(const uint8_t* data, size_t length) {
    if (length < 17 || data[0] != 8 || data[1] != 4 || data[2] != 0 || data[6] > 100) return -1;
    unsigned sum = 0;
    for (size_t i=0; i<17; ++i) sum += data[i];
    return (sum & 255) == 0x55 ? data[6] : -1;
}
}
