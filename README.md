# Desktop Companion

ESP32-S3 桌面陪伴番茄钟：240×240 单屏角色、自动 25/5 循环、WiFi 网络校时、NVS 恢复与 OTA。

## 当前功能

- 始终只有一屏，不轮播、不翻页
- 自动循环：专注 25 分钟 → 庆祝 3 秒 → 休息 5 分钟 → 下一轮
- WiFi 自动校准时间和日期
- 板端独立运行，不依赖 PC Agent
- 无实体按键、无旋转编码器
- 当前阶段和剩余时间保存在 NVS
- 管理接口需要设备令牌
- WiFi 使用 AP+STA，错误凭据不会移除配网 AP
- 最小网页仅用于 WiFi、亮度和 OTA
- mDNS 地址：`deskpal.local`

角色视觉目前是功能占位版本，后续再选择最终风格。

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

主机测试覆盖专注、庆祝、休息和跨阶段余量传递；HTTP 测试覆盖认证和 JSON 字符串解析。

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

网页只提供 WiFi、亮度和 OTA 设置，不提供计时器控制。所有修改、重启和 OTA 请求都需要 `Authorization: Bearer <token>`。

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
- 每个阶段切换时写入 NVS，不每秒擦写 Flash
- 重启后等待新鲜 NTP 同步，再按阶段保存时间恢复剩余秒数
