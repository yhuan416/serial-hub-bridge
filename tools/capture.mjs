#!/usr/bin/env node
// Continuous v0 READ capture with reconnect/resume (no npm deps, Node >= 18).
//
//   node tools/capture.mjs tcp://<device-ip>:7000 --baud 921600 --seconds 30
//
// Options:
//   --baud <n>          UART baud sent via CONFIG (default 115200)
//   --seconds <n>       capture duration (default 30)
//   --out <path>        output file (default /tmp/serial-hub-capture-<epoch>.log)
//   --from end|start|<offset>  default end (live); start replays the retained ring
//   --token <t>         HELLO token
//   --min <n>           batch size before returning (default 256)
//   --read-timeout <ms> READ timeout; on timeout reconnect and resume (default 8000)
//   --max-bytes <n>     per-READ cap (default 8192; smaller sends survive busy WiFi)

import net from "node:net";
import fs from "node:fs";
import { StringDecoder } from "node:string_decoder";

function parseArgs(argv) {
  const out = {
    endpoint: null,
    baud: 115200,
    seconds: 30,
    out: `/tmp/serial-hub-capture-${Date.now()}.log`,
    from: "end",
    token: null,
    min: 256,
    readTimeout: 8000,
    maxBytes: 8192,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--baud") out.baud = Number(argv[++i]);
    else if (a === "--seconds") out.seconds = Number(argv[++i]);
    else if (a === "--out") out.out = argv[++i];
    else if (a === "--from") out.from = argv[++i];
    else if (a === "--token") out.token = argv[++i];
    else if (a === "--min") out.min = Number(argv[++i]);
    else if (a === "--read-timeout") out.readTimeout = Number(argv[++i]);
    else if (a === "--max-bytes") out.maxBytes = Number(argv[++i]);
    else if (!out.endpoint) out.endpoint = a;
    else throw new Error(`unknown arg: ${a}`);
  }
  if (!out.endpoint) throw new Error("usage: capture.mjs tcp://host:port [options]");
  return out;
}

function parseEndpoint(ep) {
  const m = /^tcp:\/\/([^:/]+|\[[^\]]+\]):(\d+)$/.exec(ep);
  if (!m) throw new Error(`bad endpoint: ${ep}`);
  return { host: m[1].replace(/^\[|\]$/g, ""), port: Number(m[2]) };
}

