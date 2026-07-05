import type { Candle } from "./types";

// Display overlays for the chart, derived from the full candle series. The
// authoritative analytics (returns, Sharpe, drawdown, block flow) come from the
// C++ engine; these two lines are just what the chart draws over the price.
export function rollingMean(candles: Candle[], w: number): (number | null)[] {
  const out: (number | null)[] = [];
  let sum = 0;
  for (let i = 0; i < candles.length; i++) {
    sum += candles[i].close;
    if (i >= w) sum -= candles[i - w].close;
    out.push(i >= w - 1 ? sum / w : null);
  }
  return out;
}

export function rollingVwap(candles: Candle[], w: number): (number | null)[] {
  const out: (number | null)[] = [];
  let pv = 0, vol = 0;
  for (let i = 0; i < candles.length; i++) {
    pv += candles[i].close * candles[i].volume;
    vol += candles[i].volume;
    if (i >= w) {
      pv -= candles[i - w].close * candles[i - w].volume;
      vol -= candles[i - w].volume;
    }
    out.push(i >= w - 1 && vol > 0 ? pv / vol : null);
  }
  return out;
}
