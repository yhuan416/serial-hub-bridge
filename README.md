# serial-hub-bridge

ESP32-S3 无线串口桥（**设备侧 v0**），对接
[`serial-hub-mcp`](../serial-hub-mcp) 的远端链路契约：
[`docs/remote-link-protocol.md`](../serial-hub-mcp/docs/remote-link-protocol.md)。

一个独立、最小化的 ESP-IDF 工程：**UART ↔ WiFi/TCP/NDJSON**，不带 BSP、LVGL、USB CDC、
业务 App。参考了 `Wireless_USB_CDC_ACM` 的 `feat/esp_idf` 分支里 `uart_port` /
`cdc_uart_bridge` / `sdlog` 的驱动与环形缓冲写法，但代码是重新组织的，不依赖那个仓库。

```
┌──────────┐   UART    ┌──────────────────────────┐   TCP/NDJSON   ┌────────────────┐
│ 目标板/  │◄─────────►│  serial-hub-bridge (S3)  │◄──────────────►│ serial-hub-mcp │
│ 被调试设备│  TX/RX    │  ring + offsets + log    │  request/ack   │  (host, MCP)   │
└──────────┘           └──────────────────────────┘                └────────────────┘
```

更细的分层、READ 决策与取舍见 [docs/design.md](docs/design.md)。

## 已实现（v0）

| op | 状态 | 说明 |
|---|---|---|
| `HELLO` | ✅ | `proto:"v0"`、token 鉴权、caps/ports/time |
| `CONFIG` | ✅ | baud / data_bits / stop_bits / parity / flow_control |
| `WRITE` | ✅ | 一帧一次写，返回 `tx_offset` / `tx_log_seq` |
| `READ` | ✅ | long-poll：`match` / `min_bytes` / `max_bytes` / `idle` / `budget` |
| `TRANSACT` | ✅ | 设备侧 `barrier_offset`，排除写前字节 |
| `SET_LINES` | ✅（按接线） | RTS 走 UART，DTR 走普通 GPIO；未接线返回 `unsupported` |
| `BREAK` | ✅（需实机确认） | `uart_write_bytes_with_break(size=0)` |
| `PING` | ✅ | `t_ms` 回显 + `device_tick_ms` |
| `LOG_STATS` | ✅ | ring/SD/RAM 统计、`segments[]`、`ram_*`/`sd_*` seq、health/queue/overflow 计数 |
| `LOG_QUERY` | ✅（RAM） | 按 `seq/dir` 过滤、`limit` 分页、`cursor`；line/hex 受 `log_line_max` 截断 |
| `LOG_CLEAR` | ✅ | 省略 `segment` 清所有段并重置 seq；指定段只清该段 |
| `LOG_EXPORT` | ✅（需 SD） | 按 `segment` 导出 SD `raw+index`；省略导出活动段 |
| `EVENT_QUERY` | ✅ | 控制面事件：SD 挂载/重挂载/写失败、ring drop |
| `INFO` / `LIST_PORTS` / `CLOSE` | ✅ | |

`caps`（HELLO 返回）：`auth` 按 token、`has_sd` 按卡、`log_storage` 为
`"raw+index"`/`"ram"`、`log_export` 按卡、`events:true`、`can_set_lines` 按接线、`can_break:true`、
`can_config:true`、`binary_frames:false`。

### 鉴权

首次启动会在 NVS 生成 16 字节随机 token（`CONFIG_BRIDGE_AUTH_TOKEN` 非空则覆盖），
串口打印：

```
I (1575) bridge_auth: auth token from NVS: <token>
I (2519) bridge_net: auth=on
```

主机 `open({transport:"remote", endpoint:"tcp://<ip>:7000", token:"<token>"})`。
未配网时 QR payload 也带 `"token":"..."`。v0 仍是明文，跨不可信网络请走 VPN/SSH；
换 token 可清 NVS/恢复出厂后重启。

## 数据面语义（与主机一致）

- **绝对偏移 0-based**：`rx_offset` = 已接收字节数，下一个字节位置就是它；
  `READ.from_offset = N` 返回位置 `>= N` 的字节。
- **TRANSACT 屏障**：设备在受理写之前取 `barrier`，响应只从 `barrier` 起，
  等价于本机 `Connection` 的 `base = _length`。
