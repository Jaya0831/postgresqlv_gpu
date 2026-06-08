// Copyright (C) 2019-2023 Zilliz. All rights reserved.
//
// Standalone driver to benchmark cuVS-backed GPU indexes (CAGRA, IVF-Flat, IVF-PQ, Brute Force).
//
// Usage:
//   cuvs_tests <config.json>
// Build: from knowhere_tests/, run `make` (see Makefile; set KNOWHERE_LIBDIR and dependency paths).
//
// Config JSON fields:
//   dataset_path       — base vectors: .fbin (uint32 n, dim, then floats) or .fvecs (per vector: int32 dim, dim floats)
//   query_path         — query vectors (.fbin or .fvecs, same rules)
//   groundtruth_path   — optional; .fbin (n, k, uint32 ids, float dists) or .ivecs (per query: int32 k, k int32 ids).
//                        If omitted, recall is not reported.
//   index_type         — short name: cagra | ivf_flat | ivfflat | ivf_pq | ivfpq | brute_force | bruteforce
//                        or full Knowhere name e.g. GPU_CUVS_CAGRA
//   metric_type        — L2 | IP | COSINE (HAMMING only for binary indexes; this tool loads float32 vectors)
//   k                  — top-k for search
//   batch_size         — number of queries per Search() call
//   version            — optional Knowhere index version (default: current)
//   build_params       — JSON object merged into the train config (nlist, graph_degree, build_algo, …)
//   CAGRA build_algo:  "NN_DESCENT" is usually far faster than "IVF_PQ" on large nb (match your standalone cuVS script).
//
//   index_file         — optional path to a GPU index snapshot. If the file exists and is valid, the index is
//                        deserialized from disk (skips Build). After a successful Build, the index is written
//                        to this path (Knowhere BinarySet wire format with a small KH01 header).
//
//   search_runs        — array of JSON objects; each merged into the search config for one timed run.
//                        CAGRA examples: "search_algo": "MULTI_CTA" | "SINGLE_CTA" | "MULTI_KERNEL" | "AUTO"
//                        (uppercase); "search_width", "itopk_size", etc. IVF: "nprobe", …
//                        Optional string keys "name" / "label" are only printed, not passed to Knowhere.
//
// Example:
// {
//   "dataset_path": "sift1M/sift_base.fvecs",
//   "query_path": "sift1M/sift_query.fvecs",
//   "groundtruth_path": "sift1M/sift_groundtruth.ivecs",
//   "index_type": "cagra",
//   "metric_type": "L2",
//   "k": 10,
//   "batch_size": 500,
//   "build_params": { "intermediate_graph_degree": 128, "graph_degree": 64, "build_algo": "IVF_PQ" },
//   "search_runs": [
//     { "name": "default" },
//     { "name": "wider", "search_width": 4, "itopk_size": 128 }
//   ]
// }

#ifndef KNOWHERE_WITH_CUVS
#error "cuvs_tests requires building knowhere with -DWITH_CUVS=ON"
#else

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "knowhere/binaryset.h"
#include "knowhere/comp/index_param.h"
#include "knowhere/comp/knowhere_config.h"
#include "knowhere/config.h"
#include "knowhere/dataset.h"
#include "knowhere/index/index_factory.h"
#include "knowhere/version.h"

namespace {

using clock_type = std::chrono::steady_clock;

void
log_phase(std::string const& msg) {
    std::cerr << "[cuvs_tests] " << msg << std::endl;
}

void
merge_json(knowhere::Json& dest, knowhere::Json const& src) {
    if (!src.is_object()) {
        return;
    }
    for (auto const& [key, val] : src.items()) {
        dest[key] = val;
    }
}

struct FbinFloatMatrix {
    uint32_t n = 0;
    uint32_t dim = 0;
    std::vector<float> data;
};

void
read_fbin_vectors(std::string const& path, FbinFloatMatrix& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open dataset file: " + path);
    }
    in.read(reinterpret_cast<char*>(&out.n), sizeof(out.n));
    in.read(reinterpret_cast<char*>(&out.dim), sizeof(out.dim));
    if (!in) {
        throw std::runtime_error("failed to read fbin header: " + path);
    }
    const size_t elems = static_cast<size_t>(out.n) * static_cast<size_t>(out.dim);
    out.data.resize(elems);
    in.read(reinterpret_cast<char*>(out.data.data()), static_cast<std::streamsize>(elems * sizeof(float)));
    if (!in || static_cast<size_t>(in.gcount()) != elems * sizeof(float)) {
        throw std::runtime_error("unexpected EOF or short read in fbin vectors: " + path);
    }
}

