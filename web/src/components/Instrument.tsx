import { useEffect, useRef } from "react";
import type { Analysis, Quote, Stats } from "../lib/types";
import { big, cur, latency, pct, px } from "../lib/format";
import PriceChart from "./PriceChart";

function Tile({ k, v, s, cls }: { k: string; v: string; s?: string; cls?: string }) {
  return (
    <div className="tile">
      <div className="k">{k}</div>
      <div className={"v " + (cls || "")}>{v}</div>
      {s && <div className="s">{s}</div>}
    </div>
  );
}

export default function Instrument({
  data, stats, quote, onBack,
}: {
  data: Analysis; stats: Stats; quote: Quote | null; onBack: () => void;
}) {
  const priceRef = useRef<HTMLSpanElement>(null);
  const prev = useRef(stats.last);

  useEffect(() => {
    const el = priceRef.current;
    if (el && stats.last !== prev.current) {
      const dir = stats.last > prev.current ? "flash-up" : "flash-down";
      el.classList.remove("flash-up", "flash-down");
      void el.offsetWidth;
      el.classList.add(dir);
      prev.current = stats.last;
    }
  }, [stats.last]);

  const ret = stats.total_return_pct;
  const symbol = cur(data.currency);

  return (
    <>
      <div className="crumbs">
        <button className="back" onClick={onBack}>← Universe</button>
        <span className="trail">Universe / <b>{data.key}</b> · {data.sector}</span>
      </div>

      <div className="ihead">
        <div className="idblock">
          <span className="sym">{data.key}</span>
          <span className="co">{data.name} · {data.exchange}</span>
        </div>
        <div className="price">
          <span className="v tnum" ref={priceRef}>{symbol}{px(stats.last)}</span>
          <span className={"d " + (ret >= 0 ? "up" : "down")}>{pct(ret)} · 5y</span>
        </div>
        <div className="feed">
          {quote && (
            <span className="chip"><span className="feed-lamp" />{quote.source} · {quote.latencyMs ? quote.latencyMs.toFixed(0) + "ms" : "cached"}</span>
          )}
          <span className="chip">engine <b>{latency(data.latency_ns)}</b></span>
          <span className="chip">{big(data.rows_per_sec)} rows/s</span>
          <span className="chip">{data.rows} rows · {data.parallel ? data.threads + " threads" : "1 core"}</span>
        </div>
      </div>

      <div className="tiles">
        <Tile k="Total return" v={pct(ret)} s="5-year" cls={ret >= 0 ? "up" : "down"} />
        <Tile k="Annualized" v={pct(stats.ann_return_pct)} s="252-day" cls={stats.ann_return_pct >= 0 ? "up" : "down"} />
        <Tile k="Volatility" v={stats.ann_vol_pct.toFixed(1) + "%"} s="annualized" />
        <Tile k="Sharpe" v={stats.sharpe.toFixed(2)} s="rf = 0" cls={stats.sharpe >= 1 ? "up" : stats.sharpe < 0 ? "down" : ""} />
        <Tile k="Max drawdown" v={stats.max_drawdown_pct.toFixed(1) + "%"} s="peak → trough" cls="down" />
        <Tile k="Win rate" v={(stats.win_rate * 100).toFixed(0) + "%"} s="up days" />
        <Tile k="Best / worst" v={pct(stats.best_day_pct) + " / " + pct(stats.worst_day_pct)} s="daily" />
        <Tile k="Range" v={px(stats.min) + " – " + px(stats.max)} s="5y low / high" />
      </div>

      <div className="grid split">
        <div className="card">
          <h3>Price, rolling mean &amp; VWAP</h3>
          <div className="sub">Close with {data.window}-period rolling mean and volume-weighted average price. Volume histogram flags block days.</div>
          <div className="legend">
            <span><i style={{ background: "var(--forest)" }} />Close</span>
            <span><i style={{ background: "var(--clay)" }} />Mean({data.window})</span>
            <span><i style={{ background: "var(--teal)" }} />VWAP({data.window})</span>
            <span><i style={{ background: "var(--down)" }} />Block day</span>
          </div>
          <PriceChart data={data} />
        </div>

        <div className="card">
          <h3>Analytics book</h3>
          <div className="sub">Full metric set from the engine pass.</div>
          <table className="book">
            <tbody>
              <tr><td>Last</td><td>{symbol}{px(stats.last)}</td></tr>
              <tr><td>Rolling mean ({data.window})</td><td>{px(stats.latest_mean)}</td></tr>
              <tr><td>VWAP ({data.window})</td><td>{px(stats.latest_vwap)}</td></tr>
              <tr><td>Mean daily return</td><td className={stats.mean_daily_pct >= 0 ? "up" : "down"}>{pct(stats.mean_daily_pct)}</td></tr>
              <tr><td>Notional traded (5y)</td><td>{symbol}{big(stats.dollar_volume)}</td></tr>
              <tr><td>Avg daily volume</td><td>{big(stats.avg_volume)}</td></tr>
              <tr><td>Median volume</td><td>{big(stats.median_volume)}</td></tr>
              <tr><td>Block threshold (p90)</td><td>{big(stats.block_threshold)}</td></tr>
              <tr><td>Block days</td><td>{stats.block_days}</td></tr>
              <tr><td>Up-volume share</td><td className={stats.up_volume_frac >= 0.5 ? "up" : "down"}>{(stats.up_volume_frac * 100).toFixed(0)}%</td></tr>
              <tr><td>On-balance volume</td><td className={stats.obv_last >= 0 ? "up" : "down"}>{big(stats.obv_last)}</td></tr>
            </tbody>
          </table>
        </div>
      </div>
    </>
  );
}
