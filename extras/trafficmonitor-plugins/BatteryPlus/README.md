# BatteryPlus

TrafficMonitor 插件，在任务栏显示电池功率、续航和整颗芯片功耗。读取电池与能耗计数器不需要管理员权限。

## 功能

- **电池功率**：放电为 `-12.3 W`（琥珀色 `#F59E0B`），充电为 `+33.7 W`（绿色 `#22C55E`），外接电源且未充放电时为 `--`。
- **续航**：放电为 `剩 2:13`，电量 ≤ 20% 时数值为红色 `#EF4444`；充电为 `满 0:42`；已经充满为 `已充满`；没有电池为 `无电池`。
- **芯片功耗**：整颗芯片功耗，例如 `8.4 W`。计数器不可用时为 `--`。
- 鼠标悬停可查看电量、剩余/满电容量、实时与平滑功率、每小时耗电、预计剩余（注明本插件或系统估计）、到 20% 的时间、拔电后时长与平均功率、CPU / 核显 / 芯片功耗、电源模式和节电模式。

三个显示项可以在 TrafficMonitor 里分别勾选。

## 安装

1. 安装 MinGW-w64 g++ 13.2（x86_64），并把它加入 PATH。在本目录运行 `build.bat`，生成 `BatteryPlus.dll`。
2. 将 `BatteryPlus.dll` 放入 TrafficMonitor 的 `plugins` 目录。
3. 打开 TrafficMonitor 的插件管理，启用「电池增强 BatteryPlus」。
4. 在显示设置中分别勾选「电池功率」「续航」「芯片功耗」。

## 数据来源

采样在 `DataRequired` 中完成。TrafficMonitor 大约每秒调用一次，用 `GetTickCount64` 计算间隔，不阻塞。

- `CallNtPowerInformation(SystemBatteryState)`：是否接交流电、充放电状态、功率（mW，正充负放）以及剩余/满电容量（mWh）。返回值失败时沿用上次读数。功率绝对值 ≥ 500000 mW 视为未知。
- `GetSystemPowerStatus`：电量百分比、节电模式，以及系统估计的剩余秒数（`0xFFFFFFFF` 为未知）。
- `PowerGetEffectiveOverlayScheme`：当前电源模式。GUID 对应「最佳能效」「更长续航」「平衡」「最佳性能」。
- PDH 计数器 `\Energy Meter(*)\Power`：`rapl_package0_pkg`（芯片）、`pp0`（CPU）、`pp1`（核显），单位 mW。使用 `PdhAddEnglishCounterW` 与 `PdhGetFormattedCounterArrayW`，查询只建立一次。某项不可用时显示 `--`。

续航对放电或充电功率做时间常数 30 秒的指数平滑（`alpha = 1 - exp(-dt/30)`）。剩余时间 = 剩余 mWh / 平滑功率；充满时间 =（满电 − 剩余）/ 平滑充电功率；到 20% 的时间 =（剩余 − 20% × 满电）/ 平滑功率。只有在放电且本插件算不出时间时，才改用系统估计。拔电后的平均功率用累计能量除以实测秒数；间隔大于 5 秒的样本不计入。

## 致谢

插件结构参考了 [AzulEterno/PowerMonPlugin-For-TrafficMonitor](https://github.com/AzulEterno/PowerMonPlugin-For-TrafficMonitor)（MIT）。电池读取与续航算法移植自同机实测可用的 Python 实现。
