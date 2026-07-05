export interface Stats {
  first: number; last: number; min: number; max: number;
  total_return_pct: number; ann_return_pct: number; ann_vol_pct: number;
  sharpe: number; max_drawdown_pct: number; mean_daily_pct: number;
  win_rate: number; best_day_pct: number; worst_day_pct: number;
  avg_volume: number; median_volume: number; total_volume: number;
  block_threshold: number; block_days: number; dollar_volume: number;
  up_volume_frac: number; obv_last: number;
  latest_vwap: number; latest_mean: number;
}

export interface Candle { time: number; close: number; volume: number; }

export interface Analysis {
  key: string; symbol: string; name: string; sector: string; region: string;
  currency: string; exchange: string;
  rows: number; threads: number; hw_threads: number; parallel: boolean;
  latency_ns: number; rows_per_sec: number; window: number;
  stats: Stats; candles: Candle[];
}

export type Summary = Omit<Analysis, "candles">;

export interface SearchHit { key: string; name: string; sector: string; region: string; }

export interface Quote { price: number; volume?: number; at: number; source: string; latencyMs: number; }
