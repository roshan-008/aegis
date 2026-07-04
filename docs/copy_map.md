# TCP copy map

Server side:

```
Rust-owned mmap --load/encode--> reusable 32 KiB frame batch
frame batch     --write_all-->  kernel socket buffer
```

Client side:

```
kernel socket buffer --copy #1 readv--> 64B-aligned C++ arena buffer
arena buffer         --copy #0 cast-->  fixed 32-byte WireTick views
WireTick view        --copy #2 push-->  24-byte Tick in bounded SPSC slot
```

The server issues at most one `write_all` per 1,024 ticks instead of the removed
C++ implementation's one `send` call per tick. `CopyStats` reports client-side
bytes and frames. A TCP-split trailing frame uses a fixed 32-byte carry buffer,
is counted separately, and does not allocate.

Wire fields are explicit little-endian. A versioned schema is still required
before heterogeneous protocol evolution.
