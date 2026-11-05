"""MCP server for the Vibepollo HTTPS management API.

Configuration is supplied only through environment variables. The server uses
stdio transport so it can be launched by Codex/Claude Desktop/etc. without
opening a listener.
"""
from __future__ import annotations

import json
import os
import sys
from dataclasses import dataclass
from typing import Any, Literal
from urllib.parse import quote

import httpx
from mcp.server.mcpserver import MCPServer


@dataclass(frozen=True)
class Settings:
    base_url: str
    verify_tls: bool
    timeout: float
    username: str | None
    password: str | None
    api_token: str | None
    session_token: str | None

    @classmethod
    def from_env(cls) -> "Settings":
        base_url = os.getenv("VIBEPOLLO_URL", "https://localhost:47990").rstrip("/")
        verify = os.getenv("VIBEPOLLO_VERIFY_TLS", "true").strip().lower() not in {
            "0", "false", "no", "off"
        }
        try:
            timeout = float(os.getenv("VIBEPOLLO_TIMEOUT", "20"))
        except ValueError as exc:
            raise ValueError("VIBEPOLLO_TIMEOUT must be a number") from exc
        if timeout <= 0:
            raise ValueError("VIBEPOLLO_TIMEOUT must be greater than zero")
        return cls(
            base_url=base_url,
            verify_tls=verify,
            timeout=timeout,
            username=os.getenv("VIBEPOLLO_USERNAME") or None,
            password=os.getenv("VIBEPOLLO_PASSWORD") or None,
            api_token=os.getenv("VIBEPOLLO_API_TOKEN") or None,
            session_token=os.getenv("VIBEPOLLO_SESSION_TOKEN") or None,
        )


class VibepolloError(RuntimeError):
    pass


class VibepolloClient:
    def __init__(self, settings: Settings | None = None, transport: httpx.BaseTransport | None = None):
        self.settings = settings or Settings.from_env()
        self._csrf_token: str | None = None
        self.http = httpx.Client(
            base_url=self.settings.base_url,
            verify=self.settings.verify_tls,
            timeout=self.settings.timeout,
            auth=(self.settings.username, self.settings.password)
            if self.settings.username and self.settings.password
            else None,
            transport=transport,
            headers={"Accept": "application/json"},
        )

    def close(self) -> None:
        self.http.close()

    def _headers(self) -> dict[str, str]:
        if self.settings.api_token:
            return {"Authorization": f"Bearer {self.settings.api_token}"}
        if self.settings.session_token:
            return {"Authorization": f"Session {self.settings.session_token}"}
        return {}

    @staticmethod
    def _validate_api_path(path: str) -> str:
        if not path.startswith("/api/"):
            raise VibepolloError("Only /api/... paths are allowed by vibepollo_api_request")
        if "\x00" in path or ".." in path.split("?", 1)[0].split("/"):
            raise VibepolloError("Invalid API path")
        return path

    def _get_csrf(self) -> str:
        response = self.http.get("/api/csrf-token", headers=self._headers())
        if response.status_code >= 400:
            raise VibepolloError(f"CSRF request failed: HTTP {response.status_code}: {response.text[:500]}")
        try:
            token = response.json().get("csrf_token")
        except ValueError as exc:
            raise VibepolloError("CSRF endpoint returned invalid JSON") from exc
        if not isinstance(token, str) or not token:
            raise VibepolloError("CSRF endpoint did not return csrf_token")
        self._csrf_token = token
        return token

    def request(
        self,
        method: str,
        path: str,
        *,
        json_body: Any = None,
        params: dict[str, Any] | None = None,
        allow_non_api: bool = False,
    ) -> Any:
        method = method.upper()
        if not allow_non_api:
            path = self._validate_api_path(path)
        if not path.startswith("/"):
            path = "/" + path
        headers = self._headers()
        if method in {"POST", "PUT", "PATCH", "DELETE"}:
            headers["X-CSRF-Token"] = self._csrf_token or self._get_csrf()
        if json_body is not None:
            headers["Content-Type"] = "application/json"
        response = self.http.request(method, path, json=json_body, params=params, headers=headers)
        if response.status_code == 400 and method in {"POST", "PUT", "PATCH", "DELETE"} and "csrf" in response.text.lower():
            self._csrf_token = None
            headers["X-CSRF-Token"] = self._get_csrf()
            response = self.http.request(method, path, json=json_body, params=params, headers=headers)
        if response.status_code >= 400:
            detail = response.text[:2000]
            raise VibepolloError(f"{method} {path} failed: HTTP {response.status_code}: {detail}")
        if response.status_code == 204 or not response.content:
            return {"status_code": response.status_code}
        content_type = response.headers.get("content-type", "")
        if "json" in content_type:
            try:
                return response.json()
            except ValueError as exc:
                raise VibepolloError(f"{method} {path} returned invalid JSON") from exc
        return {"status_code": response.status_code, "content_type": content_type, "text": response.text}


