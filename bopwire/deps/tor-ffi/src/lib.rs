//! tor-ffi — Arti (Tor) embedded as a static library, exposing a local
//! SOCKS5 proxy over a C ABI.
//!
//! Why this exists
//! ---------------
//! napstr moves its bytes over Tor onion services and its protocol has no
//! clearnet fallback, so the player cannot download from it without a Tor
//! SOCKS proxy. Shelling out to a `tor` binary works on desktop but not on
//! Android, where spawning binaries is not the platform's model. Linking
//! Arti in as a static library gives every platform the same capability
//! from one code path.
//!
//! Scope: this is a Tor *client transport for one foreign network*. It has
//! nothing to do with librats or the bopwire swarm, which keep running on
//! clearnet exactly as before.
//!
//! Surface (all `#[no_mangle] extern "C"`):
//!   tor_ffi_start(port) -> i32   bound port, or negative error code
//!   tor_ffi_status()    -> i32   0 stopped, 1 bootstrapping, 2 ready, 3 error
//!   tor_ffi_stop()               idempotent
//!   tor_ffi_last_error() -> *const c_char   NUL-terminated, owned by us
//!
//! Bootstrapping the Tor directory takes seconds to minutes on a cold
//! start, so `tor_ffi_start` returns immediately with the bound port and
//! callers poll `tor_ffi_status`. The SOCKS listener accepts from the
//! moment it binds; connections made before bootstrap completes simply
//! block inside Arti rather than failing, which is what a caller wants.

use std::ffi::{c_char, c_int, CString};
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::atomic::{AtomicI32, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

use arti_client::config::TorClientConfigBuilder;
use arti_client::{TorClient, TorClientConfig};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpStream};
use tokio::runtime::Runtime;

pub const TOR_STOPPED: i32 = 0;
pub const TOR_BOOTSTRAPPING: i32 = 1;
pub const TOR_READY: i32 = 2;
pub const TOR_ERROR: i32 = 3;

// Negative returns from tor_ffi_start.
const ERR_ALREADY: i32 = -1;
const ERR_RUNTIME: i32 = -2;
const ERR_BIND: i32 = -3;

static STATUS: AtomicI32 = AtomicI32::new(TOR_STOPPED);
static LAST_ERROR: OnceLock<Mutex<Option<CString>>> = OnceLock::new();
static STATE: OnceLock<Mutex<Option<State>>> = OnceLock::new();
/// Caller-supplied base directory for Arti's state + cache. See
/// tor_ffi_set_state_dir.
static STATE_DIR: OnceLock<Mutex<Option<String>>> = OnceLock::new();

struct State {
    runtime: Runtime,
    shutdown: tokio::sync::watch::Sender<bool>,
}

fn errors() -> &'static Mutex<Option<CString>> {
    LAST_ERROR.get_or_init(|| Mutex::new(None))
}

fn state() -> &'static Mutex<Option<State>> {
    STATE.get_or_init(|| Mutex::new(None))
}

fn set_error(msg: impl Into<String>) {
    let s = msg.into();
    if let Ok(mut slot) = errors().lock() {
        *slot = CString::new(s).ok();
    }
}

fn state_dir() -> &'static Mutex<Option<String>> {
    STATE_DIR.get_or_init(|| Mutex::new(None))
}

/// Tell Arti where to keep its state and cache, before calling
/// tor_ffi_start.
///
/// Why this is necessary: TorClientConfig::default() resolves its
/// directories from platform conventions (${ARTI_LOCAL_DATA} /
/// ${ARTI_CACHE}). That works on desktop, but on ANDROID those expand to
/// paths outside the app sandbox that the process cannot create or write,
/// so create_bootstrapped fails and the client never produces a usable
/// proxy — which surfaced to users as "napstr downloads run over Tor...
/// set the Tor SOCKS5 proxy field", because the endpoint came back null.
///
/// Callers on Android pass their app-private directory. Passing NULL (or
/// never calling this) keeps the platform defaults, so desktop behaviour
/// is unchanged.
///
/// Returns 0 on success, negative on bad input.
#[no_mangle]
pub extern "C" fn tor_ffi_set_state_dir(path: *const c_char) -> c_int {
    let value = if path.is_null() {
        None
    } else {
        // SAFETY: caller contract is a NUL-terminated C string.
        match unsafe { std::ffi::CStr::from_ptr(path) }.to_str() {
            Ok(s) if !s.is_empty() => Some(s.to_owned()),
            Ok(_) => None,
            Err(_) => {
                set_error("state dir is not valid UTF-8");
                return -1;
            }
        }
    };
    match state_dir().lock() {
        Ok(mut slot) => {
            *slot = value;
            0
        }
        Err(_) => -1,
    }
}

