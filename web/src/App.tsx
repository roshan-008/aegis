import { useCallback, useEffect, useRef, useState } from "react";
import type { Analysis, Quote, SearchHit, Stats, Summary } from "./lib/types";
import { fetchAnalysis, fetchQuote, fetchUniverse, searchSymbols } from "./lib/api";
import { recompute } from "./lib/live";
import Home from "./components/Home";
import Instrument from "./components/Instrument";

const POLL_MS = 6000;

export default function App() {
  const [universe, setUniverse] = useState<Summary[]>([]);
  const [uniLoading, setUniLoading] = useState(true);

  const [current, setCurrent] = useState<string | null>(null);
  const [analysis, setAnalysis] = useState<Analysis | null>(null);
  const [stats, setStats] = useState<Stats | null>(null);
  const [quote, setQuote] = useState<Quote | null>(null);
  const [detailLoading, setDetailLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const [q, setQ] = useState("");
  const [hits, setHits] = useState<SearchHit[]>([]);
  const [priv, setPriv] = useState<string | null>(null);
  const [hi, setHi] = useState(-1);
  const pollRef = useRef<number | null>(null);

  // universe once
  useEffect(() => {
    fetchUniverse().then(setUniverse).catch(() => {}).finally(() => setUniLoading(false));
  }, []);

  const stopPoll = () => { if (pollRef.current) { clearTimeout(pollRef.current); pollRef.current = null; } };

  const startPoll = useCallback((a: Analysis) => {
    stopPoll();
    const tick = async () => {
      try {
        const qt = await fetchQuote(a.key);
        setQuote(qt);
        setStats(recompute(a, qt.price, qt.volume));
      } catch { /* keep last good stats */ }
      pollRef.current = window.setTimeout(tick, POLL_MS);
    };
    pollRef.current = window.setTimeout(tick, 1500);
  }, []);

  const open = useCallback((key: string) => {
    setCurrent(key); setQ(""); setHits([]); setPriv(null); setError(null);
    setDetailLoading(true); setAnalysis(null); setStats(null); setQuote(null);
    window.scrollTo({ top: 0, behavior: "smooth" });
    fetchAnalysis(key)
      .then((a) => { setAnalysis(a); setStats(a.stats); startPoll(a); })
      .catch((e) => setError(String(e.message || e)))
      .finally(() => setDetailLoading(false));
  }, [startPoll]);

  const goHome = () => { stopPoll(); setCurrent(null); setAnalysis(null); setStats(null); setError(null); setQ(""); setHits([]); };

  useEffect(() => () => stopPoll(), []);

  // search (debounced)
  useEffect(() => {
    const v = q.trim();
    if (!v) { setHits([]); setPriv(null); return; }
    const id = setTimeout(() => {
      searchSymbols(v)
        .then((h) => {
          setHits(h);
          const PRIVATE: Record<string, string> = { spacex: "SpaceX", stripe: "Stripe", databricks: "Databricks", bytedance: "ByteDance", revolut: "Revolut", canva: "Canva", discord: "Discord" };
          setPriv(!h.length ? PRIVATE[v.toLowerCase()] || null : null);
        })
        .catch(() => {});
    }, 140);
    return () => clearTimeout(id);
  }, [q]);

  const onKey = (e: React.KeyboardEvent) => {
    if (e.key === "ArrowDown") { setHi((h) => Math.min(h + 1, hits.length - 1)); e.preventDefault(); }
    else if (e.key === "ArrowUp") { setHi((h) => Math.max(h - 1, 0)); e.preventDefault(); }
    else if (e.key === "Enter" && hits.length) open(hits[hi < 0 ? 0 : hi].key);
    else if (e.key === "Escape") { setHits([]); setPriv(null); }
  };

  return (
    <>
      <header className="topbar">
        <div className="topbar-inner">
          <button className="brand" onClick={goHome}>
            <span className="glyph" />
            Aegis <small>analytics</small>
          </button>
          <div className="search">
            <svg className="icon" width="15" height="15" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2">
              <circle cx="11" cy="11" r="7" /><path d="m21 21-4-4" />
            </svg>
            <input
              value={q}
              onChange={(e) => { setQ(e.target.value); setHi(-1); }}
              onKeyDown={onKey}
              placeholder="Search NVIDIA, Tata, Nifty, Bitcoin…"
              autoComplete="off" spellCheck={false}
            />
            {(hits.length > 0 || priv) && (
              <div className="suggest">
                {hits.map((h, i) => (
                  <button key={h.key} className={i === hi ? "hi" : ""} onMouseDown={() => open(h.key)}>
                    <span className="sym">{h.key}</span>
                    <span className="nm">{h.name}</span>
                    <span className="rg">{h.region}</span>
                  </button>
                ))}
                {priv && !hits.length && (
                  <div className="empty"><b>{priv}</b> is privately held — not publicly traded, so there is no price feed to analyze.</div>
                )}
              </div>
            )}
          </div>
        </div>
      </header>

      <main className="page">
        {!current && <Home universe={universe} loading={uniLoading} onPick={open} />}
        {current && detailLoading && (
          <div className="center-state"><span className="spinner" />fetching {current} and running the engine…</div>
        )}
        {current && error && (
          <div className="center-state"><span className="err">Could not load {current}: {error}</span>
            <button className="back" onClick={goHome}>← Back</button>
          </div>
        )}
        {current && analysis && stats && (
          <Instrument data={analysis} stats={stats} quote={quote} onBack={goHome} />
        )}
      </main>

      <footer>
        <span>Aegis</span><span>·</span>
        <span>live OHLCV → C++ engine → analytics</span><span>·</span>
        <span>quotes refresh every {POLL_MS / 1000}s</span>
      </footer>
    </>
  );
}
