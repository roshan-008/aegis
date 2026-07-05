import type { Analysis, Quote, SearchHit, Summary } from "./types";

async function get<T>(url: string): Promise<T> {
  const r = await fetch(url);
  if (!r.ok) {
    const body = await r.json().catch(() => ({}));
    throw new Error((body as { error?: string }).error || `request failed (${r.status})`);
  }
  return r.json() as Promise<T>;
}

export const searchSymbols = (q: string) =>
  get<SearchHit[]>(`/api/search?q=${encodeURIComponent(q)}`);

export const fetchAnalysis = (symbol: string) =>
  get<Analysis>(`/api/analyze?symbol=${encodeURIComponent(symbol)}`);

export const fetchUniverse = () => get<Summary[]>(`/api/universe`);

export const fetchQuote = (symbol: string) =>
  get<Quote>(`/api/quote?symbol=${encodeURIComponent(symbol)}`);