mcp = MCPServer("vibepollo")


def client() -> VibepolloClient:
    return VibepolloClient()


def result(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)


@mcp.tool()
def vibepollo_api_request(
    method: Literal["GET", "POST", "PUT", "PATCH", "DELETE"],
    path: str,
    body: Any | None = None,
    query: dict[str, Any] | None = None,
) -> str:
    """Call any Vibepollo /api endpoint.

    Use only paths beginning with /api/. Mutating calls automatically obtain and
    send a CSRF token. Authentication comes from VIBEPOLLO_API_TOKEN,
    VIBEPOLLO_SESSION_TOKEN, or VIBEPOLLO_USERNAME/VIBEPOLLO_PASSWORD.
    """
    c = client()
    try:
        return result(c.request(method, path, json_body=body, params=query))
    finally:
        c.close()


@mcp.tool()
def vibepollo_get_route_catalog() -> str:
    """List API-token-compatible routes exposed by the running Vibepollo host."""
    c = client()
    try:
        return result(c.request("GET", "/api/token/routes"))
    finally:
        c.close()


@mcp.tool()
def vibepollo_get_metadata() -> str:
    """Read Vibepollo version, host readiness, capabilities, and platform metadata."""
    return vibepollo_api_request("GET", "/api/metadata")


@mcp.tool()
def vibepollo_get_config() -> str:
    """Read the current Vibepollo configuration."""
    return vibepollo_api_request("GET", "/api/config")


@mcp.tool()
def vibepollo_patch_config(changes: dict[str, Any]) -> str:
    """Partially update Vibepollo configuration. This changes persistent host settings."""
    return vibepollo_api_request("PATCH", "/api/config", changes)


@mcp.tool()
def vibepollo_list_apps() -> str:
    """List configured streaming applications."""
    return vibepollo_api_request("GET", "/api/apps")


@mcp.tool()
def vibepollo_save_app(app: dict[str, Any]) -> str:
    """Create or update a configured streaming application."""
    return vibepollo_api_request("POST", "/api/apps", app)


@mcp.tool()
def vibepollo_launch_app(app_id: str | None = None, app_uuid: str | None = None) -> str:
    """Launch a configured application by numeric id or UUID."""
    body: dict[str, Any] = {}
    if app_id is not None:
        body["appid"] = app_id
    if app_uuid is not None:
        body["appuuid"] = app_uuid
    return vibepollo_api_request("POST", "/api/apps/launch", body)


@mcp.tool()
def vibepollo_close_app() -> str:
    """Close the currently running application/session."""
    return vibepollo_api_request("POST", "/api/apps/close", {})


@mcp.tool()
def vibepollo_delete_app(identifier: str) -> str:
    """Delete an application by UUID or numeric id."""
    return vibepollo_api_request("DELETE", f"/api/apps/{quote(identifier, safe='-')}")


@mcp.tool()
def vibepollo_list_clients() -> str:
    """List paired streaming clients."""
    return vibepollo_api_request("GET", "/api/clients/list")


@mcp.tool()
def vibepollo_update_client(settings: dict[str, Any]) -> str:
    """Update a paired client, for example HDR profile or allow_sleep."""
    return vibepollo_api_request("POST", "/api/clients/update", settings)


@mcp.tool()
def vibepollo_unpair_client(uuid: str) -> str:
    """Unpair one client."""
    return vibepollo_api_request("POST", "/api/clients/unpair", {"uuid": uuid})


@mcp.tool()
def vibepollo_unpair_all_clients() -> str:
    """Unpair all clients."""
    return vibepollo_api_request("POST", "/api/clients/unpair-all", {})


