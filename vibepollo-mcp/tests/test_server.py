import json
import sys
from pathlib import Path

import httpx

sys.path.insert(0, str(Path(__file__).parents[1]))
import server


class FakeTransport(httpx.BaseTransport):
    def __init__(self):
        self.calls = []

    def handle_request(self, request: httpx.Request) -> httpx.Response:
        self.calls.append(request)
        if request.url.path == "/api/csrf-token":
            return httpx.Response(200, json={"csrf_token": "csrf-1"}, request=request)
        if request.url.path == "/api/metadata":
            return httpx.Response(200, json={"version": "test"}, request=request)
        if request.url.path == "/api/apps/close":
            return httpx.Response(200, json={"status": True}, request=request)
        return httpx.Response(404, json={"error": "not found"}, request=request)


def test_basic_get_and_mutation_csrf():
    transport = FakeTransport()
    settings = server.Settings(
        base_url="https://vibepollo.test",
        verify_tls=True,
        timeout=5,
        username="admin",
        password="secret",
        api_token=None,
        session_token=None,
    )
    client = server.VibepolloClient(settings, transport)
    assert client.request("GET", "/api/metadata") == {"version": "test"}
    assert client.request("POST", "/api/apps/close", json_body={}) == {"status": True}
    assert [r.url.path for r in transport.calls] == ["/api/metadata", "/api/csrf-token", "/api/apps/close"]
    assert transport.calls[-1].headers["x-csrf-token"] == "csrf-1"
    client.close()


def test_non_api_paths_are_rejected():
    client = server.VibepolloClient(
        server.Settings("https://x", True, 5, None, None, "token", None),
        httpx.MockTransport(lambda request: httpx.Response(200, json={}, request=request)),
    )
    try:
        client.request("GET", "/launch")
    except server.VibepolloError as exc:
        assert "Only /api/" in str(exc)
    else:
        raise AssertionError("non-api path was accepted")
    client.close()
