// BatteryPlus：TrafficMonitor 插件（API 8）
// 电池采样与续航算法移植自 reference_widget.py（本机 Windows 11 实测）。
// 目标编译器：MinGW-w64 g++ 13.2 x86_64。见 build.bat。

#if !defined(_WIN64)
#error "BatteryPlus targets x86_64 only"
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <powrprof.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>

#include "PluginInterface.h"

static_assert(sizeof(wchar_t) == 2, "wchar_t must be UTF-16");

namespace {

constexpr unsigned long kAmber = 0x000B9EF5;  // #F59E0B
constexpr unsigned long kGreen = 0x005EC522;  // #22C55E
constexpr unsigned long kRed = 0x004444EF;    // #EF4444
constexpr double kTauSec = 30.0;
constexpr double kMaxGapSec = 5.0;
constexpr double kAvgMinSec = 60.0;
constexpr unsigned long long kModeEveryMs = 5000;
constexpr long kRateUnknownAbs = 500000;

enum Mode {
    MODE_NONE = 0,
    MODE_NOBATTERY,
    MODE_DISCHARGING,
    MODE_CHARGING,
    MODE_AC
};

struct BatteryReading {
    bool present;
    bool ac;
    bool charging;
    bool discharging;
    double rate_mw;
    double remaining_mwh;
    double full_mwh;
    int percent;
    double life_s;
    bool saver;
};

struct Published {
    wchar_t power[48];
    wchar_t eta[48];
    wchar_t chip[48];
    wchar_t tooltip[2048];
    int power_semantic;
    unsigned long power_color;
    int eta_semantic;
    unsigned long eta_color;
};

struct RawBatteryState {
    unsigned char AcOnLine;
    unsigned char BatteryPresent;
    unsigned char Charging;
    unsigned char Discharging;
    unsigned char Spare1[3];
    unsigned char Tag;
    unsigned long MaxCapacity;
    unsigned long RemainingCapacity;
    long Rate;
    unsigned long EstimatedTime;
    unsigned long DefaultAlert1;
    unsigned long DefaultAlert2;
};

static_assert(sizeof(RawBatteryState) == 32, "SYSTEM_BATTERY_STATE size");
static_assert(offsetof(RawBatteryState, MaxCapacity) == 8, "MaxCapacity offset");
static_assert(offsetof(RawBatteryState, Rate) == 16, "Rate offset");

void SetTextN(wchar_t* dst, size_t n, const wchar_t* src) {
    if (n == 0) return;
    if (!src) src = L"";
    size_t i = 0;
    for (; src[i] && i + 1 < n; ++i) dst[i] = src[i];
    dst[i] = L'\0';
}

template <size_t N>
void SetText(wchar_t (&dst)[N], const wchar_t* src) {
    SetTextN(dst, N, src);
}

void ClearPublished(Published& p) {
    std::memset(&p, 0, sizeof(p));
    SetText(p.power, L"--");
    SetText(p.eta, L"--");
    SetText(p.chip, L"--");
    SetText(p.tooltip, L"正在读取…");
}

struct WBuf {
    wchar_t* d;
    size_t n;
    size_t i;
    WBuf(wchar_t* dst, size_t len) : d(dst), n(len), i(0) {
        if (n) d[0] = L'\0';
    }
    void add(const wchar_t* s) {
        if (!s) return;
        while (*s && i + 1 < n) d[i++] = *s++;
        if (n) d[i] = L'\0';
    }
    void line(const wchar_t* s) {
        if (i > 0) add(L"\r\n");
        add(s);
    }
};

void Format1(wchar_t* out, size_t n, double v) {
    if (n == 0) return;
    out[0] = L'\0';
    if (!(v >= 0.0)) v = 0.0;
    if (v > 999999.0) v = 999999.0;
    int scaled = static_cast<int>(v * 10.0 + 0.5);
    if (scaled < 0) scaled = 0;
    std::swprintf(out, n, L"%d.%d", scaled / 10, scaled % 10);
    out[n - 1] = L'\0';
}

void Format2(wchar_t* out, size_t n, double v) {
    if (n == 0) return;
    out[0] = L'\0';
    if (!(v >= 0.0)) v = 0.0;
    if (v > 999999.0) v = 999999.0;
    int scaled = static_cast<int>(v * 100.0 + 0.5);
    if (scaled < 0) scaled = 0;
    std::swprintf(out, n, L"%d.%02d", scaled / 100, scaled % 100);
    out[n - 1] = L'\0';
}

void FormatHM(double sec, wchar_t* out, size_t n) {
    if (n == 0) return;
    out[0] = L'\0';
    if (!(sec >= 0.0)) sec = 0.0;
    if (sec > 999.0 * 3600.0) {
        SetTextN(out, n, L">999:00");
        return;
    }
    const int total = static_cast<int>(sec / 60.0 + 0.5);
    const int safe = total < 0 ? 0 : total;
    std::swprintf(out, n, L"%d:%02d", safe / 60, safe % 60);
    out[n - 1] = L'\0';
}

void FormatHMS(double sec, wchar_t* out, size_t n) {
    if (n == 0) return;
    out[0] = L'\0';
    if (!(sec >= 0.0)) sec = 0.0;
    if (sec > 999.0 * 3600.0 + 3599.0) sec = 999.0 * 3600.0 + 3599.0;
    const int total = static_cast<int>(sec + 0.5);
    const int h = total / 3600;
    const int m = (total % 3600) / 60;
    const int s = total % 60;
    std::swprintf(out, n, L"%d:%02d:%02d", h, m, s);
    out[n - 1] = L'\0';
}

void FormatAbsMw(wchar_t* out, size_t n, double abs_mw, bool negative) {
    wchar_t num[32];
    Format1(num, 32, abs_mw / 1000.0);
    std::swprintf(out, n, negative ? L"-%ls W" : L"+%ls W", num);
    if (n) out[n - 1] = L'\0';
}

void FormatPlainMw(wchar_t* out, size_t n, double mw) {
    if (!(mw >= 0.0)) {
        SetTextN(out, n, L"--");
        return;
    }
    wchar_t num[32];
    Format1(num, 32, mw / 1000.0);
    std::swprintf(out, n, L"%ls W", num);
    if (n) out[n - 1] = L'\0';
}

double SanitizeRate(long rate) {
    if (rate >= kRateUnknownAbs || rate <= -kRateUnknownAbs) return 0.0;
    return static_cast<double>(rate);
}

double AbsMw(double signed_mw) {
    return signed_mw < 0.0 ? -signed_mw : signed_mw;
}

bool ReadBattery(BatteryReading& out) {
    RawBatteryState bs{};
    SYSTEM_POWER_STATUS ps{};
    const DWORD rc = CallNtPowerInformation(
        static_cast<POWER_INFORMATION_LEVEL>(5),
        nullptr,
        0,
        &bs,
        static_cast<ULONG>(sizeof(bs)));
    if (rc != 0) return false;
    if (!GetSystemPowerStatus(&ps)) return false;

    out.present = bs.BatteryPresent != 0;
    out.ac = bs.AcOnLine != 0;
    out.charging = bs.Charging != 0;
    out.discharging = bs.Discharging != 0;
    out.rate_mw = SanitizeRate(bs.Rate);
    out.remaining_mwh = static_cast<double>(bs.RemainingCapacity);
    out.full_mwh = static_cast<double>(bs.MaxCapacity);
    out.percent = ps.BatteryLifePercent <= 100 ? static_cast<int>(ps.BatteryLifePercent) : -1;
    out.life_s = ps.BatteryLifeTime != 0xFFFFFFFFu ? static_cast<double>(ps.BatteryLifeTime) : -1.0;
    out.saver = ps.SystemStatusFlag == 1;
    return true;
}

using OverlayFn = DWORD (WINAPI*)(GUID*);

OverlayFn OverlayFunction() {
    static OverlayFn fn = nullptr;
    static bool tried = false;
    if (tried) return fn;
    tried = true;
    HMODULE mod = GetModuleHandleW(L"powrprof.dll");
    if (!mod) mod = LoadLibraryW(L"powrprof.dll");
    if (!mod) return nullptr;
    fn = reinterpret_cast<OverlayFn>(GetProcAddress(mod, "PowerGetEffectiveOverlayScheme"));
    return fn;
}

bool SameGuid(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

GUID MakeGuid(unsigned long d1, unsigned short d2, unsigned short d3,
              unsigned char b0, unsigned char b1, unsigned char b2, unsigned char b3,
              unsigned char b4, unsigned char b5, unsigned char b6, unsigned char b7) {
    GUID g{};
    g.Data1 = d1;
    g.Data2 = d2;
    g.Data3 = d3;
    g.Data4[0] = b0;
    g.Data4[1] = b1;
    g.Data4[2] = b2;
    g.Data4[3] = b3;
    g.Data4[4] = b4;
    g.Data4[5] = b5;
    g.Data4[6] = b6;
    g.Data4[7] = b7;
    return g;
}

void MapPowerMode(const GUID& g, wchar_t* out, size_t n) {
    const GUID best_eff = MakeGuid(0x961cc777, 0x2547, 0x4f9d, 0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a);
    const GUID longer = MakeGuid(0x3af9b8d9, 0x7c97, 0x431d, 0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f);
    const GUID balanced = MakeGuid(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    const GUID best_perf = MakeGuid(0xded574b5, 0x45a0, 0x4f42, 0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38);
    if (SameGuid(g, best_eff)) SetTextN(out, n, L"最佳能效");
    else if (SameGuid(g, longer)) SetTextN(out, n, L"更长续航");
    else if (SameGuid(g, balanced)) SetTextN(out, n, L"平衡");
    else if (SameGuid(g, best_perf)) SetTextN(out, n, L"最佳性能");
    else SetTextN(out, n, L"其他");
}

wchar_t LowerChar(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c - L'A' + L'a');
    return c;
}

bool IsWordEnd(wchar_t c) {
    if (c >= L'0' && c <= L'9') return false;
    if (c >= L'A' && c <= L'Z') return false;
    if (c >= L'a' && c <= L'z') return false;
    return true;
}

bool ContainsToken(const wchar_t* s, const wchar_t* token) {
    if (!s || !token || !token[0]) return false;
    for (size_t i = 0; s[i]; ++i) {
        if (i > 0 && !IsWordEnd(s[i - 1])) continue;
        size_t j = 0;
        for (; token[j]; ++j) {
            const wchar_t c = s[i + j];
            if (!c || LowerChar(c) != LowerChar(token[j])) break;
        }
        if (!token[j] && IsWordEnd(s[i + j])) return true;
    }
    return false;
}

int MatchScore(const wchar_t* name, const wchar_t* token) {
    if (!ContainsToken(name, token)) return 0;
    return ContainsToken(name, L"package0") ? 2 : 1;
}

const wchar_t* LabelOf(int kind) {
    if (kind == 0) return L"电池功率";
    if (kind == 1) return L"续航";
    return L"芯片功耗";
}

const wchar_t* IdOf(int kind) {
    if (kind == 0) return L"BatteryPlus_Power";
    if (kind == 1) return L"BatteryPlus_Eta";
    return L"BatteryPlus_Chip";
}

const wchar_t* SampleOf(int kind) {
    if (kind == 0) return L"-888.8 W";
    if (kind == 1) return L"剩 88:88";
    return L"888.8 W";
}

int Width96(int kind) {
    if (kind == 1) return 160;
    return 180;
}

int GapOf(const SIZE& sp) {
    return sp.cx > 0 ? sp.cx : 4;
}

int TextWidth(HDC dc, const wchar_t* text) {
    if (!dc || !text || !text[0]) return 0;
    SIZE sz{};
    if (!GetTextExtentPoint32W(dc, text, static_cast<int>(std::wcslen(text)), &sz)) return 0;
    return sz.cx;
}

int MeasureWidth(HDC dc, int kind) {
    const wchar_t* samples[4];
    int n = 0;
    if (kind == 0) {
        samples[n++] = L"-888.8 W";
    } else if (kind == 1) {
        samples[n++] = L"剩 88:88";
        samples[n++] = L"满 88:88";
        samples[n++] = L"已充满";
        samples[n++] = L"无电池";
    } else {
        samples[n++] = L"888.8 W";
    }
    int value_w = 0;
    for (int i = 0; i < n; ++i) {
        const int w = TextWidth(dc, samples[i]);
        if (w > value_w) value_w = w;
    }
    SIZE sp{};
    GetTextExtentPoint32W(dc, L" ", 1, &sp);
    const int gap = GapOf(sp);
    const int label_w = TextWidth(dc, LabelOf(kind));
    if (label_w <= 0 || value_w <= 0) return Width96(kind);
    return label_w + gap + value_w + gap;
}

}  // namespace

class BatteryPlusPlugin;

static BatteryPlusPlugin* g_plugin = nullptr;

class PluginItem final : public IPluginItem {
public:
    explicit PluginItem(int kind = 0) : kind_(kind) {}

