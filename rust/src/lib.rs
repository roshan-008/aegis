#![deny(unsafe_op_in_unsafe_fn)]

#[cfg(not(all(unix, target_pointer_width = "64")))]
compile_error!("aegis-rust currently supports 64-bit Unix targets");

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, OsStr, OsString};
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::net::TcpListener;
use std::os::fd::AsRawFd;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::path::{Path, PathBuf};
use std::ptr::NonNull;
use std::slice;
use std::time::{Duration, Instant};

pub const HEADER_BYTES: usize = 192;
const MAGIC: &[u8; 8] = b"AEGISSEG";
const WAL_MAGIC: &[u8; 4] = b"AGWL";

#[derive(Debug)]
pub struct Error(pub String);

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for Error {}

type Result<T> = std::result::Result<T, Error>;

fn err(message: impl Into<String>) -> Error {
    Error(message.into())
}

// Slice-by-8 CRC-32 (IEEE, reflected, polynomial 0xEDB88320) — the technique
// zlib and Intel's CRC paper use. Eight 256-entry tables are built at compile
// time from the same bit-by-bit recurrence, so output is byte-for-byte identical
// to the naive loop (every previously sealed segment's stored CRC stays valid),
// but the inner loop consumes 8 bytes per iteration instead of 1. `open`
// CRC-checks the whole segment (see bench_storage), so this is the hot path.
const fn crc32_tables() -> [[u32; 256]; 8] {
    let mut t = [[0u32; 256]; 8];
    // Table 0: the classic byte-wise table.
    let mut i = 0usize;
    while i < 256 {
        let mut crc = i as u32;
        let mut bit = 0;
        while bit < 8 {
            crc = (crc >> 1) ^ (0xedb8_8320u32 & 0u32.wrapping_sub(crc & 1));
            bit += 1;
        }
        t[0][i] = crc;
        i += 1;
    }
    // Tables 1..8: each folds one more byte of look-ahead.
    let mut k = 1usize;
    while k < 8 {
        let mut j = 0usize;
        while j < 256 {
            let prev = t[k - 1][j];
            t[k][j] = (prev >> 8) ^ t[0][(prev & 0xff) as usize];
            j += 1;
        }
        k += 1;
    }
    t
}

static CRC32_TABLES: [[u32; 256]; 8] = crc32_tables();

fn crc32(data: &[u8]) -> u32 {
    let mut crc = 0xffff_ffffu32;
    let mut chunks = data.chunks_exact(8);
    for c in &mut chunks {
        crc ^= u32::from_le_bytes([c[0], c[1], c[2], c[3]]);
        crc = CRC32_TABLES[7][(crc & 0xff) as usize]
            ^ CRC32_TABLES[6][((crc >> 8) & 0xff) as usize]
            ^ CRC32_TABLES[5][((crc >> 16) & 0xff) as usize]
            ^ CRC32_TABLES[4][((crc >> 24) & 0xff) as usize]
            ^ CRC32_TABLES[3][c[4] as usize]
            ^ CRC32_TABLES[2][c[5] as usize]
            ^ CRC32_TABLES[1][c[6] as usize]
            ^ CRC32_TABLES[0][c[7] as usize];
    }
    for &byte in chunks.remainder() {
        crc = (crc >> 8) ^ CRC32_TABLES[0][((crc ^ byte as u32) & 0xff) as usize];
    }
    !crc
}

fn read_u32(bytes: &[u8], offset: usize) -> Result<u32> {
    let value: [u8; 4] = bytes
        .get(offset..offset + 4)
        .ok_or_else(|| err("truncated u32"))?
        .try_into()
        .map_err(|_| err("invalid u32"))?;
    Ok(u32::from_le_bytes(value))
}

fn read_u64(bytes: &[u8], offset: usize) -> Result<u64> {
    let value: [u8; 8] = bytes
        .get(offset..offset + 8)
        .ok_or_else(|| err("truncated u64"))?
        .try_into()
        .map_err(|_| err("invalid u64"))?;
    Ok(u64::from_le_bytes(value))
}

fn read_f64(bytes: &[u8], offset: usize) -> Result<f64> {
    Ok(f64::from_bits(read_u64(bytes, offset)?))
}

fn put_u32(bytes: &mut [u8], offset: usize, value: u32) {
    bytes[offset..offset + 4].copy_from_slice(&value.to_le_bytes());
}

