# Aegis design

## Language boundary

Rust owns the failure-prone system boundary: paths, file creation, binary
format parsing, checked offset arithmetic, CRC validation, WAL recovery, mmap
lifetime, TCP listening, batched writes, and operational CLI commands. All
public Rust APIs are safe; the Unix mmap and five C ABI entry points isolate the
small audited `unsafe` surface. FFI functions catch panics and return errors.

C++ owns data-plane machinery: the 64-byte arena, non-owning spans, SPSC ring,
task graph, worker scheduling, SIMD reductions, sliding windows, and blocked
matmul. Rust opens one segment and gives C++ three validated pointers; kernel
loops do not cross the language boundary.

Go is intentionally absent. It would be a good fit for a future distributed
control plane or metrics API, but today it would duplicate the Rust server and
add a runtime/toolchain without adding capability.

## Layering

1. C++ `mem/`, `span.hpp`: hot-path ownership and shapes.
2. C++ `kernels/`: typed naive/best implementations and registry.
3. C++ `runtime/`: graph, optimizer, scheduler, streaming and observations.
4. Rust `aegis-rust`: segment/WAL/mmap and the replay feed service.
5. C++ `storage/reader.hpp`: RAII FFI facade exposing `ConstColView`.
6. C++ `net/feed.hpp`: `readv` decode into the bounded ring.

## Storage contract

AEGISSEG/v1 is 192 bytes of explicitly encoded little-endian metadata followed
by three raw-double columns. Rust never transmutes the header into a struct. It
checks magic/version, integer conversions, checked offset arithmetic, 8-byte
alignment, payload bounds, header/column CRCs, and finite sorted timestamps
before exposing a pointer.

Seal order is WAL intent → `.partial` write → file fsync → atomic rename →
directory fsync → WAL commit. Startup removes uncommitted partial tails. The
mmap owner stays in Rust until the C++ reader destructor closes its opaque
handle.

## Performance boundary

The FFI cost is per segment open/close and seal, never per row. Replay kernels
read page-cache-backed doubles directly. The Rust server builds 1,024 fixed
32-byte frames and uses a 32 KiB `write_all`; the removed C++ server called
`send` once per tick. The receive copy map remains socket → arena → ring.

Every optimized numerical kernel retains its C++ naive oracle. Softmax uses max
subtraction, layernorm uses centered variance, and sliding variance uses a
shifted accumulator with periodic resynchronization.
