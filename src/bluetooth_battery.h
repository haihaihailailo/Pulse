#pragma once
#include <winsock2.h>
#include <ws2bth.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <bluetoothleapis.h>
#include "receiver_battery.h"
#include "oppo_protocol.h"
#include <map>

namespace pulse::peripheral {
inline std::wstring DeviceContainer(HDEVINFO set, SP_DEVINFO_DATA& info, std::wstring fallback) {
    constexpr DEVPROPKEY key{{0x8c7ed206,0x3f8a,0x4827,{0xb3,0xab,0xae,0x9e,0x1f,0xae,0xfc,0x6c}},2};
    GUID value{},zero{};DEVPROPTYPE type=0;DWORD size=0;wchar_t text[40]{};
    if(SetupDiGetDevicePropertyW(set,&info,&key,&type,reinterpret_cast<BYTE*>(&value),sizeof(value),&size,0) &&
        type==DEVPROP_TYPE_GUID && size==sizeof(value) && std::memcmp(&value,&zero,sizeof(value)) && StringFromGUID2(value,text,40)) return text;
    return fallback;
}
inline std::wstring DeviceName(HDEVINFO set, SP_DEVINFO_DATA& info) {
    wchar_t text[256]{};DWORD type=0;
    if(!SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_FRIENDLYNAME,&type,reinterpret_cast<BYTE*>(text),sizeof(text),nullptr))
        SetupDiGetDeviceRegistryPropertyW(set,&info,SPDRP_DEVICEDESC,&type,reinterpret_cast<BYTE*>(text),sizeof(text),nullptr);
    text[255]=0;return type==REG_SZ?CleanName(text):std::wstring{};
}
inline bool OppoName(std::wstring name) {
    std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return std::towlower(c);});
    return name.find(L"oppo")!=std::wstring::npos || name.find(L"enco")!=std::wstring::npos ||
        name.find(L"oneplus")!=std::wstring::npos || name.find(L"realme")!=std::wstring::npos;
}
inline bool SocketReady(SOCKET socket, bool write, const std::atomic<bool>& cancel, ULONGLONG until) {
    while(!cancel.load() && GetTickCount64()<until) {
        fd_set ready,errors;FD_ZERO(&ready);FD_ZERO(&errors);FD_SET(socket,&ready);FD_SET(socket,&errors);
        timeval timeout{0,20000};
        const int result=select(0,write?nullptr:&ready,write?&ready:nullptr,&errors,&timeout);
        if(result<0 || FD_ISSET(socket,&errors))return false;
        if(result>0)return true;
    }
    return false;
}
inline bool ReadOppo(ULONGLONG address, PeripheralBattery& row, const std::atomic<bool>& cancel, ULONGLONG deadline) {
    // UUID resolution uses Windows SDP. Never guess a channel or change pairing,
    // ANC, firmware, input mode or other user settings.
    constexpr GUID services[]={
        {0x00001107,0xd102,0x11e1,{0x9b,0x23,0,0x02,0x5b,0,0xa5,0xa5}},
        {0x0000079a,0xd102,0x11e1,{0x9b,0x23,0,0x02,0x5b,0,0xa5,0xa5}}};
    WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data)) return false;
    struct End {~End(){WSACleanup();}} end;
    for(const auto& service:services) {
        if(cancel.load() || GetTickCount64()>=deadline)return false;
        struct Socket {SOCKET s=INVALID_SOCKET;~Socket(){if(s!=INVALID_SOCKET)closesocket(s);}} peer;
        peer.s=socket(AF_BTH,SOCK_STREAM,BTHPROTO_RFCOMM);if(peer.s==INVALID_SOCKET)continue;
        u_long nonblocking=1;if(ioctlsocket(peer.s,FIONBIO,&nonblocking))continue;
        SOCKADDR_BTH target{};target.addressFamily=AF_BTH;target.btAddr=address;target.serviceClassId=service;
        const auto until=(std::min)(deadline,GetTickCount64()+1500);
        if(connect(peer.s,reinterpret_cast<sockaddr*>(&target),sizeof(target))==SOCKET_ERROR) {
            const int error=WSAGetLastError();
            if(error!=WSAEWOULDBLOCK && error!=WSAEINPROGRESS)continue;
            if(!SocketReady(peer.s,true,cancel,until))continue;
            int socketError=0,size=sizeof(socketError);
            if(getsockopt(peer.s,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&socketError),&size) || socketError)continue;
        }
        bool sent=true;
        for(bool handshake:{true,false}) {
            const auto request=OppoQuery(handshake);size_t offset=0;
            while(offset<request.size() && SocketReady(peer.s,true,cancel,until)) {
                const int count=send(peer.s,reinterpret_cast<const char*>(request.data()+offset),static_cast<int>(request.size()-offset),0);
                if(count>0)offset+=count;
                else if(count==0 || WSAGetLastError()!=WSAEWOULDBLOCK)break;
            }
            if(offset!=request.size()){sent=false;break;}
        }
        if(!sent)continue;
        OppoStream stream;size_t received=0;
        while(received<4096 && SocketReady(peer.s,false,cancel,until)) {
            uint8_t buffer[512]{};const int count=recv(peer.s,reinterpret_cast<char*>(buffer),sizeof(buffer),0);
            if(count==SOCKET_ERROR && WSAGetLastError()==WSAEWOULDBLOCK)continue;
            if(count<=0)break;
            received+=count;
            if(stream.Append(buffer,count,row))return true;
        }
    }
    return false;
}

