/**
 * Standalone test for GPU CuVS indexes in knowhere.
 *
 * Requires: knowhere built with -DWITH_CUVS=ON and a CUDA-capable GPU.
 *
 * Usage:
 *   Save mode: ./gpu_cuvs_test --index-type <type> --save-index <path> \\
 *              <data_file> <query_file> <ground_truth_file> [k]
 *   Load mode: ./gpu_cuvs_test --index-type <type> --load-index <path> \\
 *              <query_file> <ground_truth_file> [k]
 *
 *   --index-type     - One of: bruteforce, ivfflat, ivfpq, cagra
 *   --save-index     - Build index, run queries, save to path (requires data_file)
 *   --load-index     - Load index from path, run queries (no data_file needed)
 *   Exactly one of --save-index or --load-index must be set.
 *
 * Options:
 *   --use-bvecs      - Use .bvecs format (default: auto-detect from extension)
 *   --use-fbin       - Use .fbin format (default: auto-detect from extension)
 */

 #include <iostream>
 #include <iomanip>
 #include <fstream>
 #include <vector>
 #include <chrono>
 #include <cstring>
 #include <algorithm>
 
 #if __has_include(<filesystem>)
 #include <filesystem>
 namespace fs = std::filesystem;
 #elif __has_include(<experimental/filesystem>)
 #include <experimental/filesystem>
 namespace fs = std::experimental::filesystem;
 #endif
 
 #ifdef KNOWHERE_WITH_CUVS
 
 #include "knowhere/index/index_factory.h"
 #include "knowhere/index/index.h"
 #include "knowhere/comp/index_param.h"
 #include "knowhere/comp/knowhere_config.h"
 #include "knowhere/comp/brute_force.h"
 #include "knowhere/dataset.h"
 #include "knowhere/bitsetview.h"
 #include "knowhere/version.h"
 
 namespace {
 
 // File format readers (from knowhere_diskann.cpp)
 struct FvecsReader {
     std::ifstream file;
     int dim;
 
     FvecsReader(const std::string& filename) : file(filename, std::ios::binary) {
         if (!file.is_open()) {
             throw std::runtime_error("Cannot open file: " + filename);
         }
         int32_t d;
         file.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
         dim = d;
         file.seekg(0, std::ios::beg);
     }
 
     void read_all_vectors(std::vector<std::vector<float>>& vectors, int64_t skip = 0, int64_t max_vectors = -1) {
         file.seekg(0, std::ios::beg);
         vectors.clear();
         for (int64_t i = 0; i < skip; i++) {
             int32_t vec_dim;
             file.read(reinterpret_cast<char*>(&vec_dim), sizeof(int32_t));
             if (!file || file.eof()) break;
             file.seekg(vec_dim * sizeof(float), std::ios::cur);
         }
         int64_t count = 0;
         while (true) {
             if (max_vectors > 0 && count >= max_vectors) break;
             int32_t vec_dim;
             file.read(reinterpret_cast<char*>(&vec_dim), sizeof(int32_t));
             if (!file || file.eof()) break;
             std::vector<float> vec(vec_dim);
             file.read(reinterpret_cast<char*>(vec.data()), vec_dim * sizeof(float));
             vectors.push_back(std::move(vec));
             count++;
         }
     }
 };
 
 struct BvecsReader {
     std::ifstream file;
     int dim;
 
     BvecsReader(const std::string& filename) : file(filename, std::ios::binary) {
         if (!file.is_open()) {
             throw std::runtime_error("Cannot open file: " + filename);
         }
         int32_t d;
         file.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
         dim = d;
         file.seekg(0, std::ios::beg);
     }
 
     void read_all_vectors(std::vector<std::vector<float>>& vectors, int64_t skip = 0, int64_t max_vectors = -1) {
         file.seekg(0, std::ios::beg);
         vectors.clear();
         for (int64_t i = 0; i < skip; i++) {
             int32_t vec_dim;
             file.read(reinterpret_cast<char*>(&vec_dim), sizeof(int32_t));
             if (!file || file.eof()) break;
             file.seekg(vec_dim, std::ios::cur);
         }
         int64_t count = 0;
         while (true) {
             if (max_vectors > 0 && count >= max_vectors) break;
             int32_t vec_dim;
             file.read(reinterpret_cast<char*>(&vec_dim), sizeof(int32_t));
             if (!file || file.eof()) break;
             std::vector<uint8_t> bvec(vec_dim);
             file.read(reinterpret_cast<char*>(bvec.data()), vec_dim);
             std::vector<float> vec(vec_dim);
             for (int i = 0; i < vec_dim; i++) {
                 vec[i] = static_cast<float>(bvec[i]);
             }
             vectors.push_back(std::move(vec));
             count++;
         }
     }
 };
 
 struct FbinReader {
     std::ifstream file;
     int dim;
     int64_t num_vectors;
 
     FbinReader(const std::string& filename) : file(filename, std::ios::binary) {
         if (!file.is_open()) {
             throw std::runtime_error("Cannot open file: " + filename);
         }
         int32_t n, d;
         file.read(reinterpret_cast<char*>(&n), sizeof(int32_t));
         file.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
         num_vectors = static_cast<int64_t>(n);
         dim = static_cast<int>(d);
         if (num_vectors <= 0 || dim <= 0) {
             throw std::runtime_error("Invalid fbin metadata in: " + filename);
         }
     }
 
     void read_all_vectors(std::vector<std::vector<float>>& vectors, int64_t skip = 0, int64_t max_vectors = -1) {
         file.seekg(0, std::ios::beg);
         vectors.clear();
         int32_t n, d;
         file.read(reinterpret_cast<char*>(&n), sizeof(int32_t));
         file.read(reinterpret_cast<char*>(&d), sizeof(int32_t));
         file.seekg(skip * dim * sizeof(float), std::ios::cur);
         int64_t to_read = max_vectors > 0 ? max_vectors : (num_vectors - skip);
         to_read = std::min(to_read, num_vectors - skip);
         for (int64_t i = 0; i < to_read; i++) {
             std::vector<float> vec(dim);
             file.read(reinterpret_cast<char*>(vec.data()), dim * sizeof(float));
             if (!file || file.gcount() != static_cast<std::streamsize>(dim * sizeof(float))) break;
             vectors.push_back(std::move(vec));
         }
     }
 };
 
 struct IvecsReader {
     std::ifstream file;
     int k;
 
     IvecsReader(const std::string& filename, int expected_k) : k(expected_k) {
         file.open(filename, std::ios::binary);
         if (!file.is_open()) {
             throw std::runtime_error("Cannot open file: " + filename);
         }
     }
 
     void read_all_ground_truth(std::vector<std::vector<int32_t>>& ground_truth, int64_t skip = 0, int64_t max_queries = -1) {
         file.seekg(0, std::ios::beg);
         ground_truth.clear();
         for (int64_t i = 0; i < skip; i++) {
             int32_t vec_k;
             file.read(reinterpret_cast<char*>(&vec_k), sizeof(int32_t));
             if (!file || file.eof()) break;
             file.seekg(vec_k * sizeof(int32_t), std::ios::cur);
         }
         int64_t count = 0;
         while (true) {
             if (max_queries > 0 && count >= max_queries) break;
             int32_t vec_k;
             file.read(reinterpret_cast<char*>(&vec_k), sizeof(int32_t));
             if (!file || file.eof()) break;
             std::vector<int32_t> gt(vec_k);
             file.read(reinterpret_cast<char*>(gt.data()), vec_k * sizeof(int32_t));
             if (static_cast<int>(gt.size()) > k) gt.resize(k);
             ground_truth.push_back(std::move(gt));
             count++;
         }
     }
 };
 
 // Load vectors from file, return DataSet and flattened data (caller owns)
 knowhere::DataSetPtr
 LoadVectorsFromFile(const std::string& filename, bool use_fbin, bool use_bvecs,
                    std::vector<float>& flat_data) {
     std::vector<std::vector<float>> vectors;
     int dim = 0;
 
     if (use_fbin) {
         FbinReader reader(filename);
         dim = reader.dim;
         reader.read_all_vectors(vectors);
     } else if (use_bvecs) {
         BvecsReader reader(filename);
         dim = reader.dim;
         reader.read_all_vectors(vectors);
     } else {
         FvecsReader reader(filename);
         dim = reader.dim;
         reader.read_all_vectors(vectors);
     }
 
     if (vectors.empty()) {
         throw std::runtime_error("No vectors loaded from: " + filename);
     }
 
     int64_t rows = static_cast<int64_t>(vectors.size());
     flat_data.resize(rows * dim);
     for (int64_t i = 0; i < rows; i++) {
         std::memcpy(flat_data.data() + i * dim, vectors[i].data(), dim * sizeof(float));
     }
 
     auto ds = knowhere::GenDataSet(rows, dim, flat_data.data());
     ds->SetIsOwner(false);
     return ds;
 }
 
 // Create ground truth DataSet from ivecs (for recall computation)
 knowhere::DataSetPtr
 LoadGroundTruthFromFile(const std::string& filename, int k, int64_t nq,
                         std::vector<int64_t>& flat_ids) {
     IvecsReader reader(filename, k);
     std::vector<std::vector<int32_t>> ground_truth;
     reader.read_all_ground_truth(ground_truth);
 
     if (static_cast<int64_t>(ground_truth.size()) < nq) {
         throw std::runtime_error("Ground truth has " + std::to_string(ground_truth.size()) +
                                  " queries but need " + std::to_string(nq));
     }
 
     flat_ids.resize(nq * k);
     for (int64_t i = 0; i < nq; i++) {
         int copy_k = std::min(k, static_cast<int>(ground_truth[i].size()));
         for (int j = 0; j < copy_k; j++) {
             flat_ids[i * k + j] = static_cast<int64_t>(ground_truth[i][j]);
         }
         for (int j = copy_k; j < k; j++) {
             flat_ids[i * k + j] = -1;
         }
     }
 
     auto ds = std::make_shared<knowhere::DataSet>();
     ds->SetRows(nq);
     ds->SetDim(k);
     ds->SetIds(flat_ids.data());
     ds->SetIsOwner(false);
     return ds;
 }
 
 // File I/O helpers for saving/loading BinarySet to disk
 struct FileIOWriter {
     std::ofstream fs;
     std::string name;
 
     explicit FileIOWriter(const std::string& fname) {
         name = fname;
         fs.open(name, std::ios::out | std::ios::binary);
         if (!fs.is_open()) {
             throw std::runtime_error("Cannot open file for writing: " + fname);
         }
     }
 
     ~FileIOWriter() {
         if (fs.is_open()) {
             fs.close();
         }
     }
 
     size_t operator()(void* ptr, size_t size) {
         fs.write(reinterpret_cast<char*>(ptr), size);
         return size;
     }
 };
 
 struct FileIOReader {
     std::ifstream fs;
     std::string name;
 
     explicit FileIOReader(const std::string& fname) {
         name = fname;
         fs.open(name, std::ios::in | std::ios::binary);
         if (!fs.is_open()) {
             throw std::runtime_error("Cannot open file for reading: " + fname);
         }
     }
 
     ~FileIOReader() {
         if (fs.is_open()) {
             fs.close();
         }
     }
 
     size_t operator()(void* ptr, size_t size) {
         fs.read(reinterpret_cast<char*>(ptr), size);
         return fs.gcount();
     }
 
     int64_t size() {
         fs.seekg(0, std::ios::end);
         int64_t len = fs.tellg();
         fs.seekg(0, std::ios::beg);
         return len;
     }
 };
 
 static void
 write_binary_set(knowhere::BinarySet& binary_set, const std::string& filename) {
     FileIOWriter writer(filename);
     const auto& m = binary_set.binary_map_;
     for (auto it = m.begin(); it != m.end(); ++it) {
         const std::string& name = it->first;
         size_t name_size = name.length();
         const knowhere::BinaryPtr data = it->second;
         size_t data_size = data->size;
 
         writer(&name_size, sizeof(name_size));
         writer(&data_size, sizeof(data_size));
         writer(const_cast<void*>(static_cast<const void*>(name.c_str())), name_size);
         writer(data->data.get(), data_size);
     }
 }
 
 static void
 read_binary_set(knowhere::BinarySet& binary_set, const std::string& filename) {
     FileIOReader reader(filename);
     int64_t file_size = reader.size();
     if (file_size < 0) {
         throw std::runtime_error("Cannot determine file size: " + filename);
     }
 
     int64_t offset = 0;
     while (offset + static_cast<int64_t>(sizeof(size_t) * 2) <= file_size) {
         size_t name_size, data_size;
         reader(&name_size, sizeof(size_t));
         offset += sizeof(size_t);
         reader(&data_size, sizeof(size_t));
         offset += sizeof(size_t);
 
         if (name_size == 0 && data_size == 0) {
             break;
         }
         if (offset + static_cast<int64_t>(name_size + data_size) > file_size) {
             break;
         }
 
         std::string name;
         name.resize(name_size);
         reader(const_cast<void*>(static_cast<const void*>(name.data())), name_size);
         offset += name_size;
         auto data = new uint8_t[data_size];
         reader(data, data_size);
         offset += data_size;
 
         std::shared_ptr<uint8_t[]> data_ptr(data);
         binary_set.Append(name, data_ptr, data_size);
     }
 }
 
 // Compute recall: fraction of ground-truth IDs found in results
 float
 GetKNNRecall(const knowhere::DataSet& ground_truth, const knowhere::DataSet& result) {
     auto nq = result.GetRows();
     auto gt_k = ground_truth.GetDim();
     auto res_k = result.GetDim();
     auto gt_ids = ground_truth.GetIds();
     auto res_ids = result.GetIds();
 
     if (!gt_ids || !res_ids) return 0.0f;
 
     int64_t k = std::min(gt_k, res_k);
     uint32_t matched_num = 0;
     for (int64_t i = 0; i < nq; ++i) {
         std::vector<int64_t> ids_0(gt_ids + i * gt_k, gt_ids + i * gt_k + k);
         std::vector<int64_t> ids_1(res_ids + i * res_k, res_ids + i * res_k + k);
         std::sort(ids_0.begin(), ids_0.end());
         std::sort(ids_1.begin(), ids_1.end());
 
         std::vector<int64_t> v(std::max(ids_0.size(), ids_1.size()));
         auto it = std::set_intersection(ids_0.begin(), ids_0.end(), ids_1.begin(), ids_1.end(), v.begin());
         matched_num += (it - v.begin());
     }
     return static_cast<float>(matched_num) / (nq * k);
 }
 
 // Map index type string to IndexEnum
 std::string
 resolve_index_type(const std::string& type_str) {
     std::string lower;
     lower.reserve(type_str.size());
     for (char c : type_str) {
         lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
     }
     if (lower == "bruteforce" || lower == "brute_force") {
         return knowhere::IndexEnum::INDEX_CUVS_BRUTEFORCE;
     }
     if (lower == "ivfflat" || lower == "ivf_flat") {
         return knowhere::IndexEnum::INDEX_CUVS_IVFFLAT;
     }
     if (lower == "ivfpq" || lower == "ivf_pq") {
         return knowhere::IndexEnum::INDEX_CUVS_IVFPQ;
     }
     if (lower == "cagra") {
         return knowhere::IndexEnum::INDEX_CUVS_CAGRA;
     }
     throw std::runtime_error("Unknown index type: " + type_str +
                              ". Valid: bruteforce, ivfflat, ivfpq, cagra");
 }
 
 // Build config for index type (build + search params for save mode)
 knowhere::Json
 make_build_config(const std::string& index_type, int dim, int k) {
     knowhere::Json conf = {
         {knowhere::meta::DIM, dim},
         {knowhere::meta::METRIC_TYPE, knowhere::metric::L2},
         {knowhere::meta::TOPK, k},
     };
     if (index_type == knowhere::IndexEnum::INDEX_CUVS_IVFFLAT) {
         conf[knowhere::indexparam::NLIST] = 64;
         conf[knowhere::indexparam::NPROBE] = 16;
     } else if (index_type == knowhere::IndexEnum::INDEX_CUVS_IVFPQ) {
         conf[knowhere::indexparam::NLIST] = 64;
         conf[knowhere::indexparam::NPROBE] = 16;
         conf[knowhere::indexparam::M] = 8;
         conf[knowhere::indexparam::NBITS] = 8;
     } else if (index_type == knowhere::IndexEnum::INDEX_CUVS_CAGRA) {
         conf[knowhere::indexparam::INTERMEDIATE_GRAPH_DEGREE] = 64;
         conf[knowhere::indexparam::GRAPH_DEGREE] = 32;
         conf[knowhere::indexparam::ITOPK_SIZE] = 128;
     }
     return conf;
 }
 
 // Search-only config for load mode (build params are in serialized index)
 knowhere::Json
 make_search_config(const std::string& index_type, int dim, int k) {
     knowhere::Json conf = {
         {knowhere::meta::DIM, dim},
         {knowhere::meta::METRIC_TYPE, knowhere::metric::L2},
         {knowhere::meta::TOPK, k},
     };
     if (index_type == knowhere::IndexEnum::INDEX_CUVS_IVFFLAT ||
         index_type == knowhere::IndexEnum::INDEX_CUVS_IVFPQ) {
         conf[knowhere::indexparam::NPROBE] = 16;
     } else if (index_type == knowhere::IndexEnum::INDEX_CUVS_CAGRA) {
         conf[knowhere::indexparam::ITOPK_SIZE] = 128;
     }
     return conf;
 }
 
 int
 run_save_mode(const std::string& index_type,
               knowhere::DataSetPtr train_ds,
               knowhere::DataSetPtr query_ds,
               knowhere::DataSetPtr gt_result,
               int dim,
               int k,
               const std::string& save_path) {
     std::cout << "\n=== GPU CuVS Save Mode ===\n";
     std::cout << "Index type: " << index_type << "\n";
     std::cout << "Base vectors: " << train_ds->GetRows() << ", dim: " << dim << "\n";
     std::cout << "Query vectors: " << query_ds->GetRows() << ", top-k: " << k << "\n";
     std::cout << "Save path: " << save_path << "\n\n";
 
     auto conf = make_build_config(index_type, dim, k);
     auto version = knowhere::Version::GetCurrentVersion().VersionNumber();
     auto idx_result = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(index_type, version);
 
     if (!idx_result.has_value()) {
         std::cerr << "Failed to create index " << index_type << std::endl;
         return 1;
     }
 
     auto idx = std::move(idx_result.value());
 
     auto t0 = std::chrono::steady_clock::now();
     auto build_status = idx.Build(train_ds, conf);
     auto t1 = std::chrono::steady_clock::now();
     double build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
 
     if (build_status != knowhere::Status::success) {
         std::cerr << "Build failed for " << index_type << std::endl;
         return 1;
     }
     std::cout << "Build OK (" << std::fixed << std::setprecision(2) << build_ms << " ms)\n";
 
     t0 = std::chrono::steady_clock::now();
     auto search_result = idx.Search(query_ds, conf, nullptr);
     t1 = std::chrono::steady_clock::now();
     double search_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
 
     if (!search_result.has_value()) {
         std::cerr << "Search failed for " << index_type << std::endl;
         return 1;
     }
 
     float recall = gt_result ? GetKNNRecall(*gt_result, *search_result.value()) : 0.0f;
     std::cout << "Search OK (" << search_ms << " ms), Recall: " << std::fixed << std::setprecision(4)
               << recall << "\n";
 
     knowhere::BinarySet binary_set;
     auto serialize_status = idx.Serialize(binary_set);
     if (serialize_status != knowhere::Status::success) {
         std::cerr << "Serialize failed for " << index_type << std::endl;
         return 1;
     }
 
     auto parent = fs::path(save_path).parent_path();
     if (!parent.empty()) {
         fs::create_directories(parent);
     }
     write_binary_set(binary_set, save_path);
     std::cout << "Saved to " << save_path << "\n";
     return 0;
 }
 
 int
 run_load_mode(const std::string& index_type,
               knowhere::DataSetPtr query_ds,
               knowhere::DataSetPtr gt_result,
               int k,
               const std::string& load_path) {
     int dim = static_cast<int>(query_ds->GetDim());
     std::cout << "\n=== GPU CuVS Load Mode ===\n";
     std::cout << "Index type: " << index_type << "\n";
     std::cout << "Query vectors: " << query_ds->GetRows() << ", top-k: " << k << "\n";
     std::cout << "Load path: " << load_path << "\n\n";
 
     knowhere::BinarySet binary_set;
     read_binary_set(binary_set, load_path);
 
     auto conf = make_search_config(index_type, dim, k);
     auto version = knowhere::Version::GetCurrentVersion().VersionNumber();
     auto idx_result = knowhere::IndexFactory::Instance().Create<knowhere::fp32>(index_type, version);
 
     if (!idx_result.has_value()) {
         std::cerr << "Failed to create index " << index_type << std::endl;
         return 1;
     }
 
     auto idx = std::move(idx_result.value());
     auto deserialize_status = idx.Deserialize(binary_set, conf);
     if (deserialize_status != knowhere::Status::success) {
         std::cerr << "Deserialize failed for " << index_type << std::endl;
         return 1;
     }
     std::cout << "Load OK\n";
 
     auto t0 = std::chrono::steady_clock::now();
     auto search_result = idx.Search(query_ds, conf, nullptr);
     auto t1 = std::chrono::steady_clock::now();
     double search_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
 
     if (!search_result.has_value()) {
         std::cerr << "Search failed for " << index_type << std::endl;
         return 1;
     }
 
     float recall = gt_result ? GetKNNRecall(*gt_result, *search_result.value()) : 0.0f;
     std::cout << "Search OK (" << search_ms << " ms), Recall: " << std::fixed << std::setprecision(4)
               << recall << "\n";
     return 0;
 }
 
 }  // namespace
 
 int
 main(int argc, char** argv) {
     std::string prog = (argc > 0) ? argv[0] : "gpu_cuvs_test";
     std::string usage = "Usage:\n"
                         "  Save: " +
                         prog + " --index-type <type> --save-index <path> "
                                "<data_file> <query_file> <ground_truth_file> [k]\n"
                                "  Load: " +
                         prog + " --index-type <type> --load-index <path> "
                                "<query_file> <ground_truth_file> [k]\n"
                                "\n"
                                "  --index-type   One of: bruteforce, ivfflat, ivfpq, cagra\n"
                                "  --save-index   Build, query, save (exactly one with --load-index)\n"
                                "  --load-index   Load, query (exactly one with --save-index)\n"
                                "\n"
                                "Options: --use-bvecs, --use-fbin\n";
 
     std::string index_type_str;
     std::string save_path;
     std::string load_path;
     bool use_bvecs = false;
     bool use_fbin = false;
 
     for (int i = 1; i < argc; i++) {
         std::string arg = argv[i];
         if (arg == "--index-type") {
             if (i + 1 >= argc) {
                 std::cerr << "Error: --index-type requires a value\n" << usage;
                 return 1;
             }
             index_type_str = argv[++i];
         } else if (arg == "--save-index") {
             if (i + 1 >= argc) {
                 std::cerr << "Error: --save-index requires a path\n" << usage;
                 return 1;
             }
             save_path = argv[++i];
         } else if (arg == "--load-index") {
             if (i + 1 >= argc) {
                 std::cerr << "Error: --load-index requires a path\n" << usage;
                 return 1;
             }
             load_path = argv[++i];
         } else if (arg == "--use-bvecs") {
             use_bvecs = true;
         } else if (arg == "--use-fbin") {
             use_fbin = true;
         }
     }
 
     if (index_type_str.empty()) {
         std::cerr << "Error: --index-type is required\n" << usage;
         return 1;
     }
     if (save_path.empty() == load_path.empty()) {
         std::cerr << "Error: exactly one of --save-index or --load-index must be set\n" << usage;
         return 1;
     }
 
     std::vector<std::string> positional;
     for (int i = 1; i < argc; i++) {
         std::string arg = argv[i];
         if (arg == "--index-type" || arg == "--save-index" || arg == "--load-index") {
             i++;  // skip option value so it is not collected as positional
             continue;
         }
         if (arg == "--use-bvecs" || arg == "--use-fbin") {
             continue;
         }
         if (arg.empty() || arg[0] == '-') {
             continue;
         }
         positional.push_back(arg);
     }
 
     int k = 10;
     std::string query_file, ground_truth_file, data_file;
 
     if (!save_path.empty()) {
         if (positional.size() < 3) {
             std::cerr << "Error: save mode requires <data_file> <query_file> <ground_truth_file> [k]\n"
                       << usage;
             return 1;
         }
         data_file = positional[0];
         query_file = positional[1];
         ground_truth_file = positional[2];
         if (positional.size() >= 4) {
             k = std::stoi(positional[3]);
         }
     } else {
         if (positional.size() < 2) {
             std::cerr << "Error: load mode requires <query_file> <ground_truth_file> [k]\n" << usage;
             return 1;
         }
         query_file = positional[0];
         ground_truth_file = positional[1];
         if (positional.size() >= 3) {
             k = std::stoi(positional[2]);
         }
     }
 
     if (!use_bvecs && !use_fbin) {
         auto detect = [](const std::string& f) {
             std::string dl = f;
             std::transform(dl.begin(), dl.end(), dl.begin(), ::tolower);
             if (dl.length() >= 5 && dl.substr(dl.length() - 5) == ".fbin") return 2;
             if (dl.length() >= 6 && dl.substr(dl.length() - 6) == ".bvecs") return 1;
             return 0;
         };
         int q = detect(query_file);
         if (q == 1) use_bvecs = true;
         else if (q == 2) use_fbin = true;
         if (!save_path.empty()) {
             int d = detect(data_file);
             if (d == 1) use_bvecs = true;
             else if (d == 2) use_fbin = true;
         }
     }
 
     try {
         std::string index_type = resolve_index_type(index_type_str);
 
         std::vector<float> query_flat;
         std::vector<int64_t> gt_flat;
 
         std::cout << "Loading query vectors from " << query_file << " ...\n";
         auto query_ds = LoadVectorsFromFile(query_file, use_fbin, use_bvecs, query_flat);
         std::cout << "  Loaded " << query_ds->GetRows() << " vectors, dim=" << query_ds->GetDim()
                   << "\n";
 
         std::cout << "Loading ground truth from " << ground_truth_file << " ...\n";
         auto gt_ds = LoadGroundTruthFromFile(ground_truth_file, k, query_ds->GetRows(), gt_flat);
         std::cout << "  Loaded " << gt_ds->GetRows() << " query ground truth\n";
 
         if (!save_path.empty()) {
             std::vector<float> train_flat;
             std::cout << "Loading base vectors from " << data_file << " ...\n";
             auto train_ds = LoadVectorsFromFile(data_file, use_fbin, use_bvecs, train_flat);
             int dim = static_cast<int>(train_ds->GetDim());
             if (query_ds->GetDim() != static_cast<int64_t>(dim)) {
                 throw std::runtime_error("Query dim != base dim");
             }
             std::cout << "  Loaded " << train_ds->GetRows() << " vectors\n";
             return run_save_mode(index_type, train_ds, query_ds, gt_ds, dim, k, save_path);
         } else {
             return run_load_mode(index_type, query_ds, gt_ds, k, load_path);
         }
     } catch (const std::exception& e) {
         std::cerr << "Error: " << e.what() << std::endl;
         return 1;
     }
 }
 
 #else  // !KNOWHERE_WITH_CUVS
 
 int
 main(int, char**) {
     std::cerr << "knowhere was built without CuVS support (WITH_CUVS=OFF).\n";
     std::cerr << "Rebuild knowhere with: cmake -DWITH_CUVS=ON ...\n";
     return 1;
 }
 
 #endif  // KNOWHERE_WITH_CUVS
 