    const wchar_t* GetItemName() const override { return LabelOf(kind_); }
    const wchar_t* GetItemId() const override { return IdOf(kind_); }
    const wchar_t* GetItemLableText() const override { return LabelOf(kind_); }
    const wchar_t* GetItemValueText() const override;
    const wchar_t* GetItemValueSampleText() const override { return SampleOf(kind_); }
    bool IsCustomDraw() const override { return true; }
    int GetItemWidth() const override { return Width96(kind_); }
    int GetItemWidthEx(void* hDC) const override {
        if (!hDC) return Width96(kind_);
        return MeasureWidth(static_cast<HDC>(hDC), kind_);
    }
    void DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) override;
    bool DrawItemEx(IPluginDrawer* pDrawer, int x, int y, int w, int h, bool dark_mode) override;

private:
    int kind_;
};

class BatteryPlusPlugin final : public ITMPlugin {
public:
    BatteryPlusPlugin() {
        items_[0] = PluginItem(0);
        items_[1] = PluginItem(1);
        items_[2] = PluginItem(2);
        ClearPublished(pub_[0]);
        ClearPublished(pub_[1]);
        pub_i_.store(0, std::memory_order_relaxed);
        SetText(pmode_, L"--");
        g_plugin = this;
    }

    BatteryPlusPlugin(const BatteryPlusPlugin&) = delete;
    BatteryPlusPlugin& operator=(const BatteryPlusPlugin&) = delete;