inline void ReadClassicBluetooth(std::vector<PeripheralBattery>& rows,
        const std::map<ULONGLONG,std::wstring>& identities, const std::atomic<bool>& cancel) {
    struct Api {
        HMODULE module=LoadLibraryExW(L"bthprops.cpl",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&BluetoothFindFirstDevice) first=module?reinterpret_cast<decltype(first)>(GetProcAddress(module,"BluetoothFindFirstDevice")):nullptr;
        decltype(&BluetoothFindNextDevice) next=module?reinterpret_cast<decltype(next)>(GetProcAddress(module,"BluetoothFindNextDevice")):nullptr;
        decltype(&BluetoothFindDeviceClose) close=module?reinterpret_cast<decltype(close)>(GetProcAddress(module,"BluetoothFindDeviceClose")):nullptr;
        ~Api(){if(module)FreeLibrary(module);}
    } api;
    if(!api.first || !api.next || !api.close || cancel.load())return;
    BLUETOOTH_DEVICE_SEARCH_PARAMS search{};search.dwSize=sizeof(search);
    search.fReturnConnected=TRUE;search.fReturnAuthenticated=TRUE;search.fReturnRemembered=TRUE;
    search.fIssueInquiry=FALSE; // Cached paired devices only; never scans/pairs nearby devices.
    BLUETOOTH_DEVICE_INFO info{};info.dwSize=sizeof(info);
    auto handle=api.first(&search,&info);if(!handle)return;
    struct Close {HBLUETOOTH_DEVICE_FIND h;decltype(api.close) fn;~Close(){fn(h);}} close{handle,api.close};
    const auto until=GetTickCount64()+5000;
    unsigned requests=0;
    for(unsigned i=0;i<64 && !cancel.load();++i) {
        // Only audio and peripheral device classes (exclude phones/computers).
        const auto major=(info.ulClassofDevice>>8)&31;
        if(major==4 || major==5 || OppoName(info.szName)) {
            wchar_t address[32]{};swprintf(address,32,L"BT:%012llX",info.Address.ullLong);
            auto known=identities.find(info.Address.ullLong);
            PeripheralBattery row{known==identities.end()?std::wstring(address):known->second,info.szName,-1};
            row.source=L"Bluetooth / Windows";
            row.status=info.fConnected?BatteryStatus::Unknown:BatteryStatus::Disconnected;
#ifndef PULSE_DEVELOPMENT
            if(info.fConnected && OppoName(row.name) && requests<2 && GetTickCount64()<until) {
                ++requests;
                if(!ReadOppo(info.Address.ullLong,row,cancel,until))row.detail=L"HeyMelody: no battery response";
            }
#endif
            auto existing=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.id==row.id;});
            if(!info.fConnected && existing!=rows.end()) {existing->percent=-1;existing->status=BatteryStatus::Disconnected;}
            Merge(rows,std::move(row));
        }
        info={};info.dwSize=sizeof(info);
        if(!api.next(handle,&info))break;
    }
}

