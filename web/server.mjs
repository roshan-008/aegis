// Aegis data service.
// Fetches live OHLCV, runs it through the compiled C++ `analyze` engine, and
// serves the results to the frontend. The engine is the source of truth for
// every analytic; this process only moves bytes and caches.

import express from "express";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, resolve } from "node:path";
import { existsSync } from "node:fs";

const __dirname = dirname(fileURLToPath(import.meta.url));
const ENGINE = resolve(__dirname, "..", "build", "analyze");
const PORT = process.env.PORT || 5177;

if (!existsSync(ENGINE)) {
  console.error(`\n  engine binary not found at ${ENGINE}`);
  console.error(`  build it first:  cmake --build build --target analyze\n`);
}

// symbol, name, sector, region, aliases
const UNIVERSE = [
  ["AAPL", "Apple", "Technology", "US", ["apple"]],
  ["MSFT", "Microsoft", "Technology", "US", ["microsoft"]],
  ["NVDA", "NVIDIA", "Semiconductors", "US", ["nvidia"]],
  ["AMZN", "Amazon", "Consumer", "US", ["amazon"]],
  ["GOOGL", "Alphabet", "Technology", "US", ["google", "alphabet"]],
  ["META", "Meta Platforms", "Technology", "US", ["meta", "facebook"]],
  ["TSLA", "Tesla", "Automotive", "US", ["tesla"]],
  ["AMD", "AMD", "Semiconductors", "US", ["amd"]],
  ["AVGO", "Broadcom", "Semiconductors", "US", ["broadcom"]],
  ["NFLX", "Netflix", "Media", "US", ["netflix"]],
  ["JPM", "JPMorgan Chase", "Financials", "US", ["jpmorgan", "chase"]],
  ["INTC", "Intel", "Semiconductors", "US", ["intel"]],
  ["PLTR", "Palantir", "Technology", "US", ["palantir"]],
  ["COIN", "Coinbase", "Financials", "US", ["coinbase"]],
  ["ORCL", "Oracle", "Technology", "US", ["oracle"]],
  ["CRM", "Salesforce", "Technology", "US", ["salesforce"]],
  ["UBER", "Uber", "Technology", "US", ["uber"]],
  ["XOM", "Exxon Mobil", "Energy", "US", ["exxon"]],
  ["SPY", "S&P 500 ETF", "Index", "US", ["sp500", "s&p", "spy"]],
  ["QQQ", "Nasdaq-100 ETF", "Index", "US", ["nasdaq", "qqq"]],
  ["^GSPC", "S&P 500", "Index", "US", ["s&p 500", "spx"]],
  ["^IXIC", "Nasdaq Composite", "Index", "US", ["nasdaq composite"]],
  ["^DJI", "Dow Jones", "Index", "US", ["dow", "dow jones"]],
  ["^NSEI", "Nifty 50", "Index", "India", ["nifty", "nifty 50"]],
  ["^BSESN", "BSE Sensex", "Index", "India", ["sensex", "bse"]],
  ["TATAPOWER.NS", "Tata Power", "Energy", "India", ["tata power", "tata"]],
  ["TITAN.NS", "Titan", "Consumer", "India", ["titan", "tata"]],
  ["TCS.NS", "Tata Consultancy", "Technology", "India", ["tcs", "tata"]],
  ["TATASTEEL.NS", "Tata Steel", "Materials", "India", ["tata steel", "tata"]],
  ["RELIANCE.NS", "Reliance Industries", "Energy", "India", ["reliance"]],
  ["INFY.NS", "Infosys", "Technology", "India", ["infosys"]],
  ["HDFCBANK.NS", "HDFC Bank", "Financials", "India", ["hdfc"]],
  ["WIPRO.NS", "Wipro", "Technology", "India", ["wipro"]],
  ["ADANIENT.NS", "Adani Enterprises", "Industrials", "India", ["adani"]],
  ["BTC-USD", "Bitcoin", "Crypto", "Global", ["bitcoin", "btc"]],
  ["ETH-USD", "Ethereum", "Crypto", "Global", ["ethereum", "eth"]],
  ["SOL-USD", "Solana", "Crypto", "Global", ["solana", "sol"]],
];

const CRYPTO = { "BTC-USD": "bitcoin", "ETH-USD": "ethereum", "SOL-USD": "solana" };
const keyOf = (s) => s.replace(/^\^/, "").replace(/\.NS$/, "").replace(/-USD$/, "");
const META = new Map(UNIVERSE.map(([sym, name, sector, region, aliases]) => [keyOf(sym), { symbol: sym, name, sector, region, aliases }]));

const UA = { "User-Agent": "Mozilla/5.0" };
const now = () => Date.now();

async function yahooChart(symbol, range = "5y", interval = "1d") {
  const enc = encodeURIComponent(symbol);
  let lastErr;
  for (const host of ["query1", "query2"]) {
    try {
      const url = `https://${host}.finance.yahoo.com/v8/finance/chart/${enc}?range=${range}&interval=${interval}`;
      const r = await fetch(url, { headers: UA });
      if (!r.ok) throw new Error(`yahoo ${r.status}`);
      const j = await r.json();
      const res = j?.chart?.result?.[0];
      if (!res) throw new Error("yahoo empty");
      return res;
    } catch (e) {
      lastErr = e;
    }
  }
  throw lastErr;
}

function runEngine(symbol, csv) {
  return new Promise((res, rej) => {
    const proc = spawn(ENGINE, [symbol]);
    let out = "", err = "";
    proc.stdout.on("data", (d) => (out += d));
    proc.stderr.on("data", (d) => (err += d));
    proc.on("error", rej);
    proc.on("close", (code) => (code === 0 ? res(JSON.parse(out)) : rej(new Error(err || `exit ${code}`))));
    proc.stdin.write(csv);
    proc.stdin.end();
  });
}