    IPluginItem* GetItem(int index) override {
        if (index < 0 || index >= 3) return nullptr;
        return &items_[index];
    }

    void DataRequired() override;
    const wchar_t* GetInfo(PluginInfoIndex index) override;
    const wchar_t* GetTooltipInfo() override;
    void OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) override;

    const Published& Current() const {
        const int i = pub_i_.load(std::memory_order_acquire) & 1;
        return pub_[i];
    }

    unsigned long LabelColor(bool dark) const {
        if (label_set_) return label_color_;
        return dark ? 0x00FFFFFFu : 0x00000000u;
    }

    unsigned long ValueColor(int kind, const Published& pub, bool dark) const {
        if (kind == 0 && pub.power_semantic) return pub.power_color;
        if (kind == 1 && pub.eta_semantic) return pub.eta_color;
        if (value_set_) return value_color_;
        return dark ? 0x00FFFFFFu : 0x00000000u;
    }

    void DrawGdi(int kind, void* hdc, int x, int y, int w, int h, bool dark);
    bool DrawEx(int kind, IPluginDrawer* drawer, int x, int y, int w, int h, bool dark);

private:
    void Publish(const Published& src) {
        const int cur = pub_i_.load(std::memory_order_relaxed) & 1;
        const int nxt = cur ^ 1;
        pub_[nxt] = src;
        pub_i_.store(nxt, std::memory_order_release);
    }

    void UpdateModel(bool read_ok, unsigned long long now, double dt, bool has_dt);
    void UpdatePowerMode(unsigned long long now);
    void EnsurePdh();
    void ReadPdh();
    bool IsFull() const;
    void Build(Published& out) const;

    PluginItem items_[3];
    Published pub_[2]{};
    std::atomic<int> pub_i_{0};

    bool have_tick_ = false;
    unsigned long long last_tick_ = 0;
    bool have_sample_ = false;
    BatteryReading last_{};
    int prev_mode_ = MODE_NONE;
    int mode_ = MODE_NONE;
    bool prev_ok_ = false;
    bool have_ema_ = false;
    double ema_ = 0.0;
    unsigned long long unplug_tick_ = 0;
    bool since_start_ = false;
    double energy_wh_ = 0.0;
    double measured_s_ = 0.0;
    double eta_s_ = -1.0;
    int eta_source_ = 0;
    double eta20_s_ = -1.0;
    double pph_ = -1.0;
    double on_battery_s_ = -1.0;
    double avg_w_ = -1.0;

    wchar_t pmode_[48]{};
    bool pmode_ready_ = false;
    unsigned long long pmode_tick_ = 0;

    bool pdh_tried_ = false;
    bool pdh_ok_ = false;
    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    double pkg_mw_ = -1.0;
    double pp0_mw_ = -1.0;
    double pp1_mw_ = -1.0;

    unsigned long label_color_ = 0;
    unsigned long value_color_ = 0;
    bool label_set_ = false;
    bool value_set_ = false;
};

