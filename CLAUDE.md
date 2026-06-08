# Guidance for working in `decoupled_pgvector`

This file orients AI assistants and humans to this repository. The **real work** lives under `pgvector/`; the repo root holds tooling and SQL helpers.

## Project overview

This tree is a **research / experimental fork of [pgvector](https://github.com/pgvector/pgvector)** (vector similarity search as a PostgreSQL extension). On top of stock pgvector features (HNSW, IVFFlat, vector types, distance ops), this fork adds a **decoupled / LSM-style indexing path**: in-memory **memtables**, on-disk **segments**, background **flush / merge / indexing workers**, and integration with **Knowhere** (Faiss-based approximate search) for disk-oriented indexing (`DISKANN` vs in-memory `HNSW` is controlled in code; see `IS_DISK_BASED` in `pgvector/src/lsmindex.h`).

Treat behavior and APIs as **evolving**; prefer reading the headers (`lsmindex.h`, `lsm_segment.h`, `vectorindeximpl.hpp`) and call sites over assuming upstream pgvector docs alone.

## Tech stack

| Layer | Notes |
|--------|--------|
| **PostgreSQL** | Extension built with **PGXS** (`pg_config` + `$(PGXS)` include from Makefile). Targets a **local dev build** of Postgres (see Makefile `PG_CONFIG`). |
| **C** | Core extension: access methods, storage, memtables, workers, IPC-ish pieces (`ringbuffer`, `tasksend`, `statuspage`), and modified HNSW/IVF paths. |
| **C++17** | `vectorindeximpl.cpp` / `.hpp`, `folly_f14_link_alias.cpp` — bridge to Knowhere and related libs. |
| **Knowhere + Faiss** | Linked as static/shared libs from a **local Knowhere build** (paths in Makefile). |
| **Folly, glog, gflags** | From **Conan**-style install paths in Makefile (also machine-specific). |
| **nlohmann/json** | Header-only; include path from Conan package in Makefile. |
| **OpenMP, BLAS** | Used for numeric / parallel work in the C++ link line. |
| **Tests** | `pg_regress` SQL tests, Perl `prove` tests, **Python** scripts (concurrency, benchmarks), **pgbench** SQL + shell drivers, optional **C++** tests under `test/knowhere_tests/` and `test/runbooks/`. |

## Development commands

From **`pgvector/`** (extension root):

```bash
# Build (uses PG_CONFIG set in Makefile)
make

# Install into the Postgres tree pg_config points at (often needs appropriate permissions)
make install
```

**Local install script** (copies `vector.so`, `vector.control`, and `sql/vector--*.sql` using `pg_config` paths):

```bash
./install_pgvector.sh
```

The script expects `pg_config` at `$HOME/postgresql/pg_build/bin/pg_config` unless you edit it.

**SQL regression / Perl tests** (when your environment matches PGXS expectations):

```bash
make installcheck   # if defined by PGXS for this extension
# or, from Makefile snippet:
# make prove_installcheck
```

**Upstream-style extras** (Docker image build, etc.) remain in the Makefile; this fork’s **non-portable piece is the hardcoded Knowhere/Conan paths** — see below.

**Editor / static analysis**: `compile_commands.json` may exist at repo root and under `pgvector/` for `clangd`; regenerate if includes or sources change materially.

## Project structure

```
decoupled_pgvector/
├── CLAUDE.md                 # This file
├── analyze_profiling.sql     # Ad-hoc SQL for profiling / analysis (if used in your workflow)
├── compile_commands.json     # Optional clangd compile DB (root)
└── pgvector/                 # PostgreSQL extension (pgvector fork)
    ├── Makefile              # OBJS, CXX flags, Knowhere/Folly/glog paths, PGXS
    ├── vector.control        # Extension metadata
    ├── sql/                  # Extension SQL (versioned upgrade scripts)
    ├── src/                  # C/C++ sources
    │   ├── vector.c, hnsw*.c, ivf*.c, …   # Core + upstream AM paths
    │   ├── lsm*.c, lsm*.h                  # LSM-style index orchestration
    │   ├── ringbuffer.c, tasksend.c, …     # Workers / messaging / status
    │   └── vectorindeximpl.cpp/.hpp        # Knowhere / C++ index impl
    ├── test/
    │   ├── sql/              # pg_regress inputs
    │   ├── t/                # Perl tests
    │   ├── *.py              # Python workload / concurrency tests
    │   ├── pgbench/          # Benchmark SQL + drivers
    │   ├── knowhere_tests/   # Standalone C++ Makefile for Knowhere
    │   └── runbooks/         # YAML runbooks, C++ helpers
    ├── install_pgvector.sh
    └── README.md             # Upstream pgvector user documentation (mostly still applicable)
```

## Background worker topology

There are three distinct background worker roles registered by the extension:

| Worker | Entry point | Role |
|--------|-------------|------|
| **LSM background / flush worker** | `lsm_index_bgworker_main` (`lsmbackground.c`) | Detects full memtables, flushes them to disk segments, signals the vector-index worker |
| **Vector-index worker** | `vector_index_worker_main` (`vector_index_worker.c`) | Owns the `FlushedSegmentPool`; processes tasks from the ring buffer: `VectorSearch`, `IndexBuild`, `IndexLoad`, `SegmentUpdate`. Backends call `vector_search_send` / blocking helpers in `tasksend.c` and wait on their own `PGPROC` latch |
| **Merge workers** (up to `MERGE_WORKERS_COUNT = 2`) | `lsm_merge_worker_main` (`lsm_merge_worker.c`) | Manages `SharedSegmentArray`; rebuilds flat/high-deletion segments and merges two small adjacent segments into one |

The ring buffer (`RingBufferShmem` in shared memory) is the IPC backbone between regular backends and the vector-index worker. Each backend has a dedicated `VectorSearchTask` / `VectorSearchResult` slot keyed by `pgprocno`.

## Compile-time feature flags

Set these in the Makefile (`PG_CPPFLAGS`) or via `make CPPFLAGS="-DFLAG=val"`:

| Macro | Default | Effect |
|-------|---------|--------|
| `IS_DISK_BASED` | `0` | `1` → use DiskANN (Knowhere disk index); `0` → use HNSW |
| `USE_GPU_CUVS` | `1` | `1` → GPU CuVS indexes (requires CUDA + Knowhere built with `-DWITH_CUVS=ON`); `0` → CPU FAISS |

## On-disk storage path

Segment files, bitmap files, mapping files, and index binaries are written under:

```
/ssd_root/liu4127/pg_vector_extension_indexes/
```

This path is **hardcoded** as `VECTOR_STORAGE_BASE_DIR` in `lsmindex.h`. A new machine or user must change this macro before building.

## Things to know before changing code

1. **`Makefile` is environment-specific**  
   `PG_CONFIG`, `KNOWHERE_*`, `FOLLY_*`, `GLOG_*`, and `NLOHMANN_*` point at **this maintainer’s machine**. New clones must adjust these (or inject via env / wrapper) before `make` succeeds.

2. **Adding a new `.c` file**  
   Append `src/yourfile.o` to `OBJS` in `pgvector/Makefile` (and ensure headers include only what Postgres allows in the relevant context).

3. **Adding / changing C++**  
   Keep `vectorindeximpl` and related linkage in sync with `SHLIB_LINK` and `PG_CPPFLAGS`. The extension builds a single shared library (`vector.so`).

4. **Threading, LWLocks, shared memory**  
   LSM / worker code uses Postgres primitives (`LWLock`, atomics, etc.). Follow existing patterns in `lsm*.c` and worker files; avoid patterns that are unsafe in **postmaster / background worker** contexts.

5. **User-facing docs**  
   `pgvector/README.md` is upstream-oriented. Document **fork-specific** behavior in code comments or dedicated internal notes if you introduce new GUCs, index types, or operational steps.

6. **Scope of changes**  
   Prefer minimal, focused diffs. This fork already diverges heavily from upstream; keep unrelated refactors out of feature fixes unless explicitly requested.

## Quick reference: extension identity

- Extension name: **`vector`** (`EXTENSION = vector` in Makefile).
- Version: **`EXTVERSION`** in Makefile (e.g. `0.8.0`).
- After install: `CREATE EXTENSION vector;` in the target database.

For SQL usage, distance operators, and index DDL, start from `pgvector/README.md` and cross-check whether this fork adds or overrides index access methods for your scenario.
