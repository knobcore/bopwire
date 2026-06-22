# Mini-Node WebSocket Gateway: Production TLS (wss://)

## Background

Browsers loading the web player over `https://` cannot open `ws://` sockets —
the spec forbids it (mixed content). They must connect to `wss://` (RFC 6455
over TLS). The mini-node currently exposes three WebSocket surfaces, all
plain `ws://`:

| Surface              | Source                          | Default port |
|----------------------|---------------------------------|--------------|
| `WsMiniGateway`      | `src/transport/ws_mini_gateway` | 8082         |
| `WsAudioBridge`      | `src/transport/ws_audio_bridge` | 8082         |
| `WsTcpRelay`         | `src/transport/ws_tcp_relay`    | 8081         |
| `WsBridge` (legacy)  | `src/transport/ws_bridge`       | 9090         |

The current per-process header comments all say the same thing: production
deploys terminate TLS in front of these listeners (nginx / Caddy /
Cloudflare). This document picks one of those options, ratifies it, and
writes the runbook.

## Audit: what cert_util.{h,cpp} can and cannot do

`src/transport/cert_util.h` exposes a single function:

```
CertFiles make_self_signed_files();
```

Implementation in `cert_util.cpp`:

- Generates an EC P-256 key with OpenSSL `EVP_PKEY_keygen`.
- Builds a self-signed X.509 cert, CN=`musicchain`, valid 5 years, serial 1.
- Signs with SHA-256.
- Writes both PEMs to the system temp dir, names tagged with the current PID
  (`musicchain_<pid>.crt` / `.key`) so multiple instances on the same host
  don't clash.
- Caller is responsible for deleting the files.

Existing callers:

- `src/transport/mc_rats_quic.cpp:473` — peer-to-peer QUIC backend for
  librats. TLS for the peer mesh, not for browsers.
- `src/transport/h3_server.cpp:248` — HTTP/3 listener for `curl --http3`
  and `h3.request` librats tunnel. TLS for QUIC's required encryption,
  not for browsers.

What `cert_util` **does** for us today:

- Solves the "QUIC mandates TLS but we have no PKI" problem for peer
  transport, where peer identity is the librats `peer_id` (independent
  of the certificate).
- Cert is anonymous; both sides accept whatever is presented.

What `cert_util` **does not** do — and cannot, by design:

- No CA-signed cert. Browsers reject `CN=musicchain` self-signed certs
  unless the user manually trusts each VPS's cert (impossible at scale,
  and impossible at all on iOS Safari / mobile Chrome without root).
- No SAN matching the VPS hostname.
- No ACME, no renewal, no OCSP.
- No challenge-response (HTTP-01 / DNS-01 / TLS-ALPN-01) — the binary
  has nowhere for an ACME server to put `/.well-known/acme-challenge/`.
- No cert reload on disk-change; the cert path is per-PID temp and
  vanishes when the process exits.

**Conclusion:** `cert_util` is fit for purpose for QUIC peer transport
where mutual identity is the librats handshake, and unfit as the TLS
entry point for browsers.

## The two viable patterns

### Option A — mini-node terminates wss:// itself

Add an OpenSSL `SSL_CTX` to `WsMiniGateway` / `WsAudioBridge` /
`WsTcpRelay`, swap each `read()/write()` for `SSL_read()/SSL_write()`,
load a real Let's Encrypt cert + key from disk on startup, watch the
file for renewal.

What this needs:

- ACME client. No in-process implementation; would need to vendor one
  (e.g. lego, certbot via cron, acme.sh) or write our own.
- A DNS A record per VPS that is publicly resolvable, since HTTP-01
  requires port 80 reachable from Let's Encrypt's validators.
- Port 80 OR port 443 free on the mini-node host for the ACME challenge.
  If the mini-node already serves the API on 443 via msh3 / HTTP/3,
  HTTP-01 (port 80) is easier; otherwise TLS-ALPN-01 on 443 collides
  with anything else binding 443.
