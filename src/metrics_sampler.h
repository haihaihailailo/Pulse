#pragma once
#include "metrics.h"
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <algorithm>

namespace pulse {
// The collector and its GPU/provider state have one owner. UI requests coalesce
// into a single pending sample; slow providers cannot build a work queue.
class MetricsSampler {
public:
    ~MetricsSampler() {
        {std::lock_guard<std::mutex> lock(mutex_);stop_=true;}
        wake_.notify_all();if(worker_.joinable())worker_.join();
    }
    void Start(const Settings& settings,Metrics::EventCallback callback,void* user) {
        slow_=settings.slow_ms;
        worker_=std::thread([this,settings,callback,user]{Run(settings,callback,user);});
    }
    Snapshot Sample(bool full,unsigned refreshMs=1000) {
        std::lock_guard<std::mutex> lock(mutex_);full_=full;pending_=true;wake_.notify_one();
        // Leave room for scheduling jitter and two missed refreshes, including
        // the low-power 5-second mini interval. Pauses/errors clear immediately.
        const auto freshness=std::chrono::milliseconds(std::max(5000u,refreshMs*3));
        return std::chrono::steady_clock::now()-published_>freshness?Snapshot{}:latest_;
    }
    void SetWants(const Wants& wants) {std::lock_guard<std::mutex> lock(mutex_);wants_=wants;}
    void SetPaused(bool paused) {
        std::lock_guard<std::mutex> lock(mutex_);
        if(paused_==paused)return;paused_=paused;latest_={};++generation_;pending_=true;wake_.notify_one();
    }
    void SetGpuAdapter(const std::wstring& id) {
        std::lock_guard<std::mutex> lock(mutex_);gpu_=id;latest_={};++generation_;pending_=true;wake_.notify_one();
    }
    std::vector<GpuAdapter> GpuAdapters() {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now=std::chrono::steady_clock::now();
        if(now-lastEnumerationRequest_>std::chrono::seconds(30)){lastEnumerationRequest_=now;enumerate_=true;pending_=true;wake_.notify_one();}
        return adapters_;
    }
    void SetSlowThreshold(int value) {std::lock_guard<std::mutex> lock(mutex_);slow_=value;pending_=true;wake_.notify_one();}
    void SetProbeEnabled(bool value) {std::lock_guard<std::mutex> lock(mutex_);probe_=value;pending_=true;wake_.notify_one();}
    void OnSuspend() {Suspend(true);}
    void OnResume() {Suspend(false);}
    void RefreshPeripherals() {std::lock_guard<std::mutex> lock(mutex_);refreshDevices_=true;pending_=true;wake_.notify_one();}
    void SetInputSources(const std::wstring& network,const std::wstring& disk){std::lock_guard<std::mutex> lock(mutex_);network_=network;disk_=disk;++generation_;latest_={};pending_=true;wake_.notify_one();}
    std::vector<InputDevice> NetworkAdapters() const {std::lock_guard<std::mutex> lock(mutex_);return networks_;}
    std::vector<InputDevice> Disks() const {std::lock_guard<std::mutex> lock(mutex_);return disks_;}
    std::wstring LogPath() const {std::lock_guard<std::mutex> lock(mutex_);return log_;}
    double LastSampleMilliseconds() const {std::lock_guard<std::mutex> lock(mutex_);return sampleMs_;}
    bool Failed() const {std::lock_guard<std::mutex> lock(mutex_);return failed_;}
private:
    void Suspend(bool value) {
        std::lock_guard<std::mutex> lock(mutex_);suspended_=value;if(!value)resume_=true;latest_={};++generation_;pending_=true;wake_.notify_one();
    }
    void Run(Settings settings,Metrics::EventCallback callback,void* user) {
        try {
            Metrics metrics;metrics.SetProbeEnabled(false);metrics.Start(settings,callback,user);
            bool wasSuspended=false;unsigned failures=0;
            std::unique_lock<std::mutex> lock(mutex_);log_=metrics.LogPath();
            while(!stop_) {
                wake_.wait(lock,[&]{return stop_||pending_;});if(stop_)break;
                const auto generation=generation_;const auto wants=wants_;const auto gpu=gpu_;
                const auto network=network_,disk=disk_;
                const bool paused=paused_,suspended=suspended_,full=full_,enumerate=enumerate_,probe=probe_;
                const int slow=slow_;const bool refreshDevices=refreshDevices_,resume=resume_;pending_=false;enumerate_=false;refreshDevices_=false;resume_=false;
                lock.unlock();
                const auto begin=std::chrono::steady_clock::now();
                Snapshot next;std::vector<GpuAdapter> adapters;bool ok=false;
                std::vector<InputDevice> networks,disks;
                try {
                    metrics.SetPaused(paused||suspended);metrics.SetWants(wants);
                    metrics.SetProbeEnabled(probe&&!suspended);metrics.SetSlowThreshold(slow);
                    if(suspended!=wasSuspended) {if(suspended)metrics.OnSuspend();else metrics.OnResume();wasSuspended=suspended;}
                    else if(resume&&!suspended)metrics.OnResume();
                    metrics.SetGpuAdapter(gpu);
                    metrics.SetInputSources(network,disk);
                    if(refreshDevices&&!suspended)metrics.RefreshPeripherals();
                    if(enumerate&&!suspended){adapters=metrics.GpuAdapters();networks=metrics.NetworkAdapters();disks=metrics.Disks();}
                    next=metrics.Sample(full);ok=true;
                } catch(...) {}
                const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count();
                lock.lock();sampleMs_=ms;failed_=!ok;
                if(generation==generation_) {
                    if(ok&&!paused&&!suspended){latest_=std::move(next);published_=std::chrono::steady_clock::now();}
                    else latest_={};
                    if(enumerate&&ok&&!suspended){adapters_=std::move(adapters);networks_=std::move(networks);disks_=std::move(disks);}
                }
                if(!ok) {
                    failures=std::min(failures+1,5u);
                    wake_.wait_for(lock,std::chrono::seconds(1u<<failures),[&]{return stop_||generation!=generation_;});
                } else failures=0;
            }
        } catch(...) {std::lock_guard<std::mutex> lock(mutex_);failed_=true;latest_={};}
    }
    mutable std::mutex mutex_;std::condition_variable wake_;std::thread worker_;
    Wants wants_{};Snapshot latest_{};std::vector<GpuAdapter> adapters_;
    std::vector<InputDevice> networks_,disks_;std::wstring network_,disk_;
    std::wstring gpu_,log_;double sampleMs_=0;unsigned generation_=0;
    std::chrono::steady_clock::time_point published_{};
    std::chrono::steady_clock::time_point lastEnumerationRequest_{};
    int slow_=500;
    bool stop_=false,pending_=true,full_=true,paused_=false,suspended_=false,probe_=false,enumerate_=true,failed_=false,refreshDevices_=false,resume_=false;
};
}
