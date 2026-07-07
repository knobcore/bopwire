//! C-ABI static library wrapping sacad's cover-art search+download for in-process
//! use by the bopwire node.
//!
//! One entry point fetches a JPEG for `(artist, album, size)`; the C++ caller owns
//! the returned buffer and releases it with [`sacad_free`]. The whole surface is
//! panic-safe (`catch_unwind` at the boundary) so a Rust panic can never unwind
//! into C++.

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, OnceLock};

use clap::Parser as _;
use sacad::cl::SacadArgs;
use sacad::{search_and_download, SearchStatus};

/// Shared multi-thread tokio runtime.
///
/// sacad uses `tokio::spawn` internally (one task per cover source), so it needs a
/// real runtime. The node scrapes serially (concurrency 1), so a single shared
/// runtime created once is enough and avoids per-call worker-thread churn.
fn runtime() -> &'static tokio::runtime::Runtime {
    static RT: OnceLock<tokio::runtime::Runtime> = OnceLock::new();
    RT.get_or_init(|| {
        tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .expect("failed to build tokio runtime")
    })
}

/// Monotonic counter for unique temp filenames.
static SEQ: AtomicU64 = AtomicU64::new(0);

/// Fetch a cover for `artist`/`album` at approximately `size` px (sacad resizes).
///
/// On success writes a heap-allocated JPEG buffer to `*out_ptr`/`*out_len` and
/// returns `0`; the caller owns the buffer and must free it with [`sacad_free`].
/// Returns `1` when no cover was found, `-3` on a transient source error (worth a
/// later retry), and other negatives on bad input / panic.
///
/// # Safety
/// `artist` and `album` must be valid NUL-terminated C strings; `out_ptr` and
/// `out_len` must be valid, writable pointers.
#[no_mangle]
pub extern "C" fn sacad_fetch_cover(
    artist: *const c_char,
    album: *const c_char,
    size: u32,
    out_ptr: *mut *mut u8,
    out_len: *mut usize,
) -> c_int {
    let result =
        std::panic::catch_unwind(|| unsafe { fetch_impl(artist, album, size, out_ptr, out_len) });
    result.unwrap_or(-2)
}

/// # Safety
/// See [`sacad_fetch_cover`].
unsafe fn fetch_impl(
    artist: *const c_char,
    album: *const c_char,
    size: u32,
    out_ptr: *mut *mut u8,
    out_len: *mut usize,
) -> c_int {
    if artist.is_null() || album.is_null() || out_ptr.is_null() || out_len.is_null() {
        return -1;
    }
    *out_ptr = std::ptr::null_mut();
    *out_len = 0;

    let artist = match CStr::from_ptr(artist).to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return -1,
    };
    let album = match CStr::from_ptr(album).to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => return -1,
    };
    if artist.is_empty() || album.is_empty() {
        return -1;
    }

    // sacad writes the chosen cover to a file path; give it a unique temp target.
    let seq = SEQ.fetch_add(1, Ordering::Relaxed);
    let tmp = std::env::temp_dir().join(format!("bopwire_art_{}_{seq}.jpg", std::process::id()));
    let tmp_s = tmp.to_string_lossy().into_owned();
    let size_s = size.to_string();

    // Reuse sacad's own CLI parsing (positional order: artist album size output).
    // "--" guards against an artist/album that begins with '-'.
    let argv = [
        "sacad",
        "--",
        artist.as_str(),
        album.as_str(),
        size_s.as_str(),
        tmp_s.as_str(),
    ];
    let cl_args = match SacadArgs::try_parse_from(argv) {
        Ok(a) => a,
        Err(_) => return -1,
    };

    let status = runtime().block_on(search_and_download(
        &cl_args.output_filepath,
        Arc::new(cl_args.query),
        Arc::new(cl_args.search_opts),
        &cl_args.image_proc,
    ));

    let found = matches!(status, Ok(SearchStatus::Found));
    if !found {
        let _ = std::fs::remove_file(&tmp);
        // Ok(NotFound) => 1 (real miss); Err => -3 (transient, retry later).
        return if status.is_ok() { 1 } else { -3 };
    }

    let bytes = match std::fs::read(&tmp) {
        Ok(b) => b,
        Err(_) => {
            let _ = std::fs::remove_file(&tmp);
            return -1;
        }
    };
    let _ = std::fs::remove_file(&tmp);
    if bytes.is_empty() {
        return 1;
    }

    let mut boxed = bytes.into_boxed_slice();
    *out_len = boxed.len();
    *out_ptr = boxed.as_mut_ptr();
    std::mem::forget(boxed);
    0
}

/// Free a buffer previously returned by [`sacad_fetch_cover`].
///
/// # Safety
/// `ptr`/`len` must be a buffer returned by [`sacad_fetch_cover`] (or `ptr` null),
/// freed at most once.
#[no_mangle]
pub extern "C" fn sacad_free(ptr: *mut u8, len: usize) {
    if ptr.is_null() || len == 0 {
        return;
    }
    // SAFETY: reconstruct the exact Box<[u8]> leaked in fetch_impl.
    unsafe {
        let slice = std::slice::from_raw_parts_mut(ptr, len);
        drop(Box::from_raw(slice as *mut [u8]));
    }
}
