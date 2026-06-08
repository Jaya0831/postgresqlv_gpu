# GPU Batch Query Mode

## Overview

GPU CUVS indexes (CAGRA, IVF-Flat via Knowhere) have high per-call overhead but scale efficiently with batch size. The single-query path — calling `knowhere::Search(GenDataSet(1, dim, query))` for every incoming request — leaves most of that throughput on the table.

This feature adds a **time-windowed, per-segment batch search path** for GPU indexes. Queries from different PostgreSQL backends accumulate for a 1 ms window, then a single `knowhere::Search(GenDataSet(nq, dim, packed_vectors))` call is issued per segment, amortizing the GPU launch overhead across all concurrent queries.

The CPU paths (HNSW, IVFFlat, DiskANN) are **entirely unaffected**. The feature is compiled in or out via the `USE_GPU_CUVS` preprocessor flag (1 = GPU batch path, 0 = original CPU path).

---

## Architecture

### Batching granularity

Batching is done at the **per-segment level**. For each flushed segment, all queries that need to search that segment are packed into a single `GenDataSet` call. This maps cleanly onto how Knowhere exposes batched search.

### Time window

The vector-index worker sleeps for **1 ms** (outside the ring buffer lock) at the top of its main loop before draining the ring. This gives concurrent backends a window to enqueue their queries. After the sleep, all tasks present in the ring are drained under a single `LWLockAcquire(LW_EXCLUSIVE)`, after which the lock is released and batch dispatch proceeds asynchronously.

### IPC flow

```
Backend A  ──┐
Backend B  ──┼──► ring buffer (shmem) ──► vector_index_worker drains after 1ms
Backend C  ──┘                                │
                                              │  batch_vector_search()
                                              │    groups by index_relid
                                              │    per-segment: pack query vectors
                                              │    BatchedConcurrentVectorSearchOnSegments()
                                              │      submits one Folly task per segment
                                              │        knowhere::Search(nq queries at once)
                                              │        distribute results back per-query
                                              │        when pending_segments reaches 0:
                                              │          finalize_batch_query()
                                              │            merge top-k across segments
                                              │            write shmem result slot
                                              └──────────► SetLatch(backend procLatch)
```

---

## Key Data Structures

### `CollectedVectorSearchTask` (`vector_index_worker.c`)

Copied from the ring buffer slot while the lock is held. Contains an embedded `float query_vector[MAX_DIM]` array so the vector data remains valid after the ring slot is recycled.

```c
typedef struct CollectedVectorSearchTask {
    Oid          index_relid;
    int          backend_pgprocno;
    int          topk;
    int          efs_nprobe;
    int          dim;
    LSMSnapshot  snapshot;
    float        query_vector[MAX_DIM];
} CollectedVectorSearchTask;
```

### `BatchQueryDescriptor` (`vectorindeximpl.hpp`)

One entry per active query passed into the C++ batch function. `query_vector` points into the `CollectedVectorSearchTask` embedded array and remains valid for the synchronous duration of `BatchedConcurrentVectorSearchOnSegments`.

### `SegmentQueryBatch` (`vectorindeximpl.hpp`)

One entry per segment that has at least one query. `query_indices` is a malloc'd array of indices into the `BatchQueryDescriptor[]` array; it is freed inside `BatchedConcurrentVectorSearchOnSegments` after the packed buffer is built.

### `BatchQueryState` (`vectorindeximpl.cpp`, internal)

Per-query state allocated by `BatchedConcurrentVectorSearchOnSegments`. Contains `std::atomic<int> pending_segments` (the countdown), a `seg_results[]` array (per-segment top-k scratch), and a pointer back to the query descriptor. The last segment task to finish for a given query calls `finalize_batch_query`.

---

## C++ Implementation (`vectorindeximpl.cpp`)

### `BatchedConcurrentVectorSearchOnSegments`

1. Counts how many segments each query touches (from `seg_batches`).
2. Allocates one `BatchQueryState` per query; initializes `pending_segments` to that count.
3. For each `SegmentQueryBatch`:
   - Skips empty batches (guards against `n_queries == 0`), decrementing the segment ref count and freeing `query_indices` before continuing.
   - Packs the `m` query vectors into a contiguous `float* packed_vectors` buffer.
   - Allocates a `SegBatchTaskCtx` and submits it as a Folly task via `GetPgOuterSearchExecutor()`.
4. Returns immediately; all result delivery is asynchronous via latches.

### `batch_search_segment_task` (Folly task)

