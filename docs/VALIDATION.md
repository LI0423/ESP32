# Desktop Companion 验证记录

验证日期：2026-11-25（产品规则更新后部分项待本轮命令实测补齐）

## 产品规则（本轮实现）

- 工作时段：CST-8 本地 08:00–22:00；之外自动待命并临时降亮
- 取消 7 天墙钟快进；改为约 30 分钟 stale-save gap 恢复（见 README；明确不是“用户离开检测”）
- 时间无效：不按墙钟乱切待命/乱降亮
- 离线 UI：阶段表情与文案保持；仅右上角指示点反映网络
- HTTPS 设置页：`POST /api/timer/focus`、`POST /api/timer/standby`（Bearer 令牌）
- 庆祝默认约 8 秒；屏幕显示今日完成轮次（本地 00:00 清零）
- 夜间「开始新一轮」使用 `work_hours_override`，避免立刻被时钟打回待命；「今晚先待命」挂到下一个 08:00（跨午夜不解除）

## 代码审查修复（既有）

本轮之前已修复：

- HTTP JSON 解析释放后使用（UAF）
- 管理接口无认证
- HTTP 请求体无绝对接收期限
- 首次生成令牌可能覆盖有效配置
- 错误 WiFi 凭据可能导致设备失联
- OTA 无总超时、无进展超时和取消机制
- HTTPS 管理页面（自签名证书）
- OTA 自动重定向已禁用
- SNTP 初始化失败不重试
- NVS 计时记录缺少版本和范围校验
- LCD Kconfig 校准值被硬编码覆盖
- 时间有效性没有新鲜度概念
- 配网 AP 密码非法时静默开放
- 关键任务未加入看门狗
- 全屏渲染从 20Hz 降到 10Hz
- 配置结构未初始化
- NVS 保存失败无重试
- 测试脚本、README 和 IDF 版本声明不一致

Secure Boot V2 与 Flash Encryption 属于不可逆 eFuse 操作，未自动启用；生产部署前需单独规划签名密钥。

## 构建

- ESP-IDF v5.5.4 / ESP32-S3（以本机 `export.sh` 为准）
- 应用：`build/desktop_companion.bin`
- 既有记录大小约 **1.063MB** / OTA 槽剩余约 **32%**（本轮是否重新构建见文末实测栏）
- 编译优化：`-Os`

## 主机测试

命令：

```bash
bash tools/run_tests.sh
```

覆盖：

- 专注 / 庆祝 / 休息与跨阶段余量
- 工作时段边界 08:00 / 22:00
- 跨午夜手动待命与下一工作时段解除
- 短间隔（&lt;30min）与长间隔（≥30min）恢复，含工作/非工作时段、不虚增轮次
- 无效 / 未同步时间
- 今日日界 day key
- 离线阶段文案（不再使用 `Waiting for Wi-Fi`）

实测结果：见文末「本轮命令实测」。

## HTTPS 认证与 JSON 回归

`tools/http_smoke_test.py`：需设备在线与管理令牌；逻辑仍覆盖认证与非法亮度/OTA URL。本轮未要求板端联调则不伪造通过结果。

## WiFi / mDNS / NTP

既有验证摘要：

```text
SSID=AI_pe
deskpal.local -> 192.168.10.218
wifi_mode=AP+STA
NTP: ntp.aliyun.com + 162.159.200.1
time_valid=true
```

WiFi 使用 AP+STA，STA 连接失败时仍保留 `DeskPal-Setup` 配网 AP。AP 默认密码为 `deskpal1`，管理接口另需设备令牌。

## 计时恢复

旧行为（7 天快进累加轮次）已移除。新策略见 README「重启恢复策略」。板端实测需在烧录本轮固件后观察串口 `boot policy action=...` 日志（本轮任务禁止 flash，故不伪造板端结果）。

## OTA

既有开发验证曾临时启用 HTTP OTA 并通过；生产配置保持：

```text
# CONFIG_COMPANION_OTA_ALLOW_HTTP is not set
```

## 看门狗与稳定性

既有观察未发现 `task_wdt` / `panic` / `stack overflow`。UI 与 service 任务均订阅并主动喂养 Task Watchdog。

## 管理令牌

- 首次启动生成 24 位十六进制随机令牌
- 只保存在板端 NVS
- 网络 API 不返回令牌
- USB 串口发送 `token` 可本地读取

## 待确认

- 陪伴角色最终视觉风格
- 屏幕实际观感、亮度和庆祝动画
- 生产环境启用 Secure Boot / Flash Encryption
- 板端工作时段 / 手动待命 / 恢复策略的实机体验（需烧录，本轮不做）

## 本轮命令实测

本轮 Cursor CLI Agent 会话内所有 `Shell` 调用均被环境拒绝（返回 `Rejected:`，无 stdout）。因此：

- `bash tools/run_tests.sh`：**未能在本会话执行**（请在本机项目根目录手动运行核对）
- `idf.py build`：**未能在本会话执行**
- `git status` / `git diff --stat`：**未能在本会话执行**

ESP-IDF `export.sh` 路径 `/Users/litengjiang/.espressif/v5.5.4/esp-idf/export.sh` 可通过文件系统读取确认存在，但不代替实际构建结果。

请在本机执行：

```bash
cd /Users/litengjiang/Desktop/MyCode/ESP32/DesktopCompanion
bash tools/run_tests.sh
source /Users/litengjiang/.espressif/v5.5.4/esp-idf/export.sh && idf.py build
git status
git diff --stat
```