/// Start Arti and a local SOCKS5 listener.
///
/// `port` may be 0 to let the OS choose, which is what callers should do —
/// a fixed port collides with a system tor the user may already run.
/// Returns the bound port on success.
#[no_mangle]
pub extern "C" fn tor_ffi_start(port: u16) -> c_int {
    let mut guard = match state().lock() {
        Ok(g) => g,
        Err(_) => return ERR_RUNTIME,
    };
    if guard.is_some() {
        return ERR_ALREADY;
    }

    // rustls will not choose a crypto backend on its own, and Arti reaches
    // TLS on the first directory fetch — so this must happen before any
    // Tor work, or the tokio worker panics and the process aborts.
    // install_default() errors only if one is already installed, which is
    // fine and means someone else got there first.
    let _ = rustls::crypto::ring::default_provider().install_default();

    let runtime = match tokio::runtime::Builder::new_multi_thread()
        // Arti is not bandwidth-bound here; a small pool keeps the
        // footprint sane on phones.
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(r) => r,
        Err(e) => {
            set_error(format!("tokio runtime: {e}"));
            STATUS.store(TOR_ERROR, Ordering::SeqCst);
            return ERR_RUNTIME;
        }
    };

    // Bind synchronously so the caller gets a usable port back before
    // bootstrap begins; napstr can then be configured immediately.
    let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, port));
    let listener = match runtime.block_on(async { TcpListener::bind(addr).await }) {
        Ok(l) => l,
        Err(e) => {
            set_error(format!("bind 127.0.0.1:{port}: {e}"));
            STATUS.store(TOR_ERROR, Ordering::SeqCst);
            return ERR_BIND;
        }
    };
    let bound = match listener.local_addr() {
        Ok(a) => a.port(),
        Err(e) => {
            set_error(format!("local_addr: {e}"));
            STATUS.store(TOR_ERROR, Ordering::SeqCst);
            return ERR_BIND;
        }
    };

    let (tx, rx) = tokio::sync::watch::channel(false);
    STATUS.store(TOR_BOOTSTRAPPING, Ordering::SeqCst);

    runtime.spawn(async move {
        // Honour a caller-supplied directory (Android must supply one —
        // see tor_ffi_set_state_dir). Fall back to platform defaults when
        // none was set, which is the desktop path and unchanged.
        let dir = state_dir().lock().ok().and_then(|d| d.clone());
        let config = match dir {
            Some(base) => {
                let state = format!("{base}/arti-state");
                let cache = format!("{base}/arti-cache");
                if let Err(e) = std::fs::create_dir_all(&state)
                    .and_then(|_| std::fs::create_dir_all(&cache))
                {
                    set_error(format!("tor state dir {base}: {e}"));
                    STATUS.store(TOR_ERROR, Ordering::SeqCst);
                    return;
                }
                match TorClientConfigBuilder::from_directories(&state, &cache).build() {
                    Ok(c) => c,
                    Err(e) => {
                        set_error(format!("tor config: {e}"));
                        STATUS.store(TOR_ERROR, Ordering::SeqCst);
                        return;
                    }
                }
            }
            None => TorClientConfig::default(),
        };
        let client = match TorClient::create_bootstrapped(config).await {
            Ok(c) => c,
            Err(e) => {
                set_error(format!("tor bootstrap: {e}"));
                STATUS.store(TOR_ERROR, Ordering::SeqCst);
                return;
            }
        };
        STATUS.store(TOR_READY, Ordering::SeqCst);
        serve_socks(listener, client, rx).await;
    });

    *guard = Some(State { runtime, shutdown: tx });
    bound as c_int
}

#[no_mangle]
pub extern "C" fn tor_ffi_status() -> c_int {
    STATUS.load(Ordering::SeqCst)
}

/// Stop the proxy and drop the Tor client. Idempotent.
#[no_mangle]
pub extern "C" fn tor_ffi_stop() {
    if let Ok(mut guard) = state().lock() {
        if let Some(st) = guard.take() {
            let _ = st.shutdown.send(true);
            // Don't block an app teardown on circuit close.
            st.runtime.shutdown_timeout(std::time::Duration::from_secs(3));
        }
    }
    STATUS.store(TOR_STOPPED, Ordering::SeqCst);
}

/// Most recent error, or NULL. Owned by this library; do not free.
#[no_mangle]
pub extern "C" fn tor_ffi_last_error() -> *const c_char {
    match errors().lock() {
        Ok(slot) => slot
            .as_ref()
            .map(|c| c.as_ptr())
            .unwrap_or(std::ptr::null()),
        Err(_) => std::ptr::null(),
    }
}

