# serial-hub-bridge 设计说明

> 设备侧 v0 实现。权威契约是
> [`serial-hub-mcp/docs/remote-link-protocol.md`](../../serial-hub-mcp/docs/remote-link-protocol.md)；
> 本文只解释本固件怎么落地、哪些地方做了取舍。

## 1. 目标与非目标

**目标**

- 让 `serial-hub-mcp` 用 `open({transport:"remote", endpoint:"tcp://ip:7000"})` 像本地串口一样操作远端 UART。
- 字节透明：不 framing、不判读；`read/transact/receive/dump_to_file` 语义与本地一致。
- 独立工程：一个 `idf.py build` 就能烧，不依赖业务 App / BSP / USB CDC。
- 可在 PC 上验证核心语义（ring + READ 决策），不依赖硬件。

**非目标（v0）**

- 设备主动推流、压缩、二进制帧、TLS、多 host 共享。
- 跨重启 SD segment 列表/导出（当前只导出本次启动的 segment）。
- 多端口（固定 `uart0`，但协议字段已处理：其它 `port` 返回 `port_not_found`）。

## 2. 代码分层

```
main/main.c                 Kconfig -> bridge_port / bridge_ctx，启动 net
components/bridge/
  bridge_ring.c/.h          纯 C：绝对偏移环形缓冲（无 IDF 依赖）
  bridge_read.c/.h          纯 C：一次 READ 决策（无 IDF 依赖）
  bridge_port.c/.h          UART 驱动 + ring + 行日志 + 控制线
  bridge_proto.c/.h         NDJSON -> op 分发（cJSON + mbedtls base64）
  bridge_net.c/.h           WiFi STA + TCP server + 会话读行
```

依赖方向：`net → proto → port → (ring, read)`。`ring/read` 保持零依赖，
`test/ring_test.c` 用 gcc 直接编译验证。

## 3. 数据流

```
UART RX ISR/驱动 ──► uart_rx_task ──► ring_append(+abs offsets)
                                   └► line assembler → bounded log

host READ ──► bridge_port_read ──► bridge_read_eval(ring, now, started)
                                   └► 满足条件则 copy 到 scratch → base64 → JSON

host WRITE ──► uart_write_bytes ──► tx_offset += len，TX 日志一条

host TRANSACT ──► barrier = rx_offset
              ──► write
              ──► 同 READ，但从 barrier 起
```

- **单生产者**：只有 `uart_rx_task` 写 ring/log；锁保护 ring + log + tx_offset。
- **单会话**：TCP server 一次只服务一个 client，READ 的 long-poll 期间天然只有一个
  in-flight，符合 v0。
- **唤醒**：`port_ingest` 置 EventGroup bit；`bridge_port_wait` 等它，最长 100 ms 一轮。

## 4. 计数器与偏移

| 名称 | 归属 | 语义 |
|---|---|---|
| `ring.start` / `ring.end` | 设备 RAM | 0-based 绝对字节偏移；`end` = 已收到字节数 |
| `tx_offset` | 设备 | 已写入 UART 的字节数 |
| `log_seq` | 设备 | 行日志条目序号（RX 一行一条、TX 一次写一条） |

`ring` 满时丢最旧：`start` 前进、`dropped` 累加、`end` 继续增长。
因此 `READ.from_offset < ring.start` 必然 `gap=true`。

## 5. READ 决策（与 fake bridge 对齐）

`bridge_read_eval` 每次计算：

```
from      = has_from ? from_offset : ring.end
max_wait  = min(requested_wait, max_hold)          # requested 缺省 5000
idle      = min(idle_timeout, max_wait)            # 缺省 300
budget    = min(budget, max_hold)                  # 缺省 max_wait
limit     = min(max_wait, budget)
max_bytes = min(requested_max_bytes, max_frame)

start = clamp(from, ring.start, ring.end)
gap   = from < ring.start
view_end = ring.end
若 match 命中：view_end = match_end；matched = true
n = min(view_end - start, max_bytes)

if matched                                -> "match"
else if (!match && n >= min_bytes)        -> n >= max_bytes ? "max_bytes" : "min_bytes"
else if elapsed >= idle                   -> "idle"
else if elapsed >= limit                  -> elapsed >= budget ? "budget" : "idle"
else wait min(limit-elapsed, idle-elapsed, 100)
```

