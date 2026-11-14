#pragma once
#include "peripheral_state.h"
#include "receiver_battery.h"
#include <windows.h>
#include <objbase.h>
#include <setupapi.h>
#include <devpropdef.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pulse {
namespace peripheral {

// Cached Windows battery properties plus whitelisted receiver battery queries.
// No pairing, radio scan or synchronous hardware request on the UI thread.
inline PeripheralSnapshot Read(const std::atomic<bool>& cancel) {
    constexpr DEVPROPKEY battery{{0x104ea319,0x6ee2,0x4701,{0xbd,0x47,0x8d,0xdb,0xf4,0x25,0xbb,0xe5}},2};
    constexpr DEVPROPKEY life{{0x49cd1f76,0x5626,0x4b17,{0xa4,0xe8,0x18,0xb4,0xaa,0x1a,0x22,0x13}},10};
    constexpr DEVPROPKEY container{{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
    constexpr DEVPROPKEY nodeStatus{{0x4340a6c5,0x93fa,0x4706,{0x97,0x2c,0x7b,0x64,0x80,0x08,0xa5,0xa7}},2};
    PeripheralSnapshot out;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { out.state = PeripheralState::Unavailable; return out; }
    struct Close { HDEVINFO h; ~Close() { SetupDiDestroyDeviceInfoList(h); } } close{set};
    out.state = PeripheralState::Ready;
    const auto until = GetTickCount64() + 3000;
    for (DWORD index = 0; index < 2048 && !cancel.load(); ++index) {
        if (GetTickCount64() >= until) { out.state = PeripheralState::Unavailable; out.devices.clear(); break; }
        SP_DEVINFO_DATA info{}; info.cbSize = sizeof(info);
        if (!SetupDiEnumDeviceInfo(set, index, &info)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) { out.state = PeripheralState::Unavailable; out.devices.clear(); }
            break;
        }
        wchar_t instance[512]{};
        if (!SetupDiGetDeviceInstanceIdW(set, &info, instance, 512, nullptr)) continue;
        const std::wstring id(instance);
        const bool bluetooth = id.rfind(L"BTHENUM\\",0)==0 || id.rfind(L"BTHLE\\",0)==0 || id.rfind(L"BTHLEDEVICE\\",0)==0;
        const bool hid = id.rfind(L"HID\\",0)==0;
        if (!bluetooth && !hid) continue;
        DEVPROPTYPE type = 0; DWORD bytes = 0, status = 0;
        if (SetupDiGetDevicePropertyW(set, &info, &nodeStatus, &type, reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes, 0) &&
            type == DEVPROP_TYPE_UINT32 && bytes == sizeof(status) && (status & 0x02000000u)) continue; // DN_DEVICE_DISCONNECTED
        int percent = -1;
        for (const auto& key : {battery,life}) {
            unsigned value = 0;
            if (SetupDiGetDevicePropertyW(set, &info, &key, &type, reinterpret_cast<BYTE*>(&value), sizeof(value), &bytes, 0) &&
                ((type == DEVPROP_TYPE_BYTE && bytes == 1) || (type == DEVPROP_TYPE_UINT32 && bytes == 4)))
                percent = Percentage(value);
            if (percent >= 0) break;
        }
        // Unknown rows only for physical Bluetooth roots, not every service,
        // wired keyboard, USB receiver, adapter or generic HID collection.
        const bool root = id.rfind(L"BTHENUM\\DEV_",0)==0 || id.rfind(L"BTHLE\\DEV_",0)==0;
        if (percent < 0 && !root) continue;
        wchar_t name[256]{}; DWORD regType = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_FRIENDLYNAME,&regType,reinterpret_cast<BYTE*>(name),sizeof(name),nullptr))
            SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_DEVICEDESC,&regType,reinterpret_cast<BYTE*>(name),sizeof(name),nullptr);
        name[255] = 0;
        if (regType != REG_SZ || !name[0]) continue;
        GUID group{}, empty{}; std::wstring identity = id;
        if (SetupDiGetDevicePropertyW(set,&info,&container,&type,reinterpret_cast<BYTE*>(&group),sizeof(group),&bytes,0) &&
            type == DEVPROP_TYPE_GUID && bytes == sizeof(group) && std::memcmp(&group,&empty,sizeof(group)) != 0) {
            wchar_t text[40]{};
            if (StringFromGUID2(group,text,40)) identity = text;
        }
        Merge(out.devices, {std::move(identity),name,percent});
    }
    if (!cancel.load()) ReadReceivers(out.devices,cancel);
    if (!out.devices.empty()) out.state = PeripheralState::Ready;
    std::sort(out.devices.begin(),out.devices.end(),[](const auto& a,const auto& b){return a.id < b.id;});
    return out;
}
}

// A single lazy worker, bounded results, minute polling and interruptible waits.
// Disable clears visible results immediately; generation protects off/on races.
class PeripheralMonitor {
public:
    ~PeripheralMonitor() {
        { std::lock_guard<std::mutex> lock(mutex_); stop_ = true; cancel_ = true; }
        wake_.notify_all(); if (worker_.joinable()) worker_.join();
    }
    void Enable(bool enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ == enabled) return;
        enabled_ = enabled; ++generation_; cancel_ = !enabled; sample_ = {};
        if (enabled && !worker_.joinable()) {
            try { worker_ = std::thread([this]{ Run(); }); }
            catch (...) { enabled_ = false; sample_.state = PeripheralState::Unavailable; }
        }
        wake_.notify_all();
    }
    PeripheralSnapshot Snapshot() { std::lock_guard<std::mutex> lock(mutex_); return sample_; }
    void Refresh() {
        std::lock_guard<std::mutex> lock(mutex_);
        if(enabled_ && GetTickCount64()-requestedAt_>=5000) {requestedAt_=GetTickCount64();refresh_=true;wake_.notify_all();}
    }
private:
    void Run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stop_) {
            wake_.wait(lock,[&]{return stop_ || enabled_;}); if (stop_) break;
            const auto generation = generation_; refresh_=false;lock.unlock();
            PeripheralSnapshot next;
            try { next = peripheral::Read(cancel_); } catch (...) { next.state = PeripheralState::Unavailable; }
            next.sampledAt=GetTickCount64();
            lock.lock();
            if (enabled_ && generation == generation_) sample_ = std::move(next);
            const auto delay=sample_.state==PeripheralState::Unavailable?120:sample_.devices.empty()?180:60;
            wake_.wait_for(lock,std::chrono::seconds(delay),[&]{return stop_ || refresh_ || generation != generation_;});
        }
    }
    std::mutex mutex_; std::condition_variable wake_; std::thread worker_;
    std::atomic<bool> cancel_{false};
    bool enabled_ = false, stop_ = false; unsigned generation_ = 0;
    bool refresh_=false;unsigned long long requestedAt_=0;
    PeripheralSnapshot sample_;
};
}