const wchar_t* PluginItem::GetItemValueText() const {
    if (!g_plugin) return L"--";
    const Published& pub = g_plugin->Current();
    if (kind_ == 0) return pub.power;
    if (kind_ == 1) return pub.eta;
    return pub.chip;
}

void PluginItem::DrawItem(void* hDC, int x, int y, int w, int h, bool dark_mode) {
    if (g_plugin) g_plugin->DrawGdi(kind_, hDC, x, y, w, h, dark_mode);
}

bool PluginItem::DrawItemEx(IPluginDrawer* pDrawer, int x, int y, int w, int h, bool dark_mode) {
    if (!g_plugin || !pDrawer) return false;
    return g_plugin->DrawEx(kind_, pDrawer, x, y, w, h, dark_mode);
}

void BatteryPlusPlugin::EnsurePdh() {
    if (pdh_tried_) return;
    pdh_tried_ = true;
    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        query_ = nullptr;
        return;
    }
    if (PdhAddEnglishCounterW(query_, L"\\Energy Meter(*)\\Power", 0, &counter_) != ERROR_SUCCESS) {
        PdhCloseQuery(query_);
        query_ = nullptr;
        counter_ = nullptr;
        return;
    }
    PdhCollectQueryData(query_);
    pdh_ok_ = true;
}