// analyze one instrument end to end (fetch -> engine -> shape).
async function analyze(key, range = "5y") {
  const meta = META.get(key);
  if (!meta) throw new Error(`unknown instrument ${key}`);
  const res = await yahooChart(meta.symbol, range);
  const q = res.indicators?.quote?.[0] || {};
  const ts = res.timestamp || [];
  const closes = q.close || [];
  const vols = q.volume || [];
  const candles = [];
  const lines = [];
  for (let i = 0; i < ts.length; i++) {
    const c = closes[i];
    if (c == null) continue;
    const v = vols[i] == null ? 0 : vols[i];
    candles.push({ time: ts[i], close: c, volume: v });
    lines.push(`${c},${v}`);
  }
  const report = await runEngine(meta.symbol, lines.join("\n"));
  const m = res.meta || {};
  return {
    key,
    symbol: meta.symbol,
    name: meta.name,
    sector: meta.sector,
    region: meta.region,
    currency: m.currency || "USD",
    exchange: m.fullExchangeName || meta.region,
    rows: report.rows,
    threads: report.threads,
    hw_threads: report.hw_threads,
    parallel: report.parallel,
    latency_ns: report.latency_ns,
    rows_per_sec: report.rows_per_sec,
    window: report.window,
    stats: report.stats,
    candles,
  };
}

// --- simple in-memory cache so the table and detail don't refetch constantly ---
const cache = new Map(); // key -> { at, data }
const TTL = 3 * 60 * 1000;
async function analyzeCached(key, range = "5y") {
  const hit = cache.get(key + range);
  if (hit && now() - hit.at < TTL) return hit.data;
  const data = await analyze(key, range);
  cache.set(key + range, { at: now(), data });
  return data;
}

const app = express();

app.get("/api/search", (req, res) => {
  const q = String(req.query.q || "").trim().toLowerCase();
  if (!q) return res.json([]);
  const scored = [];
  for (const [, m] of META) {
    const key = keyOf(m.symbol).toLowerCase();
    const hay = [key, m.name.toLowerCase(), ...m.aliases];
    let s = 99;
    if (key === q) s = 0;
    else if (key.startsWith(q)) s = 1;
    else if (m.name.toLowerCase().startsWith(q)) s = 2;
    else if (m.aliases.some((a) => a.startsWith(q))) s = 3;
    else if (hay.some((h) => h.includes(q))) s = 4;
    if (s < 99) scored.push({ s, key: keyOf(m.symbol), name: m.name, sector: m.sector, region: m.region });
  }
  scored.sort((a, b) => a.s - b.s || a.key.localeCompare(b.key));
  res.json(scored.slice(0, 8));
});

app.get("/api/analyze", async (req, res) => {
  try {
    const key = String(req.query.symbol || "").toUpperCase();
    const data = await analyzeCached(key, String(req.query.range || "5y"));
    res.json(data);
  } catch (e) {
    res.status(502).json({ error: String(e.message || e) });
  }
});

// live quote: crypto via CoinGecko, everything else via Yahoo 1m.
app.get("/api/quote", async (req, res) => {
  const key = String(req.query.symbol || "").toUpperCase();
  const meta = META.get(key);
  if (!meta) return res.status(404).json({ error: "unknown" });
  const started = now();
  try {
    if (CRYPTO[meta.symbol]) {
      const id = CRYPTO[meta.symbol];
      const r = await fetch(
        `https://api.coingecko.com/api/v3/simple/price?ids=${id}&vs_currencies=usd&include_24hr_vol=true&include_last_updated_at=true`,
        { headers: UA }
      );
      const j = await r.json();
      const row = j[id] || {};
      if (!Number.isFinite(row.usd)) throw new Error("no price");
      return res.json({ price: row.usd, volume: row.usd_24h_vol, at: (row.last_updated_at || now() / 1000) * 1000, source: "CoinGecko", latencyMs: now() - started });
    }
    const chart = await yahooChart(meta.symbol, "1d", "1m");
    const q = chart.indicators?.quote?.[0] || {};
    const closes = (q.close || []).filter(Number.isFinite);
    const vols = (q.volume || []).filter(Number.isFinite);
    const m = chart.meta || {};
    const price = closes.length ? closes[closes.length - 1] : m.regularMarketPrice;
    if (!Number.isFinite(price)) throw new Error("no price");
    res.json({ price, volume: vols.length ? vols[vols.length - 1] : undefined, at: (m.regularMarketTime || now() / 1000) * 1000, source: "Yahoo", latencyMs: now() - started });
  } catch (e) {
    res.status(502).json({ error: String(e.message || e) });
  }
});

// universe summaries for the comparison table (analyzed on the engine, cached).
app.get("/api/universe", async (_req, res) => {
  const keys = [...META.keys()];
  const out = [];
  await Promise.all(
    keys.map(async (k) => {
      try {
        const d = await analyzeCached(k);
        const { candles, ...summary } = d;
        void candles;
        out.push(summary);
      } catch {
        /* skip instruments that fail to fetch */
      }
    })
  );
  const order = new Map(keys.map((k, i) => [k, i]));
  out.sort((a, b) => (order.get(a.key) ?? 0) - (order.get(b.key) ?? 0));
  res.json(out);
});

app.listen(PORT, () => {
  console.log(`  aegis data service  →  http://localhost:${PORT}`);
  console.log(`  engine: ${ENGINE}`);
});
