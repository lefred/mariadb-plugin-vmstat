# mariadb-plugin-vmstat

![mariabd-plugin-vmstat](logo/vmstat.png)

This plugin exposes Linux host statistics as three read-only
`INFORMATION_SCHEMA` tables:

- `VMSTAT_CPU` reads `/proc/stat`.
- `VMSTAT_MEM` reads `/proc/meminfo` and `/proc/vmstat`.
- `VMSTAT_DISK` reads `/proc/diskstats`.

Reading these tables requires the global `PROCESS` privilege. If `/proc` is
unavailable, a table returns no rows.

Build this directory as `plugin/vmstat` in a MariaDB Server source tree, then:

```sql
INSTALL SONAME 'vmstat';

SELECT * FROM information_schema.VMSTAT_CPU;
SELECT * FROM information_schema.VMSTAT_MEM;
SELECT * FROM information_schema.VMSTAT_DISK;
```

`READ_BYTES` and `WRITE_BYTES` use the Linux diskstats ABI definition of one
sector as 512 bytes. Counter values are snapshots taken when each query runs.

## `INFORMATION_SCHEMA.VMSTAT_CPU`

This table contains one aggregate row named `cpu`, followed by one row for
each logical CPU (`cpu0`, `cpu1`, and so on). Time counters come from
`/proc/stat` and are converted from kernel clock ticks to milliseconds.
They are cumulative since the system boot.

| Column | Description |
| --- | --- |
| `CPU` | CPU identifier. `cpu` is the aggregate for the whole system; `cpuN` identifies logical CPU number N. |
| `USER_MS` | Time in milliseconds spent running normal user-space processes. |
| `NICE_MS` | Time in milliseconds spent running user-space processes with an adjusted nice priority. |
| `SYSTEM_MS` | Time in milliseconds spent running in kernel space. |
| `IDLE_MS` | Time in milliseconds spent idle. |
| `IOWAIT_MS` | Time in milliseconds spent idle while waiting for I/O to complete. Linux notes that this value may not always be reliable. |
| `IRQ_MS` | Time in milliseconds spent servicing hardware interrupts. |
| `SOFTIRQ_MS` | Time in milliseconds spent servicing software interrupts. |
| `STEAL_MS` | Time in milliseconds taken by other virtual machines while running under a hypervisor. |
| `GUEST_MS` | Time in milliseconds spent running a virtual CPU for a guest operating system under the normal user priority. This time is already included in `USER_MS`. |
| `GUEST_NICE_MS` | Time in milliseconds spent running a niced virtual CPU for a guest operating system. This time is already included in `NICE_MS`. |
| `PROCS_RUNNING` | Number of processes currently runnable. It is populated only on the aggregate `cpu` row and is `NULL` on individual CPU rows or when unavailable. |
| `PROCS_BLOCKED` | Number of processes currently blocked while waiting for I/O. It is populated only on the aggregate `cpu` row and is `NULL` on individual CPU rows or when unavailable. |

## `INFORMATION_SCHEMA.VMSTAT_MEM`

This table contains one row describing the current system-wide memory state.
Memory quantities are reported in bytes. `SWAP_IN` and `SWAP_OUT` preserve
the column names from the original component, but contain the cumulative
Linux `pgpgin` and `pgpgout` counters rather than current swap-space sizes.

| Column | Description |
| --- | --- |
| `MEM_TOT` | Total usable physical memory, in bytes (`MemTotal` from `/proc/meminfo`). |
| `MEM_FREE` | Unused physical memory, in bytes (`MemFree`). |
| `MEM_USED` | Estimated used physical memory, in bytes. It is calculated as `MEM_TOT - MEM_FREE - BUFFERS - Cached - Slab` and is clamped to zero if the source counters are inconsistent. |
| `MEM_AVAILABLE` | Estimate of the memory available for starting new applications without swapping, in bytes (`MemAvailable`). |
| `TOT_SWAP` | Total configured swap space, in bytes (`SwapTotal`). |
| `SWAP_FREE` | Unused swap space, in bytes (`SwapFree`). |
| `SWAP_USED` | Used swap space, in bytes, calculated as `TOT_SWAP - SWAP_FREE` and clamped to zero. |
| `BUFFERS` | Physical memory used by kernel block-device buffers, in bytes (`Buffers`). |
| `CACHED` | Physical memory used by the page cache and kernel slab allocations, in bytes (`Cached + Slab`). |
| `SWAP_IN` | Cumulative `pgpgin` counter from `/proc/vmstat`: kilobytes paged in by the system since boot. It is `NULL` if `/proc/vmstat` cannot be read or the counter is absent. |
| `SWAP_OUT` | Cumulative `pgpgout` counter from `/proc/vmstat`: kilobytes paged out by the system since boot. It is `NULL` if `/proc/vmstat` cannot be read or the counter is absent. |

## `INFORMATION_SCHEMA.VMSTAT_DISK`

This table contains one row for each block device reported by
`/proc/diskstats`. The counters are cumulative since the device or system was
started. Linux reports sector counters in units of 512 bytes, independently
of a device's physical or logical sector size.

| Column | Description |
| --- | --- |
| `DISK` | Kernel block-device name, such as `sda`, `nvme0n1`, or `dm-0`. |
| `READS` | Number of read operations completed successfully. |
| `READ_BYTES` | Number of sectors read multiplied by 512, giving the cumulative bytes read. |
| `READ_TIME_MS` | Total time spent by read operations, in milliseconds. |
| `WRITES` | Number of write operations completed successfully. |
| `WRITE_BYTES` | Number of sectors written multiplied by 512, giving the cumulative bytes written. |
| `WRITE_TIME_MS` | Total time spent by write operations, in milliseconds. |
| `FLUSHES` | Number of completed cache-flush requests. It is zero on kernels or devices that do not expose flush statistics. |
| `FLUSH_TIME_MS` | Total time spent by cache-flush requests, in milliseconds. It is zero when flush statistics are unavailable. |
