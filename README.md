# serial-hub-bridge

ESP32-S3 无线串口桥。把板上的 UART 经 Wi-Fi/TCP 暴露给 [`serial-hub-mcp`](https://github.com/yhuan416/serial-hub-mcp)。

现行链路是二进制帧（魔数 `SH`），不是 NDJSON。设备只推原始字节块（BATCH），host 切行并保存日志。设备侧只留还没被 ACK 的发送缓存。契约在 mcp 仓库的 `docs/remote-link-protocol-v1.md`。

```
目标板 UART ──► ESP32-S3 ──TCP:7000──► serial-hub-mcp
                 uart1 / uart2          host 切行、检索
                 RAM 发送缓存
                 满了溢到 SD
```

## 目录

```
main/main.c                 板级启动：端口、显示、配网、TCP
main/Kconfig.projbuild      menuconfig：端口、UART、SD、配网
components/bridge/          协议与端口
  bridge_v1.c               TCP 会话：HELLO / CONFIG / SUBSCRIBE / BATCH / ACK
  bridge_net.c              接受连接，交给 v1 会话
  bridge_port.c             UART、环形缓冲、控制线
  bridge_ring.c             绝对偏移环（无 IDF 依赖）
  bridge_read.c             从环里取一段（无 IDF 依赖）
  bridge_sd.c               SD 挂载与旧的行索引日志
  bridge_auth.c             NVS token
  bridge_prov.c             BLE 配网
components/display/         ST7789 状态页与二维码
components/qrcodegen/       二维码
test/ring_test.c            主机侧 gcc 测 ring/read
sdkconfig.defaults          本板默认：ESP32-S3、16MB flash、8MB PSRAM
```

`build/`、`managed_components/`、`sdkconfig` 不进库。

## 构建

本机用 ESP-IDF v6.1。没有 `export.sh` 时用 `eim`：

```bash
eim run 'idf.py -C /path/to/serial-hub-bridge set-target esp32s3'
eim run 'idf.py -C /path/to/serial-hub-bridge build'
eim run 'idf.py -C /path/to/serial-hub-bridge -p /dev/ttyACM0 flash'
```

首次构建会拉 `espressif/cjson` 和 `espressif/network_provisioning`。

## 接线

可在 menuconfig 里改。当前默认：

| 信号 | 默认 |
|---|---|
| TCP | `0.0.0.0:7000` |
| UART1 TX / RX | GPIO17 / GPIO18 |
| UART2 TX / RX | GPIO4 / GPIO5 |
| 配网长按 | GPIO0，5 秒 |
| SD CS | GPIO2，挂载点 `/sdcard` |
| LCD | ST7789，SPI2，160×80 |

LCD 右下角显示 `U1 TX17 RX18`、`U2 TX4 RX5`。初始化失败不影响桥，二维码仍打在启动日志里。

## 鉴权与缓存

首次启动在 NVS 命名空间 `shb` 生成 token，并打到串口：

```
bridge_auth: auth token from NVS: <token>
```

主机：

```text
open({ remote: "esp32-s3", port_id: "uart1", baud_rate: 1500000 })
```

`port_id` 为 `uart1` 或 `uart2`。未配网时二维码 payload 里也带这个 token。

发送缓存大小同样在 `shb` / `cap_bytes`。默认 256KiB，允许 32KiB–2MiB。主机 `cache_set({ connection_id, cap_bytes })` 写入 NVS。已有 capture 下次新建才换大小。RAM 满且 SD 可用时，未 ACK 字节写到 `/sdcard/shb-cache/`；写失败才丢弃并 GAP。

## 配网

BLE Security1 + PoP，用 ESP BLE Provisioning App 扫 LCD 上的二维码。长按 GPIO0 五秒清除 Wi-Fi 配网并重启。Wi-Fi 凭据由 provisioning manager 自己存，不在 `shb` 里。

## 和旧 SD 日志的关系

`components/bridge/bridge_sd.c` 仍会把 UART 行写到 `/sdcard/shb`，供本机排障。TCP 会话不再把这些行索引当协议。host 的 `log_query` 查的是它自己切开的缓存。
