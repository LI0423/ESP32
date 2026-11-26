# Desktop Companion

ESP32-S3 桌面陪伴番茄钟：240×240 单屏角色、工作时段自动 25/5 循环、WiFi 网络校时、NVS 恢复与 OTA。

## 当前功能

- 始终只有一屏，不轮播、不翻页
- 时区默认 `CST-8`；本地 **08:00–22:00** 为工作时段，自动循环：专注 25 分钟 → 庆祝约 8 秒 → 休息 5 分钟 → 下一轮
- **22:00–次日 08:00** 自动待命（Standby）：停止计时循环、不增加完成轮次、屏幕临时降亮（不改写 NVS 亮度配置；退出待命后恢复用户亮度）
- 屏幕显示 **今日进度 `Today N/G`**（本地日界 00:00 把 N 清零；G 默认 4，N 可以超过 G）和 **连续天数 `ND`**（当天达标则加一，当天未达标则在日界清零）
- WiFi 自动校准时间和日期；时间未同步/无效时，不按错误墙钟乱切待命或乱降亮
- Wi-Fi 离线时计时与角色表情/文案仍按当前阶段显示；网络状态仅体现在右上角在线指示点
- 板端独立运行，不依赖 PC Agent
- 无实体按键、无旋转编码器、无触摸、无音频
- 当前阶段、剩余时间、今日目标、连续天数与日程相关状态保存在 NVS（阶段、达标或日界变化时写入；不每秒擦写）
- 管理接口需要设备令牌
- WiFi 使用 AP+STA，错误凭据不会移除配网 AP
- 最小网页用于 WiFi、亮度、今日目标、OTA，以及两个轻量计时操作
- mDNS 地址：`deskpal.local`

角色视觉已换成冻结的 128×128 像素形象，覆盖 FOCUS / BREAK / COMPLETE / STANDBY 四态；时间、轮次和阶段标签由固件叠画。

## 重启恢复策略（stale-save gap）

NVS **只在阶段切换等事件时写入**，`saved_epoch` 是上次保存快照的时间，**不是**精确断电时刻，也 **不能** 据此断言“用户离开了多久”。

因此本产品 **取消**「重启后最多按 7 天墙钟快进并累加 completed_sessions」的行为，改为：

| 条件 | 行为 |
| --- | --- |
| 时间未同步/无效 | 安全恢复已保存的阶段与剩余秒数；不按墙钟切入待命、不快进、不虚增轮次 |
| 距上次保存 **&lt; 约 30 分钟** | 按保存的阶段剩余时间恢复；再结合工作时段规则（非工作时段可进入待命），不虚增轮次 |
| 距上次保存 **≥ 约 30 分钟**，且在工作时段 | 不快进；从一轮全新专注开始；保留当日已统计轮次（跨本地日则清零） |
| 距上次保存 **≥ 约 30 分钟**，且非工作时段 | 不快进；进入待命 |

局限：短于约 30 分钟的停电/重启可以尽量续上当前阶段；更长间隔只保证不虚增轮次与不乱快进，不声称能检测人是否在场。

## 网页轻量操作

在现有令牌保护的 HTTPS 设置页中：

- **开始新一轮**：立即进入全新专注。若在夜间待命中手动触发，会设置临时 `work_hours_override`，避免下一秒又被时钟判回待命；覆盖在次日 08:00 清除，或被「今晚先待命」取消。
- **今晚先待命**：立即待命，直到 **下一个本地 08:00** 才自动恢复。21:00 或 01:00 触发都能跨午夜稳定保持，不会在 00:00 误解除。

## 构建与烧录

使用 ESP-IDF v5.5.4。先导出本机的 IDF 环境（`IDF_PATH` 指向该版本）：

```bash
. "$IDF_PATH/export.sh"
```

`main/admin_cert.h` 内嵌管理 HTTPS 的证书和私钥，已在 `.gitignore` 中，干净检出里没有这个文件。构建前在本机生成，不要把该头文件或私钥提交到 git：