void BatteryPlusPlugin::ReadPdh() {
    if (!pdh_ok_ || !query_ || !counter_) {
        pkg_mw_ = -1.0;
        pp0_mw_ = -1.0;
        pp1_mw_ = -1.0;
        return;
    }
    if (PdhCollectQueryData(query_) != ERROR_SUCCESS) return;

    alignas(16) static unsigned char buf[65536];
    DWORD need = 0;
    DWORD count = 0;
    PDH_STATUS st = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &need, &count, nullptr);
    if (static_cast<DWORD>(st) != static_cast<DWORD>(PDH_MORE_DATA) || need == 0 || need > sizeof(buf)) return;
    auto pull = [&]() -> PDH_STATUS {
        count = 0;
        return PdhGetFormattedCounterArrayW(
            counter_,
            PDH_FMT_DOUBLE,
            &need,
            &count,
            reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf));
    };
    st = pull();
    if (static_cast<DWORD>(st) == static_cast<DWORD>(PDH_MORE_DATA) && need > 0 && need <= sizeof(buf)) st = pull();
    if (st != ERROR_SUCCESS) return;

    const auto* items = reinterpret_cast<const PDH_FMT_COUNTERVALUE_ITEM_W*>(buf);
    if (count > 128) count = 128;
    double pkg = -1.0;
    double pp0 = -1.0;
    double pp1 = -1.0;
    int best_pkg = 0;
    int best_pp0 = 0;
    int best_pp1 = 0;
    bool saw = false;
    for (DWORD i = 0; i < count; ++i) {
        const wchar_t* name = items[i].szName;
        if (!name) continue;
        const DWORD cs = items[i].FmtValue.CStatus;
        if (cs != PDH_CSTATUS_VALID_DATA && cs != PDH_CSTATUS_NEW_DATA) continue;
        const double v = items[i].FmtValue.doubleValue;
        if (!(v >= 0.0) || v > 1.0e9) continue;
        saw = true;
        const int sp = MatchScore(name, L"package0_pkg");
        if (sp > best_pkg) {
            best_pkg = sp;
            pkg = v;
        }
        const int s0 = MatchScore(name, L"pp0");
        if (s0 > best_pp0) {
            best_pp0 = s0;
            pp0 = v;
        }
        const int s1 = MatchScore(name, L"pp1");
        if (s1 > best_pp1) {
            best_pp1 = s1;
            pp1 = v;
        }
    }
    if (!saw) return;
    pkg_mw_ = pkg;
    pp0_mw_ = pp0;
    pp1_mw_ = pp1;
}

void BatteryPlusPlugin::UpdatePowerMode(unsigned long long now) {
    if (pmode_ready_ && now - pmode_tick_ < kModeEveryMs) return;
    pmode_ready_ = true;
    pmode_tick_ = now;
    OverlayFn fn = OverlayFunction();
    GUID g{};
    if (!fn || fn(&g) != 0) {
        SetText(pmode_, L"未知");
        return;
    }
    MapPowerMode(g, pmode_, 48);
}

void BatteryPlusPlugin::UpdateModel(bool read_ok, unsigned long long now, double dt, bool has_dt) {
    const double rate = last_.rate_mw;
    const double p = AbsMw(rate);
    int mode = MODE_AC;
    if (!last_.present) mode = MODE_NOBATTERY;
    else if (rate < 0.0 || (last_.discharging && !last_.ac)) mode = MODE_DISCHARGING;
    else if (last_.charging && rate > 0.0) mode = MODE_CHARGING;

    if ((mode == MODE_DISCHARGING || mode == MODE_CHARGING) && p > 0.0) {
        if (prev_mode_ != mode || !have_ema_ || !has_dt || !(dt > 0.0)) {
            ema_ = p;
            have_ema_ = true;
        } else {
            const double alpha = 1.0 - std::exp(-dt / kTauSec);
            ema_ += alpha * (p - ema_);
        }
    } else {
        have_ema_ = false;
        ema_ = 0.0;
    }

    if (mode == MODE_DISCHARGING) {
        if (prev_mode_ != MODE_DISCHARGING) {
            unplug_tick_ = now;
            since_start_ = prev_mode_ == MODE_NONE;
            energy_wh_ = 0.0;
            measured_s_ = 0.0;
        } else if (has_dt && dt > 0.0 && dt <= kMaxGapSec && p > 0.0 && read_ok && prev_ok_) {
            energy_wh_ += (p / 1000.0) * dt / 3600.0;
            measured_s_ += dt;
        }
    } else {
        unplug_tick_ = 0;
    }

    prev_mode_ = mode;
    prev_ok_ = read_ok;
    mode_ = mode;

    eta_s_ = -1.0;
    eta_source_ = 0;
    eta20_s_ = -1.0;
    pph_ = -1.0;
    on_battery_s_ = -1.0;
    avg_w_ = -1.0;

    if (have_ema_ && ema_ > 0.0) {
        if (mode == MODE_DISCHARGING) {
            eta_s_ = last_.remaining_mwh / ema_ * 3600.0;
            eta_source_ = 0;
            if (last_.full_mwh > 0.0) {
                pph_ = ema_ / last_.full_mwh * 100.0;
                if (last_.percent > 20) {
                    const double t = (last_.remaining_mwh - 0.2 * last_.full_mwh) / ema_ * 3600.0;
                    if (t >= 0.0) eta20_s_ = t;
                }
            }
        } else if (mode == MODE_CHARGING && last_.full_mwh > last_.remaining_mwh) {
            eta_s_ = (last_.full_mwh - last_.remaining_mwh) / ema_ * 3600.0;
            eta_source_ = 0;
            if (last_.full_mwh > 0.0) pph_ = ema_ / last_.full_mwh * 100.0;
        }
    }
    if (mode == MODE_DISCHARGING && eta_s_ < 0.0 && last_.life_s >= 0.0) {
        eta_s_ = last_.life_s;
        eta_source_ = 1;
    }

    if (unplug_tick_ != 0) {
        on_battery_s_ = static_cast<double>(now - unplug_tick_) / 1000.0;
        if (on_battery_s_ > 0.0 && measured_s_ > kAvgMinSec) {
            avg_w_ = energy_wh_ / (measured_s_ / 3600.0);
        }
    }
}