// ---------------------------------------------------------------------
// Minimal SOCKS5 front end
//
// Only what napstr uses: no auth, CONNECT, and DOMAIN targets (onion
// addresses). BIND/UDP-ASSOCIATE are refused with the correct reply code
// rather than dropped, so a client sees a protocol error instead of a
// hang. IPv4/IPv6 literals are accepted too — harmless, and it makes this
// a general-purpose proxy for anything else that wants one later.
// ---------------------------------------------------------------------

async fn serve_socks(
    listener: TcpListener,
    client: Arc<TorClient<tor_rtcompat::PreferredRuntime>>,
    mut shutdown: tokio::sync::watch::Receiver<bool>,
) {
    loop {
        tokio::select! {
            _ = shutdown.changed() => {
                if *shutdown.borrow() { return; }
            }
            accepted = listener.accept() => {
                let (sock, _peer) = match accepted {
                    Ok(v) => v,
                    Err(_) => continue,
                };
                let c = client.isolated_client();
                tokio::spawn(async move {
                    let _ = handle_conn(sock, c).await;
                });
            }
        }
    }
}

async fn handle_conn(
    mut sock: TcpStream,
    client: Arc<TorClient<tor_rtcompat::PreferredRuntime>>,
) -> std::io::Result<()> {
    // ---- greeting ----
    let mut head = [0u8; 2];
    sock.read_exact(&mut head).await?;
    if head[0] != 0x05 {
        return Ok(()); // not SOCKS5
    }
    let nmethods = head[1] as usize;
    let mut methods = vec![0u8; nmethods];
    sock.read_exact(&mut methods).await?;
    // 0x00 = no authentication
    sock.write_all(&[0x05, 0x00]).await?;

    // ---- request ----
    let mut req = [0u8; 4];
    sock.read_exact(&mut req).await?;
    if req[0] != 0x05 {
        return Ok(());
    }
    let cmd = req[1];
    let atyp = req[3];

    let host: String = match atyp {
        0x01 => {
            let mut a = [0u8; 4];
            sock.read_exact(&mut a).await?;
            std::net::Ipv4Addr::from(a).to_string()
        }
        0x03 => {
            let mut l = [0u8; 1];
            sock.read_exact(&mut l).await?;
            let mut name = vec![0u8; l[0] as usize];
            sock.read_exact(&mut name).await?;
            String::from_utf8_lossy(&name).to_string()
        }
        0x04 => {
            let mut a = [0u8; 16];
            sock.read_exact(&mut a).await?;
            std::net::Ipv6Addr::from(a).to_string()
        }
        _ => {
            reply(&mut sock, 0x08).await?; // address type not supported
            return Ok(());
        }
    };
    let mut p = [0u8; 2];
    sock.read_exact(&mut p).await?;
    let port = u16::from_be_bytes(p);

    if cmd != 0x01 {
        reply(&mut sock, 0x07).await?; // command not supported
        return Ok(());
    }

    // ---- open the Tor circuit ----
    let stream = match client.connect((host.as_str(), port)).await {
        Ok(s) => s,
        Err(_) => {
            reply(&mut sock, 0x05).await?; // connection refused
            return Ok(());
        }
    };
    reply(&mut sock, 0x00).await?;

    // ---- relay ----
    //
    // Each direction propagates its end-of-stream to the other side the
    // moment its copy finishes, by shutting down the write half it was
    // feeding. This matters for freeing the far end promptly: with the
    // previous bare `try_join!`, a client that closed (or destroyed) its
    // socket did not close the Tor stream until BOTH copies had ended,
    // so a seeder serving one transfer at a time saw its slot occupied
    // long after the app had abandoned the download — and the next
    // request to that seeder starved.
    let (mut tor_r, mut tor_w) = stream.split();
    let (mut cli_r, mut cli_w) = sock.split();
    let up = async {
        let r = tokio::io::copy(&mut cli_r, &mut tor_w).await;
        // Client is done sending (EOF or error): half-close the Tor
        // stream so the seeder sees the end immediately.
        let _ = tor_w.shutdown().await;
        r
    };
    let down = async {
        let r = tokio::io::copy(&mut tor_r, &mut cli_w).await;
        // Seeder is done sending: pass the FIN on to the client.
        let _ = cli_w.shutdown().await;
        r
    };
    // join (not try_join): one direction failing must not abandon the
    // other before its shutdown has been sent.
    let _ = tokio::join!(up, down);
    Ok(())
}

/// SOCKS5 reply with a zeroed BND.ADDR — clients that only CONNECT don't
/// use it, and inventing one would leak our local addressing.
async fn reply(sock: &mut TcpStream, code: u8) -> std::io::Result<()> {
    sock.write_all(&[0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0]).await
}