- **gap**：`from_offset < ring_start_offset` 时 `gap=true`，从 `ring_start_offset` 起返回；
  host 会把它计入 `rx_dropped`。
- **结束优先级**：`match > max_bytes/min_bytes > idle > budget`，
  `stopped_reason ∈ {match, max_bytes, min_bytes, idle, budget}`。
- **幂等**：`READ` 只读不消耗；`WRITE/TRANSACT` 支持 `op_id` 去重（32 条 / 60s，
  结果 ≤ 16 KiB 才缓存；重复请求返回首次结果并置 `deduped:true`）。
- **背压**：ring 满丢最旧，`LOG_STATS.ring.dropped_bytes` 单调递增。
- **UART/SD 解耦**：UART task 只写 4 MiB ring/RAM 日志，并把行/TX 写入 3 MiB PSRAM
  queue；独立 writer task 做 raw+index。UART 不再等 SD/fsync。
- **可观测丢失**：`uart.overflow`（UART 驱动缓冲溢出）、`sd.queue_drop`（PSRAM queue 满）、
  `sd.write`/`sd.remount` 都会进 `EVENT_QUERY`，并在 `LOG_STATS` 计数。
- **partial 行**：sync 周期会把未结束的 RX 行落成独立条目，避免一直等换行。

## 快速开始

前置：ESP-IDF（**v6.1 已完整编译验证**；5.2+ 的驱动拆分已在 CMake 里做条件兼容），目标 `esp32s3`。

