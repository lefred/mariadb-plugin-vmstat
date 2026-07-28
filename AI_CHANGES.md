# AI Changes

## 2026-07-28 — claude-sonnet-5

Fixed `VMSTAT_MEM.CACHED` / `MEM_USED` overcounting reclaimable memory
(`vmstat.cc`, `memory_fill`).

The code parsed `/proc/meminfo`'s `Slab` value and treated all of it as
reclaimable cache. `Slab` is actually `SReclaimable + SUnreclaim`, and
`SUnreclaim` is kernel memory that cannot be freed on demand. Counting it
as reclaimable overstated `CACHED` and understated `MEM_USED` by the
`SUnreclaim` amount (hundreds of MB to GB on a busy server).

Change: parse `SReclaimable` from `/proc/meminfo` and use it instead of
`Slab` when computing the reclaimed-memory bucket, falling back to the
full `Slab` value only on kernels old enough to lack `SReclaimable`.