fn put_u64(bytes: &mut [u8], offset: usize, value: u64) {
    bytes[offset..offset + 8].copy_from_slice(&value.to_le_bytes());
}

fn put_f64(bytes: &mut [u8], offset: usize, value: f64) {
    put_u64(bytes, offset, value.to_bits());
}

extern "C" {
    fn mmap(
        addr: *mut c_void,
        len: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
    fn madvise(addr: *mut c_void, len: usize, advice: c_int) -> c_int;
}

struct Mmap {
    ptr: NonNull<u8>,
    len: usize,
}

impl Mmap {
    fn open(path: &Path) -> Result<Self> {
        let file = File::open(path).map_err(|e| err(format!("open {}: {e}", path.display())))?;
        let len = usize::try_from(
            file.metadata()
                .map_err(|e| err(format!("stat {}: {e}", path.display())))?
                .len(),
        )
        .map_err(|_| err("segment too large for address space"))?;
        if len < HEADER_BYTES {
            return Err(err("truncated segment header"));
        }
        // SAFETY: fd is open for the call, len is non-zero, and the returned
        // mapping is retained by Mmap until munmap in Drop.
        let raw = unsafe { mmap(std::ptr::null_mut(), len, 1, 2, file.as_raw_fd(), 0) };
        if raw as isize == -1 {
            return Err(err(format!("mmap {} failed", path.display())));
        }
        let ptr = NonNull::new(raw.cast::<u8>()).ok_or_else(|| err("mmap returned null"))?;
        // MADV_SEQUENTIAL is 2 on Darwin and Linux. A failed hint is harmless.
        unsafe { madvise(ptr.as_ptr().cast(), len, 2) };
        Ok(Self { ptr, len })
    }

    fn bytes(&self) -> &[u8] {
        // SAFETY: the mapping is readable for len bytes and lives as long as self.
        unsafe { slice::from_raw_parts(self.ptr.as_ptr(), self.len) }
    }
}

impl Drop for Mmap {
    fn drop(&mut self) {
        // SAFETY: ptr/len are exactly the successful mmap pair owned by self.
        unsafe { munmap(self.ptr.as_ptr().cast(), self.len) };
    }
}

#[derive(Clone, Copy, Debug)]
pub struct SegmentMeta {
    pub n: usize,
    pub offsets: [usize; 3],
    pub minimum: [f64; 3],
    pub maximum: [f64; 3],
}

pub struct Segment {
    map: Mmap,
    meta: SegmentMeta,
}

impl Segment {
    pub fn open(path: &Path) -> Result<Self> {
        if cfg!(target_endian = "big") {
            return Err(err("AEGISSEG v1 requires a little-endian target"));
        }
        let map = Mmap::open(path)?;
        // Mmap::open already rejects files shorter than HEADER_BYTES, so the
        // fixed-offset header slices below cannot go out of bounds; every other
        // read uses the bounds-checked read_*/get helpers. The fuzz test
        // (open_never_panics_on_malformed_input) is what proves this holds.
        let bytes = map.bytes();
        if &bytes[0..8] != MAGIC || read_u32(bytes, 8)? != 1 || read_u32(bytes, 12)? != 3 {
            return Err(err("bad segment magic/version"));
        }
        let mut header = bytes[0..HEADER_BYTES].to_vec();
        let expected_header_crc = read_u32(&header, 108)?;
        put_u32(&mut header, 108, 0);
        if crc32(&header) != expected_header_crc {
            return Err(err("segment header CRC mismatch"));
        }
        let n = usize::try_from(read_u64(bytes, 16)?).map_err(|_| err("row count overflow"))?;
        let payload = n
            .checked_mul(std::mem::size_of::<f64>())
            .ok_or_else(|| err("segment payload overflow"))?;
        let mut offsets = [0usize; 3];
        let mut minimum = [0.0; 3];
        let mut maximum = [0.0; 3];
        for column in 0..3 {
            offsets[column] = usize::try_from(read_u64(bytes, 24 + column * 8)?)
                .map_err(|_| err("column offset overflow"))?;
            minimum[column] = read_f64(bytes, 48 + column * 8)?;
            maximum[column] = read_f64(bytes, 72 + column * 8)?;
            if offsets[column] % std::mem::align_of::<f64>() != 0 {
                return Err(err("unaligned segment column"));
            }
            let end = offsets[column]
                .checked_add(payload)
                .ok_or_else(|| err("column bounds overflow"))?;
            let column_bytes = bytes
                .get(offsets[column]..end)
                .ok_or_else(|| err("segment column outside file"))?;
            if crc32(column_bytes) != read_u32(bytes, 96 + column * 4)? {
                return Err(err(format!("segment column {column} CRC mismatch")));
            }
        }
        let segment = Self {
            map,
            meta: SegmentMeta {
                n,
                offsets,
                minimum,
                maximum,
            },
        };
        if segment.column(2).iter().any(|value| !value.is_finite())
            || segment.column(2).windows(2).any(|pair| pair[0] > pair[1])
        {
            return Err(err("segment timestamps must be finite and sorted"));
        }
        Ok(segment)
    }

    pub fn meta(&self) -> SegmentMeta {
        self.meta
    }

    pub fn column(&self, column: usize) -> &[f64] {
        assert!(column < 3);
        let ptr = self.map.bytes()[self.meta.offsets[column]..]
            .as_ptr()
            .cast::<f64>();
        // SAFETY: open checked alignment, bounds, and n; mapping remains alive.
        unsafe { slice::from_raw_parts(ptr, self.meta.n) }
    }
}

fn doubles_as_bytes(values: &[f64]) -> &[u8] {
    // SAFETY: a byte view has alignment 1 and spans exactly the initialized slice.
    unsafe { slice::from_raw_parts(values.as_ptr().cast::<u8>(), std::mem::size_of_val(values)) }
}

fn append_wal(wal: &Path, state: u8, path: &Path) -> Result<()> {
    let path_bytes = path.as_os_str().as_bytes();
    let length = u32::try_from(path_bytes.len()).map_err(|_| err("WAL path too long"))?;
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(wal)
        .map_err(|e| err(format!("open WAL: {e}")))?;
    file.write_all(WAL_MAGIC)
        .and_then(|_| file.write_all(&[state]))
        .and_then(|_| file.write_all(&length.to_le_bytes()))
        .and_then(|_| file.write_all(path_bytes))
        .and_then(|_| file.sync_all())
        .map_err(|e| err(format!("write WAL: {e}")))
}

fn partial_path(path: &Path) -> PathBuf {
    let mut value = path.as_os_str().to_os_string();
    value.push(".partial");
    PathBuf::from(value)
}

fn write_segment(path: &Path, price: &[f64], volume: &[f64], timestamps: &[f64]) -> Result<()> {
    if price.len() != volume.len() || price.len() != timestamps.len() {
        return Err(err("segment columns have different lengths"));
    }
    if timestamps.iter().any(|value| !value.is_finite())
        || timestamps.windows(2).any(|pair| pair[0] > pair[1])
    {
        return Err(err("segment timestamps must be finite and sorted"));
    }
    let n = price.len();
    let payload = n.checked_mul(8).ok_or_else(|| err("segment too large"))?;
    let second = HEADER_BYTES
        .checked_add(payload)
        .ok_or_else(|| err("segment offset overflow"))?;
    let third = second
        .checked_add(payload)
        .ok_or_else(|| err("segment offset overflow"))?;
    let _end = third
        .checked_add(payload)
        .ok_or_else(|| err("segment offset overflow"))?;
    let offsets = [HEADER_BYTES, second, third];
    let columns = [price, volume, timestamps];
    let mut header = vec![0u8; HEADER_BYTES];
    header[0..8].copy_from_slice(MAGIC);
    put_u32(&mut header, 8, 1);
    put_u32(&mut header, 12, 3);
    put_u64(&mut header, 16, n as u64);
    for column in 0..3 {
        put_u64(&mut header, 24 + column * 8, offsets[column] as u64);
        let (minimum, maximum) = if columns[column].is_empty() {
            (f64::NAN, f64::NAN)
        } else {
            columns[column]
                .iter()
                .copied()
                .fold((f64::INFINITY, f64::NEG_INFINITY), |(mn, mx), value| {
                    (mn.min(value), mx.max(value))
                })
        };
        put_f64(&mut header, 48 + column * 8, minimum);
        put_f64(&mut header, 72 + column * 8, maximum);
        put_u32(
            &mut header,
            96 + column * 4,
            crc32(doubles_as_bytes(columns[column])),
        );
    }
    put_u32(&mut header, 108, 0);
    let header_crc = crc32(&header);
    put_u32(&mut header, 108, header_crc);

    let partial = partial_path(path);
    let mut file =
        File::create(&partial).map_err(|e| err(format!("create {}: {e}", partial.display())))?;
    file.write_all(&header)
        .and_then(|_| file.write_all(doubles_as_bytes(price)))
        .and_then(|_| file.write_all(doubles_as_bytes(volume)))
        .and_then(|_| file.write_all(doubles_as_bytes(timestamps)))
        .and_then(|_| file.sync_all())
        .map_err(|e| err(format!("seal segment: {e}")))?;
    drop(file);
    fs::rename(&partial, path).map_err(|e| err(format!("atomic segment rename: {e}")))?;
    if let Some(parent) = path.parent() {
        if let Ok(directory) = File::open(parent) {
            let _ = directory.sync_all();
        }
    }
    Ok(())
}

pub fn store_append(path: &Path, price: &[f64], volume: &[f64], timestamps: &[f64]) -> Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| err("segment path has no parent"))?;
    fs::create_dir_all(parent).map_err(|e| err(format!("create store: {e}")))?;
    let wal = parent.join("seal-rs.wal");
    append_wal(&wal, 1, path)?;
    write_segment(path, price, volume, timestamps)?;
    append_wal(&wal, 2, path)
}