inline bool ShortUuid(const BTH_LE_UUID& value, USHORT expected) {
    if(value.IsShortUuid)return value.Value.ShortUuid==expected;
    const GUID uuid{expected,0,0x1000,{0x80,0,0,0x80,0x5f,0x9b,0x34,0xfb}};
    return std::memcmp(&value.Value.LongUuid,&uuid,sizeof(uuid))==0;
}
inline void ReadBleBatteries(std::vector<PeripheralBattery>& rows,const std::atomic<bool>& cancel) {
    struct Api {
        HMODULE module=LoadLibraryExW(L"BluetoothApis.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        decltype(&BluetoothGATTGetServices) services=module?reinterpret_cast<decltype(services)>(GetProcAddress(module,"BluetoothGATTGetServices")):nullptr;
        decltype(&BluetoothGATTGetCharacteristics) characteristics=module?reinterpret_cast<decltype(characteristics)>(GetProcAddress(module,"BluetoothGATTGetCharacteristics")):nullptr;
        decltype(&BluetoothGATTGetCharacteristicValue) value=module?reinterpret_cast<decltype(value)>(GetProcAddress(module,"BluetoothGATTGetCharacteristicValue")):nullptr;
        ~Api(){if(module)FreeLibrary(module);}
    } api;
    if(!api.services || !api.characteristics || !api.value)return;
    constexpr GUID interfaceId{0x781aee18,0x7733,0x4ce4,{0xad,0xd0,0x91,0xf4,0x1c,0x67,0xb5,0x92}};
    HDEVINFO set=SetupDiGetClassDevsW(&interfaceId,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    if(set==INVALID_HANDLE_VALUE)return;
    struct End {HDEVINFO h;~End(){SetupDiDestroyDeviceInfoList(h);}} end{set};
    const auto until=GetTickCount64()+3000;unsigned reads=0;
    for(DWORD i=0;i<64 && !cancel.load() && GetTickCount64()<until;++i) {
        SP_DEVICE_INTERFACE_DATA iface{};iface.cbSize=sizeof(iface);
        if(!SetupDiEnumDeviceInterfaces(set,nullptr,&interfaceId,i,&iface))break;
        DWORD size=0;SetupDiGetDeviceInterfaceDetailW(set,&iface,nullptr,0,&size,nullptr);
        if(size<sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W) || size>8192)continue;
        std::vector<BYTE> bytes(size);auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data());detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA info{};info.cbSize=sizeof(info);
        if(!SetupDiGetDeviceInterfaceDetailW(set,&iface,detail,size,nullptr,&info))continue;
        PeripheralBattery row{DeviceContainer(set,info,detail->DevicePath),DeviceName(set,info),-1};row.source=L"Bluetooth LE";
        if(row.name.empty())continue;
        Merge(rows,row);
#ifndef PULSE_DEVELOPMENT
        // Existing Windows readings take priority. Limit active GATT reads; the
        // synchronous Windows API can exceed our between-call deadline on a bad driver.
        auto existing=std::find_if(rows.begin(),rows.end(),[&](const auto& r){return r.id==row.id;});
        if(existing!=rows.end() && existing->percent>=0)continue;
        if(reads>=4)continue;
        ReceiverHandle device{CreateFileW(detail->DevicePath,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr)};
        if(device.value==INVALID_HANDLE_VALUE) {row.status=BatteryStatus::Unavailable;row.detail=L"GATT: access unavailable";Merge(rows,row);continue;}
        BTH_LE_GATT_SERVICE services[32]{};USHORT serviceCount=0;
        if(FAILED(api.services(device.value,32,services,&serviceCount,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE)) || serviceCount>32)continue;
        for(USHORT s=0;s<serviceCount && !cancel.load() && GetTickCount64()<until;++s) {
            if(!ShortUuid(services[s].ServiceUuid,0x180f))continue;
            BTH_LE_GATT_CHARACTERISTIC characteristics[32]{};USHORT count=0;
            if(FAILED(api.characteristics(device.value,&services[s],32,characteristics,&count,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_CACHE)) || count>32)continue;
            for(USHORT c=0;c<count && !cancel.load() && reads<4 && GetTickCount64()<until;++c) {
                if(!ShortUuid(characteristics[c].CharacteristicUuid,0x2a19) || !characteristics[c].IsReadable)continue;
                alignas(BTH_LE_GATT_CHARACTERISTIC_VALUE) BYTE buffer[64]{};
                auto* value=reinterpret_cast<BTH_LE_GATT_CHARACTERISTIC_VALUE*>(buffer);USHORT required=0;++reads;
                if(SUCCEEDED(api.value(device.value,&characteristics[c],sizeof(buffer),value,&required,BLUETOOTH_GATT_FLAG_FORCE_READ_FROM_DEVICE)) && value->DataSize==1) {
                    const int percent=Percentage(value->Data[0]);
                    if(percent>=0) {row.percent=row.percent<0?percent:(std::min)(row.percent,percent);row.source=L"Bluetooth GATT / 180F";}
                }
            }
        }
        Merge(rows,row);
#endif
    }
}
}
