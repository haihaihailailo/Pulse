# 第三方声明

Pulse 本体（`src/`）是原创代码，许可证见仓库根目录的 [LICENSE](LICENSE)（MIT）。本体没有引用、复制或静态链接下列项目的实现。

`extras/trafficmonitor-plugins/` 是早期可独立使用的 TrafficMonitor 插件（NetProbe、BatteryPlus）。下面的声明针对这些插件及其带入的头文件。

## TrafficMonitor

- 作者：zhongyang219
- 项目：<https://github.com/zhongyang219/TrafficMonitor>
- 许可证：Anti-996 License
- 本仓库文件：`extras/trafficmonitor-plugins/PluginInterface.h`

该头文件随 `extras/trafficmonitor-plugins/` 分发。再分发这些插件时，需要同时附上 Anti-996 许可证原文和版权声明。

头文件开头的版权声明如下：

```text
TrafficMonitor 插件接口
Copyright (C) by Zhong Yang 2021
zhongyang219@hotmail.com
```

以下为 TrafficMonitor 仓库 LICENSE 原文（取自 https://github.com/zhongyang219/TrafficMonitor/blob/master/LICENSE ）：

```text
Copyright (c) <2019> <copyright zhongyang219>

"Anti 996" License Version 1.0 (Draft)

Permission is hereby granted to any individual or legal entity
obtaining a copy of this licensed work (including the source code,
documentation and/or related items, hereinafter collectively referred
to as the "licensed work"), free of charge, to deal with the licensed
work for any purpose, including without limitation, the rights to use,
reproduce, modify, prepare derivative works of, distribute, publish
and sublicense the licensed work, subject to the following conditions:

1. The individual or the legal entity must conspicuously display,
without modification, this License and the notice on each redistributed
or derivative copy of the Licensed Work.

2. The individual or the legal entity must strictly comply with all
applicable laws, regulations, rules and standards of the jurisdiction
relating to labor and employment where the individual is physically
located or where the individual was born or naturalized; or where the
legal entity is registered or is operating (whichever is stricter). In
case that the jurisdiction has no such laws, regulations, rules and
standards or its laws, regulations, rules and standards are
unenforceable, the individual or the legal entity are required to
comply with Core International Labor Standards.

3. The individual or the legal entity shall not induce, suggest or force
its employee(s), whether full-time or part-time, or its independent
contractor(s), in any methods, to agree in oral or written form, to
directly or indirectly restrict, weaken or relinquish his or her
rights or remedies under such laws, regulations, rules and standards
relating to labor and employment as mentioned above, no matter whether
such written or oral agreements are enforceable under the laws of the
said jurisdiction, nor shall such individual or the legal entity
limit, in any methods, the rights of its employee(s) or independent
contractor(s) from reporting or complaining to the copyright holder or
relevant authorities monitoring the compliance of the license about
its violation(s) of the said license.

THE LICENSED WORK IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN ANY WAY CONNECTION WITH THE
LICENSED WORK OR THE USE OR OTHER DEALINGS IN THE LICENSED WORK.
```


## TrafficMonitorPlugins

- 作者：zhongyang219
- 项目：<https://github.com/zhongyang219/TrafficMonitorPlugins>
- 许可证：MIT
- 关系：插件结构参考。许可证原文以其仓库中的 LICENSE 为准。

## PowerMonPlugin-For-TrafficMonitor

- 作者：AzulEterno
- 项目：<https://github.com/AzulEterno/PowerMonPlugin-For-TrafficMonitor>
- 许可证：MIT
- 关系：`extras/trafficmonitor-plugins/BatteryPlus` 的插件结构参考。许可证原文以其仓库中的 LICENSE 为准。

## 未纳入本仓库的项目

LibreHardwareMonitor（MPL-2.0）未使用，本仓库不包含其代码。