pub fn recover_store(directory: &Path) -> Result<usize> {
    fs::create_dir_all(directory).map_err(|e| err(format!("create store: {e}")))?;
    let wal = directory.join("seal-rs.wal");
    let mut bytes = Vec::new();
    match File::open(wal) {
        Ok(mut file) => file
            .read_to_end(&mut bytes)
            .map_err(|e| err(format!("read WAL: {e}")))?,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(0),
        Err(e) => return Err(err(format!("open WAL: {e}"))),
    };
    let mut states: HashMap<Vec<u8>, u8> = HashMap::new();
    let mut cursor = 0usize;
    while cursor + 9 <= bytes.len() {
        if &bytes[cursor..cursor + 4] != WAL_MAGIC {
            break;
        }
        let state = bytes[cursor + 4];
        let length = u32::from_le_bytes(bytes[cursor + 5..cursor + 9].try_into().unwrap()) as usize;
        cursor += 9;
        let Some(path) = bytes.get(cursor..cursor.saturating_add(length)) else {
            break;
        };
        states.insert(path.to_vec(), state);
        cursor += length;
    }
    let mut removed = 0;
    for (path, state) in states {
        if state == 1 {
            let path = PathBuf::from(OsString::from_vec(path));
            match fs::remove_file(partial_path(&path)) {
                Ok(()) => removed += 1,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => {}
                Err(e) => return Err(err(format!("remove torn tail: {e}"))),
            }
        }
    }
    Ok(removed)
}