- A renewal hook: when certbot rewrites `fullchain.pem`, the mini-node
  must `SSL_CTX_use_certificate_chain_file` again or close + re-listen.
  No code for this exists.
- Per-VPS cert provisioning during `vps-deploy-all.sh`. Currently the
  deploy script just copies a binary; we'd need to bootstrap ACME on
  every fresh VPS, plus keep one TXT/A record per VPS DNS-side.
- Bigger attack surface inside the C++ binary: TLS bugs become
  mini-node bugs.

What this **avoids**:

- No extra process / daemon on the VPS.
- One fewer hop on each WebSocket frame.

### Option B — fronting proxy (nginx or Caddy) on the VPS terminates wss://

The mini-node keeps speaking plain `ws://` on `127.0.0.1:8082` (and
`8081` if `WsTcpRelay` stays). A reverse proxy on the same VPS owns
the public `:443` socket and the cert. Browsers see `wss://<vps>/`,
the proxy unwraps TLS and forwards the WebSocket upgrade to the
mini-node loopback.

What this needs:

- One package install per VPS (`apt-get install caddy` or `nginx` +
  `certbot`).
- A DNS A record per VPS pointing to its public IP, so the proxy can
  prove ownership for HTTP-01 / TLS-ALPN-01.
- A static config file per surface (caddy.json or nginx site stanza).
- Mini-node bind address changes from `0.0.0.0:8082` to `127.0.0.1:8082`
  so only the proxy can hit it — closes the plaintext port to the
  public Internet at the same time.

What this **avoids**:

- No ACME / cert reload logic in C++.
- No DNS / firewall changes inside the mini-node binary.
- Cert renewal is a Caddy automatic / certbot timer detail; mini-node
  has no notion of cert lifetime.
- TLS bugs stay in the proxy.

### Pick

**Option B (Caddy front proxy).** Same source comments in
`ws_tcp_relay.h:35`, `ws_audio_bridge.cpp:8`, `ws_bridge.h:17` already
assume this model — picking it ratifies the existing design instead
of adding ACME logic into the C++ binary. Caddy over nginx because
Caddy's automatic HTTPS provisions and renews Let's Encrypt certs
with zero config beyond the domain name, which is the runbook below.

## Runbook (Caddy fronting `ws_mini_gateway` on a fresh Linux VPS)

### 0. Prereqs (one-time, per VPS)

- VPS already provisioned with the mini-node binary at
  `/opt/musicchain/mini-node-vps/build/mini-node` (per
  `project-musicchain-platforms`).
- VPS has a public IPv4. For the example below it is `85.239.238.226`.
- A DNS A record points at it. Pick a per-VPS hostname so multi-VPS
  load-balancing (see `project_musicchain_multi_vps_topology`) keeps
  one cert per VPS:

  ```
  vps01.musicchain.example.   IN A   85.239.238.226
  ```

- UDP/443 (`h3_server`) and TCP/443 (Caddy) are both free. They don't
  collide — different protocols on different sockets — but a server
  firewall must open both.

### 1. Mini-node config change

Rebuild the mini-node so the WebSocket gateways bind loopback only.
The patch is one constant per surface; do not expose the plaintext
WS port to the public Internet once Caddy is in front:

- `ws_mini_gateway`: bind `127.0.0.1:8082` instead of `0.0.0.0:8082`.
- `ws_audio_bridge`: same (or remove entirely once `ws_mini_gateway`
  has fully replaced it).
- `ws_tcp_relay`: bind `127.0.0.1:8081` if still in service.

VPS firewall (`ufw`):

```
ufw allow 22/tcp
ufw allow 443/tcp     # caddy (wss)
ufw allow 443/udp     # h3_server (peer / HTTP/3 over QUIC)
ufw allow 8080/tcp    # librats peer mesh (plain TCP)
ufw enable
```

`8081`, `8082` are deliberately not opened — only Caddy on loopback
talks to them.

### 2. Install Caddy

