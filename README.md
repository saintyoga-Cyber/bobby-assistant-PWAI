# Tiny Assistant

Tiny Assistant is an LLM-based assistant that runs on your Pebble smartwatch,
if you still have a smartwatch that ceased production in 2016 lying around.

![A screenshot from a Pebble smartwatch running the Tiny Assistant. The user asked for the time, the assistant responded that it was 3:59 PM.](./docs/screenshot.png)

## Usage

### Server

To use Tiny Assistant, you will need to run the server in `service/` somewhere
your phone can reach.

You will also need to set a few environment variables:

- `ANTHROPIC_API_KEY` - a key for Anthropic's Claude API - you can get one at
  the [Claude Console](https://platform.claude.com). (This fork replaces the
  upstream Gemini backend with Claude; `GEMINI_KEY` is no longer used.)
- `REDIS_URL` - a URL for a functioning Redis server. Long-term memories are
  stored in Redis, so enable persistence (AOF) or use a managed instance —
  a purely in-memory server will lose memories on restart.
- `USER_IDENTIFICATION_URL` - a URL pointing to an instance of
  [user-identifier](https://github.com/pebble-dev/user-identifier).
- `MAPBOX_KEY` - an API key for [Mapbox](https://www.mapbox.com), which is
  used for geocoding. If no key is provided, geocoding will be unavailable.

Optional:

- `SELF_HOSTED=1` - skip the Rebble subscription check and lift the monthly
  quota cap (for personal deployments).
- `CHAT_MODEL` / `VERIFIER_MODEL` - exact Anthropic model ID strings; both
  default to `claude-haiku-4-5`. Step up to e.g. `claude-sonnet-4-6` if
  answer quality disappoints.
- `PERPLEXITY_API_KEY` - enables the `web_search` function (live web answers
  via Perplexity sonar). Without it, the assistant has no web search.

### Client

Update the URL in `app/src/pkjs/urls.js` to point at your instance of the
server.

Then you can simply build it using the Pebble SDK and install on your watch.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for details.

## License

Apache 2.0; see [`LICENSE`](LICENSE) for details.

## Disclaimer

This project is not an official Google project. It is not supported by
Google and Google specifically disclaims all warranties as to its quality,
merchantability, or fitness for a particular purpose.
  