bool BatteryPlusPlugin::IsFull() const {
    if (last_.percent >= 100) return true;
    if (last_.full_mwh > 0.0 && last_.remaining_mwh >= last_.full_mwh) return true;
    return false;
}

void BatteryPlusPlugin::Build(Published& out) const {
    ClearPublished(out);

    if (have_sample_ && (mode_ == MODE_DISCHARGING || mode_ == MODE_CHARGING)) {
        const double p = AbsMw(last_.rate_mw);
        if (p > 0.0) {
            FormatAbsMw(out.power, 48, p, mode_ == MODE_DISCHARGING);
            out.power_semantic = 1;
            out.power_color = mode_ == MODE_DISCHARGING ? kAmber : kGreen;
        }
    }

    if (!have_sample_) {
        SetText(out.eta, L"--");
    } else if (mode_ == MODE_NOBATTERY) {
        SetText(out.eta, L"无电池");
    } else if (mode_ == MODE_CHARGING && eta_s_ >= 0.0 && last_.full_mwh > last_.remaining_mwh) {
        wchar_t hm[16];
        FormatHM(eta_s_, hm, 16);
        std::swprintf(out.eta, 48, L"满 %ls", hm);
        out.eta[47] = L'\0';
    } else if (mode_ != MODE_DISCHARGING && IsFull()) {
        SetText(out.eta, L"已充满");
    } else if (mode_ == MODE_DISCHARGING && eta_s_ >= 0.0) {
        wchar_t hm[16];
        FormatHM(eta_s_, hm, 16);
        std::swprintf(out.eta, 48, L"剩 %ls", hm);
        out.eta[47] = L'\0';
        if (last_.percent >= 0 && last_.percent <= 20) {
            out.eta_semantic = 1;
            out.eta_color = kRed;
        }
    } else {
        SetText(out.eta, L"--");
    }

    FormatPlainMw(out.chip, 48, pkg_mw_);

    wchar_t line[192];
    wchar_t a[48];
    wchar_t b[48];
    wchar_t c[48];
    WBuf tip(out.tooltip, 2048);

    if (!have_sample_ || !last_.present || last_.percent < 0) {
        tip.line(L"电量：--");
    } else {
        std::swprintf(line, 192, L"电量：%d%%", last_.percent);
        line[191] = L'\0';
        tip.line(line);
    }

    if (!have_sample_ || !last_.present) {
        tip.line(L"剩余/满电：--");
    } else if (last_.full_mwh > 0.0) {
        Format2(a, 48, last_.remaining_mwh / 1000.0);
        Format2(b, 48, last_.full_mwh / 1000.0);
        std::swprintf(line, 192, L"剩余/满电：%ls / %ls Wh", a, b);
        line[191] = L'\0';
        tip.line(line);
    } else {
        Format2(a, 48, last_.remaining_mwh / 1000.0);
        std::swprintf(line, 192, L"剩余/满电：%ls / -- Wh", a);
        line[191] = L'\0';
        tip.line(line);
    }

    std::swprintf(line, 192, L"实时功率：%ls", out.power);
    line[191] = L'\0';
    tip.line(line);

    if (have_ema_ && ema_ > 0.0 && (mode_ == MODE_DISCHARGING || mode_ == MODE_CHARGING)) {
        FormatAbsMw(a, 48, ema_, mode_ == MODE_DISCHARGING);
        std::swprintf(line, 192, L"平滑功率：%ls", a);
    } else {
        SetTextN(line, 192, L"平滑功率：--");
    }
    line[191] = L'\0';
    tip.line(line);

    if (pph_ >= 0.0 && have_sample_ && (mode_ == MODE_DISCHARGING || mode_ == MODE_CHARGING)) {
        Format1(a, 48, pph_);
        std::swprintf(line, 192, mode_ == MODE_CHARGING ? L"每小时充电：%ls%%" : L"每小时耗电：%ls%%", a);
    } else {
        SetTextN(line, 192, L"每小时耗电：--");
    }
    line[191] = L'\0';
    tip.line(line);

    if (have_sample_ && mode_ == MODE_DISCHARGING && eta_s_ >= 0.0) {
        FormatHM(eta_s_, a, 48);
        std::swprintf(line, 192, L"预计剩余：%ls（%ls）", a, eta_source_ == 1 ? L"系统估计" : L"本插件");
        line[191] = L'\0';
        tip.line(line);
    } else if (have_sample_ && mode_ == MODE_CHARGING && eta_s_ >= 0.0) {
        FormatHM(eta_s_, a, 48);
        std::swprintf(line, 192, L"预计充满：%ls（本插件）", a);
        line[191] = L'\0';
        tip.line(line);
    } else if (have_sample_ && mode_ == MODE_CHARGING) {
        tip.line(L"预计充满：--");
    } else {
        tip.line(L"预计剩余：--");
    }

    if (have_sample_ && mode_ == MODE_DISCHARGING && last_.percent >= 0 && last_.percent <= 20) {
        tip.line(L"到 20%：0:00");
    } else if (eta20_s_ >= 0.0) {
        FormatHM(eta20_s_, a, 48);
        std::swprintf(line, 192, L"到 20%%：%ls", a);
        line[191] = L'\0';
        tip.line(line);
    } else {
        tip.line(L"到 20%：--");
    }

    if (on_battery_s_ < 0.0) {
        tip.line(L"拔电后：--");
    } else {
        FormatHMS(on_battery_s_, a, 48);
        if (avg_w_ >= 0.0) {
            wchar_t num[32];
            Format1(num, 32, avg_w_);
            std::swprintf(b, 48, L"%ls W", num);
            b[47] = L'\0';
        } else {
            SetText(b, L"--");
        }
        if (since_start_) std::swprintf(line, 192, L"拔电后：%ls（自启动），平均 %ls", a, b);
        else std::swprintf(line, 192, L"拔电后：%ls，平均 %ls", a, b);
        line[191] = L'\0';
        tip.line(line);
    }

    FormatPlainMw(a, 48, pp0_mw_);
    FormatPlainMw(b, 48, pp1_mw_);
    FormatPlainMw(c, 48, pkg_mw_);
    std::swprintf(line, 192, L"CPU：%ls", a);
    line[191] = L'\0';
    tip.line(line);
    std::swprintf(line, 192, L"核显：%ls", b);
    line[191] = L'\0';
    tip.line(line);
    std::swprintf(line, 192, L"芯片：%ls", c);
    line[191] = L'\0';
    tip.line(line);

    std::swprintf(line, 192, L"电源模式：%ls", pmode_);
    line[191] = L'\0';
    tip.line(line);
    if (!have_sample_) tip.line(L"节电模式：--");
    else tip.line(last_.saver ? L"节电模式：开" : L"节电模式：关");
}