@mcp.tool()
def vibepollo_get_sessions() -> str:
    """List active RTSP, WebRTC, and historical session data."""
    c = client()
    try:
        return result({
            "session_status": c.request("GET", "/api/session/status"),
            "rtsp": c.request("GET", "/api/rtsp/sessions"),
            "webrtc": c.request("GET", "/api/webrtc/sessions"),
            "history_active": c.request("GET", "/api/history/sessions/active"),
        })
    finally:
        c.close()


@mcp.tool()
def vibepollo_webrtc_capabilities() -> str:
    """Read WebRTC host and codec capabilities."""
    return vibepollo_api_request("GET", "/api/webrtc/capabilities")


@mcp.tool()
def vibepollo_create_webrtc_session(configuration: dict[str, Any]) -> str:
    """Create a WebRTC streaming session."""
    return vibepollo_api_request("POST", "/api/webrtc/sessions", configuration)


@mcp.tool()
def vibepollo_delete_webrtc_session(session_id: str) -> str:
    """Delete a WebRTC session."""
    return vibepollo_api_request("DELETE", f"/api/webrtc/sessions/{quote(session_id, safe='-')}")


@mcp.tool()
def vibepollo_display_devices(full: bool = False) -> str:
    """List host display devices and optional detailed capability data."""
    return vibepollo_api_request("GET", "/api/display-devices", query={"detail": "full"} if full else None)


@mcp.tool()
def vibepollo_provider_data(provider: Literal["steam", "lutris", "playnite"], kind: Literal["status", "games", "categories"]) -> str:
    """Read Steam, Lutris, or Playnite integration status/data."""
    if provider == "playnite" and kind == "categories":
        path = "/api/playnite/categories"
    else:
        path = f"/api/{provider}/{kind}"
    return vibepollo_api_request("GET", path)


@mcp.tool()
def vibepollo_provider_action(
    provider: Literal["steam", "lutris", "playnite"],
    action: Literal["force_sync", "launch", "install", "uninstall"],
    body: dict[str, Any] | None = None,
) -> str:
    """Run a provider action such as force_sync, launch, install, or uninstall."""
    return vibepollo_api_request("POST", f"/api/{provider}/{action}", body or {})


@mcp.tool()
def vibepollo_health() -> str:
    """Read available health/integration status endpoints."""
    c = client()
    try:
        paths = [
            "/api/health/vigem",
            "/api/health/vulkan-hdr-layer",
            "/api/health/crashdump",
            "/api/frame-limiter/status",
            "/api/rtss/status",
            "/api/lossless_scaling/status",
            "/api/steam/status",
            "/api/lutris/status",
            "/api/playnite/status",
        ]
        data: dict[str, Any] = {}
        for path in paths:
            try:
                data[path] = c.request("GET", path)
            except VibepolloError as exc:
                data[path] = {"unavailable": str(exc)}
        return result(data)
    finally:
        c.close()


@mcp.tool()
def vibepollo_get_logs() -> str:
    """Read the most recent Vibepollo session log."""
    return vibepollo_api_request("GET", "/api/logs")


@mcp.tool()
def vibepollo_restart() -> str:
    """Restart Vibepollo. The MCP connection may become unavailable briefly."""
    return vibepollo_api_request("POST", "/api/restart", {})


@mcp.tool()
def vibepollo_create_api_token(scopes: list[dict[str, Any]]) -> str:
    """Create an API token with explicit path/method scopes. The raw token is returned once."""
    return vibepollo_api_request("POST", "/api/token", {"scopes": scopes})


@mcp.tool()
def vibepollo_list_api_tokens() -> str:
    """List API-token metadata and scopes; raw token values are never returned by Vibepollo."""
    return vibepollo_api_request("GET", "/api/tokens")


@mcp.tool()
def vibepollo_revoke_api_token(token_hash: str) -> str:
    """Revoke an API token by its metadata hash."""
    return vibepollo_api_request("DELETE", f"/api/token/{quote(token_hash, safe='')}")


if __name__ == "__main__":
    # Never print configuration or credentials to stdout: stdout is the MCP wire.
    print("vibepollo MCP server starting on stdio", file=sys.stderr)
    mcp.run(transport="stdio")


def main() -> None:
    """Console-script entry point."""
    mcp.run(transport="stdio")
