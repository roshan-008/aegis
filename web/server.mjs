// Aegis data service.
// Fetches live OHLCV, runs it through the compiled C++ `analyze` engine, and
// serves the results to the frontend. The engine is the source of truth for
// every analytic; this process only moves bytes and caches. Any symbol Yahoo
// knows can be analyzed — the curated list below only powers the watchlist and
// alias search ("tata", "nifty", "nvidia").

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

// symbol, name, sector, region, aliases — the watchlist + alias search.
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
  ["TATAMOTORS.NS", "Tata Motors", "Automotive", "India", ["tata motors", "tata"]],
  ["TATAPOWER.NS", "Tata Power", "Energy", "India", ["tata power", "tata"]],
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
const displayKey = (s) => s.replace(/^\^/, "").replace(/\.(NS|BO|KS|L|TO|HK|DE|PA|SS|SZ)$/i, "").replace(/-USD$/, "");
const SYM = new Map(UNIVERSE.map(([symbol, name, sector, region, aliases]) => [symbol.toUpperCase(), { symbol, name, sector, region, aliases }]));

function regionOf(symbol) {
  const s = symbol.toUpperCase();
  if (s.endsWith(".NS") || s.endsWith(".BO")) return "India";
  if (s.endsWith("-USD")) return "Crypto";
  if (s.startsWith("^")) return "Index";
  if (s.endsWith(".L")) return "UK";
  if (s.endsWith(".KS")) return "Korea";
  if (s.endsWith(".TO")) return "Canada";
  if (s.endsWith(".HK")) return "Hong Kong";
  return "US";
}

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

async function yahooSearch(q) {
  for (const host of ["query1", "query2"]) {
    try {
      const url = `https://${host}.finance.yahoo.com/v1/finance/search?q=${encodeURIComponent(q)}&quotesCount=10&newsCount=0&enableFuzzyQuery=true`;
      const r = await fetch(url, { headers: UA });
      if (!r.ok) throw new Error(`search ${r.status}`);
      const j = await r.json();
      return (j.quotes || [])
        .filter((x) => x.symbol && ["EQUITY", "ETF", "INDEX", "CRYPTOCURRENCY", "MUTUALFUND", "CURRENCY"].includes(x.quoteType))
        .map((x) => ({
          symbol: x.symbol,
          key: displayKey(x.symbol),
          name: x.shortname || x.longname || x.symbol,
          region: regionOf(x.symbol),
          exchange: x.exchDisp || x.exchange || "",
        }));
    } catch {
      /* try next host */
    }
  }
  return [];
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

// analyze any symbol end to end: fetch -> engine -> shape. `range` is a Yahoo
// range token (1mo, 6mo, 1y, 5y, 10y, max, ...).
async function analyze(symbol, range = "5y") {
  const res = await yahooChart(symbol, range);
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
  if (candles.length < 8) throw new Error("not enough history for this range");
  const report = await runEngine(symbol, lines.join("\n"));
  const m = res.meta || {};
  const curated = SYM.get(symbol.toUpperCase());
  return {
    key: displayKey(symbol),
    symbol,
    range,
    name: curated?.name || m.longName || m.shortName || displayKey(symbol),
    sector: curated?.sector || m.instrumentType || regionOf(symbol),
    region: curated?.region || regionOf(symbol),
    currency: m.currency || "USD",
    exchange: m.fullExchangeName || curated?.region || regionOf(symbol),
    firstTime: candles[0].time,
    lastTime: candles[candles.length - 1].time,
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

const cache = new Map(); // symbol|range -> { at, data }
const TTL = 3 * 60 * 1000;
async function analyzeCached(symbol, range = "5y") {
  const k = symbol.toUpperCase() + "|" + range;
  const hit = cache.get(k);
  if (hit && now() - hit.at < TTL) return hit.data;
  const data = await analyze(symbol, range);
  cache.set(k, { at: now(), data });
  return data;
}

const app = express();

// live search: curated (alias-aware) first, then anything Yahoo knows.
app.get("/api/search", async (req, res) => {
  const q = String(req.query.q || "").trim();
  if (!q) return res.json([]);
  const lower = q.toLowerCase();
  const curated = [];
  for (const [, m] of SYM) {
    const key = displayKey(m.symbol).toLowerCase();
    let s = 99;
    if (key === lower) s = 0;
    else if (key.startsWith(lower)) s = 1;
    else if (m.name.toLowerCase().startsWith(lower)) s = 2;
    else if (m.aliases.some((a) => a.startsWith(lower))) s = 3;
    else if (key.includes(lower) || m.name.toLowerCase().includes(lower) || m.aliases.some((a) => a.includes(lower))) s = 4;
    if (s < 99) curated.push({ s, symbol: m.symbol, key: displayKey(m.symbol), name: m.name, region: m.region, exchange: m.region });
  }
  curated.sort((a, b) => a.s - b.s || a.key.localeCompare(b.key));

  let live = [];
  try { live = await yahooSearch(q); } catch { /* offline: curated only */ }

  const seen = new Set(curated.map((c) => c.symbol.toUpperCase()));
  const merged = curated.map(({ s, ...rest }) => (void s, rest));
  for (const r of live) {
    if (seen.has(r.symbol.toUpperCase())) continue;
    seen.add(r.symbol.toUpperCase());
    merged.push(r);
  }
  res.json(merged.slice(0, 10));
});

app.get("/api/analyze", async (req, res) => {
  try {
    const symbol = String(req.query.symbol || "");
    if (!symbol) throw new Error("symbol required");
    const range = String(req.query.range || "5y");
    res.json(await analyzeCached(symbol, range));
  } catch (e) {
    res.status(502).json({ error: String(e.message || e) });
  }
});

// live quote: crypto via CoinGecko, everything else via Yahoo 1m.
app.get("/api/quote", async (req, res) => {
  const symbol = String(req.query.symbol || "");
  if (!symbol) return res.status(400).json({ error: "symbol required" });
  const started = now();
  try {
    const id = CRYPTO[symbol.toUpperCase()];
    if (id) {
      const r = await fetch(
        `https://api.coingecko.com/api/v3/simple/price?ids=${id}&vs_currencies=usd&include_24hr_vol=true&include_last_updated_at=true`,
        { headers: UA }
      );
      const j = await r.json();
      const row = j[id] || {};
      if (!Number.isFinite(row.usd)) throw new Error("no price");
      return res.json({ price: row.usd, volume: row.usd_24h_vol, at: (row.last_updated_at || now() / 1000) * 1000, source: "CoinGecko", latencyMs: now() - started });
    }
    const chart = await yahooChart(symbol, "1d", "1m");
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

// watchlist summaries for the comparison table (analyzed on the engine, cached).
app.get("/api/universe", async (_req, res) => {
  const out = [];
  await Promise.all(
    [...SYM.values()].map(async (m) => {
      try {
        const d = await analyzeCached(m.symbol);
        const { candles, ...summary } = d;
        void candles;
        out.push(summary);
      } catch {
        /* skip instruments that fail to fetch */
      }
    })
  );
  const order = new Map([...SYM.values()].map((m, i) => [m.symbol.toUpperCase(), i]));
  out.sort((a, b) => (order.get(a.symbol.toUpperCase()) ?? 0) - (order.get(b.symbol.toUpperCase()) ?? 0));
  res.json(out);
});

app.listen(PORT, () => {
  console.log(`  aegis data service  →  http://localhost:${PORT}`);
  console.log(`  engine: ${ENGINE}`);
});