void BatteryPlusPlugin::DataRequired() {
    const unsigned long long now = GetTickCount64();
    double dt = 0.0;
    bool has_dt = false;
    if (have_tick_) {
        dt = static_cast<double>(now - last_tick_) / 1000.0;
        has_dt = true;
    }
    last_tick_ = now;
    have_tick_ = true;

    BatteryReading cur{};
    const bool read_ok = ReadBattery(cur);
    if (read_ok) {
        last_ = cur;
        have_sample_ = true;
    }
    if (have_sample_) UpdateModel(read_ok, now, dt, has_dt);

    UpdatePowerMode(now);
    EnsurePdh();
    ReadPdh();

    Published built{};
    Build(built);
    Publish(built);
}

const wchar_t* BatteryPlusPlugin::GetInfo(PluginInfoIndex index) {
    switch (index) {
    case TMI_NAME:
        return L"电池增强 BatteryPlus";
    case TMI_DESCRIPTION:
        return L"在任务栏显示电池功率、续航与芯片功耗。插件结构参考了 AzulEterno/PowerMonPlugin-For-TrafficMonitor（MIT）。";
    case TMI_AUTHOR:
        return L"yftx";
    case TMI_COPYRIGHT:
        return L"Copyright (C) 2026 yftx";
    case TMI_VERSION:
        return L"1.0.0";
    case TMI_URL:
        return L"";
    default:
        return L"";
    }
}