// Faiss / big-ann style: each vector is int32 dimension d, then d float32 values (no global header).
void
read_fvecs_vectors(std::string const& path, FbinFloatMatrix& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open fvecs file: " + path);
    }
    constexpr int32_t k_dim_max = 1 << 20;
    int32_t dim = -1;
    uint32_t n = 0;
    std::vector<float> buf;
    while (true) {
        int32_t d = 0;
        in.read(reinterpret_cast<char*>(&d), sizeof(d));
        if (!in) {
            if (in.eof()) {
                break;
            }
            throw std::runtime_error("read error in fvecs: " + path);
        }
        if (d <= 0 || d > k_dim_max) {
            throw std::runtime_error("invalid dimension " + std::to_string(d) + " in fvecs: " + path);
        }
        if (dim < 0) {
            dim = d;
        } else if (d != dim) {
            throw std::runtime_error("inconsistent dimension in fvecs (expected " + std::to_string(dim) + ", got " +
                                     std::to_string(d) + "): " + path);
        }
        const size_t prev = buf.size();
        buf.resize(prev + static_cast<size_t>(d));
        in.read(reinterpret_cast<char*>(buf.data() + prev), static_cast<std::streamsize>(d) * sizeof(float));
        if (!in || static_cast<size_t>(in.gcount()) != static_cast<size_t>(d) * sizeof(float)) {
            throw std::runtime_error("truncated vector in fvecs: " + path);
        }
        ++n;
    }
    if (n == 0 || dim < 0) {
        throw std::runtime_error("empty or invalid fvecs file: " + path);
    }
    out.n = n;
    out.dim = static_cast<uint32_t>(dim);
    out.data = std::move(buf);
}

bool
path_ends_with_ci(std::string const& path, std::string const& suffix_lower) {
    if (path.size() < suffix_lower.size()) {
        return false;
    }
    for (size_t i = 0; i < suffix_lower.size(); ++i) {
        char a = path[path.size() - suffix_lower.size() + i];
        char b = suffix_lower[i];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

void
read_float_vectors_file(std::string const& path, FbinFloatMatrix& out) {
    if (path_ends_with_ci(path, ".fvecs")) {
        read_fvecs_vectors(path, out);
    } else if (path_ends_with_ci(path, ".fbin")) {
        read_fbin_vectors(path, out);
    } else {
        throw std::runtime_error(
            "dataset_path / query_path must end with .fbin or .fvecs (got: " + path + ")");
    }
}

struct FbinGroundtruth {
    uint32_t n = 0;
    uint32_t k = 0;
    std::vector<int32_t> ids;
};

void
read_fbin_groundtruth(std::string const& path, FbinGroundtruth& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open groundtruth file: " + path);
    }
    in.read(reinterpret_cast<char*>(&out.n), sizeof(out.n));
    in.read(reinterpret_cast<char*>(&out.k), sizeof(out.k));
    if (!in) {
        throw std::runtime_error("failed to read groundtruth header: " + path);
    }
    const size_t id_bytes = static_cast<size_t>(out.n) * static_cast<size_t>(out.k) * sizeof(uint32_t);
    std::vector<uint32_t> uids(static_cast<size_t>(out.n) * static_cast<size_t>(out.k));
    in.read(reinterpret_cast<char*>(uids.data()), static_cast<std::streamsize>(id_bytes));
    if (!in || static_cast<size_t>(in.gcount()) != id_bytes) {
        throw std::runtime_error("failed to read groundtruth ids: " + path);
    }
    const size_t dist_bytes = static_cast<size_t>(out.n) * static_cast<size_t>(out.k) * sizeof(float);
    in.seekg(static_cast<std::streamoff>(dist_bytes), std::ios::cur);
    if (!in) {
        throw std::runtime_error("failed to skip groundtruth distances: " + path);
    }
    out.ids.resize(uids.size());
    for (size_t i = 0; i < uids.size(); ++i) {
        out.ids[i] = static_cast<int32_t>(uids[i]);
    }
}

// Per query row: int32 k, then k int32 neighbor ids (Faiss ivecs). All rows must use the same k.
void
read_ivecs_groundtruth(std::string const& path, FbinGroundtruth& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open ivecs file: " + path);
    }
    constexpr int32_t k_max = 65536;
    int32_t k0 = -1;
    uint32_t n = 0;
    std::vector<int32_t> ids;
    while (true) {
        int32_t k = 0;
        in.read(reinterpret_cast<char*>(&k), sizeof(k));
        if (!in) {
            if (in.eof()) {
                break;
            }
            throw std::runtime_error("read error in ivecs: " + path);
        }
        if (k <= 0 || k > k_max) {
            throw std::runtime_error("invalid k " + std::to_string(k) + " in ivecs: " + path);
        }
        if (k0 < 0) {
            k0 = k;
        } else if (k != k0) {
            throw std::runtime_error("ivecs ground truth must have fixed k per row (first k=" + std::to_string(k0) +
                                     ", saw k=" + std::to_string(k) + "): " + path);
        }
        const size_t prev = ids.size();
        ids.resize(prev + static_cast<size_t>(k));
        in.read(reinterpret_cast<char*>(ids.data() + prev), static_cast<std::streamsize>(k) * sizeof(int32_t));
        if (!in || static_cast<size_t>(in.gcount()) != static_cast<size_t>(k) * sizeof(int32_t)) {
            throw std::runtime_error("truncated ivecs row: " + path);
        }
        ++n;
    }
    if (n == 0 || k0 < 0) {
        throw std::runtime_error("empty or invalid ivecs file: " + path);
    }
    out.n = n;
    out.k = static_cast<uint32_t>(k0);
    out.ids = std::move(ids);
}

