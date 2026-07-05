const CUR: Record<string, string> = { USD: "$", INR: "₹", EUR: "€", GBP: "£" };

export const cur = (c: string) => CUR[c] || "";

export const pct = (v: number) => (v >= 0 ? "+" : "") + v.toFixed(2) + "%";

export const px = (v: number) =>
  v >= 1000 ? v.toLocaleString(undefined, { maximumFractionDigits: 0 }) : v.toFixed(2);

export function big(v: number): string {
  const a = Math.abs(v);
  if (a >= 1e12) return (v / 1e12).toFixed(2) + "T";
  if (a >= 1e9) return (v / 1e9).toFixed(2) + "B";
  if (a >= 1e6) return (v / 1e6).toFixed(1) + "M";
  if (a >= 1e3) return (v / 1e3).toFixed(1) + "K";
  return v.toFixed(0);
}

export function latency(ns: number): string {
  if (ns < 1000) return ns.toFixed(0) + " ns";
  if (ns < 1e6) return (ns / 1000).toFixed(1) + " µs";
  return (ns / 1e6).toFixed(2) + " ms";
}