- Builds a Knowhere JSON config (ITOPK_SIZE/SEARCH_ALGO for CAGRA-HNSW, NPROBE for IVF-Flat).
- Calls `knowhere::GenDataSet(nq, dim, packed_vectors)` then `index->Search(dataset, conf, bitset_view)`.
- Iterates over the flat `[nq × topk]` output array, mapping internal vector IDs through the segment's `map_ptr` and writing per-query per-segment results into `qs->seg_results[slot]`.
- Atomically decrements `pending_segments`; if the result is 0, calls `finalize_batch_query`.
- Decrements the segment ref count and frees the ctx.

### `finalize_batch_query`

- Merges all per-segment top-k result lists using `merge_top_k_cpp`.
- Writes the merged result into the query's shmem result slot (`result->result_count`, distances, IDs).
- Calls `SetLatch(&client->procLatch)` to wake the waiting backend.
- Frees per-segment scratch arrays and the `BatchQueryState`.

---

## C Implementation (`vector_index_worker.c`)

### Worker main loop (`vector_index_worker_main`)

```
#if USE_GPU_CUVS
    pg_usleep(1000);                 /* 1ms accumulation window */
    malloc gpu_tasks[ring_size];
    LWLockAcquire(LW_EXCLUSIVE);
    while ring has tasks:
        if VectorSearch: copy to gpu_tasks[]   (Assert bounds before write)
        else:            submit_maintenance_task immediately
        advance head, decrement count
    LWLockRelease();
    if n_gpu_tasks > 0: batch_vector_search(gpu_tasks, n_gpu_tasks)
    free(gpu_tasks)
#else
    /* original per-task drain loop — unchanged */
#endif
```

### `batch_vector_search`

1. Groups `CollectedVectorSearchTask[]` entries by `index_relid`.
2. For each index:
   - Snapshots the `FlushedSegmentPool` (one ref-count increment per segment).
   - Applies per-query `LSMSnapshot` filtering to determine which segments each query must search (same logic as the CPU `vector_search` path).
   - Handles **zero-segment queries** inline: sets `result_count = 0` and calls `SetLatch` immediately, to avoid passing them to the C++ layer and risking a double-SetLatch.
   - Builds `BatchQueryDescriptor[]` (active queries only) and `SegmentQueryBatch[]` (segments with ≥ 1 query, with remapped `query_indices`).
   - Calls `BatchedConcurrentVectorSearchOnSegments`.

---

## Correctness Notes

| Concern | Resolution |
|---------|-----------|
| Query vector lifetime | Vectors are copied into `CollectedVectorSearchTask.query_vector[MAX_DIM]` before the ring lock is released. |
| Double-SetLatch | Zero-segment queries are resolved in C before calling the C++ function; `finalize_batch_query` is only reached by queries with ≥ 1 segment. |
| Empty segment batch | `BatchedConcurrentVectorSearchOnSegments` skips `SegmentQueryBatch` entries with `n_queries == 0`, releasing the ref count and freeing `query_indices` before continuing. |
| Ring buffer overflow | `Assert(n_gpu_tasks < ring_buffer_shmem->ring_size)` guards the write into `gpu_tasks[]`. |
| Multiple indexes in one batch | `batch_vector_search` processes one `index_relid` group at a time, each with its own `FlushedSegmentPool` snapshot. |
| C89 compliance | All variable declarations in `batch_vector_search` and the worker drain block are placed before any statements, as required by the PostgreSQL build flags. |

---

## Compile-Time Control

| Macro | Value | Effect |
|-------|-------|--------|
| `USE_GPU_CUVS` | `1` (default) | GPU batch path active; CPU single-query helpers (`handle_task`, `vector_search`, `merge_top_k`) compiled out to avoid `-Wunused-function` warnings. |
| `USE_GPU_CUVS` | `0` | Original CPU single-query drain loop; all GPU batch code compiled out. |

Set in `pgvector/Makefile` under `PG_CPPFLAGS`, or override on the command line:

```bash
make USE_GPU_CUVS=0   # CPU-only build
```

---

## Files Changed

| File | Change |
|------|--------|
| `pgvector/src/vectorindeximpl.hpp` | Added `BatchQueryDescriptor`, `SegmentQueryBatch`, and `BatchedConcurrentVectorSearchOnSegments` declaration under `#if USE_GPU_CUVS`. |
| `pgvector/src/vectorindeximpl.cpp` | Added `BatchQueryState`, `SegBatchTaskCtx`, `finalize_batch_query`, `batch_search_segment_task`, and `BatchedConcurrentVectorSearchOnSegments` under `#if USE_GPU_CUVS`. |
| `pgvector/src/vector_index_worker.c` | Added `CollectedVectorSearchTask`, `batch_vector_search`, and the GPU drain block in `vector_index_worker_main` under `#if USE_GPU_CUVS`. Wrapped CPU-only helpers with `#if !USE_GPU_CUVS`. |
