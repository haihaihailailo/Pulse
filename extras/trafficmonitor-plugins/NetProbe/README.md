# 网络探测 NetProbe

TrafficMonitor API 8 插件，作者 yftx，版本 1.0.0。以 C++17 单文件实现，用 MinGW-w64 g++ 13.2（x86_64）构建，不依赖 MFC、ATL、PowerShell 或独立托盘程序。

## 功能与判定

- 后台按检测间隔发送 HTTP GET，使用 WinHTTP 自动系统代理；自动代理会话创建失败时回退默认代理。请求携带 `Connection: close`，不跟随重定向，不下载响应正文；收到任意 HTTP 状态的响应头都算请求成功。延迟从本地址发起探测到 `WinHttpReceiveResponse` 成功计算。
- 优先请求 Url1，失败后依次尝试 Url2、Url3；每次换地址重新计时。第一个成功响应结束本轮，响应较慢时也不会改试备用地址。每个地址具有独立的总超时，多个地址依次超时可能使本轮超过检测间隔；下一轮不会与本轮重叠。
- 默认延迟达到 300 ms 显示较慢；请求失败或延迟达到 1000 ms，记一次不可用。连续三轮不可用才确认断网；断网后只要一轮成功且低于 1000 ms 就恢复，慢速但可用的响应也能恢复。
- 断网计时起点是本轮连续不可用中的第一次结果。确认前无响应保留原显示，高延迟显示“较慢”；确认后显示“断网”或“不可用 xxxx ms”。未完成首次探测时显示“检测中”。
- 显示项标签为“延迟”，正常绿色 `#22C55E`，较慢琥珀色 `#F59E0B`，确认断网红色 `#EF4444`，检测中灰色。使用 API 8 的 IPluginDrawer 绘制，并提供 GDI 回退；标签使用宿主传入的标签颜色。
- 断网与恢复各通知一次并异步播放 `C:\Windows\Media\Windows Notify System Generic.wav`，文件不存在或播放失败时使用 MessageBeep。恢复通知示例：`网络已恢复 · 12:30 · 本次断网 2 分 8 秒`。
- 提示信息包括最近延迟、状态、连续不可用次数、上次断网时间和时长。命令菜单提供“测试提醒”“打开日志”“打开配置文件”。通知通过宿主 UI 线程调用 ITrafficMonitor::ShowNotifyMessage。

## 构建与安装

1. 将 `NetProbePlugin.cpp`、当前提供的 `PluginInterface.h` 和 `build.bat` 放在同一目录。使用 MinGW-w64 g++ 13.2 的 x86_64 工具链，并确保 `g++` 已加入 PATH。
2. 手动运行 `build.bat` 生成 `NetProbe.dll`。脚本使用 C++17、UTF-8 源码与执行字符集、静态运行库，并链接 winhttp、winmm、gdi32、user32、shell32。交付时未执行编译或联网验证。
3. 退出 TrafficMonitor，将 `NetProbe.dll` 放进 TrafficMonitor 的 `plugins` 目录，再启动同为 64 位的 TrafficMonitor。
4. 在“选项 → 插件管理”启用 NetProbe，在主窗口或任务栏的显示设置中勾选“网络探测延迟”显示项。
5. 用插件命令“测试提醒”检查通知和声音。通知样式、出现位置和保留时间由 TrafficMonitor 决定。

## 配置

插件从 `ITrafficMonitor::GetPluginConfigDir()` 返回的目录读取 `NetProbe.ini`。首次运行自动写入默认配置，使用 UTF-16 LE BOM 以兼容 Win32 配置 API 和中文路径/URL；日志始终使用 UTF-8。通过“打开配置文件”编辑，保存后重启 TrafficMonitor 生效。

```ini
[NetProbe]
IntervalSeconds=5
TimeoutSeconds=4
FailThreshold=3
SlowMs=300
UnusableMs=1000
Url1=http://www.msftconnecttest.com/connecttest.txt
Url2=http://www.gstatic.com/generate_204
Url3=http://connect.rom.miui.com/generate_204
```

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| IntervalSeconds | 5 | 两轮探测起点的最小间隔，单位秒，范围 1～86400。耗时超过间隔时紧接下一轮。 |
| TimeoutSeconds | 4 | 每个地址的总超时，单位秒，范围 1～3600，同时设置 WinHTTP 各阶段超时。 |
| FailThreshold | 3 | 连续不可用轮数达到此值时确认断网，范围 1～1000000。 |
| SlowMs | 300 | 较慢阈值，单位毫秒，必须为正整数且小于 UnusableMs。 |
| UnusableMs | 1000 | 不可用阈值，单位毫秒，范围 2～2147483647。等于阈值也计不可用。 |
| Url1 | 微软连接测试地址 | 首选 HTTP/HTTPS 地址；空值使用默认地址。 |
| Url2 | Google 204 地址 | 第一个备用地址；可留空禁用。 |
| Url3 | 小米 204 地址 | 第二个备用地址；可留空禁用。 |

缺失或越界的数值使用对应默认值；SlowMs 大于或等于 UnusableMs 时，两者都恢复默认。插件不覆盖现有配置中的错误值。系统代理配置由 Windows/WinHTTP 决定。

## 日志与运行方式

日志位于同一配置目录下的 `NetProbe.log`，UTF-8 BOM，行格式与 NetMonitor 一致：

```text
[2026-10-20 12:00:00] 启动监测
[2026-10-20 12:01:10] 网络断开：无响应
[2026-10-20 12:02:03] 网络已恢复，断网时长 1 分 3 秒
[2026-10-20 12:30:00] 退出监测
```

也会记录“短暂波动（失败 N 次）后恢复”和高延迟断网原因。日志中的时长使用原脚本的秒/分秒/小时分钟格式；恢复通知始终使用总分钟数和秒数。配置目录需要可写。

专用线程池中的后台线程负责探测，通过异步 WinHTTP 回调、停止事件与剩余超时共同等待响应。显示状态以 64 位原子值发布；DataRequired 仅原子读取，不加锁、不等待。提示信息通过 try_lock 获取快照。停止时用事件立即唤醒间隔和网络等待，关闭异步请求以取消网络操作，等待 WinHTTP 会话卸载通知和系统确认工作回调返回后再释放资源；不会等待整个检测间隔。主窗口销毁时先停止探测，DLL 卸载另有停止回退；进程终止路径不等待已被系统结束的线程。

## 与 NetMonitor.ps1 的关系

`NetMonitor.ps1` 仅作为既有行为的参考，保持原文件不变。默认地址、阈值、连续不可用状态机、断网计时起点、短暂波动日志及恢复条件沿用原脚本。按本次要求，备用地址改为依次探测，确认断网也会立即通知；原脚本并行请求备用地址且仅在恢复时弹卡片。通知改用 TrafficMonitor 的接口，不复制脚本自建的托盘和卡片窗口。

启用插件后可退出原 NetMonitor 托盘程序，并取消原来的开机启动，从而只运行 TrafficMonitor。插件不会安装、启动、关闭或修改原脚本，也不会自动更改原有开机启动项。