边界与主机一致：

- `max_wait_ms=0,budget_ms=0`（flush 用）→ 立即 `idle`，返回空。
- `from_offset` 超过 `end` → clamp 到 `end`；等待行为仍由 idle/budget 决定。
- match 命中但被 `max_bytes` 截断时，`matched` 仍为 true（fake bridge 同样行为）。

## 6. TRANSACT 屏障

```
barrier = bridge_port_rx_offset()   # 写之前
uart_write_bytes(payload)
READ(from_offset = barrier)
```

这样 RTT 期间对端主动吐出的字节不会被误算成响应。响应里回 `barrier_offset`、
`tx_offset`、`tx_log_seq`。host 不应自己用 `WRITE + READ` 拼，否则屏障不精确。

## 7. 行日志与 SD 权威存储

### RAM 行日志（live）

- 固定 N 条元数据 + N × `log_line_max` 文本槽；写满后覆盖最旧（`log_head` 前进）。
- RX 按 `\n` 切行、剥尾部 `\r`；超过 `log_partial_max` 未换行则作为截断条目落一条。
- TX 一次 write 一条。
- `line` 为 ASCII 安全化视图（非可打印替换为 `.`），`hex` 是事实源；
  超长行截断时 `bytes` 仍是全长并置 `truncated:true`。
- `LOG_QUERY` 只查这个 RAM 窗口；`time_from/time_to` 不承诺（无 NTP）。

### SD raw + index（权威）

- SPI2 与 LCD 共用总线，CS=GPIO2；挂载 `/sdcard`，目录 `/sdcard/shb`。
- 每次启动新建一组文件：`raw-rx/idx-rx/raw-tx/idx-tx-<seg>.bin`。
- 索引 20 B/条：`offset(u64) + seq(u32) + tick(u32) + len(u16) + dir + pad`；
  RX 按行、TX 按一次 write。
- UART RX/TX 双写；默认 1 s `fsync`；按目录总大小保留（`BRIDGE_SD_RETAIN_MB`）。
- `LOG_STATS` 读 SD 容量/已用/segment/raw/index/earliest/latest；
  `LOG_EXPORT` 按 `dir/seq_from/cursor/max_bytes` 分页返回 raw + index。
- 无卡 → `log_storage:"ram"`、`log_export:false`，行为回退到 RAM 窗口。

实测（921600 摄像头、90 s 不读 TCP）：SD raw +472,149 B / +6,296 行，同时 RAM ring
已满并累计丢弃 1.3 MB；导出的 frame no 连续。当前限制：只导出本次启动 segment、
导出速度受 WiFi/视频上传影响、时间仍是 uptime。

## 8. 鉴权与并发

- `HELLO` 必须是首帧；token 用长度/字节比较（常量时间风格）。
- 失败回 `unauthorized` 并关闭；未 HELLO 的其它 op 同样关闭。
- 一个连接串行处理请求；不实现 `busy`（v0 host 本来就不 pipeline）。
- 断线即丢会话，重连重新 `HELLO`；游标在 host 侧的 `next_offset`，设备无状态。

## 9. 去重

- `op_id` → 首次 result JSON，32 槽 / 60 s / 单条 ≤ 16 KiB。
- 命中时返回首次结果并置 `deduped:true`（JSON 里补字段）。
- 超出大小上限（例如很大的 TRANSACT 响应）不缓存 → 退化为 `deduped:false`，
  文档已注明；host 不应盲目重试。

## 10. 资源与性能

- ring 默认 256 KiB（PSRAM 优先，失败回落内部 RAM）。
- `max_frame` 默认 32 KiB：响应要走 base64（×4/3）+ cJSON 字符串，64 KiB 帧在无 PSRAM
  的板子上峰值偏高。
- `READ` 的 `max_bytes` 被 `max_frame` 夹住；`scratch` 在启动时一次性分配。
- 115200 无压力；921600 建议 ring ≥ 256 KiB；1.5 M 建议 PSRAM + 1 MiB ring。

## 11. 测试策略

1. **PC 纯逻辑**（已跑）：`test/ring_test.c` 覆盖 append/copy 跨环、溢出丢旧、
   min/max/match/idle/budget、gap/clamp、transact 屏障。