pub fn list_segments(directory: &Path) -> Result<Vec<PathBuf>> {
    let mut paths = Vec::new();
    for entry in fs::read_dir(directory).map_err(|e| err(format!("read store: {e}")))? {
        let path = entry
            .map_err(|e| err(format!("read store entry: {e}")))?
            .path();
        if path.extension() == Some(OsStr::new("aegis")) {
            paths.push(path);
        }
    }
    paths.sort();
    Ok(paths)
}

pub fn serve_store(directory: &Path, port: u16, rate: f64) -> Result<u64> {
    if rate.is_sign_negative() || !rate.is_finite() {
        return Err(err("ticks_per_sec must be finite and non-negative"));
    }
    let listener = TcpListener::bind(("127.0.0.1", port))
        .map_err(|e| err(format!("bind feed server: {e}")))?;
    let (mut stream, _) = listener
        .accept()
        .map_err(|e| err(format!("accept feed client: {e}")))?;
    stream
        .set_nodelay(true)
        .map_err(|e| err(format!("set TCP_NODELAY: {e}")))?;
    let started = Instant::now();
    let mut sequence = 0u64;
    const BATCH_BYTES: usize = 32 * 1024;
    let mut frames = Vec::with_capacity(BATCH_BYTES);
    for path in list_segments(directory)? {
        let segment = Segment::open(&path)?;
        let price = segment.column(0);
        let volume = segment.column(1);
        let timestamps = segment.column(2);
        for index in 0..segment.meta().n {
            frames.extend_from_slice(&(timestamps[index] as u64).to_le_bytes());
            frames.extend_from_slice(&price[index].to_le_bytes());
            frames.extend_from_slice(&volume[index].to_le_bytes());
            frames.extend_from_slice(&sequence.to_le_bytes());
            sequence += 1;
            if frames.len() >= BATCH_BYTES {
                stream
                    .write_all(&frames)
                    .map_err(|e| err(format!("write feed: {e}")))?;
                frames.clear();
            }
            if rate > 0.0 {
                let due = Duration::from_secs_f64(sequence as f64 / rate);
                if let Some(wait) = due.checked_sub(started.elapsed()) {
                    std::thread::sleep(wait);
                }
            }
        }
    }
    if !frames.is_empty() {
        stream
            .write_all(&frames)
            .map_err(|e| err(format!("write feed tail: {e}")))?;
    }
    stream
        .flush()
        .map_err(|e| err(format!("flush feed: {e}")))?;
    Ok(sequence)
}

