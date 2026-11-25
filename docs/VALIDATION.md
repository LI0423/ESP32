# Desktop Companion 验证记录

验证日期：2026-11-25

## 代码审查修复

本轮已修复：

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

- ESP-IDF v5.5.4 / ESP32-S3
- 应用：`build/desktop_companion.bin`
- 大小：`0x103440`，约 **1.063MB**
- OTA 单槽 1.5MB，剩余约 **32%**
- 编译优化：`-Os`

## 主机测试

`bash tools/run_tests.sh`：通过。

覆盖专注、庆祝、休息、跨阶段余量传递和异常恢复。

## HTTPS 认证与 JSON 回归

`tools/http_smoke_test.py`：通过（TLS 迁移前的同一认证/解析逻辑）。

```text
HTTP auth and JSON parser smoke test passed
```

验证内容：

- 无令牌修改配置返回 401
- 无令牌重启返回 401
- 合法字符串 JSON 配置返回 200
- 非法亮度返回 400
- 非法 OTA URL 返回 400

HTTPS 服务已在板端成功启动并监听 443，管理页面和 API 已切换到 HTTPS。当前 Mac 已切换到其他 WiFi，`AI_pe` 信号较弱，板端暂时无法取得 STA IP，因此 HTTPS 实际客户端握手待 `AI_pe` 恢复后确认；配网 AP 仍可用于本地访问。

## WiFi / mDNS / NTP

```text
SSID=AI_pe
deskpal.local -> 192.168.10.218
wifi_mode=AP+STA
NTP: ntp.aliyun.com + 162.159.200.1
time_valid=true
```

WiFi 使用 AP+STA，STA 连接失败时仍保留 `DeskPal-Setup` 配网 AP。AP 默认密码为 `deskpal1`，管理接口另需设备令牌。

## 计时恢复

设备多次重启后，NVS 计时记录按版本和范围校验恢复。串口日志实测：

```text
timer started: phase=focus remaining=1358
```

重启前剩余时间没有重置为完整 1500 秒。

## OTA

开发验证时临时启用 HTTP OTA：

```text
OTA before: running_partition=ota_0; expected_after=ota_1
OTA state=downloading written=0 total=1055008 running=ota_0
OTA state=validating written=1055008 total=1055008 running=ota_0
OTA state=ready written=1055008 total=1055008 running=ota_0
OTA state=idle written=0 total=0 running=ota_1
LAN OTA passed and healthy: ota_0 -> ota_1
```

验证后已恢复生产配置：

```text
# CONFIG_COMPANION_OTA_ALLOW_HTTP is not set
```

生产固件默认只接受 HTTPS OTA。

## 看门狗与稳定性

串口持续观察未发现：

```text
task_wdt
panic
stack overflow
```

UI 与 service 任务均订阅并主动喂养 Task Watchdog。

## 管理令牌

- 首次启动生成 24 位十六进制随机令牌
- 只保存在板端 NVS
- 网络 API 不返回令牌
- USB 串口发送 `token` 可本地读取

## 待确认

- 陪伴角色最终视觉风格
- 屏幕实际观感、亮度和动画速度
- 生产环境启用 Secure Boot / Flash Encryption