```bash
# 1) 配置 UART / SD / BLE 配网参数
idf.py menuconfig      # serial-hub-bridge -> Network/provisioning, SD log, UART

# 2) 构建 + 烧录 + 监视
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

首次构建会通过组件管理器拉取 `espressif/cjson` 到 `managed_components/`（已 gitignore），
需要能访问 components.espressif.com；离线环境可把该目录预置好。

当前镜像约 **1.3 MB**（BLE/NimBLE + LCD/QR + FATFS/SD）。板级默认（16 MB flash、
4 MB factory、8 MB PSRAM、NimBLE、LCD/SD 引脚）都已写进 `sdkconfig.defaults`，直接构建：

```bash
idf.py build
```

启动日志会打印：

```
I (1234) bridge_net: got ip 192.168.1.50
I (1235) bridge_net: listening on tcp://0.0.0.0:7000 (proto v0)
```

默认接线（可在 menuconfig 改）：

| 信号 | 默认 | 说明 |
|---|---|---|
| UART | 1 | 避免占用 UART0 console |
| TX | GPIO17 | |
| RX | GPIO18 | 上拉 |
| RTS | 未接 | 接 EN 可做复位（`SET_LINES rts`，低有效） |
| CTS | 未接 | `flow_control=rtscts` 时必需 |
| DTR | 未接 | 接 GPIO0 可做下载模式（低有效） |

## 配网（二维码 + BLE）

使用 ESP-IDF 的 **`network_provisioning`**（IDF 6.x 的 `wifi_provisioning` 继任者），
BLE Security1 + PoP，二维码就是官方 payload：

```json
{"ver":"v1","name":"PROV_00B160","pop":"abcd1234","transport":"ble","token":"<nvs-token>"}
```

- BLE 名：`<BRIDGE_PROV_BLE_PREFIX><MAC 末三字节>`，默认 `PROV_00B160`
- PoP：`BRIDGE_PROV_POP`，默认 `abcd1234`（量产应逐台不同，并随二维码下发）
- 未配网：160×80 ST7789 显示二维码（33 modules × scale 2 = **66×66 px**），
  串口同时打印 `QR payload: ...`；用 ESP BLE Provisioning App 扫码即可
- 配网成功：隐藏二维码、显示 SSID/IP；串口桥在设备 STA IP 的 7000 端口
- 长按 **GPIO0 5 秒**：`network_prov_mgr_reset_wifi_provisioning()` + 重启，回到配网模式
  （也可调用 `bridge_prov_factory_reset()`）
- 凭据由 provisioning manager / WiFi 驱动存 NVS；`bridge_prov` 只负责状态与显示

LCD 是正点原子 ESP32-S3 板的 160×80 ST7789（SPI2）：

| 信号 | GPIO |
|---|---|
| SCLK / MOSI / MISO | 12 / 11 / 13 |
| DC / CS / RST / PWR | 40 / 39 / 38 / 41 |

板子不同就改 `components/display/spilcd.h` 的引脚宏；LCD 初始化失败不影响 BLE 配网，
二维码仍会打印在串口日志里。

## SD 权威日志

SD 卡与 LCD 共用 SPI2（CS=GPIO2），上电挂载到 `/sdcard`，日志目录 `/sdcard/shb`。
`LOG_STATS.sd_queue` 显示 UART→SD 队列水位；`uart_overflow_bytes`/`ring.dropped_bytes`
是 UART/ring 侧丢失计数。默认 UART 驱动缓冲 32 KiB、SD queue 3 MiB（可 menuconfig 调整）。
每次启动/重挂载会扫描已有 segment，**续写最新完整段**并恢复 seq；达到
`CONFIG_BRIDGE_SD_SEGMENT_MAX_MB`（默认 3072）滚动新段，避免 FAT32 4 GiB 单文件上限。
`LOG_STATS.segments[]` 列出全部段，旧段可按 `segment` 导出/清理。


- 每个方向独立文件：`raw-rx-<seg>.bin` / `idx-rx-<seg>.bin`，TX 同理
- 索引 20 B/条：`offset(u64) + seq(u32) + tick(u32) + len(u16) + dir + pad`
- UART RX/TX 双写；默认 1 s `fsync`；按总大小保留（`BRIDGE_SD_RETAIN_MB`，默认 4 GB）
- `LOG_STATS` 返回 SD 容量/已用/segment/raw/index/earliest/latest
- `LOG_EXPORT` 按 `dir/seq_from/cursor/max_bytes` 分页返回 raw + index；host 侧用
  `tools/log_export.mjs` 还原文本
- 卡不是 FAT（exFAT/空卡）时默认挂载失败回退 RAM；`BRIDGE_SD_FORMAT_IF_FAILED=y`
  会**清空并格式化**卡
- 运行期每 10 s 尝试重挂载；`fwrite` 失败会把 SD 标记 broken、卸载后自动重挂，
  这段时间继续写 RAM ring，并发 `sd.write`/`sd.remount` 事件
- 默认 ring **4 MiB（PSRAM）**：1.5M 满速约 27 s，卡片机 ~1.4 KB/s 约 48 分钟
- 控制面事件走独立 `EVENT_QUERY`（RAM 64 条，同时串口打印），不占数据日志

实测（921600 摄像头日志、90 s 不读 TCP；当时 ring 还是 256 KB）：

```
SD  raw 519,600 -> 991,749   (+472,149 B / +6,296 行)
RAM ring used=262,144 (满)   dropped=1,306,717
```

即 ring 已经覆盖不下，但 SD 完整保留了这 90 s；导出内容是可读的
`[stcloud_init_camera] ... frame no` 文本。当前限制：只导出**本次启动的 segment**、
导出页 32 KB/512 行；cursor 是索引序号，首屏按 seq 二分定位，之后 64 条/块读，
通常 <1 s/页（WiFi 拥塞时会慢）。时间仍是 uptime（无 NTP）。

## 与主机联调

主机侧（Node ≥ 18）：

```bash
cd ../serial-hub-mcp
npm install && npm test          # 62 项，含 fake bridge
npx serial-hub-mcp               # 或 node src/server.js
```

MCP 工具调用示例（`endpoint` 指向设备 IP；波特率按目标传：旧摄像头 921600，当前卡片机 **1500000**）：

```text
open({ transport: "remote", endpoint: "tcp://192.168.1.50:7000",
       baud_rate: 1500000, token: "<token>" })
