# Aegis Terminal

A live market-analytics front end for the Aegis engine. Search any instrument
and its full quant profile — returns, volatility, Sharpe, drawdown, rolling
VWAP and block-trade flow — is computed on live data by the compiled C++ engine.

## Stack

- **Front end** — React + TypeScript (Vite), charts via `lightweight-charts`.
- **Data service** — Node/Express (`server.mjs`): fetches OHLCV, pipes it
  through `build/analyze`, and serves the results. Quotes refresh on a poll.

## Run

From the repository root, build the engine:

```bash
cmake --build build --target analyze
```

Then start the app:

```bash
cd web
npm install
npm run dev
```

Open http://localhost:5173. The Vite dev server proxies `/api` to the data
service on port 5177.

## Endpoints

| Route | Purpose |
|-------|---------|
| `GET /api/universe` | Every instrument, analyzed on the engine (cached) |
| `GET /api/analyze?symbol=NVDA` | Full analysis + dated candles for one instrument |
| `GET /api/quote?symbol=NVDA` | Latest quote for the live tape |
| `GET /api/search?q=tata` | Symbol/alias search |
