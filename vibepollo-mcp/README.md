# Vibepollo MCP server

A local stdio MCP server for managing a Vibepollo host over its HTTPS API.

## Configuration

Set exactly one authentication mode where possible:

```bash
export VIBEPOLLO_URL='https://192.168.1.158:47990'
export VIBEPOLLO_API_TOKEN='[REDACTED]'
export VIBEPOLLO_VERIFY_TLS=false  # only for a self-signed certificate
```

Alternatively use a session token:

```bash
export VIBEPOLLO_SESSION_TOKEN='[REDACTED]'
```

Or Basic Auth:

```bash
export VIBEPOLLO_USERNAME='admin'
export VIBEPOLLO_PASSWORD='[REDACTED]'
```

Never commit credentials or place them in this README. `VIBEPOLLO_TIMEOUT`
defaults to 20 seconds.

## Install and run

```bash
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r requirements.txt
python server.py
```

The server uses stdio and does not open a listening socket.

Example Codex MCP registration from this directory:

```bash
codex mcp add vibepollo --env VIBEPOLLO_URL=https://192.168.1.158:47990 --env VIBEPOLLO_API_TOKEN='[REDACTED]' -- python server.py
```

Prefer putting secrets in the environment or a secret manager rather than in a
shell command recorded in history.

## Tools

- `vibepollo_api_request`: generic `/api/...` request with automatic CSRF handling.
- `vibepollo_get_route_catalog`: read the host's live API-token route catalog.
- metadata/config/app/client/session/WebRTC/display/provider/health/log tools.
- controlled mutating tools for configuration, app/client/provider/WebRTC actions,
  restart, and API-token lifecycle.

The generic tool intentionally rejects non-`/api` NVHTTP routes. Pairing,
Moonlight launch, and power endpoints use a separate protocol and are not
exposed through the generic management tool.
