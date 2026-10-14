# Windows 发布与安装

发布版本：2026.10.15。使用 Release 构建，安装包带 Qt、MinGW 运行库、SerialBus/SerialPort 与 SQLite 插件。

双击安装包，默认安装至 `D:\温湿度数据采集系统`，创建或更新桌面快捷方式。便携 ZIP 解压后可运行 `温湿度数据采集系统.exe`。新安装从 `config.example.ini` 创建配置；更新保留原 `config.ini`、`data` 和 `logs`。

更新前关闭程序。安装脚本先校验 SHA-256，对将被覆盖的旧程序文件备份到安装目录 `_update-backups`，不删除运行数据。包内只含示例配置，不含用户测量数据和日志。

复现打包：先执行 `scripts/verify.ps1 -LifecycleCycles 100`，再执行 `python scripts/package.py`。Qt、编译器、CMake、Ninja、7-Zip、WinRAR 路径可由命令行参数覆盖。需要现有 7-Zip 与 WinRAR SFX 模块，不自动安装工具。

产物写入独立 `dist/release-*`，`dist/latest-release.json` 记录路径及哈希。运行目录有源码清单 `release-manifest.json` 和运行文件校验 `SHA256.json`。`install.ps1` 支持 `-InstallDirectory`、`-NoShortcuts`、`-Quiet` 进行隔离安装验证。当前为本地自解压安装器，没有卸载注册项或代码签名；真实硬件测量和长期运行仍按项目验证说明执行。

## 2026-10-15发布修复

配置原子提交成功后才发布内存，保存失败不假报成功；实时曲线包含合法温度边界并保持1200点上限；区域报警与任务采用100条游标分页，未恢复报警置顶，旧待确认报警和旧任务报告可达。本轮验证仅使用模拟设备/临时数据，仍不宣称长期现场可靠性。