2. **固件构建**（已跑）：ESP-IDF v6.1 `fullclean && build`，0 warning，镜像约 1.3 MB。
   CMake 对 IDF 5.2+ 的 `esp_driver_uart/gpio` 拆分做条件 REQUIRES。
3. **实机**：ESP-IDF v6.1 烧录启动；LCD `160x80 ST7789 ready`，二维码 66×66 px，
   BLE `PROV_00B160` 开始广播；App 配网成功后设备为 `<device-ip>`，
   `tools/smoke.mjs --no-transact` 的 HELLO/CONFIG/WRITE/READ/LOG_STATS/PING/CLOSE 全通过。
   （CONFIG 早期失败是 `uart_set_hw_flow_ctrl` 阈值 128 越界，已改 0/127。）
   接摄像头后确认目标为 **921600 8N1**：8 s 抓 31,834 B、97% 可打印、0 个非法 UTF-8；
   `LOG_QUERY` 返回 `stcloud_init_camera ... frame no` / `ai detect` / `CPU Usage` 等行。
   1500000/2000000 为乱码。
   SD：32 GB 卡 FAT 挂载成功；90 s 空闲窗口 SD 增长 472 KB / 6,296 行，期间 ring
   已满并累计丢弃 1.3 MB；`LOG_EXPORT` 回读的 frame no 连续。
4. **裸协议冒烟**（脚本已验证）：`tools/smoke.mjs`（纯 Node）对真实设备跑
   `HELLO/CONFIG/WRITE/READ/TRANSACT/LOG_STATS/CLOSE`；已用主机 fake bridge 跑通全流程。
5. **主机 E2E**：`serial-hub-mcp` 的 `test/remote_tcp.unit.js` 用 fake bridge 验证同一
   契约；把 `endpoint` 换成设备 IP 即可人工对拍。

## 12. 配网（二维码 + BLE）

采用 ESP-IDF **`network_provisioning`**（IDF 6.x 对 `wifi_provisioning` 的重命名/扩展）：

| 项 | 实现 |
|---|---|
| 传输 | `network_prov_scheme_ble`（NimBLE GATT，默认 protocomm service UUID） |
| 安全 | `NETWORK_PROV_SECURITY_1`（X25519 + PoP + AES-CTR） |
| 设备名 | `PROV_<STA MAC 末 3 字节>`，如 `PROV_00B160` |
| 二维码 | `{"ver":"v1","name":"PROV_...","pop":"...","transport":"ble"}` |
| 显示 | 160×80 ST7789，33 modules × scale 2 = 66×66 px（白底即 quiet zone） |
| 凭据 | 由 provisioning manager / WiFi 驱动写 NVS |
| 恢复 | GPIO0 长按 5 s → `network_prov_mgr_reset_wifi_provisioning()` + `esp_restart()` |

状态机由 `network_prov_mgr` 内部维护；本工程只包一层：
`bridge_prov.c` 负责事件（START/CRED_RECV/CRED_SUCCESS/FAIL/END、STA_DISCONNECTED、
GOT_IP）、LCD 二维码/状态、以及断线重连（1 s 定时器）。配网结束后 manager 释放 BT
（`NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM`），WiFi STA 继续工作。

安全边界：PoP 在二维码里，App 扫码获得；AP 侧无任何 HTTP 服务，只有 BLE。
量产应使用逐台 PoP 或 Security2。

## 13. 与协议文档的差异 / 待办

| 项 | 现状 | 计划 |
|---|---|---|
| `LOG_EXPORT` | ✅ SD `raw+index`（本次启动 segment） | 跨重启 segment 列表 |
| `caps.log_storage` | `"raw+index"`（有卡）/ `"ram"` | — |
| SD 保留 | 4 GB / 目录总量 | 按天/容量轮转与导出 |
| `time` | uptime tick，`synced:false` | SNTP 后给 epoch |
| 多端口 | 仅 `uart0` | 端口表 + Kconfig |
| `busy` | 不返回（串行处理） | 若未来 pipeline 再加 |
| 压缩/二进制帧 | 无 | v1 |
| `BREAK` | `uart_write_bytes_with_break(size=0)` | 实机确认，必要时 GPIO 方案 |