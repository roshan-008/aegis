import { useEffect, useRef } from "react";
import {
  createChart,
  ColorType,
  LineStyle,
  type IChartApi,
  type UTCTimestamp,
} from "lightweight-charts";
import type { Analysis } from "../lib/types";
import { rollingMean, rollingVwap } from "../lib/analytics";

const css = (v: string) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();

export default function PriceChart({ data }: { data: Analysis }) {
  const ref = useRef<HTMLDivElement>(null);
  const chartRef = useRef<IChartApi | null>(null);

  useEffect(() => {
    const el = ref.current;
    if (!el) return;

    const chart = createChart(el, {
      layout: {
        background: { type: ColorType.Solid, color: "transparent" },
        textColor: css("--muted"),
        fontFamily: css("--mono") || "monospace",
        fontSize: 11,
      },
      grid: {
        vertLines: { visible: false },
        horzLines: { color: css("--line") },
      },
      rightPriceScale: { borderColor: css("--line"), scaleMargins: { top: 0.08, bottom: 0.28 } },
      timeScale: { borderColor: css("--line"), timeVisible: false },
      crosshair: {
        vertLine: { color: css("--line-strong"), width: 1, style: LineStyle.Solid, labelBackgroundColor: css("--forest") },
        horzLine: { color: css("--line-strong"), labelBackgroundColor: css("--forest") },
      },
      handleScroll: { mouseWheel: true, pressedMouseMove: true, horzTouchDrag: true, vertTouchDrag: false },
      handleScale: { mouseWheel: true, pinch: true, axisPressedMouseMove: { time: true, price: false } },
      autoSize: true,
    });
    chartRef.current = chart;

    const w = data.window || 20;
    const mean = rollingMean(data.candles, w);
    const vwap = rollingVwap(data.candles, w);
    const t = (i: number) => data.candles[i].time as UTCTimestamp;

    const price = chart.addAreaSeries({
      lineColor: css("--forest"),
      topColor: "rgba(45,125,95,0.16)",
      bottomColor: "rgba(45,125,95,0.0)",
      lineWidth: 2,
      priceLineVisible: false,
      lastValueVisible: true,
    });
    price.setData(data.candles.map((c) => ({ time: c.time as UTCTimestamp, value: c.close })));

    const meanSeries = chart.addLineSeries({ color: css("--clay"), lineWidth: 1, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
    meanSeries.setData(mean.map((v, i) => (v == null ? null : { time: t(i), value: v })).filter(Boolean) as { time: UTCTimestamp; value: number }[]);

    const vwapSeries = chart.addLineSeries({ color: css("--teal"), lineWidth: 1, priceLineVisible: false, lastValueVisible: false, crosshairMarkerVisible: false });
    vwapSeries.setData(vwap.map((v, i) => (v == null ? null : { time: t(i), value: v })).filter(Boolean) as { time: UTCTimestamp; value: number }[]);

    const thr = data.stats.block_threshold;
    const vol = chart.addHistogramSeries({ priceScaleId: "vol", priceFormat: { type: "volume" }, lastValueVisible: false });
    vol.priceScale().applyOptions({ scaleMargins: { top: 0.82, bottom: 0 } });
    vol.setData(
      data.candles.map((c) => ({
        time: c.time as UTCTimestamp,
        value: c.volume,
        color: c.volume >= thr ? "rgba(178,58,72,0.55)" : "rgba(138,140,130,0.32)",
      }))
    );

    chart.timeScale().fitContent();
    return () => { chart.remove(); chartRef.current = null; };
  }, [data]);

  return <div ref={ref} style={{ width: "100%", height: 340 }} />;
}
