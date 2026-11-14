#pragma once
#include "receiver_protocol.h"
#include "peripheral_state.h"
#include <windows.h>
#include <hidsdi.h>
#include <winioctl.h>
#include <hidclass.h>
#include <setupapi.h>
#include <objbase.h>
#include <devpropdef.h>
#include <atomic>
#include <cstring>
#include <map>
#include <cwctype>

namespace pulse::peripheral {
struct ReceiverHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ReceiverHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// Only the polling worker calls these APIs. Pending buffers remain alive until
// Windows acknowledges completion, including cancellation after timeout.
inline bool ReceiverTransfer(HANDLE device, bool writing, BYTE* buffer, DWORD length,
                             DWORD& count, const std::atomic<bool>& cancel, ULONGLONG until, DWORD control=0) {
    if (cancel.load() || GetTickCount64() >= until) return false;
    ReceiverHandle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    if (!event.value) return false;
    OVERLAPPED operation{}; operation.hEvent = event.value;
    const BOOL done = control ? DeviceIoControl(device,control,writing?buffer:nullptr,writing?length:0,
        writing?nullptr:buffer,writing?0:length,&count,&operation) :
        writing ? WriteFile(device,buffer,length,&count,&operation) : ReadFile(device,buffer,length,&count,&operation);
    if (done) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    while (!cancel.load() && GetTickCount64() < until) {
        if (WaitForSingleObject(event.value,20) == WAIT_OBJECT_0)
            return GetOverlappedResult(device,&operation,&count,FALSE) != FALSE;
    }
    CancelIoEx(device,&operation);
    GetOverlappedResult(device,&operation,&count,TRUE);
    return false;
}

inline void ReadReceivers(std::vector<PeripheralBattery>& rows, const std::atomic<bool>& cancel,
                          std::map<std::wstring,std::wstring>& successful) {
    struct HidApi {
        HMODULE module = LoadLibraryExW(L"hid.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&HidD_GetAttributes) attributes = module ? reinterpret_cast<decltype(attributes)>(GetProcAddress(module,"HidD_GetAttributes")) : nullptr;
        decltype(&HidD_GetPreparsedData) preparsed = module ? reinterpret_cast<decltype(preparsed)>(GetProcAddress(module,"HidD_GetPreparsedData")) : nullptr;
        decltype(&HidD_FreePreparsedData) freeData = module ? reinterpret_cast<decltype(freeData)>(GetProcAddress(module,"HidD_FreePreparsedData")) : nullptr;
        decltype(&HidP_GetCaps) caps = module ? reinterpret_cast<decltype(caps)>(GetProcAddress(module,"HidP_GetCaps")) : nullptr;
        decltype(&HidD_GetProductString) product = module ? reinterpret_cast<decltype(product)>(GetProcAddress(module,"HidD_GetProductString")) : nullptr;
        ~HidApi() { if (module) FreeLibrary(module); }
    } api;
    if (!api.attributes || !api.preparsed || !api.freeData || !api.caps) return;
    constexpr GUID hid{0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
    constexpr DEVPROPKEY container{{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
    HDEVINFO set = SetupDiGetClassDevsW(&hid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    struct Cleanup { HDEVINFO value; ~Cleanup(){ SetupDiDestroyDeviceInfoList(value); } } cleanup{set};
    const auto scanUntil = GetTickCount64()+3000;
    struct Candidate {std::wstring path; ReceiverSpec spec; HIDP_CAPS caps;};
    std::map<std::wstring,std::vector<Candidate>> candidates;
    // The owning monitor's single worker uses this bounded cache; removal evicts it.
    for (DWORD i=0; i<512 && !cancel.load() && GetTickCount64()<scanUntil; ++i) {
        SP_DEVICE_INTERFACE_DATA iface{}; iface.cbSize=sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(set,nullptr,&hid,i,&iface)) break;
        DWORD size=0;
        SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&size,nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size>8192) continue;
        std::vector<BYTE> detailBuffer(size);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detailBuffer.data());
        detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA info{}; info.cbSize=sizeof(info);
        if (!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,size,nullptr,&info)) continue;
        // Metadata access first; no protocol request goes to an unrecognised device.
        ReceiverHandle metadata{CreateFileW(detail->DevicePath,0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
        if (metadata.value==INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attrs{}; attrs.Size=sizeof(attrs);
        if (!api.attributes(metadata.value,&attrs)) continue;
        const auto spec=Receiver(attrs.VendorID,attrs.ProductID);
        wchar_t product[128]{};
        if(api.product) api.product(metadata.value,product,sizeof(product));
        product[127]=0;
        std::wstring name=product;
        if(!DiscoverReceiver(attrs.VendorID,attrs.ProductID,name)) continue;
        if(name.empty()) name=spec.name?spec.name:L"无线接收器";
        PHIDP_PREPARSED_DATA preparsed=nullptr;
        HIDP_CAPS caps{};
        if(api.preparsed(metadata.value,&preparsed)) {api.caps(preparsed,&caps);api.freeData(preparsed);}
        std::wstring identity=detail->DevicePath;
        GUID group{},empty{}; DEVPROPTYPE type=0; DWORD bytes=0;
        if (SetupDiGetDevicePropertyW(set,&info,&container,&type,reinterpret_cast<BYTE*>(&group),sizeof(group),&bytes,0) &&
            type==DEVPROP_TYPE_GUID && bytes==sizeof(group) && std::memcmp(&group,&empty,sizeof(group))!=0) {
            wchar_t text[40]{}; if (StringFromGUID2(group,text,40)) identity=text;
        }
        wchar_t identifier[64]{};
        swprintf(identifier,64,L"VID %04X · PID %04X",attrs.VendorID,attrs.ProductID);
        PeripheralBattery row{identity,name,-1}; row.source=L"USB HID";row.detail=identifier;
        row.status=spec.name?BatteryStatus::Unknown:BatteryStatus::Unsupported;
        Merge(rows,row); // Discovery is independent of protocol support and I/O success.
        if(ReceiverInterface(spec,caps.UsagePage,caps.Usage,caps.InputReportByteLength,caps.OutputReportByteLength,caps.FeatureReportByteLength) &&
           candidates.size()<32 && candidates[identity].size()<8)
            candidates[identity].push_back({detail->DevicePath,spec,caps});
    }
    for(auto it=successful.begin();it!=successful.end();) {
        const auto group=candidates.find(it->first);
        if(group==candidates.end() || std::none_of(group->second.begin(),group->second.end(),[&](const auto& c){return c.path==it->second;}))
            it=successful.erase(it);
        else ++it;
    }
#ifndef PULSE_DEVELOPMENT
    unsigned queries=0;
    for(auto& group:candidates) {
        auto& list=group.second;
        if(auto cached=successful.find(group.first);cached!=successful.end())
            std::stable_partition(list.begin(),list.end(),[&](const auto& c){return c.path==cached->second;});
        auto row=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.id==group.first;});
        if(row==rows.end()) continue;
        for(const auto& candidate:list) {
            if(queries>=12 || cancel.load() || GetTickCount64()>=scanUntil) break;
            ++queries;
            const auto& caps=candidate.caps;
            const auto identifier=row->detail.substr(0,row->detail.find(L" | "));
            wchar_t collection[100]{};
            swprintf(collection,100,L" | usage %04X:%04X · in/out/feature %u/%u/%u",caps.UsagePage,caps.Usage,
                caps.InputReportByteLength,caps.OutputReportByteLength,caps.FeatureReportByteLength);
            row->detail=identifier+collection;
            ReceiverHandle device{CreateFileW(candidate.path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
                nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr)};
            if(device.value==INVALID_HANDLE_VALUE) {
                if(row->percent<0)row->status=BatteryStatus::Unavailable;
                row->detail+=L" | open: "+std::to_wstring(GetLastError());continue;
            }
            std::array<BYTE,65> request{},response{};
            if(candidate.spec.family==ReceiverFamily::VgnFeature) {
                request=VgnFeatureRequest();
            } else if(candidate.spec.family==ReceiverFamily::Nordic54) {
                const auto command=Nordic54Request(candidate.spec.wired);std::copy(command.begin(),command.end(),request.begin());
            } else {const auto command=BatteryRequest();std::copy(command.begin(),command.end(),request.begin());}
            DWORD count=0;const auto until=(std::min)(scanUntil,GetTickCount64()+350);
            int percent=-1;
            if(candidate.spec.family==ReceiverFamily::VgnFeature) {
                if(ReceiverTransfer(device.value,true,request.data(),65,count,cancel,until,IOCTL_HID_SET_FEATURE) &&
                   ReceiverTransfer(device.value,false,response.data(),65,count,cancel,until,IOCTL_HID_GET_FEATURE))
                    percent=VgnFeaturePercentage(response.data(),count);
            } else if(ReceiverTransfer(device.value,true,request.data(),caps.OutputReportByteLength,count,cancel,until) && count==caps.OutputReportByteLength) {
                for(unsigned attempt=0;attempt<8 && percent==-1;++attempt) {
                    if(!ReceiverTransfer(device.value,false,response.data(),caps.InputReportByteLength,count,cancel,until))break;
                    percent=candidate.spec.family==ReceiverFamily::Nordic54?Nordic54Percentage(response.data(),count):ReceiverPercentage(response.data(),count);
                }
            }
            if(percent>=0) {
                row->percent=percent;row->status=BatteryStatus::Unknown;
                row->source=candidate.spec.family==ReceiverFamily::VgnFeature?L"HID / VGN Feature":
                    candidate.spec.family==ReceiverFamily::Nordic54?L"HID / Nordic 54":L"HID / Nordic 52";
                row->charging=candidate.spec.wired && percent<100;
                successful[group.first]=candidate.path;break;
            }
            if(row->percent<0) row->status=percent==-2?BatteryStatus::Sleeping:BatteryStatus::Unknown;
            row->detail+=percent==-2?L" | mouse offline":L" | no valid battery reply";
            if(percent==-2) break; // Valid offline response: do not keep waking this receiver.
            // Failed I/O never marks the physical device done: try its next eligible interface.
        }
    }
#endif
}
}