transact({ connection_id: "...", data: "AT", newline: "crlf", match: "OK", timeout_ms: 2000 })
receive({ connection_id: "...", start: "flush", budget_ms: 3000, idle_timeout_ms: 300 })
get_status({ connection_id: "..." })     # remote.caps / gap_count / fw_version
dump_to_file({ connection_id: "...", budget_ms: 10000 })
close({ connection_id: "..." })
```

不装 MCP 也能做裸协议冒烟（纯 Node，无依赖）：

```bash
node tools/smoke.mjs tcp://192.168.1.50:7000 --baud 1500000 --no-transact --token <token>
```

它会依次跑 `HELLO → CONFIG → WRITE/READ → TRANSACT → LOG_STATS → CLOSE` 并断言字段。

> 注意：v0 是明文链路。跨不可信网络请套 VPN / SSH 隧道；token 只是最低门槛。

## 配置项

| Kconfig | 默认 | 说明 |
|---|---|---|
| `BRIDGE_TCP_PORT` | 7000 | |
| `BRIDGE_AUTH_TOKEN` | 空 | 空 = 关闭鉴权（`caps.auth=false`） |
| `BRIDGE_UART_NUM` | 1 | |
| `BRIDGE_UART_TX_GPIO` / `RX_GPIO` | 17 / 18 | |
| `BRIDGE_UART_RTS_GPIO` / `CTS_GPIO` / `DTR_GPIO` | -1 | -1 = 未接线 |
| `BRIDGE_UART_BAUD` | 1500000（当前卡片机；旧摄像头 921600；Kconfig 兜底 115200） | CONFIG 可运行时改 |
| `BRIDGE_RING_BYTES` | 4194304 | 4 MiB PSRAM ring；1.5M 满速约 27 s |
| `BRIDGE_UART_RX_BUF` | 32768 | UART 驱动缓冲；吸收短 CPU/Wi‑Fi 停顿 |
| `BRIDGE_SD_QUEUE_BYTES` | 3145728 | UART→SD writer PSRAM queue；1.5M 约 16 s |
| `BRIDGE_MAX_FRAME` | 32768 | 同时限制 `READ.max_bytes` |
| `BRIDGE_MAX_HOLD_MS` | 10000 | 超过被 clamp 并回 `hold_clamped` |
| `BRIDGE_LOG_ENTRIES` | 256 | 0 = 关闭 RAM 行日志 |
| `BRIDGE_LOG_LINE_MAX` | 192 | 超长行截断（`bytes` 仍为全长） |
| `BRIDGE_SD_ENABLE` | y | SD `raw+index` 权威日志 |
| `BRIDGE_SD_CS_GPIO` | 2 | SPI2 与 LCD 共用 |
| `BRIDGE_SD_FORMAT_IF_FAILED` | n | 挂载失败自动格式化（**清空卡**） |
| `BRIDGE_SD_RETAIN_MB` | 4096 | 日志目录保留上限 |
| `BRIDGE_SD_SYNC_MS` | 1000 | `fsync` 周期 |
| `BRIDGE_EVENT_ENTRIES` | 64 | 控制面事件环形队列 |
| `BRIDGE_PROV_POP` | `abcd1234` | BLE Security1 PoP，二维码里带 |
| `BRIDGE_PROV_BLE_PREFIX` | `PROV_` | 最终 BLE 名 `<prefix>XXXXXX` |
| `BRIDGE_LCD_ENABLE` | y | 160×80 ST7789 显示二维码/状态 |
| `BRIDGE_BUTTON_ENABLE` | y | GPIO0 长按恢复出厂配网 |
| `BRIDGE_BUTTON_LONG_PRESS_MS` | 5000 | 长按阈值 |

内存粗略：ring（默认 4 MiB，PSRAM）+ SD queue（3 MiB，PSRAM）+ 行日志（256 × (192+40) ≈
58 KiB）+ 部分行 1 KiB + 响应 scratch 32 KiB + 运行时 base64/JSON 峰值。无 PSRAM 的 S3 建议把
`BRIDGE_RING_BYTES` 降到 128 KiB、`BRIDGE_MAX_FRAME` 降到 16 KiB；无 PSRAM 时 4 MiB 会自动回退内部 RAM 并可能失败。

## 主机侧纯逻辑测试

`bridge_ring` / `bridge_read` 不依赖 ESP-IDF，可直接在 PC 上验证 v0 的停止条件、
跨环匹配、gap、屏障等（不需要硬件）：

```bash
./test/run.sh
# ring/read: all checks passed
```

固件已用 **ESP-IDF v6.1** 做过 `fullclean && build`：0 warning，镜像约 **1.3 MB**
（4 MB factory 分区）。实机已验证：16 MB flash / 8 MB PSRAM、160×80 ST7789 LCD ready、
二维码 66×66 px、BLE `PROV_00B160` 广播；App 配网成功后设备地址为 `<device-ip>`，主机跑

```bash
node tools/smoke.mjs tcp://<device-ip>:7000 --no-transact
```

HELLO / CONFIG / WRITE / READ / LOG_STATS / PING / CLOSE 全部通过。
（`CONFIG` 曾因 `uart_set_hw_flow_ctrl` 阈值给了 128 返回 ESP_FAIL，已改为 0/127。）

旧摄像头确认 **921600 8N1**：8 秒抓到 31,834 B，可打印率 97%、
UTF-8 非法 0；`LOG_QUERY` 能解析出：

```
[stcloud_init_camera] live_stream_read_callback_impl ... frame no :407 ...
ai detect ret:0 cnt:0 ch:0 w:416 h:224 ...
CPU Usage: 41%
```

旧摄像头在 1500000 / 2000000 下是乱码。新卡片机 WiFi 侧为 **1,500,000 8N1**：
10 秒 14.2 KiB / 228 行、97.5% 可打印（`XMWEI`/`WDG_HAL`/`KEEPALIVE`/电池），板级默认已改为 1500000。
`TRANSACT` 需要目标能应答；当前是持续日志，用 `--no-transact`。host 侧 `open` 按目标传
`baud_rate`（旧摄像头 921600，卡片机 1500000）或存 profile。

SD 已实机验证：32 GB 卡格式化/FAT 挂载成功，`LOG_STATS` 显示 31 GB、`raw+index`；
TX 的 `LOG_EXPORT` 精确回读；90 s 空闲窗口 SD 增长 472 KB / 6,296 行，同时 ring
已满并累计丢弃 1.3 MB。详见上文“SD 权威日志”。

## 已知限制 / 下一步

1. **SD 跨重启导出**：当前只导出本次启动的 segment；旧 segment 仍在卡里，
   但缺少协议级 segment 列表/导出。
2. **时间**：无 NTP，`ts`/`time` 是 uptime（`source:"tick"`, `synced:false`）；
   接 SNTP 后可对齐 epoch。
3. **多端口**：目前固定 `uart0`；契约已支持 `port_id`。
4. **传输扩展**：`ws / udp / kcp / mqtt`（先做 `ws` 最省事，复用同一 NDJSON）。
5. **断线重连状态**：当前 host 重连即重新 `HELLO`，用 `next_offset` 续拉；
   设备侧无会话，符合 v0。
6. **压缩 / 二进制帧**（v1）。
7. **OTA**：当前 4 MB factory 单分区，后续可加 ota_0/ota_1。
8. `BREAK` 依赖 IDF 的 `uart_write_bytes_with_break(size=0)`，需在实机上确认；
   若你的 IDF 版本不生效，可改成 GPIO 拉低 TX 的方案。
9. **配网安全**：当前 Security1 + 固定 PoP；量产建议逐台 PoP 或 Security2（SRP6a）。
   重新配网：长按 GPIO0 5 秒（清凭据 + 重启），或调 `bridge_prov_factory_reset()`。
10. **LCD/SD 引脚**：固定为验证板（SPI2 + LCD CS39/SD CS2）；换板需改
    `spilcd.h` 与 `BRIDGE_SD_CS_GPIO`。
11. **LOG_QUERY vs SD**：`LOG_QUERY` 仍只查 RAM 窗口；历史统一走 `LOG_EXPORT`。
12. **事件日志**：`EVENT_QUERY` 是 RAM 环形（默认 64 条），重启丢失；SD 写失败/重挂载
    期间的数据仍只在 4 MiB ring + host 缓存里。

## 与 `feat/esp_idf` 的关系

参考了其 `components/uart_port`（UART 驱动封装、line coding 映射）、
`components/cdc_uart_bridge`（PSRAM ring、绝对游标思路）、`components/sdlog`（按时间
切文件、flush 任务）。本工程刻意**不**引入 BSP / LVGL / USB CDC / App 框架，
只保留最小 LCD/QR/SD 驱动，便于单独维护和烧到任意 S3 板。

## License

MIT（与 `serial-hub-mcp` 一致）。