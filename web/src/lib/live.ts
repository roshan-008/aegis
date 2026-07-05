import type { Analysis, Stats } from "./types";

// When a fresh quote arrives, patch the latest close and recompute the stats
// that move. Mirrors the engine's reductions in JS so the numbers stay
// consistent between the batch analysis and the live tape.
export function recompute(a: Analysis, price: number, volume?: number): Stats {
  const c = a.candles;
  const n = c.length;
  const closes = c.map((k) => k.close);
  const vols = c.map((k) => k.volume);
  closes[n - 1] = price;
  if (Number.isFinite(volume) && (volume as number) > 0) vols[n - 1] = volume as number;

  let min = closes[0], max = closes[0], peak = closes[0], mdd = 0;
  let rsum = 0, rss = 0, up = 0, best = -Infinity, worst = Infinity, dv = 0, upv = 0, downv = 0;
  for (let i = 0; i < n; i++) {
    const p = closes[i];
    min = Math.min(min, p); max = Math.max(max, p);
    peak = Math.max(peak, p); mdd = Math.min(mdd, p / peak - 1);
    dv += p * vols[i];
    if (i > 0) {
      const r = Math.log(p / closes[i - 1]);
      rsum += r; rss += r * r; best = Math.max(best, r); worst = Math.min(worst, r);
      if (r > 0) { up++; upv += vols[i]; } else if (r < 0) downv += vols[i];
    }
  }
  const rn = Math.max(1, n - 1);
  const rmu = rsum / rn;
  const rsd = Math.sqrt(Math.max(0, rss / rn - rmu * rmu));
  const w = Math.min(a.window || 20, n);
  let ms = 0, pv = 0, vv = 0;
  for (let i = n - w; i < n; i++) { ms += closes[i]; pv += closes[i] * vols[i]; vv += vols[i]; }

  const s = a.stats;
  return {
    ...s,
    last: price, min, max,
    total_return_pct: (price / s.first - 1) * 100,
    ann_return_pct: rmu * 252 * 100,
    ann_vol_pct: rsd * Math.sqrt(252) * 100,
    sharpe: rsd > 0 ? (rmu * 252) / (rsd * Math.sqrt(252)) : 0,
    max_drawdown_pct: mdd * 100,
    mean_daily_pct: rmu * 100,
    win_rate: up / rn,
    best_day_pct: best * 100,
    worst_day_pct: worst * 100,
    dollar_volume: dv,
    up_volume_frac: upv + downv > 0 ? upv / (upv + downv) : s.up_volume_frac,
    latest_mean: ms / w,
    latest_vwap: vv > 0 ? pv / vv : price,
  };
}
