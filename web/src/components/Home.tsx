import { useMemo, useState } from "react";
import type { Summary } from "../lib/types";
import { big, latency, pct, px } from "../lib/format";

type Col = {
  key: string; label: string;
  get: (s: Summary) => number | string;
  fmt: (v: number) => string;
  tone?: "sign" | "neg";
};

const COLS: Col[] = [
  { key: "sym", label: "Instrument", get: (s) => s.key, fmt: (v) => String(v) },
  { key: "last", label: "Last", get: (s) => s.stats.last, fmt: (v) => px(v) },
  { key: "ret", label: "5y return", get: (s) => s.stats.total_return_pct, fmt: (v) => pct(v), tone: "sign" },
  { key: "ann", label: "Ann. return", get: (s) => s.stats.ann_return_pct, fmt: (v) => pct(v), tone: "sign" },
  { key: "vol", label: "Ann. vol", get: (s) => s.stats.ann_vol_pct, fmt: (v) => v.toFixed(1) + "%" },
  { key: "shp", label: "Sharpe", get: (s) => s.stats.sharpe, fmt: (v) => v.toFixed(2), tone: "sign" },
  { key: "dd", label: "Max DD", get: (s) => s.stats.max_drawdown_pct, fmt: (v) => v.toFixed(1) + "%", tone: "neg" },
  { key: "dv", label: "Notional", get: (s) => s.stats.dollar_volume, fmt: (v) => big(v) },
  { key: "blk", label: "Block days", get: (s) => s.stats.block_days, fmt: (v) => String(v) },
  { key: "lat", label: "Engine", get: (s) => s.latency_ns, fmt: (v) => latency(v) },
];

export default function Home({
  universe, loading, onPick,
}: {
  universe: Summary[]; loading: boolean; onPick: (symbol: string) => void;
}) {
  const [sortKey, setSortKey] = useState("ret");
  const [asc, setAsc] = useState(false);

  const rows = useMemo(() => {
    const col = COLS.find((c) => c.key === sortKey)!;
    return [...universe].sort((a, b) => {
      const av = col.get(a), bv = col.get(b);
      const d = typeof av === "string" ? String(av).localeCompare(String(bv)) : (av as number) - (bv as number);
      return d * (asc ? 1 : -1);
    });
  }, [universe, sortKey, asc]);

  return (
    <>
      <section className="hero">
        <div className="eyebrow">Market intelligence</div>
        <h1>Every instrument, measured the moment you ask.</h1>
        <p>
          Search a name and get its complete quant profile — returns, volatility, drawdown,
          rolling VWAP and block-trade flow — computed on live data in microseconds.
        </p>
      </section>

      <div className="section-head">
        <span className="section-num">01</span>
        <h2>Universe</h2>
        <span className="hint">{loading ? "analyzing…" : `${universe.length} instruments · click to open`}</span>
      </div>

      <div className="table-wrap">
        {loading && !universe.length ? (
          <div className="center-state"><span className="spinner" />fetching live data and running the engine…</div>
        ) : (
          <div className="table-scroll">
            <table className="uni">
              <thead>
                <tr>
                  {COLS.map((c) => (
                    <th
                      key={c.key}
                      className={c.key === sortKey ? (asc ? "asc" : "sorted") : ""}
                      onClick={() => (c.key === sortKey ? setAsc(!asc) : (setSortKey(c.key), setAsc(false)))}
                    >
                      {c.label}
                    </th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {rows.map((s) => (
                  <tr key={s.symbol} onClick={() => onPick(s.symbol)}>
                    {COLS.map((c) => {
                      const raw = c.get(s);
                      let cls = "";
                      if (c.tone === "sign") cls = (raw as number) >= 0 ? "up" : "down";
                      if (c.tone === "neg") cls = "down";
                      if (c.key === "sym") {
                        return (
                          <td key={c.key} className="sym">
                            {s.key}
                            <span className="co">{s.name}</span>
                          </td>
                        );
                      }
                      return <td key={c.key} className={cls}>{c.fmt(raw as number)}</td>;
                    })}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </>
  );
}