class Rpc {
  constructor(socket) {
    this.socket = socket;
    this.id = 1;
    this.pending = new Map();
    this.buf = "";
    socket.setEncoding("utf8");
    socket.on("data", (c) => this.onData(c));
    socket.on("close", () => this.fail(new Error("link closed")));
    socket.on("error", (e) => this.fail(e));
  }
  onData(chunk) {
    this.buf += chunk;
    let i;
    while ((i = this.buf.indexOf("\n")) >= 0) {
      const line = this.buf.slice(0, i).trim();
      this.buf = this.buf.slice(i + 1);
      if (!line) continue;
      let msg;
      try {
        msg = JSON.parse(line);
      } catch {
        continue;
      }
      const p = this.pending.get(msg.id);
      if (!p) continue;
      this.pending.delete(msg.id);
      clearTimeout(p.timer);
      if (msg.ok) p.resolve(msg.result ?? {});
      else p.reject(Object.assign(new Error(msg.error?.message || "remote error"), { code: msg.error?.code }));
    }
  }
  fail(err) {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
  }
  request(op, params = {}, timeoutMs = 10000) {
    const id = this.id++;
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`timeout: ${op}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.write(JSON.stringify({ id, op, ...params }) + "\n");
    });
  }
  close() {
    try {
      this.socket.destroy();
    } catch {}
  }
}

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const human = (n) => (n >= 1024 ? `${(n / 1024).toFixed(1)} KiB` : `${n} B`);

const args = parseArgs(process.argv.slice(2));
const { host, port } = parseEndpoint(args.endpoint);

async function openRpc() {
  const socket = await new Promise((resolve, reject) => {
    const s = net.createConnection({ host, port });
    s.setNoDelay(true);
    const t = setTimeout(() => {
      s.destroy();
      reject(new Error("connect timeout"));
    }, 5000);
    s.once("connect", () => {
      clearTimeout(t);
      resolve(s);
    });
    s.once("error", reject);
  });
  const rpc = new Rpc(socket);
  const hello = await rpc.request("HELLO", {
    proto: "v0",
    ...(args.token ? { token: args.token } : {}),
    client: "serial-hub-bridge-capture/0.1.0",
  });
  await rpc.request("CONFIG", {
    baud_rate: args.baud,
    data_bits: 8,
    stop_bits: 1,
    parity: "none",
    flow_control: "none",
  });
  return { rpc, hello };
}

let oldLog = console.log;
let first = true;
let rpc;
try {
  const opened = await openRpc();
  rpc = opened.rpc;
  console.log(`# ${args.endpoint}  fw=${opened.hello.fw_version}  baud=${args.baud}`);
} catch (e) {
  console.error(`connect failed: ${e.message}`);
  process.exit(1);
}

let stopping = false;
process.on("SIGINT", () => {
  stopping = true;
});

const st = await rpc.request("LOG_STATS", {});
let cursor =
  args.from === "start"
    ? st.ring.start_offset
    : args.from === "end"
      ? st.ring.end_offset
      : Number(args.from);
if (!Number.isFinite(cursor)) cursor = st.ring.end_offset;

const out = fs.createWriteStream(args.out);
const decoder = new StringDecoder("utf8");
let tail = "";
let bytes = 0;
let lines = 0;
let gaps = 0;
let reconnects = 0;
let lastLine = "";
const t0 = Date.now();
const deadline = t0 + args.seconds * 1000;
let nextReport = t0 + 5000;

const feed = (text) => {
  tail += text;
  let idx;
  while ((idx = tail.indexOf("\n")) >= 0) {
    const line = tail.slice(0, idx).replace(/\r$/, "");
    tail = tail.slice(idx + 1);
    lines++;
    if (line.trim()) lastLine = line;
  }
  if (tail.length > 4096) tail = tail.slice(-4096);
};

while (!stopping && Date.now() < deadline) {
  let r;
  try {
    r = await rpc.request(
      "READ",
      {
        from_offset: cursor,
        min_bytes: args.min,
        max_wait_ms: 1000,
        idle_timeout_ms: 300,
        budget_ms: 1500,
        max_bytes: args.maxBytes,
      },
      args.readTimeout,
    );
  } catch (e) {
    reconnects++;
    console.error(`[reconnect #${reconnects}] ${e.message}`);
    rpc.close();
    await sleep(500);
    try {
      const opened = await openRpc();
      rpc = opened.rpc;
      if (first) {
        first = false;
        console.log(`# ${args.endpoint}  fw=${opened.hello.fw_version}  baud=${args.baud}`);
      }
    } catch (e2) {
      console.error(`  reconnect failed: ${e2.message}`);
      await sleep(1000);
    }
    continue;
  }

  const d = Buffer.from(r.data_b64 || "", "base64");
  if (d.length) {
    out.write(d);
    bytes += d.length;
    feed(decoder.write(d));
  }
  if (r.gap) gaps++;
  cursor = r.next_offset;

  const now = Date.now();
  if (now >= nextReport) {
    const secs = (now - t0) / 1000;
    console.log(
      `[${secs.toFixed(1).padStart(5)}s] ${human(bytes).padStart(9)}  ${String(lines).padStart(5)} lines  ${(bytes / secs).toFixed(0).padStart(5)} B/s  reconnects=${reconnects}  last: ${lastLine.slice(0, 90)}`,
    );
    nextReport = now + 5000;
  }
}

await new Promise((res) => out.end(res));

let st2 = {};
try {
  st2 = await rpc.request("LOG_STATS", {}, 3000);
} catch {}
const secs = (Date.now() - t0) / 1000;
console.log(`\ndone: ${secs.toFixed(1)}s  ${human(bytes)}  ${lines} lines  ${(bytes / secs).toFixed(0)} B/s  gaps=${gaps}  reconnects=${reconnects}`);
console.log(`file: ${args.out}`);
if (st2.ring) {
  console.log(
    `ring: ${st2.ring.start_offset} -> ${st2.ring.end_offset}  dropped=${st2.ring.dropped_bytes}  log_seq=${st2.earliest_seq}..${st2.latest_seq}`,
  );
}

if (lines > 0) {
  const content = fs.readFileSync(args.out, "utf8").split(/\r?\n/).filter((l) => l.trim());
  console.log("\nlast lines:");
  for (const line of content.slice(-5)) console.log(`  ${line}`);
}

try {
  await rpc.request("CLOSE", {}, 2000);
} catch {}
rpc.close();
void oldLog;