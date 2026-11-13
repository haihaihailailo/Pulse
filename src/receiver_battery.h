#pragma once
#include "receiver_protocol.h"
#include "peripheral_state.h"
#include <windows.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <objbase.h>
#include <devpropdef.h>
#include <atomic>
#include <cstring>

namespace pulse::peripheral {
struct ReceiverHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~ReceiverHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

// Only the polling worker calls these APIs. Pending buffers remain alive until
// Windows acknowledges completion, including cancellation after timeout.
inline bool ReceiverTransfer(HANDLE device, bool writing, BYTE* buffer, DWORD length,
                             DWORD& count, const std::atomic<bool>& cancel, ULONGLONG until) {
    if (cancel.load() || GetTickCount64() >= until) return false;
    ReceiverHandle event{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    if (!event.value) return false;
    OVERLAPPED operation{}; operation.hEvent = event.value;
    const BOOL done = writing ? WriteFile(device,buffer,length,&count,&operation) : ReadFile(device,buffer,length,&count,&operation);
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

inline void ReadReceivers(std::vector<PeripheralBattery>& rows, const std::atomic<bool>& cancel) {
    struct HidApi {
        HMODULE module = LoadLibraryExW(L"hid.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&HidD_GetAttributes) attributes = module ? reinterpret_cast<decltype(attributes)>(GetProcAddress(module,"HidD_GetAttributes")) : nullptr;
        decltype(&HidD_GetPreparsedData) preparsed = module ? reinterpret_cast<decltype(preparsed)>(GetProcAddress(module,"HidD_GetPreparsedData")) : nullptr;
        decltype(&HidD_FreePreparsedData) freeData = module ? reinterpret_cast<decltype(freeData)>(GetProcAddress(module,"HidD_FreePreparsedData")) : nullptr;
        decltype(&HidP_GetCaps) caps = module ? reinterpret_cast<decltype(caps)>(GetProcAddress(module,"HidP_GetCaps")) : nullptr;
        ~HidApi() { if (module) FreeLibrary(module); }
    } api;
    if (!api.attributes || !api.preparsed || !api.freeData || !api.caps) return;
    constexpr GUID hid{0x4d1e55b2,0xf16f,0x11cf,{0x88,0xcb,0x00,0x11,0x11,0x00,0x00,0x30}};
    constexpr DEVPROPKEY container{{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
    HDEVINFO set = SetupDiGetClassDevsW(&hid,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return;
    struct Cleanup { HDEVINFO value; ~Cleanup(){ SetupDiDestroyDeviceInfoList(value); } } cleanup{set};
    const auto scanUntil = GetTickCount64()+3000;
    unsigned queries=0;
    std::vector<std::wstring> queried;
    for (DWORD i=0; i<512 && queries<8 && !cancel.load() && GetTickCount64()<scanUntil; ++i) {
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
        const auto* name=ReceiverName(attrs.VendorID,attrs.ProductID);
        if (!name) continue;
        PHIDP_PREPARSED_DATA preparsed=nullptr;
        if (!api.preparsed(metadata.value,&preparsed)) continue;
        HIDP_CAPS caps{}; const auto status=api.caps(preparsed,&caps); api.freeData(preparsed);
        if (status != HIDP_STATUS_SUCCESS || (caps.UsagePage!=0xff02 && caps.UsagePage!=0xff04) || caps.Usage!=2 ||
            caps.InputReportByteLength<17 || caps.InputReportByteLength>65 ||
            caps.OutputReportByteLength<17 || caps.OutputReportByteLength>65) continue;
        std::wstring identity=detail->DevicePath;
        GUID group{},empty{}; DEVPROPTYPE type=0; DWORD bytes=0;
        if (SetupDiGetDevicePropertyW(set,&info,&container,&type,reinterpret_cast<BYTE*>(&group),sizeof(group),&bytes,0) &&
            type==DEVPROP_TYPE_GUID && bytes==sizeof(group) && std::memcmp(&group,&empty,sizeof(group))!=0) {
            wchar_t text[40]{}; if (StringFromGUID2(group,text,40)) identity=text;
        }
        if (std::find(queried.begin(),queried.end(),identity)!=queried.end()) continue;
        queried.push_back(identity); ++queries;
        int percent=-1;
        ReceiverHandle device{CreateFileW(detail->DevicePath,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr)};
        if (device.value!=INVALID_HANDLE_VALUE) {
            std::array<BYTE,65> request{},response{};
            const auto command=BatteryRequest(); std::copy(command.begin(),command.end(),request.begin());
            DWORD count=0; const auto until=(std::min)(scanUntil,GetTickCount64()+350);
            if (ReceiverTransfer(device.value,true,request.data(),caps.OutputReportByteLength,count,cancel,until) && count==caps.OutputReportByteLength) {
                // Ignore unrelated notifications while staying within one deadline.
                for (unsigned attempt=0; attempt<8 && percent<0; ++attempt) {
                    if (!ReceiverTransfer(device.value,false,response.data(),caps.InputReportByteLength,count,cancel,until)) break;
                    percent=ReceiverPercentage(response.data(),count);
                }
            }
        }
        Merge(rows,{std::move(identity),name,percent});
    }
}
}
