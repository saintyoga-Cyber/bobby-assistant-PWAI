# Self-Hosting Bobby

This guide covers deploying your own backend so the watch app uses your Claude API key instead of
Rebble's subscription-gated server.

## How it works

```
Watch  →  Backend (service/)  →  Claude API
```

The backend is a Go service that holds your `ANTHROPIC_API_KEY` and calls Claude on each request.
The watch/phone app is a thin client — it just sends your prompt to the backend over a WebSocket.

You need two things running:
- The **Go backend** (from `service/`, port 8080)
- A **Redis** instance (stores conversation memory and quota counters)

## Deploy on Railway (recommended)

Railway auto-detects the Dockerfile, provides a managed Redis add-on, and gives you a free
`https://*.up.railway.app` URL with TLS — which is required for the phone app's secure WebSocket
(`wss://`).

### Steps

1. Push this repository to GitHub.

2. Go to [railway.app](https://railway.app) → **New Project** → **Deploy from GitHub repo** →
   select this repo. Railway picks up `railway.toml` and uses `Dockerfile-service` automatically.

3. Add Redis: in the Railway project dashboard, click **+ New** → **Database** → **Redis**.
   Railway injects `REDIS_URL` into the bobby service automatically.

4. Set environment variables on the bobby service (**Variables** tab):

   | Variable | Value |
   |----------|-------|
   | `ANTHROPIC_API_KEY` | Your Claude API key (`sk-ant-...`) |
   | `SELF_HOSTED` | `1` |
   | `MAPBOX_KEY` | Your Mapbox token (optional, enables geocoding) |
   | `CHAT_MODEL` | `claude-sonnet-4-6` (optional upgrade from haiku) |

   See `.env.example` for the full list of optional features.

5. Railway generates a public URL like `https://bobby-production-xxxx.up.railway.app`.

6. Verify the backend is running:
   ```
   curl https://bobby-production-xxxx.up.railway.app/heartbeat
   # Expected response: bobby
   ```

7. Rebuild the watch app and sideload it to your watch.

8. In the **Rebble phone app** → open Bobby → tap the settings gear → scroll to
   **Connection → Custom server URL** → enter your Railway URL (e.g.
   `https://bobby-production-xxxx.up.railway.app`) → tap **Save**.

9. Ask the watch something — it should now respond using your own Claude key.

## Deploy on Fly.io (alternative)

The existing `Dockerfile-service` works on Fly.io too.

```bash
# Install flyctl, then:
fly launch --dockerfile Dockerfile-service --no-deploy
fly redis create           # creates a managed Upstash Redis; note the URL
fly secrets set \
  ANTHROPIC_API_KEY=sk-ant-... \
  REDIS_URL=<url from above> \
  SELF_HOSTED=1
fly deploy
```

Your app will be available at `https://<app-name>.fly.dev`.

## Environment variables reference

See `.env.example` in the repo root for the full annotated list.

**Minimum required:**

| Variable | Purpose |
|----------|---------|
| `ANTHROPIC_API_KEY` | Your Claude API key |
| `REDIS_URL` | Redis connection string (injected by Railway automatically) |
| `SELF_HOSTED` | Set to `1` to skip Rebble subscription checks |

## Notes

- **API key security:** `ANTHROPIC_API_KEY` is a server-side secret. It is set as an environment
  variable on your PaaS platform and is never sent by the phone app.
- **Redis persistence:** Railway's managed Redis has persistence enabled by default. If you run
  Redis yourself, enable AOF (`--appendonly yes`) so long-term memories survive restarts.
- **Model choice:** The default model is `claude-haiku-4-5`. Set `CHAT_MODEL=claude-sonnet-4-6`
  for noticeably better answers at higher cost.