#[repr(C)]
pub struct AegisSegmentMeta {
    pub n: u64,
    pub minimum: [f64; 3],
    pub maximum: [f64; 3],
}

fn write_error(buffer: *mut c_char, capacity: usize, message: &str) {
    if buffer.is_null() || capacity == 0 {
        return;
    }
    let bytes = message.as_bytes();
    let count = bytes.len().min(capacity - 1);
    // SAFETY: caller provides capacity writable bytes; count is bounded by it.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), count);
        *buffer.add(count) = 0;
    }
}

unsafe fn ffi_path<'a>(path: *const c_char) -> Result<&'a Path> {
    if path.is_null() {
        return Err(err("null path"));
    }
    // SAFETY: caller promises a live NUL-terminated path for this call.
    let bytes = unsafe { CStr::from_ptr(path) }.to_bytes();
    Ok(Path::new(OsStr::from_bytes(bytes)))
}

#[no_mangle]
/// Opens and validates a segment, returning an owning opaque handle.
///
/// # Safety
/// `path` must be NUL-terminated, `meta` must be writable, and the optional
/// error buffer must contain `error_capacity` writable bytes.
pub unsafe extern "C" fn aegis_rs_segment_open(
    path: *const c_char,
    meta: *mut AegisSegmentMeta,
    error: *mut c_char,
    error_capacity: usize,
) -> *mut Segment {
    let outcome = std::panic::catch_unwind(|| -> Result<*mut Segment> {
        // SAFETY: validated within ffi_path.
        let segment = Box::new(Segment::open(unsafe { ffi_path(path)? })?);
        if meta.is_null() {
            return Err(err("null segment metadata output"));
        }
        let m = segment.meta();
        // SAFETY: caller supplies a writable metadata object.
        unsafe {
            *meta = AegisSegmentMeta {
                n: m.n as u64,
                minimum: m.minimum,
                maximum: m.maximum,
            };
        }
        Ok(Box::into_raw(segment))
    });
    match outcome {
        Ok(Ok(handle)) => handle,
        Ok(Err(e)) => {
            write_error(error, error_capacity, &e.0);
            std::ptr::null_mut()
        }
        Err(_) => {
            write_error(error, error_capacity, "panic in Rust segment_open");
            std::ptr::null_mut()
        }
    }
}

#[no_mangle]
/// Returns a column pointer whose lifetime is tied to `handle`.
///
/// # Safety
/// `handle` must be a live value returned by `aegis_rs_segment_open` and must
/// not be closed while the returned pointer is used.
pub unsafe extern "C" fn aegis_rs_segment_column(
    handle: *const Segment,
    column: usize,
) -> *const f64 {
    if handle.is_null() || column >= 3 {
        return std::ptr::null();
    }
    // SAFETY: handle was created by segment_open and remains owned by caller.
    unsafe { (*handle).column(column).as_ptr() }
}

#[no_mangle]
/// Releases a segment handle and its mmap.
///
/// # Safety
/// `handle` must be null or a live handle returned by `aegis_rs_segment_open`,
/// and each non-null handle must be passed exactly once.
pub unsafe extern "C" fn aegis_rs_segment_close(handle: *mut Segment) {
    if !handle.is_null() {
        // SAFETY: ownership is returned exactly once by the C++ RAII wrapper.
        unsafe { drop(Box::from_raw(handle)) };
    }
}