void
read_groundtruth_file(std::string const& path, FbinGroundtruth& out) {
    if (path_ends_with_ci(path, ".ivecs")) {
        read_ivecs_groundtruth(path, out);
    } else if (path_ends_with_ci(path, ".fbin")) {
        read_fbin_groundtruth(path, out);
    } else {
        throw std::runtime_error(
            "groundtruth_path must end with .fbin or .ivecs (got: " + path + ")");
    }
}

float
recall_at_k(const int64_t* result_ids, int32_t nq, int32_t k, const int32_t* gt_ids, int32_t gt_k) {
    if (nq <= 0 || k <= 0 || gt_k <= 0) {
        return 0.0f;
    }
    const int32_t use_k = std::min(k, gt_k);
    int64_t hit = 0;
    for (int32_t i = 0; i < nq; ++i) {
        std::unordered_set<int32_t> ground(gt_ids + static_cast<size_t>(i) * gt_k,
                                            gt_ids + static_cast<size_t>(i) * gt_k + use_k);
        for (int32_t j = 0; j < use_k; ++j) {
            const auto id = static_cast<int32_t>(result_ids[static_cast<size_t>(i) * k + j]);
            if (ground.count(id) > 0) {
                hit++;
            }
        }
    }
    return static_cast<float>(hit) / static_cast<float>(static_cast<int64_t>(nq) * use_k);
}

std::string
to_lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

std::string
resolve_index_name(std::string const& index_type) {
    std::string t = to_lower(index_type);
    if (t == "gpu_cuvs_cagra" || t == "gpu_cagra" || t == "cagra") {
        return knowhere::IndexEnum::INDEX_CUVS_CAGRA;
    }
    if (t == "gpu_cuvs_ivf_flat" || t == "gpu_ivf_flat" || t == "ivf_flat" || t == "ivfflat") {
        return knowhere::IndexEnum::INDEX_CUVS_IVFFLAT;
    }
    if (t == "gpu_cuvs_ivf_pq" || t == "gpu_ivf_pq" || t == "ivf_pq" || t == "ivfpq") {
        return knowhere::IndexEnum::INDEX_CUVS_IVFPQ;
    }
    if (t == "gpu_cuvs_brute_force" || t == "gpu_brute_force" || t == "brute_force" || t == "bruteforce") {
        return knowhere::IndexEnum::INDEX_CUVS_BRUTEFORCE;
    }
    if (index_type.rfind("GPU_", 0) == 0 || index_type.rfind("gpu_", 0) == 0) {
        return index_type;
    }
    throw std::runtime_error("unknown index_type: " + index_type);
}

knowhere::Json
strip_meta_keys(knowhere::Json run) {
    if (!run.is_object()) {
        return run;
    }
    run.erase("name");
    run.erase("label");
    return run;
}

