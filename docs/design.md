# 设计笔记

现行契约是 [`serial-hub-mcp` 的 `docs/remote-link-protocol-v1.md`](https://github.com/yhuan416/serial-hub-mcp/blob/master/docs/remote-link-protocol-v1.md)。这里只记本固件怎么落地。

## 分层

```
main/main.c
  └─ bridge_net.c          accept TCP
       └─ bridge_v1.c      二进制会话、发送缓存、SD 溢出
            └─ bridge_port.c
                 ├─ bridge_ring.c
                 └─ bridge_read.c
bridge_sd.c                挂载 SD；另有一套旧的行索引，不进 TCP 数据面
bridge_auth.c              NVS token
bridge_prov.c              BLE 配网
```

`bridge_ring.c` 和 `bridge_read.c` 不依赖 IDF。`test/ring_test.c` 在主机上用 gcc 测它们。

## 数据面

UART 任务只往端口环里写。`v1_pump` 从环里拷进该端口的 RAM 发送缓存，不在锁里 `send()`。每个 TCP 会话自己把控制帧和 BATCH 发出去，发不动就留在队列里，先读 ACK。

一个 capture 的 RAM 大小在创建时取 NVS `shb/cap_bytes`。环形下标用这次分配的大小，不能中途改。`cache_set` 只影响下一次新建的 capture。

RAM 窗口满了，先把最旧的一段拷出来，解锁后再写 `/sdcard/shb-cache/pN/<offset>.bin`。写成功才记入溢出表；写失败就 GAP。ACK 越过整段后 `unlink`。

## 不做

- 不再说 NDJSON，也不再按行分配 seq。
- 发送缓存不是日志库。`history_query` 返回 `unsupported`。