#[no_mangle]
/// Atomically seals three columns into a segment and commits its WAL record.
///
/// # Safety
/// `path` must be NUL-terminated; for `n > 0`, each column pointer must address
/// `n` initialized doubles; the optional error buffer must be writable.
pub unsafe extern "C" fn aegis_rs_store_append(
    path: *const c_char,
    price: *const f64,
    volume: *const f64,
    timestamps: *const f64,
    n: usize,
    error: *mut c_char,
    error_capacity: usize,
) -> c_int {
    let outcome = std::panic::catch_unwind(|| -> Result<()> {
        if n > 0 && (price.is_null() || volume.is_null() || timestamps.is_null()) {
            return Err(err("null segment column"));
        }
        // SAFETY: caller guarantees each input points to n initialized doubles.
        let (price, volume, timestamps) = if n == 0 {
            (&[][..], &[][..], &[][..])
        } else {
            // SAFETY: non-null was checked and caller guarantees n initialized doubles.
            unsafe {
                (
                    slice::from_raw_parts(price, n),
                    slice::from_raw_parts(volume, n),
                    slice::from_raw_parts(timestamps, n),
                )
            }
        };
        // SAFETY: validated within ffi_path.
        store_append(unsafe { ffi_path(path)? }, price, volume, timestamps)
    });
    match outcome {
        Ok(Ok(())) => 0,
        Ok(Err(e)) => {
            write_error(error, error_capacity, &e.0);
            -1
        }
        Err(_) => {
            write_error(error, error_capacity, "panic in Rust store_append");
            -2
        }
    }
}

#[no_mangle]
/// Recovers uncommitted partial segment tails in a store.
///
/// # Safety
/// `directory` must be NUL-terminated; `removed`, when non-null, must be
/// writable; the optional error buffer must contain the declared capacity.
pub unsafe extern "C" fn aegis_rs_store_recover(
    directory: *const c_char,
    removed: *mut usize,
    error: *mut c_char,
    error_capacity: usize,
) -> c_int {
    let outcome = std::panic::catch_unwind(|| -> Result<usize> {
        // SAFETY: validated within ffi_path.
        recover_store(unsafe { ffi_path(directory)? })
    });
    match outcome {
        Ok(Ok(count)) => {
            if !removed.is_null() {
                // SAFETY: caller supplies a writable size_t.
                unsafe { *removed = count };
            }
            0
        }
        Ok(Err(e)) => {
            write_error(error, error_capacity, &e.0);
            -1
        }
        Err(_) => {
            write_error(error, error_capacity, "panic in Rust store_recover");
            -2
        }
    }
}

