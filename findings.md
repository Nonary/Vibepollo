# Investigation: Issue #222 — "Vibepollo not working (RTSP Error 54) after updating to 1.15.4-stable2 from 1.15.3"

Issue: https://github.com/Nonary/Vibepollo/issues/222

## TL;DR

The reported "RTSP Error 54" on the Moonlight (macOS) client is a **downstream symptom**. The
real failure is **server-side**: the RTSP control server fails to bind its TCP socket at startup
with Windows error **WSAEFAULT (10014)** ("The system detected an invalid pointer address..."),
which is logged `Fatal`, so Sunshine aborts and the service restarts in a loop. With no RTSP port
listening, the client's RTSP connection is reset → Moonlight reports **error 54 (ECONNRESET)**.

Root cause: the RTSP acceptor opens its socket with a protocol family chosen from
`address_family`, but binds it to an address chosen from `bind_address`. When those disagree —
specifically `address_family = both` (→ IPv6 socket) combined with an **IPv4** `bind_address` —
boost hands a `sockaddr_in` (IPv4, 16 bytes) to `bind()` on an `AF_INET6` socket, and Windows
rejects it with WSAEFAULT.

This is **not** a Moonlight caching problem (the maintainer's working hypothesis on the issue) and
has **nothing to do with the GPU** (the NvEnc YUV444 errors in the logs are harmless encoder-probe
noise; the bug reproduces regardless of vendor — Tsubajashi's "works on AMD" box simply has a
different network config).

---

## What the logs showed (evidence)

I downloaded and extracted the log bundles attached to the issue:

- Tsubajashi: `vibeshine_logs-20260505-164849.zip`, `vibeshine_logs-20260506-131016.zip`
- lozengelover: the individual `sunshine-20260515-*.log` files

Both reporters were running the fork (bundle name `vibeshine_logs-*`; banner
`Vibepollo version: 1.15.4-stable.2 commit: ce42ae2c...` for lozengelover, `1.15.5-alpha.1` for
Tsubajashi).

### The fatal line (both reporters, identical mechanism)

lozengelover (`sunshine-20260515-134251-397.log`, also -134328):
```
Fatal: Couldn't bind RTSP server to port [48000], The system detected an invalid pointer address
       in attempting to use a pointer argument in a call
```

Tsubajashi (`t1/sunshine-20260505-060731-862.log`, German locale, same error):
```
Fatal: Couldn't bind RTSP server to port [48010], Bei dem Versuch das Zeigerargument eines Aufrufs
       zu verwenden, wurde eine ungültige Zeigeradresse gefunden
```

That message text is the boost/`std::system_category` rendering of Windows error **10014
(WSAEFAULT)**. The ports differ (48000 vs 48010) only because the two users have different base
`port` settings; the failure is the same.

### The smoking-gun config (lozengelover, `sunshine-20260515-134240-334.log`)

```
config: 'address_family' = both
config: 'bind_address'   = 192.168.0.16
```

`address_family = both` and a literal **IPv4** `bind_address`. This is exactly the conflicting
combination required to trigger the bug.

### Crash-restart loop

lozengelover's bundle contains a burst of short-lived `sunshine-*.log` files within seconds of each
other (…134240, 134251, 134328, 134628, 134639…), each ending in the same Fatal bind line. That is
the service repeatedly starting, failing to bind RTSP, and being restarted — which is why the
client never finds a listening RTSP port.

---

## Why WSAEFAULT, mechanically

The RTSP server bind, `src/rtsp.cpp` (`rtsp_server_t::bind`):

```cpp
int bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
  acceptor.open(af == net::IPV4 ? tcp::v4() : tcp::v6(), ec);   // (1) family chosen from af
  if (ec) { return -1; }

  acceptor.set_option(boost::asio::socket_base::reuse_address {true});

  auto bind_addr_str = net::get_bind_address(af);               // (2) address chosen from bind_address
  const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
  if (ec) { ... return -1; }

  acceptor.bind(tcp::endpoint(bind_addr, port), ec);            // (3) IPv4 addr onto IPv6 socket
  if (ec) { return -1; }
  ...
}
```

`src/network.cpp`:

```cpp
af_e af_from_enum_string(const std::string_view &view) {
  if (view == "ipv4") return IPV4;
  if (view == "both") return BOTH;
  return BOTH;                       // default/empty → BOTH
}

std::string get_bind_address(const af_e af) {
  if (!config::sunshine.bind_address.empty()) {
    return config::sunshine.bind_address;   // returns the user's IPv4 literal verbatim
  }
  return std::string(af_to_any_address_string(af));  // wildcard "::" (both) or "0.0.0.0" (ipv4)
}
```

Step by step for the failing config (`address_family=both`, `bind_address=192.168.0.16`):

1. `af` = `BOTH` (not `IPV4`) → `acceptor.open(tcp::v6())` → an **AF_INET6** socket.
2. `get_bind_address(BOTH)` sees a non-empty `bind_address` and returns **"192.168.0.16"**, an
   **IPv4** address. (The wildcard branch that would have returned `"::"` is skipped.)
3. `tcp::endpoint(192.168.0.16, port)` is an IPv4 endpoint → boost passes a `sockaddr_in`
   (16 bytes) to Windows `bind()` on the AF_INET6 socket.

Windows `bind()` returns **WSAEFAULT** because the `sockaddr`/`namelen` (IPv4, 16 bytes) is invalid
for the socket's family (AF_INET6 expects a 28-byte `sockaddr_in6`). The two halves of the function
derived the address family from *different* inputs and never reconciled them.

`enet` (audio/video/control ports) survives the same config because enet always uses a
`sockaddr_in6` and stores an IPv4 `bind_address` as a v4-mapped address — which is why only the RTSP
acceptor throws the Fatal, matching the logs exactly.

### Why "Error 54" specifically

macOS `errno 54 = ECONNRESET`. With the RTSP server never bound (and the process crash-looping),
the client's RTSP TCP connection gets reset, surfacing as error 54 in Moonlight. The user attributes
it to RTSP/streaming, but the actual failure happened seconds earlier at socket bind.

---

## Why a downgrade "fixes" it (the regression is config, not code)

I compared the bind path across the relevant tags:

- `git diff 1.15.3..1.15.4 -- src/rtsp.cpp src/network.cpp src/network.h` → **no changes**.
- `get_bind_address` body is **byte-identical** at tags `1.15.2`, `1.15.3`, `1.15.4`
  (verified via `git show <tag>:src/network.cpp`).

So nothing in the bind path changed across the reporters' 1.15.3 → 1.15.4 upgrade. The trip-wire is
the **config value**: at some point `bind_address` got populated with the user's IPv4 interface
address (the "bind to a specific interface" UI from PR #4481 writes an IPv4 literal) while
`address_family` stayed at its default `both`. Their previously-working setup had `bind_address`
empty (→ binds the `::` wildcard, which is consistent with the v6 socket) or `address_family=ipv4`.
Downgrading to a binary whose stored config still had `bind_address` empty avoids the mismatch — it
looks like a code fix but it is really a config difference.

---

## When the relevant code was introduced (commits + dates)

- **Family-selection line** (`acceptor.open(af == IPV4 ? v4 : v6)`): long-standing Sunshine code,
  did **not** change. `git blame` bottoms out at boundary commit `1a5f5e59` (ReenigneArcher,
  authored 2025-05-08, `build(deps): bump vite ... #3858`); the line predates that boundary. This is
  the *latent* half of the bug.

- **The coupling that armed the bug** — `get_bind_address` returning the configured `bind_address`
  verbatim — was introduced by:
  - commit `0aa7e3fd67c03926efc97edc5b2077afa6256524`
  - `feat(network): allow binding to specific interface (#4481)` — David Lane
  - authored & committed **2025-12-23 13:08:12 -0500**

  Before this commit, `get_bind_address` returned **only** the wildcard, which always matches the
  family `open()` picks, so the mismatch could not occur. This commit is the origin of the change.

### Caveat on dating (history was rebased)

This repo's history was rewritten during the 1.16.0 upstream sync, so tag→commit→date mappings are
not chronologically reliable. Proof it is scrambled: the earliest tag in the current DAG that
*contains* `0aa7e3fd` is `1.14.5-alpha.1` dated **2026-05-06**, yet `1.14.7` is dated
**2026-02-18** — out of order. Consequently `git tag --contains` and commit dates here reflect the
post-rebase unified tree, not the true release chronology. What is solid regardless of the rewrite:
`0aa7e3fd` is the introducing commit, and the coupling was live and identical in every 1.15.x build
the reporters touched. (Determining the *true* pre-rebase ship date in the Apollo/Vibepollo lineage
would require the pre-1.16.0 refs/reflog or the release artifacts, not this tree.)

---

## Upstream Sunshine is affected too (verified against live source)

Note: my first pass wrongly inferred "inherited from upstream" from `git blame` of the **current**
(post-1.16.0) tree — that's invalid, because the blame already contains the upstream merge. To
answer it correctly I fetched the **live upstream `LizardByte/Sunshine` master** `src/rtsp.cpp`
directly. Its `bind()` is byte-identical:

```cpp
acceptor.open(af == net::IPV4 ? tcp::v4() : tcp::v6(), ec);
...
auto bind_addr_str = net::get_bind_address(af);
const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
...
acceptor.bind(tcp::endpoint(bind_addr, port), ec);
```

So upstream Sunshine on Windows would hit the same WSAEFAULT under the same config (IPv4
`bind_address` + `address_family=both`). This is convergent shared-ancestry code, not something
Vibepollo took from upstream — but the fix is equally valid upstream and would be a good PR. The
reason there are few reports despite many downloads is that it requires the specific combination;
most users leave `bind_address` empty.

---

## Immediate workaround (no rebuild required)

Either one unblocks affected users by removing the family/address mismatch:

- Set **`address_family = ipv4`** (their `bind_address` is IPv4, so the socket then opens as v4), or
- Clear **`bind_address`** (falls back to the `::` wildcard, consistent with the v6 socket).

---

## Proposed fix

Derive the socket's protocol family from the **resolved bind address** instead of from
`address_family`, so the two can never disagree. In `src/rtsp.cpp` `rtsp_server_t::bind`, parse the
address first, then open with the matching family:

```cpp
int bind(net::af_e af, std::uint16_t port, boost::system::error_code &ec) {
  auto bind_addr_str = net::get_bind_address(af);
  const auto bind_addr = boost::asio::ip::make_address(bind_addr_str, ec);
  if (ec) {
    BOOST_LOG(error) << "Invalid bind address: "sv << bind_addr_str << " - " << ec.message();
    return -1;
  }

  acceptor.open(bind_addr.is_v6() ? tcp::v6() : tcp::v4(), ec);
  if (ec) {
    return -1;
  }

  acceptor.set_option(boost::asio::socket_base::reuse_address {true});

  acceptor.bind(tcp::endpoint(bind_addr, port), ec);
  if (ec) {
    return -1;
  }
  // ... listen / async_accept unchanged
}
```

Effects:
- `both` + empty `bind_address` → `"::"` → v6 socket (unchanged behavior).
- `ipv4` + empty `bind_address` → `"0.0.0.0"` → v4 socket (unchanged behavior).
- any `bind_address` literal → socket family follows the literal → no mismatch, no WSAEFAULT.

Consider auditing the parallel enet path (`net::host_create` in `src/network.cpp`, used by
`src/stream.cpp` for audio/video/control) for the same family/address coupling for consistency,
even though it does not currently throw the Fatal.

---

## Commands used (for reproducibility)

- Issue + comments: `gh issue view 222 --repo Nonary/Vibepollo --json ...`
- Logs: `curl -sL <attachment-url>` then `unzip`; grep for `Couldn't bind RTSP`, `address_family`,
  `bind_address`.
- Bind code: `src/rtsp.cpp:409` (`rtsp_server_t::bind`), `src/network.cpp` (`get_bind_address`,
  `af_from_enum_string`, `af_to_any_address_string`), `src/network.h` (`enum af_e { IPV4, BOTH }`).
- Diffs/blame: `git diff 1.15.3..1.15.4 -- src/rtsp.cpp src/network.cpp`;
  `git show <tag>:src/network.cpp`; `git blame -L 409,427 src/rtsp.cpp`;
  `git log -S "config::sunshine.bind_address.empty()" -- src/network.cpp`.
- Upstream check: fetched `raw.githubusercontent.com/LizardByte/Sunshine/master/src/rtsp.cpp`.
