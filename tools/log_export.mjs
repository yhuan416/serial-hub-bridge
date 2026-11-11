#!/usr/bin/env node
// Export the device SD log (raw+index) over v0 LOG_EXPORT and rebuild lines.
//
//   node tools/log_export.mjs tcp://<device-ip>:7000 --dir rx --seq-from 1 --out /tmp/rx.log
//
// Options:
//   --dir rx|tx          default rx
//   --seq-from <n>       first seq (default: earliest)
//   --out <path>         output text file (default /tmp/serial-hub-export-<epoch>.log)
//   --max-bytes <n>      per page, clamp 32768 (default 32768)
//   --token <t>          HELLO token
//
// LOG_EXPORT reads the device SD log only; it never sends CONFIG, so it does
// not disturb the live UART baud.

import net from "node:net";
import fs from "node:fs";

function parseArgs(argv) {
  const out = {
    endpoint: null,
    dir: "rx",
    seqFrom: null,
    out: `/tmp/serial-hub-export-${Date.now()}.log`,
    maxBytes: 32768,
    token: null,
    statsOnly: false,
  };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--dir") out.dir = argv[++i];
    else if (a === "--seq-from") out.seqFrom = Number(argv[++i]);
    else if (a === "--out") out.out = argv[++i];
    else if (a === "--max-bytes") out.maxBytes = Number(argv[++i]);
    else if (a === "--token") out.token = argv[++i];
    else if (a === "--stats-only") out.statsOnly = true;
    else if (!out.endpoint) out.endpoint = a;
    else throw new Error(`unknown arg: ${a}`);
  }
  if (!out.endpoint) throw new Error("usage: log_export.mjs tcp://host:port [options]");
  if (out.dir !== "rx" && out.dir !== "tx") throw new Error("--dir must be rx|tx");
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
  request(op, params = {}, timeoutMs = 15000) {
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
      this.socket.end();
    } catch {}
  }
}

const args = parseArgs(process.argv.slice(2));
const { host, port } = parseEndpoint(args.endpoint);
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
  client: "serial-hub-bridge-log-export/0.1.0",
});
if (!hello.caps?.log_export) {
  console.error(`device has no LOG_EXPORT (has_sd=${hello.caps?.has_sd}, storage=${hello.caps?.log_storage})`);
  process.exit(1);
}
const st = await rpc.request("LOG_STATS", {});
const seqFrom = args.seqFrom ?? st.earliest_seq ?? 1;
console.log(
  `device segment=${st.segment} capacity=${st.capacity_bytes} used=${st.used_bytes} ` +
    `seq=${st.earliest_seq}..${st.latest_seq} raw=${st.raw_bytes} idx=${st.index_count}`,
);
if (st.ring) {
  console.log(
    `ring start=${st.ring.start_offset} end=${st.ring.end_offset} ` +
      `used=${st.ring.used} dropped=${st.ring.dropped_bytes}`,
  );
}
console.log(`export dir=${args.dir} seq_from=${seqFrom}`);
if (args.statsOnly) {
  try { await rpc.request("CLOSE", {}, 2000); } catch {}
  rpc.close();
  process.exit(0);
}

const out = fs.createWriteStream(args.out);
let cursor = "0";
let pages = 0;
let bytes = 0;
let lines = 0;
let lastSeq = 0;
const frames = [];

for (;;) {
  const r = await rpc.request(
    "LOG_EXPORT",
    {
      dir: args.dir,
      seq_from: seqFrom,
      cursor,
      max_bytes: args.maxBytes,
    },
    20000,
  );
  const chunk = Buffer.from(r.chunk_b64 || "", "base64");
  const idx = Buffer.from(r.index_b64 || "", "base64");
  for (let o = 0; o + 20 <= idx.length; o += 20) {
    const off = Number(idx.readBigUInt64LE(o));
    const seq = idx.readUInt32LE(o + 8);
    const len = idx.readUInt16LE(o + 16);
    if (len === 0 || off + len > chunk.length) continue;
    const text = chunk.subarray(off, off + len);
    out.write(text);
    lines++;
    lastSeq = seq;
    const s = text.toString("utf8");
    const m = /frame no :(\d+)/.exec(s);
    if (m) frames.push(Number(m[1]));
  }
  bytes += chunk.length;
  pages++;
  console.log(`  page ${pages}: chunk=${chunk.length}B index=${idx.length}B eof=${r.eof}`);
  if (r.eof) break;
  if (!r.next_cursor || r.next_cursor === cursor) break;
  cursor = r.next_cursor;
  if (pages > 1000) break;
}

await new Promise((res) => out.end(res));

let frameGaps = 0;
if (frames.length > 1) {
  for (let i = 1; i < frames.length; i++) {
    if (frames[i] - frames[i - 1] !== 1) frameGaps++;
  }
}
console.log(
  `done: pages=${pages} bytes=${bytes} lines=${lines} last_seq=${lastSeq} ` +
    `frames=${frames.length} first=${frames[0] ?? "-"} last=${frames[frames.length - 1] ?? "-"} frame_gaps=${frameGaps}`,
);
console.log(`file: ${args.out}`);

try {
  await rpc.request("CLOSE", {}, 2000);
} catch {}
rpc.close();