#[no_mangle]
/// Serves one replay client and returns after all segment frames are written.
///
/// # Safety
/// `directory` must be NUL-terminated; `sent`, when non-null, must be writable;
/// the optional error buffer must contain the declared capacity.
pub unsafe extern "C" fn aegis_rs_serve_once(
    directory: *const c_char,
    port: u16,
    rate: f64,
    sent: *mut u64,
    error: *mut c_char,
    error_capacity: usize,
) -> c_int {
    let outcome = std::panic::catch_unwind(|| -> Result<u64> {
        // SAFETY: validated within ffi_path.
        serve_store(unsafe { ffi_path(directory)? }, port, rate)
    });
    match outcome {
        Ok(Ok(count)) => {
            if !sent.is_null() {
                // SAFETY: caller supplies a writable u64.
                unsafe { *sent = count };
            }
            0
        }
        Ok(Err(e)) => {
            write_error(error, error_capacity, &e.0);
            -1
        }
        Err(_) => {
            write_error(error, error_capacity, "panic in Rust feed server");
            -2
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn test_dir() -> PathBuf {
        // Tests run in parallel threads in one process; the OS clock is coarser
        // than a nanosecond, so a timestamp alone lets two threads collide on
        // the same dir (shared seal-rs.wal -> flaky recovery counts). A process-
        // wide atomic counter makes every dir unique regardless of clock ticks.
        use std::sync::atomic::{AtomicU64, Ordering};
        static SEQ: AtomicU64 = AtomicU64::new(0);
        std::env::temp_dir().join(format!(
            "aegis-rust-test-{}-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos(),
            SEQ.fetch_add(1, Ordering::Relaxed)
        ))
    }

    #[test]
    fn segment_roundtrip_and_recovery() {
        let dir = test_dir();
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("one.aegis");
        let price = [100.0, 101.0, 102.0];
        let volume = [4.0, 5.0, 6.0];
        let ts = [10.0, 20.0, 30.0];
        store_append(&path, &price, &volume, &ts).unwrap();
        let segment = Segment::open(&path).unwrap();
        assert_eq!(segment.column(0), price);
        assert_eq!(segment.meta().minimum[2], 10.0);

        let torn = dir.join("torn.aegis");
        append_wal(&dir.join("seal-rs.wal"), 1, &torn).unwrap();
        File::create(partial_path(&torn)).unwrap();
        assert_eq!(recover_store(&dir).unwrap(), 1);
        assert!(!partial_path(&torn).exists());
        fs::remove_dir_all(dir).unwrap();
    }

    // Deterministic xorshift so the fuzz corpus is reproducible without a crate.
    fn xorshift(state: &mut u64) -> u64 {
        let mut x = *state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *state = x;
        x
    }

    #[test]
    fn crc32_matches_bitwise_reference() {
        // The slice-by-8 crc32 must equal the naive bit-by-bit definition for
        // every length (including the <8-byte remainder path) and content. This
        // is what lets us claim "byte-identical, all sealed CRCs stay valid".
        fn reference(data: &[u8]) -> u32 {
            let mut crc = 0xffff_ffffu32;
            for &byte in data {
                crc ^= byte as u32;
                for _ in 0..8 {
                    crc = (crc >> 1) ^ (0xedb8_8320u32 & 0u32.wrapping_sub(crc & 1));
                }
            }
            !crc
        }
        let mut state = 0x1234_5678_9abc_def0u64;
        for len in 0..64usize {
            let bytes: Vec<u8> = (0..len)
                .map(|_| (xorshift(&mut state) & 0xff) as u8)
                .collect();
            assert_eq!(crc32(&bytes), reference(&bytes), "len {len}");
        }
    }

    #[test]
    fn open_never_panics_on_malformed_input() {
        // The trust boundary: Segment::open takes bytes from disk/network. It
        // must return Err on garbage, never panic. Fuzz it with random-length,
        // random-byte files plus adversarial cases: empty, truncated header, a
        // valid segment with one flipped byte. A panic here fails the test.
        let dir = test_dir();
        fs::create_dir_all(&dir).unwrap();
        let good = dir.join("good.aegis");
        store_append(&good, &[1.0, 2.0, 3.0], &[4.0, 5.0, 6.0], &[7.0, 8.0, 9.0]).unwrap();
        let template = fs::read(&good).unwrap();

        let mut state = 0x9e37_79b9_7f4a_7c15u64;
        for i in 0..4000 {
            let path = dir.join(format!("fuzz-{i}.bin"));
            let bytes: Vec<u8> = if i % 4 == 0 {
                // Corrupt a real segment: flip one random byte.
                let mut b = template.clone();
                if !b.is_empty() {
                    let idx = (xorshift(&mut state) as usize) % b.len();
                    b[idx] ^= (xorshift(&mut state) & 0xff) as u8;
                }
                b
            } else {
                // Fully random buffer, length 0..=200 (spans below/around header).
                let len = (xorshift(&mut state) % 201) as usize;
                (0..len)
                    .map(|_| (xorshift(&mut state) & 0xff) as u8)
                    .collect()
            };
            fs::write(&path, &bytes).unwrap();
            // The contract: never panic. Ok or Err are both acceptable outcomes.
            let _ = Segment::open(&path);
        }
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn rejects_corrupt_payload() {
        let dir = test_dir();
        fs::create_dir_all(&dir).unwrap();
        let path = dir.join("bad.aegis");
        store_append(&path, &[1.0], &[2.0], &[3.0]).unwrap();
        let mut file = OpenOptions::new().write(true).open(&path).unwrap();
        use std::io::{Seek, SeekFrom};
        file.seek(SeekFrom::Start(HEADER_BYTES as u64)).unwrap();
        file.write_all(&[0xff]).unwrap();
        file.sync_all().unwrap();
        assert!(Segment::open(&path).is_err());
        fs::remove_dir_all(dir).unwrap();
    }
}
