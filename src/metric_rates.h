#pragma once
#include <cmath>
#include <cstdint>
namespace pulse {
inline double CounterRate(std::uint64_t current,std::uint64_t previous,double seconds){
    return current>=previous&&std::isfinite(seconds)&&seconds>0?static_cast<double>(current-previous)/seconds:NAN;
}
inline double MemoryPercent(std::uint64_t total,std::uint64_t available){
    return total&&available<=total?100.0*static_cast<double>(total-available)/static_cast<double>(total):NAN;
}
inline double CpuTimePercent(std::uint64_t total,std::uint64_t previousTotal,std::uint64_t idle,std::uint64_t previousIdle){
    if(total<=previousTotal||idle<previousIdle||idle-previousIdle>total-previousTotal)return NAN;
    return 100.0*(1.0-static_cast<double>(idle-previousIdle)/static_cast<double>(total-previousTotal));
}
}
