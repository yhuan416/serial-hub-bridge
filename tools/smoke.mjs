#!/usr/bin/env node
// Bare v0 protocol smoke test (no npm deps, Node >= 18).
//
//   node tools/smoke.mjs tcp://192.168.1.50:7000 [--token T] [--data AT] [--match OK]
//
// Runs: HELLO -> CONFIG -> WRITE -> READ -> TRANSACT -> LOG_STATS -> PING -> CLOSE
// Exits non-zero on the first protocol-level failure.

import net from "node:net";

function parseArgs(argv) {
  const out = { endpoint: null, token: null, data: "AT", match: "OK", waitMs: 2000, noTransact: false, baud: 115200 };
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--token") out.token = argv[++i];
    else if (a === "--data") out.data = argv[++i];
    else if (a === "--match") out.match = argv[++i];
    else if (a === "--wait") out.waitMs = Number(argv[++i]);
    else if (a === "--no-transact") out.noTransact = true;
    else if (a === "--baud") out.baud = Number(argv[++i]);
    else if (!out.endpoint) out.endpoint = a;
    else throw new Error(`unknown arg: ${a}`);
  }
  if (!out.endpoint) throw new Error("usage: smoke.mjs tcp://host:port [--token T]");
  return out;
}

function parseEndpoint(ep) {
  const m = /^tcp:\/\/([^:/]+|\[[^\]]+\]):(\d+)$/.exec(ep);
  if (!m) throw new Error(`bad endpoint (expect tcp://host:port): ${ep}`);
  return { host: m[1].replace(/^\[|\]$/g, ""), port: Number(m[2]) };
}

class Rpc {
  constructor(socket, timeoutMs = 5000) {
    this.socket = socket;
    this.timeoutMs = timeoutMs;
    this.id = 1;
    this.pending = new Map();
    this.buf = "";
    socket.setEncoding("utf8");
    socket.on("data", (chunk) => this.onData(chunk));
    socket.on("close", () => this.failAll(new Error("link closed")));
    socket.on("error", (err) => this.failAll(err));
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

  failAll(err) {
    for (const [, p] of this.pending) {
      clearTimeout(p.timer);
      p.reject(err);
    }
    this.pending.clear();
  }

  request(op, params = {}, timeoutMs = this.timeoutMs) {
    const id = this.id++;
    const line = JSON.stringify({ id, op, ...params }) + "\n";
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`timeout: ${op}`));
      }, timeoutMs);
      this.pending.set(id, { resolve, reject, timer });
      this.socket.write(line);
    });
  }

  close() {
    try {
      this.socket.end();
    } catch {}
  }
}

const b64 = (s) => Buffer.from(s, "utf8").toString("base64");
const unb64 = (s) => Buffer.from(s || "", "base64").toString("utf8");
const show = (s) => JSON.stringify(s);

let failures = 0;
function check(cond, what) {
  if (cond) {
    console.log(`  ok   ${what}`);
  } else {
    failures++;
    console.error(`  FAIL ${what}`);
  }
}

async function main() {
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
  try {
    console.log(`# ${args.endpoint}`);

    const hello = await rpc.request("HELLO", {
      proto: "v0",
      ...(args.token ? { token: args.token } : {}),
      client: "serial-hub-bridge-smoke/0.1.0",
    });
    check(hello.proto === "v0", `HELLO proto=v0 (fw=${hello.fw_version})`);
    check(Array.isArray(hello.ports) && hello.ports.length >= 1, `ports: ${show(hello.ports)}`);
    check(hello.caps && typeof hello.caps === "object", "caps present");

    const cfg = await rpc.request("CONFIG", {
      baud_rate: args.baud,
      data_bits: 8,
      stop_bits: 1,
      parity: "none",
      flow_control: "none",
    });
    check(cfg.applied && cfg.applied.baud_rate === args.baud, `CONFIG applied (${args.baud})`);

    const w = await rpc.request("WRITE", { data_b64: b64(args.data) });
    check(w.bytes_written === Buffer.byteLength(args.data), `WRITE bytes=${w.bytes_written} tx_offset=${w.tx_offset}`);

    const r = await rpc.request(
      "READ",
      { from_offset: 0, min_bytes: 1, max_wait_ms: 500, idle_timeout_ms: 300, budget_ms: 500 },
      3000,
    );
    check(typeof r.next_offset === "number", `READ stopped=${r.stopped_reason} next=${r.next_offset}`);
    if (r.data_b64) console.log(`       rx: ${show(unb64(r.data_b64))}`);

    if (args.noTransact) {
      console.log("  skip TRANSACT (--no-transact)");
    } else {
      const t = await rpc.request(
        "TRANSACT",
        {
          data_b64: b64(args.data),
          match_b64: b64(args.match),
          min_bytes: 1,
          max_wait_ms: args.waitMs,
          idle_timeout_ms: args.waitMs,
          budget_ms: args.waitMs,
        },
        args.waitMs + 3000,
      );
      check(t.matched === true, `TRANSACT matched=${t.matched} reason=${t.stopped_reason}`);
      check(typeof t.barrier_offset === "number", `barrier=${t.barrier_offset} tx_offset=${t.tx_offset}`);
      console.log(`       tx-rx: ${show(unb64(t.data_b64))}`);
    }

    const st = await rpc.request("LOG_STATS", {});
    check(st.ring && typeof st.ring.end_offset === "number", `LOG_STATS ring.end=${st.ring?.end_offset}`);

    const pong = await rpc.request("PING", { t_ms: Date.now() });
    check(typeof pong.device_tick_ms === "number", `PING tick=${pong.device_tick_ms}`);

    const bye = await rpc.request("CLOSE", {});
    check(bye.closed === true, "CLOSE");
  } finally {
    rpc.close();
  }

  if (failures) {
    console.error(`\n${failures} check(s) failed`);
    process.exit(1);
  }
  console.log("\nall checks passed");
}

main().catch((err) => {
  console.error(`error: ${err.message}`);
  process.exit(1);
});