std::string
run_label(knowhere::Json const& run, int index) {
    if (run.is_object()) {
        if (run.contains("name") && run["name"].is_string()) {
            return run["name"].get<std::string>();
        }
        if (run.contains("label") && run["label"].is_string()) {
            return run["label"].get<std::string>();
        }
    }
    return "run_" + std::to_string(index);
}

// Knowhere GPU Serialize() stores one or more named blobs; preserve names on disk (e.g. CAGRA+CPU uses "<TYPE>_cpu").
constexpr char k_index_file_magic[4] = {'K', 'H', '0', '1'};

bool
index_snapshot_file_nonempty(std::string const& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const auto sz = in.tellg();
    return sz > 0;
}

bool
read_index_snapshot(std::string const& path, knowhere::BinarySet& bs) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return false;
    }
    const auto end = in.tellg();
    if (end <= static_cast<std::streamoff>(sizeof(k_index_file_magic))) {
        return false;
    }
    in.seekg(0);
    char magic[4];
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, k_index_file_magic, sizeof(magic)) != 0) {
        return false;
    }
    uint32_t n_entries = 0;
    in.read(reinterpret_cast<char*>(&n_entries), sizeof(n_entries));
    if (!in || n_entries == 0) {
        return false;
    }
    for (uint32_t e = 0; e < n_entries; ++e) {
        uint32_t key_len = 0;
        in.read(reinterpret_cast<char*>(&key_len), sizeof(key_len));
        if (!in || key_len == 0 || key_len > (1u << 20)) {
            return false;
        }
        std::string key(key_len, '\0');
        in.read(key.data(), static_cast<std::streamsize>(key_len));
        if (!in) {
            return false;
        }
        uint64_t blob_len = 0;
        in.read(reinterpret_cast<char*>(&blob_len), sizeof(blob_len));
        if (!in || blob_len == 0 || blob_len > static_cast<uint64_t>(1ull << 40)) {
            return false;
        }
        auto data = std::shared_ptr<uint8_t[]>(new (std::nothrow) uint8_t[static_cast<size_t>(blob_len)]);
        if (!data) {
            return false;
        }
        in.read(reinterpret_cast<char*>(data.get()), static_cast<std::streamsize>(blob_len));
        if (!in || static_cast<uint64_t>(in.gcount()) != blob_len) {
            return false;
        }
        bs.Append(key, data, static_cast<int64_t>(blob_len));
    }
    return in.tellg() == end && !bs.binary_map_.empty();
}

bool
write_index_snapshot(std::string const& path, knowhere::BinarySet const& bs) {
    if (bs.binary_map_.empty()) {
        return false;
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(k_index_file_magic, sizeof(k_index_file_magic));
    const uint32_t n_entries = static_cast<uint32_t>(bs.binary_map_.size());
    out.write(reinterpret_cast<const char*>(&n_entries), sizeof(n_entries));
    for (auto const& [key, bp] : bs.binary_map_) {
        if (bp == nullptr || bp->size <= 0) {
            return false;
        }
        const uint32_t key_len = static_cast<uint32_t>(key.size());
        if (key_len == 0) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(&key_len), sizeof(key_len));
        out.write(key.data(), static_cast<std::streamsize>(key_len));
        const uint64_t blob_len = static_cast<uint64_t>(bp->size);
        out.write(reinterpret_cast<const char*>(&blob_len), sizeof(blob_len));
        out.write(reinterpret_cast<const char*>(bp->data.get()), static_cast<std::streamsize>(bp->size));
    }
    return static_cast<bool>(out);
}

}  // namespace