const wchar_t* BatteryPlusPlugin::GetTooltipInfo() {
    return Current().tooltip;
}

void BatteryPlusPlugin::OnExtenedInfo(ExtendedInfoIndex index, const wchar_t* data) {
    if (!data || !data[0]) return;
    if (index == EI_LABEL_TEXT_COLOR) {
        label_color_ = static_cast<unsigned long>(std::wcstoul(data, nullptr, 10));
        label_set_ = true;
    } else if (index == EI_VALUE_TEXT_COLOR) {
        value_color_ = static_cast<unsigned long>(std::wcstoul(data, nullptr, 10));
        value_set_ = true;
    }
}

namespace {

void PaintPair(int kind, int x, int y, int w, int h, bool dark,
               int label_w, int gap, unsigned long label_color, unsigned long value_color,
               const wchar_t* label, const wchar_t* value,
               void (*draw)(void* ctx, int x, int y, int w, int h, const wchar_t* text, unsigned long color),
               void* ctx) {
    int lw = label_w;
    if (lw > w) lw = w;
    int vx = x + lw + gap;
    if (vx > x + w) vx = x + w;
    const int vw = x + w - vx;
    draw(ctx, x, y, lw, h, label, label_color);
    if (vw > 0) draw(ctx, vx, y, vw, h, value, value_color);
}

struct GdiCtx {
    HDC dc;
};

struct ExCtx {
    IPluginDrawer* drawer;
};

void DrawGdiText(void* ctx, int x, int y, int w, int h, const wchar_t* text, unsigned long color) {
    auto* g = static_cast<GdiCtx*>(ctx);
    SetTextColor(g->dc, color);
    RECT rc{x, y, x + w, y + h};
    DrawTextW(g->dc, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

void DrawExText(void* ctx, int x, int y, int w, int h, const wchar_t* text, unsigned long color) {
    auto* g = static_cast<ExCtx*>(ctx);
    g->drawer->DrawWindowText(x, y, w, h, text, color, IPluginDrawer::LEFT, false, 255);
}

}  // namespace

void BatteryPlusPlugin::DrawGdi(int kind, void* hdc, int x, int y, int w, int h, bool dark) {
    if (!hdc || w <= 0 || h <= 0) return;
    HDC dc = static_cast<HDC>(hdc);
    const Published& pub = Current();
    const wchar_t* label = LabelOf(kind);
    const wchar_t* value = kind == 0 ? pub.power : (kind == 1 ? pub.eta : pub.chip);
    SIZE ls{};
    SIZE sp{};
    int label_w = 0;
    int gap = 4;
    if (GetTextExtentPoint32W(dc, label, static_cast<int>(std::wcslen(label)), &ls)) label_w = ls.cx;
    if (GetTextExtentPoint32W(dc, L" ", 1, &sp)) gap = GapOf(sp);

    const int old_bk = SetBkMode(dc, TRANSPARENT);
    const COLORREF old_c = SetTextColor(dc, LabelColor(dark));
    GdiCtx ctx{dc};
    PaintPair(kind, x, y, w, h, dark, label_w, gap, LabelColor(dark), ValueColor(kind, pub, dark),
              label, value, DrawGdiText, &ctx);
    SetTextColor(dc, old_c);
    SetBkMode(dc, old_bk);
}

bool BatteryPlusPlugin::DrawEx(int kind, IPluginDrawer* drawer, int x, int y, int w, int h, bool dark) {
    if (!drawer || w <= 0 || h <= 0) return false;
    const Published& pub = Current();
    const wchar_t* label = LabelOf(kind);
    const wchar_t* value = kind == 0 ? pub.power : (kind == 1 ? pub.eta : pub.chip);
    int lw = 0;
    int lh = 0;
    int sw = 0;
    int sh = 0;
    drawer->SetDrawRect(x, y, w, h);
    drawer->GetTextExtent(label, lw, lh);
    drawer->GetTextExtent(L" ", sw, sh);
    const int gap = sw > 0 ? sw : 4;
    ExCtx ctx{drawer};
    PaintPair(kind, x, y, w, h, dark, lw, gap, LabelColor(dark), ValueColor(kind, pub, dark),
              label, value, DrawExText, &ctx);
    return true;
}

extern "C" __declspec(dllexport) ITMPlugin* TMPluginGetInstance() {
    static BatteryPlusPlugin plugin;
    return &plugin;
}
