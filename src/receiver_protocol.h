#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cwctype>
#include <string>

namespace pulse::peripheral {
enum class ReceiverFamily { Nordic52, Nordic54, VgnFeature, VgnMouseEnc };
struct ReceiverSpec { const wchar_t* name; ReceiverFamily family; bool wired; };
inline ReceiverSpec Receiver(unsigned vid, unsigned pid) {
    if(vid==0x391d) switch(pid) {
        // Dragonfly 3 Master / Master+: MouseEnc report 8, not VgnRy Feature.
        // The PID does not reliably identify connection mode across firmware.
        case 0x1005: case 0x1a05:
            return {L"VGN 蜻蜓 3 大师版 / 大师版+",ReceiverFamily::VgnMouseEnc,false};
        case 0x1004: case 0x1a04: case 0x1008: case 0x1a08: case 0x1028: case 0x1a25: case 0x102f: case 0x1a2b:
            return {L"VGN 无线鼠标",ReceiverFamily::VgnFeature,false};
    }
    if (vid==0x373b && (pid==0x1155 || pid==0x124f || pid==0x1154))
        return {L"ATK Zero",ReceiverFamily::Nordic54,pid==0x1154};
    if (vid==0x3554) {
        switch(pid) {
        case 0xf502: case 0xf503: case 0xf505: case 0xf50a: case 0xf50b:
        case 0xfb3d: case 0xfb3e: case 0xfb53: case 0xfb52: case 0xfb55: case 0xfb54:
        case 0xfb57: case 0xfb56: case 0xfb51: case 0xfb50: case 0xfb59: case 0xfb58:
            return {L"VGN 无线鼠标",ReceiverFamily::Nordic52,
                pid==0xf502 || pid==0xf50a || pid==0xfb3d || pid==0xfb53 || pid==0xfb55 || pid==0xfb57 || pid==0xfb51 || pid==0xfb59};
        case 0xf58a: case 0xf58c: case 0xf58d: case 0xf58e: case 0xf58f:
            return {L"VXE 无线鼠标",ReceiverFamily::Nordic52,pid==0xf58c || pid==0xf58f};
        }
    }
    if (vid==0x373b) {
        switch(pid) {
        case 0x1031: case 0x102e: case 0x11d9: case 0x11b6:
            return {L"ATK 无线鼠标",ReceiverFamily::Nordic52,pid==0x102e || pid==0x11b6};
        case 0x104d: case 0x1040: case 0x103f: case 0x104c: case 0x1085:
            return {L"VXE 无线鼠标",ReceiverFamily::Nordic52,pid==0x103f || pid==0x104c};
        }
    }
    return {nullptr,ReceiverFamily::Nordic52,false};
}
// Nordic receiver battery command used by the ATK/VGN Hub. Only confirmed
// receiver IDs are eligible; sharing a vendor ID does not imply compatibility.
inline const wchar_t* ReceiverName(unsigned vid, unsigned pid) {
    return Receiver(vid,pid).name;
}
inline bool DiscoverReceiver(unsigned vid, unsigned pid, std::wstring name) {
    if(ReceiverName(vid,pid) || vid==0x3554 || vid==0x373b || vid==0x391d) return true;
    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towlower(c);});
    return name.find(L"receiver")!=std::wstring::npos || name.find(L"dongle")!=std::wstring::npos ||
        name.find(L"2.4")!=std::wstring::npos || name.find(L"wireless")!=std::wstring::npos || name.find(L"接收器")!=std::wstring::npos;
}
inline bool ReceiverInterface(ReceiverSpec spec, unsigned page, unsigned usage, unsigned input, unsigned output, unsigned feature=0) {
    if(spec.name && spec.family==ReceiverFamily::VgnFeature) return page>=0xff00 && feature==65;
    if(spec.name && spec.family==ReceiverFamily::VgnMouseEnc)
        return page>=0xff00 && input>=17 && input<=65 && output>=17 && output<=65;
    if (!spec.name || input>65 || output>65) return false;
    if (spec.family==ReceiverFamily::Nordic54) return page==0xff05 && usage==1 && input>=64 && output>=64;
    return (page==0xff02 || page==0xff04) && usage==2 && input>=17 && output>=17;
}
inline std::array<uint8_t,65> VgnFeatureRequest() {
    std::array<uint8_t,65> bytes{};bytes[1]=0xf7;bytes[64]=8;return bytes;
}
inline int VgnFeaturePercentage(const uint8_t* data, size_t size) {
    if(size!=65 || data[0]!=0 || data[1]!=0xf7) return -1;
    unsigned sum=0;for(size_t i=1;i<size;++i)sum+=data[i];
    if((sum&255)!=255) return -1;
    if(data[5]==1) return -2;
    return data[3]<=100?data[3]:-1;
}
inline constexpr std::array<uint8_t,17> BatteryRequest() {
    return {8,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x49};
}
inline constexpr std::array<uint8_t,17> MouseOnlineRequest() {
    return {8,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0x4a};
}
inline bool MouseEncReply(const uint8_t* data,size_t size,uint8_t command) {
    if(size<17 || data[0]!=8 || data[1]!=command || data[2]!=0) return false;
    unsigned sum=0;for(size_t i=0;i<17;++i)sum+=data[i];
    return (sum&255)==0x55;
}
inline int MouseOnline(const uint8_t* data,size_t size) {
    return MouseEncReply(data,size,3)?(data[6]>0?1:0):-1;
}
inline int ReceiverPercentage(const uint8_t* data, size_t length) {
    if (length < 17 || data[0] != 8 || data[1] != 4 || data[2] != 0 || data[6] > 100) return -1;
    unsigned sum = 0;
    for (size_t i=0; i<17; ++i) sum += data[i];
    return (sum & 255) == 0x55 ? data[6] : -1;
}
inline std::array<uint8_t,64> Nordic54Request(bool wired) {
    std::array<uint8_t,64> bytes{};
    bytes[0]=8; bytes[1]=0x7d; bytes[2]=0x72; bytes[3]=2;
    bytes[5]=wired?0:1; bytes[6]=7; bytes[7]=1;
    return bytes;
}
inline int Nordic54Percentage(const uint8_t* data, size_t size) {
    // -2: receiver answered but mouse is asleep/out of range. Never use its stale percentage.
    if (size<10 || data[0]!=8 || data[1]!=0x72 || data[5]!=7 || data[6]!=1) return -1;
    if (data[2]==1) return -2;
    return data[2]==0 && data[7]<=100 ? data[7] : -1;
}
}