int
main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: " << (argc > 0 ? argv[0] : "cuvs_tests") << " <config.json>\n";
        return 2;
    }

    knowhere::Json cfg;
    try {
        std::ifstream f(argv[1]);
        if (!f) {
            std::cerr << "cannot open config: " << argv[1] << '\n';
            return 1;
        }
        f >> cfg;
    } catch (std::exception const& e) {
        std::cerr << "failed to parse JSON config: " << e.what() << '\n';
        return 1;
    }

    try {
        const std::string base_path = cfg.at("dataset_path").get<std::string>();
        const std::string query_path = cfg.at("query_path").get<std::string>();
        const std::string metric_type = cfg.at("metric_type").get<std::string>();
        const std::string index_type = cfg.at("index_type").get<std::string>();
        const int32_t k = cfg.at("k").get<int32_t>();
        const int32_t batch_size = cfg.at("batch_size").get<int32_t>();
        if (k <= 0 || batch_size <= 0) {
            throw std::runtime_error("k and batch_size must be positive");
        }

        const int32_t version = cfg.value("version", knowhere::Version::GetCurrentVersion().VersionNumber());
        const std::string index_file = cfg.value("index_file", std::string{});

        knowhere::Json build_params =
            (cfg.contains("build_params") && cfg["build_params"].is_object()) ? cfg["build_params"]
                                                                              : knowhere::Json::object();

        knowhere::Json search_runs = knowhere::Json::array();
        if (cfg.contains("search_runs")) {
            search_runs = cfg["search_runs"];
            if (!search_runs.is_array()) {
                throw std::runtime_error("search_runs must be a JSON array");
            }
        }
        if (search_runs.empty()) {
            search_runs = knowhere::Json::array({knowhere::Json::object()});
        }

        FbinFloatMatrix base;
        FbinFloatMatrix queries;

        auto t0 = clock_type::now();
        log_phase("loading base vectors...");
        read_float_vectors_file(base_path, base);
        log_phase("loaded base: nb=" + std::to_string(base.n) + " dim=" + std::to_string(base.dim) + " in " +
                   std::to_string(std::chrono::duration<double>(clock_type::now() - t0).count()) + " s");

        t0 = clock_type::now();
        log_phase("loading query vectors...");
        read_float_vectors_file(query_path, queries);
        log_phase("loaded queries: nq=" + std::to_string(queries.n) + " dim=" + std::to_string(queries.dim) + " in " +
                   std::to_string(std::chrono::duration<double>(clock_type::now() - t0).count()) + " s");

        if (base.dim != queries.dim) {
            throw std::runtime_error("base dim != query dim");
        }

        std::optional<FbinGroundtruth> gt;
        if (cfg.contains("groundtruth_path") && !cfg["groundtruth_path"].get<std::string>().empty()) {
            log_phase("loading ground truth...");
            FbinGroundtruth g;
            read_groundtruth_file(cfg["groundtruth_path"].get<std::string>(), g);
            if (g.n != queries.n) {
                throw std::runtime_error("groundtruth n does not match query count");
            }
            gt = std::move(g);
            log_phase("ground truth loaded.");
        }

        const std::string index_name = resolve_index_name(index_type);

        knowhere::Json train_json;
        train_json[knowhere::meta::DIM] = static_cast<int64_t>(base.dim);
        train_json[knowhere::meta::METRIC_TYPE] = metric_type;
        merge_json(train_json, build_params);

        if (index_name.find("CAGRA") != std::string::npos) {
            auto ba = train_json.value("build_algo", std::string("NN_DESCENT"));
            std::string ba_lower = ba;
            for (char& c : ba_lower) {
                if (c >= 'A' && c <= 'Z') {
                    c = static_cast<char>(c - 'A' + 'a');
                }
            }
            if (ba_lower == "ivf_pq" && base.n >= 100000u) {
                log_phase(
                    "WARNING: CAGRA + build_algo IVF_PQ on large nb is often orders of magnitude slower than "
                    "NN_DESCENT; many raw cuVS demos use NN_DESCENT. If build seems stuck, switch build_algo.");
            }
        }

        log_phase("creating GPU index handle: " + index_name + " ...");
        t0 = clock_type::now();
        auto idx_result = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(index_name, version);
        if (!idx_result.has_value()) {
            std::cerr << "Create index failed: " << static_cast<int>(idx_result.error()) << " " << idx_result.what()
                      << '\n';
            return 1;
        }
        auto index = std::move(idx_result.value());
        log_phase("index object ready in " +
                  std::to_string(std::chrono::duration<double>(clock_type::now() - t0).count()) +
                  " s (Knowhere glog line may appear slightly out of order).");

        auto base_ds = knowhere::GenDataSet(static_cast<int64_t>(base.n), static_cast<int64_t>(base.dim),
                                            base.data.data());
        base_ds->SetIsOwner(false);

        bool loaded_from_disk = false;
        if (!index_file.empty() && index_snapshot_file_nonempty(index_file)) {
            knowhere::BinarySet bs;
            if (read_index_snapshot(index_file, bs)) {
                const auto ds_st = index.Deserialize(bs, train_json);
                if (ds_st == knowhere::Status::success &&
                    index.Dim() == static_cast<int64_t>(base.dim) &&
                    index.Count() == static_cast<int64_t>(base.n)) {
                    loaded_from_disk = true;
                    log_phase("loaded GPU index from " + index_file);
                } else {
                    if (ds_st == knowhere::Status::success) {
                        log_phase("index snapshot dim/nb mismatch dataset; rebuilding");
                    } else {
                        log_phase("index snapshot deserialize failed; rebuilding");
                    }
                    auto idx_retry = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(index_name, version);
                    if (!idx_retry.has_value()) {
                        std::cerr << "Create index failed: " << static_cast<int>(idx_retry.error()) << " "
                                  << idx_retry.what() << '\n';
                        return 1;
                    }
                    index = std::move(idx_retry.value());
                }
            } else {
                log_phase("index snapshot unreadable or bad KH01 header; rebuilding");
            }
        }

        double build_sec = 0.0;
        if (!loaded_from_disk) {
            log_phase(
                "building index on GPU (CAGRA/IVF on millions of vectors often takes many minutes; use nvidia-smi to see GPU activity)...");
            const auto t_build_0 = clock_type::now();
            const auto build_st = index.Build(base_ds, train_json);
            const auto t_build_1 = clock_type::now();
            if (build_st != knowhere::Status::success) {
                std::cerr << "Build failed\n";
                return 1;
            }
            build_sec = std::chrono::duration<double>(t_build_1 - t_build_0).count();

            if (!index_file.empty()) {
                knowhere::BinarySet bs;
                const auto ser_st = index.Serialize(bs);
                if (ser_st == knowhere::Status::success) {
                    if (write_index_snapshot(index_file, bs)) {
                        log_phase("saved GPU index to " + index_file);
                    } else {
                        log_phase("WARNING: could not write index snapshot to " + index_file);
                    }
                } else {
                    log_phase("WARNING: Serialize failed; index not saved to disk");
                }
            }
        }

        std::cout << "build_time_s=" << build_sec << " nb=" << base.n << " dim=" << base.dim
                  << " index=" << index_name;
        if (loaded_from_disk) {
            std::cout << " index_source=disk";
        } else {
            std::cout << " index_source=build";
        }
        std::cout << '\n';

        std::vector<int64_t> all_ids(static_cast<size_t>(queries.n) * static_cast<size_t>(k));

        int run_i = 0;
        for (auto const& run_entry : search_runs) {
            log_phase("search run " + std::to_string(run_i) + " / " + std::to_string(search_runs.size()) + " ...");
            knowhere::Json search_json;
            search_json[knowhere::meta::DIM] = static_cast<int64_t>(base.dim);
            search_json[knowhere::meta::METRIC_TYPE] = metric_type;
            search_json[knowhere::meta::TOPK] = k;
            merge_json(search_json, strip_meta_keys(run_entry));

            const auto t0 = clock_type::now();
            for (uint32_t q0 = 0; q0 < queries.n; q0 += static_cast<uint32_t>(batch_size)) {
                const uint32_t bs = std::min(static_cast<uint32_t>(batch_size), queries.n - q0);
                auto q_ds = knowhere::GenDataSet(static_cast<int64_t>(bs), static_cast<int64_t>(queries.dim),
                                                 queries.data.data() + static_cast<size_t>(q0) * queries.dim);
                q_ds->SetIsOwner(false);
                auto sr = index.Search(q_ds, search_json, nullptr);
                if (!sr.has_value()) {
                    std::cerr << "Search failed: " << static_cast<int>(sr.error()) << " " << sr.what() << '\n';
                    return 1;
                }
                const int64_t* ids = sr.value()->GetIds();
                std::memcpy(all_ids.data() + static_cast<size_t>(q0) * k, ids,
                            static_cast<size_t>(bs) * static_cast<size_t>(k) * sizeof(int64_t));
            }
            const auto t1 = clock_type::now();
            const double search_sec = std::chrono::duration<double>(t1 - t0).count();
            const double qps = static_cast<double>(queries.n) / std::max(search_sec, 1e-9);

            std::cout << "run[" << run_i << "] label=" << run_label(run_entry, run_i)
                      << " search_time_s=" << search_sec << " qps=" << qps;

            if (gt.has_value()) {
                const float r = recall_at_k(all_ids.data(), static_cast<int32_t>(queries.n), k, gt->ids.data(),
                                            static_cast<int32_t>(gt->k));
                std::cout << " recall@" << std::min(k, static_cast<int32_t>(gt->k)) << "=" << r;
            }
            std::cout << '\n';
            ++run_i;
        }

        return 0;
    } catch (std::exception const& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
}

#endif  // KNOWHERE_WITH_CUVS