```
apt-get update
apt-get install -y debian-keyring debian-archive-keyring apt-transport-https
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/gpg.key' \
   | gpg --dearmor -o /usr/share/keyrings/caddy-stable-archive-keyring.gpg
curl -1sLf 'https://dl.cloudsmith.io/public/caddy/stable/debian.deb.txt' \
   | tee /etc/apt/sources.list.d/caddy-stable.list
apt-get update
apt-get install -y caddy
```

(Use the Caddy package repo, not the Debian-shipped version, which
lags by a year.)

### 3. Caddy site config

`/etc/caddy/Caddyfile`:

```
vps01.musicchain.example {
    # Browser hits wss://vps01.musicchain.example/mini/
    # Caddy terminates TLS, forwards the WS upgrade to the
    # ws_mini_gateway loopback listener on 8082.
    reverse_proxy /mini/* 127.0.0.1:8082

    # Optional: keep the old surfaces alive during migration.
    reverse_proxy /audio/* 127.0.0.1:8082
    reverse_proxy /relay/* 127.0.0.1:8081

    # ACME contact + log location (Caddy default is fine; spelled out
    # here so renewal is observable).
    log {
        output file /var/log/caddy/access.log
    }
}
```

The Caddyfile syntax `reverse_proxy <path> <upstream>` automatically
forwards the `Upgrade: websocket` / `Connection: upgrade` headers —
no extra `header_up` lines required.

```
systemctl reload caddy
journalctl -u caddy -f
```

First reload triggers ACME HTTP-01 against
`vps01.musicchain.example:80` (Caddy temporarily binds 80 to satisfy
the challenge); cert lands in
`/var/lib/caddy/.local/share/caddy/certificates/`. Renewal is
automatic (Caddy checks once a day, renews 30 days before expiry).

### 4. Browser side

The web player previously hard-coded `ws://<vps>:8082/`. Switch the
URL builder to:

```
const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
const url   = `${proto}//${host}/mini/`;     // path matches Caddyfile
```

Plain `http://` localhost dev still works (no Caddy, direct
`ws://localhost:8082/`); production `https://` pages get `wss://`.

### 5. Smoke test (no loopback per `feedback_no_loopback`)

From the home node (Windows) — NOT from the VPS itself:

1. `curl -sS https://vps01.musicchain.example/` returns HTTP 4xx / 5xx
   from the mini-node verb router (a successful TLS handshake; the
   path isn't a real GET endpoint so 404 is fine).
2. Open the player at `https://player.musicchain.example/`. The
   browser console should show one successful
   `wss://vps01.musicchain.example/mini/` connection and a
   `routes.get` reply with at least the home node.
3. Confirm `journalctl -u caddy -n 100` shows the upgrade
   (`status 101`).
4. Confirm `ss -tnlp` on the VPS shows Caddy on `:443`,
   `mini-node` on `127.0.0.1:8082`, and nothing else listening on
   `0.0.0.0:8082` / `:8081`.

### 6. Multi-VPS rollout

Per `project_musicchain_multi_vps_topology`, repeat steps 0-5 on every
VPS with its own hostname (`vps02.musicchain.example`, ...). Each VPS
gets its own Let's Encrypt cert via Caddy's automatic HTTPS — they do
not share private keys. The `mininodes.list` reply from any VPS
already advertises peer mini-nodes, so the player will fail over
between `wss://vps01.../mini/` and `wss://vps02.../mini/`.

### 7. Renewal / rotation

Nothing required. Caddy renews automatically and SIGHUP-reloads its
own listeners; mini-node never sees the cert change because it
never holds the cert.

If Caddy is ever swapped for nginx, the equivalent runbook is
`certbot --nginx -d vps01.musicchain.example`, plus a manual
`Connection upgrade` block in the nginx site stanza, plus
`systemctl enable certbot.timer`.

### 8. What `cert_util` keeps doing

Unchanged. `cert_util` still generates the anonymous QUIC cert for:

- `mc_rats_quic` (peer mesh TLS).
- `h3_server` on UDP/443 (HTTP/3 listener for non-browser HTTP/3
  clients and the `h3.request` librats tunnel).

Browsers never see those certs — they see Caddy's Let's Encrypt cert
on TCP/443 only.
