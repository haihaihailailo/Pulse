#pragma once
#include "peripheral_state.h"
#include <array>
#include <cstdint>

namespace pulse::peripheral {
// HeyMelody wire format: AA, length, reserved(2), command LE16,
// sequence, payload length LE16, payload. This module only builds read requests.
inline std::array<uint8_t,9> OppoQuery(bool handshake=false) {
    return {0xaa,7,0,0,static_cast<uint8_t>(handshake?0:6),1,
        static_cast<uint8_t>(handshake?1:0xf0),0,0};
}
inline bool OppoBattery(const uint8_t* frame, size_t size, PeripheralBattery& row) {
    if(size<9 || frame[0]!=0xaa || size!=static_cast<size_t>(frame[1])+2 ||
       frame[1]<7 || frame[2]!=0 || frame[3]!=0 || frame[8]!=0 || frame[7]!=size-9) return false;
    const unsigned command=frame[4]|(frame[5]<<8);
    size_t start=9, end=size;
    if(command==0x0204) {
        if(size<11 || frame[9]!=1 || frame[10]>(size-11)/2) return false;
        start=11; end=start+frame[10]*2;
    } else if(command!=0x8106 || frame[6]!=0xf0) return false;
    if((end-start)%2) return false;
    PeripheralBattery next=row;
    bool valid=false;
    for(size_t i=start;i+1<end;i+=2) {
        int* component=frame[i]==1?&next.left:frame[i]==2?&next.right:frame[i]==3?&next.chargingCase:nullptr;
        if(!component) continue;
        *component=Percentage(frame[i+1]&0x7f);
        if(*component>=0) {valid=true;next.charging=next.charging || (frame[i+1]&0x80)!=0;}
    }
    if(!valid) return false;
    next.percent=next.left<0?next.right:next.right<0?next.left:(std::min)(next.left,next.right);
    if(next.percent<0) next.percent=next.chargingCase;
    next.source=L"OPPO / HeyMelody"; next.status=BatteryStatus::Unknown; row=std::move(next);
    return true;
}
class OppoStream {
    std::vector<uint8_t> pending_;
public:
    bool Append(const uint8_t* bytes, size_t size, PeripheralBattery& row) {
        if(size>512 || pending_.size()+size>1024) {pending_.clear();return false;}
        pending_.insert(pending_.end(),bytes,bytes+size);
        bool found=false;
        while(!pending_.empty()) {
            auto start=std::find(pending_.begin(),pending_.end(),0xaa);
            pending_.erase(pending_.begin(),start);
            if(pending_.size()<2) break;
            if(pending_[1]<7) {pending_.erase(pending_.begin());continue;}
            const size_t length=pending_[1]+2;
            if(pending_.size()<length) break;
            found=OppoBattery(pending_.data(),length,row) || found;
            pending_.erase(pending_.begin(),pending_.begin()+length);
        }
        return found;
    }
};
}