```bash
bash tools/gen_admin_cert.sh
```

脚本用 OpenSSL 生成一张 CN 为 `deskpal.local` 的自签名证书（SAN 包含 `deskpal.local` 和配网地址 `192.168.4.1`），并写入 `main/admin_cert.h`。再次运行会替换本机证书。

```bash
idf.py set-target esp32s3
idf.py build
```

`sdkconfig` 同样被忽略。没有私有 `sdkconfig` 时，`sdkconfig.defaults` 会指定 ESP32-S3、16MB Flash、自定义 `partitions.csv`、八线 PSRAM，以及 `-Os`。Secure Boot 和 Flash Encryption 保持关闭。

烧录串口用环境变量 `ESPPORT`（`idf.py` 会读取它）。不设置时，只有一个串口的话 `idf.py` 会自动选择。

```bash
# 按本机修改。Linux 常见 /dev/ttyACM0，macOS 常见 /dev/cu.usbmodem*
export ESPPORT=/dev/ttyACM0
idf.py flash
idf.py monitor
```

应用输出为 `build/desktop_companion.bin`。ESP-IDF v5.5.4 按上面的默认配置干净构建约 1.07MB，1.5MB OTA 槽剩余约 32%。

## 测试

```bash
bash tools/run_tests.sh
DESKPAL_ADMIN_TOKEN=<串口日志中的令牌> python tools/http_smoke_test.py
```

主机测试覆盖：专注/庆祝/休息循环、工作时段边界（08:00/22:00）、跨午夜手动待命、短/长间隔恢复（含不虚增轮次）、无效时间行为、今日日界、目标默认值与修改、达标加连续天数、未达标清零、离线阶段文案。

## 首次配网

连接 `DeskPal-Setup`，默认密码 `deskpal1`，打开：

```text
https://192.168.4.1
```

板端使用自签名证书，浏览器首次会提示不受信任，确认证书名为 `deskpal.local` 后可继续。生产部署应替换为自己的受信任证书。

已有 WiFi 凭据会从旧项目的 NVS 命名空间自动继承。

首次启动会生成随机管理令牌并输出到串口日志：

```bash
idf.py monitor
```

查找 `Admin token`，在网页的“管理令牌”输入框中填写。令牌只保存在板端 NVS，网页不会返回它。

如果错过首次启动日志，可连接 USB 串口并发送一行 `token`，板端会打印当前管理令牌。串口命令仅用于本地维护，不提供网络控制。

## 网页与 OTA

连接 WiFi 后访问：

```text
https://deskpal.local/
```

网页提供 WiFi、亮度、今日目标（1–99 轮）、OTA，以及「开始新一轮 / 今晚先待命」。不提供计时时长微调。所有修改、重启、OTA 与计时操作都需要 `Authorization: Bearer <token>`。读取配置不需要令牌；修改今日目标走原来的 `POST /api/config`。

生产固件默认只接受 HTTPS OTA。本地开发测试如需 HTTP OTA，在 menuconfig 启用 `COMPANION_OTA_ALLOW_HTTP`，然后运行：

```bash
DESKPAL_ADMIN_TOKEN=<token> python tools/lan_ota_test.py \
  --device-ip <board-ip> --allow-http
```

OTA 支持取消、总超时和无进展超时。Secure Boot V2 与 Flash Encryption 属于不可逆 eFuse 配置，当前未自动启用；生产部署前应单独规划签名密钥并开启。

## 时间与持久化

- 时区默认 `CST-8`，可在 menuconfig 修改
- NTP 默认 `ntp.aliyun.com`，备用 `162.159.200.1`
- SNTP 初始化失败会每 5 秒重试
- 每个阶段/日程相关变化时写入 NVS，不每秒擦写 Flash
- 重启后等待新鲜 NTP 同步，再按上方 **stale-save gap** 策略恢复（不快进轮次）
