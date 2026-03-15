// OceanEterna Chat Server - HTTP server for chat interface
// Based on ocean_benchmark_fast.cpp with added HTTP endpoints

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <list>
#include <queue>
#include <condition_variable>
#include <omp.h>
#include <lz4frame.h>
#include <zstd.h>
#include <curl/curl.h>
#include <regex>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <filesystem>
#include <unistd.h>
#include <signal.h>
#include <climits>
#include "httplib.h"
#include "json.hpp"
#include "binary_manifest.hpp"
#include "porter_stemmer.hpp"
// BM25S removed in v3 - using original BM25 search which is faster

using json = nlohmann::json;
using namespace std;
namespace fs = std::filesystem;

// Graceful shutdown support
atomic<bool> g_shutdown_requested(false);
httplib::Server* g_server_ptr = nullptr;

// v4.2: Track active streaming threads for clean shutdown (fix detached thread bug)
mutex g_stream_threads_mutex;
vector<thread> g_stream_threads;

void signal_handler(int sig) {
    cerr << "\nReceived signal " << sig << ", shutting down gracefully..." << endl;
    g_shutdown_requested = true;
    if (g_server_ptr) {
        g_server_ptr->stop();
    }
}

// Debug mode: uncomment to enable verbose logging
// #define DEBUG_MODE
#ifdef DEBUG_MODE
#define DEBUG_LOG(x) do { cout << x << endl; } while(0)
#define DEBUG_ERR(x) do { cerr << x << endl << flush; } while(0)
#else
#define DEBUG_LOG(x)
#define DEBUG_ERR(x)
#endif

// Configuration system (Config struct, loader, env var overrides)
#include "config.hpp"

// Global config instance
Config g_config;

// Legacy compatibility aliases (used throughout the codebase)
#define USE_EXTERNAL_API (g_config.llm.use_external)
#define LOCAL_LLM_URL (g_config.llm.local_url)
#define LOCAL_MODEL (g_config.llm.local_model)
#define EXTERNAL_API_URL (g_config.llm.external_url)
#define EXTERNAL_API_KEY (g_config.llm.api_key)
#define EXTERNAL_MODEL (g_config.llm.external_model)
#define TOPK (g_config.search.top_k)
#define LLM_TIMEOUT_SEC (g_config.llm.timeout_sec)
#define HTTP_PORT (g_config.server.port)

string MANIFEST;
string STORAGE;

// DocMeta is defined in binary_manifest.hpp
// Prevent redefinition
#define DOCMETA_DEFINED

struct Corpus {
    vector<DocMeta> docs;
    unordered_map<string, vector<uint32_t>> inverted_index;
    // Step 22: Term frequency index for new documents (BM25 improvement)
    // Maps keyword -> (doc_id -> term_frequency)
    // Only populated for new docs; legacy docs assumed tf=1
    unordered_map<string, unordered_map<uint32_t, uint16_t>> tf_index;
    // low-mem: global keyword dictionary — keyword ID <-> string
    vector<string> keyword_dict;
    unordered_map<string, uint32_t> keyword_to_id;
    size_t total_tokens = 0;
    double avgdl = 0;
};

// low-mem: intern a keyword string, returning its uint32_t ID
// adds to dictionary if not already present
inline uint32_t intern_keyword(Corpus& corpus, const string& kw) {
    auto it = corpus.keyword_to_id.find(kw);
    if (it != corpus.keyword_to_id.end()) return it->second;
    uint32_t id = static_cast<uint32_t>(corpus.keyword_dict.size());
    corpus.keyword_to_id[kw] = id;
    corpus.keyword_dict.push_back(kw);
    return id;
}

// low-mem: resolve keyword ID to string
inline const string& resolve_keyword(const Corpus& corpus, uint32_t id) {
    return corpus.keyword_dict[id];
}

// Stemming support: stem cache and reverse mapping
// Protected by g_stem_mutex for thread-safe concurrent access
unordered_map<string, string> g_stem_cache;              // keyword -> stem
unordered_map<string, vector<string>> g_stem_to_keywords; // stem -> original keywords
shared_mutex g_stem_mutex;  // read-heavy: shared for lookups, unique for inserts

struct Hit {
    uint32_t doc_idx;
    double score;
    string context;
};

// v4.2: Safe prefix check (avoids substr on short strings)
inline bool has_prefix(const string& s, const string& prefix) {
    return s.length() >= prefix.length() && s.compare(0, prefix.length(), prefix) == 0;
}

// ChunkReference - stores source chunk info with relevance and snippet (Feature 2)
struct ChunkReference {
    string chunk_id;
    double relevance_score;
    string snippet;  // First 200 chars of chunk content

    json to_json() const {
        return {
            {"chunk_id", chunk_id},
            {"relevance_score", relevance_score},
            {"snippet", snippet}
        };
    }
};

// Global corpus (loaded once)
Corpus global_corpus;
Corpus chat_corpus;
string global_storage_path;

string chat_storage_path = "guten_9m_build/storage/chat_history.bin";
// No longer needed - conversation chunks are stored in main manifest
atomic<int> chat_chunk_counter{0};

// Thread safety: shared_mutex for corpus (read-heavy, write-rare)
// Use shared_lock for reads (search), unique_lock for writes (save_conversation_turn, add_file)
shared_mutex corpus_mutex;

// Thread safety: mutex for stats (updated on every query)
mutex stats_mutex;

// Thread safety: mutex for recent_turns_cache
mutex cache_mutex;

// Feature 2: O(1) chunk lookup by ID
unordered_map<string, uint32_t> chunk_id_to_index;

// Conversation turn structure
struct ConversationTurn {
    string user_message;
    string system_response;
    vector<ChunkReference> source_refs;  // Feature 2: Full source references with scores and snippets
    chrono::time_point<chrono::system_clock> timestamp;
    string chunk_id;  // This turn's chunk ID (CH01, CH02, etc.)
};

// Feature 2: Cache of recent conversation turns for "tell me more" without re-search
// LRU cache: list maintains insertion order (front=newest), map provides O(1) lookup
list<pair<string, ConversationTurn>> recent_turns_list;
unordered_map<string, list<pair<string, ConversationTurn>>::iterator> recent_turns_cache;
const size_t MAX_CACHED_TURNS = 20;  // low-memory: reduced from 100

// No longer needed - conversation chunks are stored in main corpus

// Stats tracking
struct Stats {
    int total_queries = 0;
    double total_search_time_ms = 0;
    double total_llm_time_ms = 0;
    size_t db_size_mb = 0;
} stats;

// Feature 4: Chapter guide for navigation
json chapter_guide;
string chapter_guide_path = "guten_9m_build/chapter_guide.json";

// Original file catalog — tracks preserved original documents
json originals_catalog = json::array();
string originals_catalog_path = "corpus/originals.json";
mutex originals_mutex;

// Build a lookup from chunk_id to original_file_id
unordered_map<string, string> chunk_to_original;

void load_originals_catalog() {
    ifstream f(originals_catalog_path);
    if (!f.is_open()) {
        originals_catalog = json::array();
        return;
    }
    try {
        originals_catalog = json::parse(f);
        // rebuild chunk_to_original lookup
        chunk_to_original.clear();
        for (const auto& entry : originals_catalog) {
            string fid = entry.value("file_id", "");
            if (entry.contains("chunk_ids") && entry["chunk_ids"].is_array()) {
                for (const auto& cid : entry["chunk_ids"]) {
                    chunk_to_original[cid.get<string>()] = fid;
                }
            }
        }
        cout << "Loaded originals catalog: " << originals_catalog.size()
             << " files, " << chunk_to_original.size() << " chunk mappings" << endl;
    } catch (const exception& e) {
        cerr << "Warning: Failed to parse originals catalog: " << e.what() << endl;
        originals_catalog = json::array();
    }
}

void save_originals_catalog() {
    try {
        ofstream f(originals_catalog_path);
        if (f.is_open()) {
            f << originals_catalog.dump(2);
        }
    } catch (const exception& e) {
        cerr << "Warning: Failed to save originals catalog: " << e.what() << endl;
    }
}
mutex chapter_guide_mutex;

// Forward declarations
void save_chapter_guide();

// Load manifest into memory
Corpus load_manifest(const string& path) {
    Corpus corpus;
    ifstream file(path);

    if (!file.is_open()) {
        cerr << "Failed to open manifest: " << path << endl;
        return corpus;
    }

    cout << "Loading manifest..." << flush;
    auto start = chrono::high_resolution_clock::now();

    string line;
    while (getline(file, line)) {
        if (line.empty()) continue;

        try {
            json obj = json::parse(line);

            DocMeta doc;
            doc.id = obj.value("chunk_id", "");
            // low-mem: skip summary in RAM
            doc.offset = obj.value("offset", 0ULL);
            doc.length = obj.value("length", 0ULL);
            doc.start = obj.value("token_start", 0U);
            doc.end = obj.value("token_end", 0U);
            doc.timestamp = obj.value("timestamp", 0LL);

            // v4.8: raised from 50 to 100 — more keywords for two-phase coverage scoring
            constexpr size_t MAX_KW_PER_CHUNK = 200;  // v7.11 winner
            if (obj.contains("keywords") && obj["keywords"].is_array()) {
                for (const auto& kw : obj["keywords"]) {
                    if (doc.keyword_ids.size() >= MAX_KW_PER_CHUNK) break;
                    string kw_str = kw.get<string>();
                    doc.keyword_ids.push_back(intern_keyword(corpus, kw_str));
                }
            }

            // v4.2: Load cross-reference fields if present
            doc.source_file = obj.value("source_file", "");
            doc.prev_chunk_id = obj.value("prev_chunk_id", "");
            doc.next_chunk_id = obj.value("next_chunk_id", "");

            corpus.docs.push_back(doc);

            // Build inverted index using resolved keyword strings
            uint32_t doc_idx = corpus.docs.size() - 1;
            for (uint32_t kid : corpus.docs.back().keyword_ids) {
                corpus.inverted_index[resolve_keyword(corpus, kid)].push_back(doc_idx);
            }

            // v4.7: load TF data from manifest if present
            if (obj.contains("tf") && obj["tf"].is_object()) {
                for (auto& [kw_str, tf_val] : obj["tf"].items()) {
                    uint16_t tf = static_cast<uint16_t>(tf_val.get<int>());
                    corpus.tf_index[kw_str][doc_idx] = tf;
                }
            }

            // Feature 2: Build chunk_id to index mapping for O(1) lookup
            chunk_id_to_index[doc.id] = corpus.docs.size() - 1;

            corpus.total_tokens += (doc.end - doc.start);
        } catch (const exception& e) {
            cerr << "\nError parsing line: " << e.what() << endl;
        }
    }

    corpus.avgdl = corpus.total_tokens / (double)corpus.docs.size();

    auto end = chrono::high_resolution_clock::now();
    double elapsed = chrono::duration<double, milli>(end - start).count();

    cout << " Done!" << endl;
    cout << "Loaded " << corpus.docs.size() << " chunks in " << elapsed << "ms" << endl;
    cout << "Total tokens: " << corpus.total_tokens << endl;

    return corpus;
}

// Compress chunk with Zstd (default for new data)
string compress_chunk(const string& input) {
    size_t bound = ZSTD_compressBound(input.size());
    string out(bound, '\0');
    size_t written = ZSTD_compress(out.data(), out.size(),
                                   input.data(), input.size(), 3);
    if (ZSTD_isError(written)) {
        throw runtime_error("Zstd compression failed: " + string(ZSTD_getErrorName(written)));
    }
    out.resize(written);
    return out;
}

// Step 20: Removed compress_chunk_lz4() - was dead code (never called)

// Auto-detect compression format and decompress
// LZ4 frame magic: 04 22 4D 18
// Zstd frame magic: 28 B5 2F FD
string decompress_chunk(const string& storage_path, uint64_t offset, uint64_t length) {
    ifstream file(storage_path, ios::binary);
    if (!file.is_open()) return "";

    vector<char> compressed(length);
    file.seekg(offset);
    file.read(compressed.data(), length);
    file.close();

    if (length < 4) return "";

    // Check magic bytes to detect format
    unsigned char m0 = (unsigned char)compressed[0];
    unsigned char m1 = (unsigned char)compressed[1];
    unsigned char m2 = (unsigned char)compressed[2];
    unsigned char m3 = (unsigned char)compressed[3];

    if (m0 == 0x28 && m1 == 0xB5 && m2 == 0x2F && m3 == 0xFD) {
        // Zstd format
        unsigned long long decomp_size = ZSTD_getFrameContentSize(compressed.data(), length);
        if (decomp_size == ZSTD_CONTENTSIZE_UNKNOWN || decomp_size == ZSTD_CONTENTSIZE_ERROR) {
            decomp_size = length * 10;  // fallback estimate
        }
        vector<char> decompressed(decomp_size);
        size_t result = ZSTD_decompress(decompressed.data(), decompressed.size(),
                                        compressed.data(), length);
        if (ZSTD_isError(result)) return "";
        return string(decompressed.data(), result);
    } else {
        // LZ4 format (default for existing data)
        // Step 19 fix: Safe iterative decompression with buffer growth
        // Old code: assumed 10:1 ratio, would overflow if ratio > 10:1
        const size_t MAX_DECOMP_SIZE = 100 * 1024 * 1024;  // 100 MB sanity limit
        size_t decomp_size = length * 10;  // Start with 10x estimate
        if (decomp_size > MAX_DECOMP_SIZE) decomp_size = MAX_DECOMP_SIZE;

        LZ4F_decompressionContext_t ctx;
        LZ4F_errorCode_t err = LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);
        if (LZ4F_isError(err)) return "";

        vector<char> decompressed(decomp_size);
        size_t src_pos = 0;
        size_t dst_pos = 0;

        while (src_pos < length) {
            size_t src_remaining = length - src_pos;
            size_t dst_remaining = decomp_size - dst_pos;

            size_t result = LZ4F_decompress(ctx,
                decompressed.data() + dst_pos, &dst_remaining,
                compressed.data() + src_pos, &src_remaining,
                nullptr);

            if (LZ4F_isError(result)) {
                LZ4F_freeDecompressionContext(ctx);
                return "";  // Decompression failed
            }

            src_pos += src_remaining;
            dst_pos += dst_remaining;

            // If we've consumed all input and result is 0, we're done
            if (result == 0 && src_pos >= length) break;

            // If we need more output space and haven't hit the limit
            if (dst_pos >= decomp_size * 0.9 && decomp_size < MAX_DECOMP_SIZE) {
                size_t new_size = min(decomp_size * 2, MAX_DECOMP_SIZE);
                decompressed.resize(new_size);
                decomp_size = new_size;
            }
        }

        LZ4F_freeDecompressionContext(ctx);

        return string(decompressed.data(), dst_pos);
    }
}

// Feature 2: O(1) chunk lookup by ID
// Returns pair<content, score> - score is 0 if not from a search result
pair<string, double> get_chunk_by_id(const string& chunk_id) {
    // Thread safety: acquire shared lock for read access
    shared_lock<shared_mutex> lock(corpus_mutex);

    auto it = chunk_id_to_index.find(chunk_id);
    if (it == chunk_id_to_index.end()) {
        return {"", -1.0};  // Not found
    }

    uint32_t idx = it->second;
    if (idx >= global_corpus.docs.size()) {
        return {"", -1.0};  // Index out of bounds
    }

    const DocMeta& doc = global_corpus.docs[idx];
    // Release lock before I/O (decompression doesn't need corpus access)
    uint64_t offset = doc.offset;
    uint64_t length = doc.length;
    lock.unlock();

    string content = decompress_chunk(global_storage_path, offset, length);
    return {content, 1.0};
}

// Feature 2: Create snippet from content (first 200 chars, trimmed to word boundary)
string create_snippet(const string& content, size_t max_len = 200) {
    if (content.length() <= max_len) {
        return content;
    }

    // Find last space before max_len
    size_t cut_pos = content.rfind(' ', max_len);
    if (cut_pos == string::npos || cut_pos < max_len / 2) {
        cut_pos = max_len;
    }

    return content.substr(0, cut_pos) + "...";
}

// v4.2: Get adjacent chunks for context expansion
// Returns content of neighboring chunks (prev/next) up to `window` hops
// Caller must NOT hold corpus_mutex (this function acquires it)
vector<pair<string, string>> get_adjacent_chunks(const string& chunk_id, int window) {
    vector<pair<string, string>> adjacent;  // (chunk_id, content)
    if (window <= 0) return adjacent;

    // Collect adjacent chunk IDs under lock
    vector<string> prev_ids, next_ids;
    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        // Walk backward
        string current = chunk_id;
        for (int i = 0; i < window; i++) {
            auto it = chunk_id_to_index.find(current);
            if (it == chunk_id_to_index.end() || it->second >= global_corpus.docs.size()) break;
            const auto& doc = global_corpus.docs[it->second];
            if (doc.prev_chunk_id.empty()) break;
            prev_ids.push_back(doc.prev_chunk_id);
            current = doc.prev_chunk_id;
        }
        // Walk forward
        current = chunk_id;
        for (int i = 0; i < window; i++) {
            auto it = chunk_id_to_index.find(current);
            if (it == chunk_id_to_index.end() || it->second >= global_corpus.docs.size()) break;
            const auto& doc = global_corpus.docs[it->second];
            if (doc.next_chunk_id.empty()) break;
            next_ids.push_back(doc.next_chunk_id);
            current = doc.next_chunk_id;
        }
    }

    // Decompress adjacent chunks (no lock needed)
    // Reverse prev_ids so they're in document order
    for (auto it = prev_ids.rbegin(); it != prev_ids.rend(); ++it) {
        auto [content, score] = get_chunk_by_id(*it);
        if (score >= 0 && !content.empty()) {
            adjacent.push_back({*it, content});
        }
    }
    // Add next chunks in order
    for (const auto& nid : next_ids) {
        auto [content, score] = get_chunk_by_id(nid);
        if (score >= 0 && !content.empty()) {
            adjacent.push_back({nid, content});
        }
    }

    return adjacent;
}

// Feature 3: Extract chunk IDs from context text
// Looks for patterns like: chunk_id or [chunk_id] or guten9m_DOC_123
vector<string> extract_chunk_ids_from_context(const string& text) {
    vector<string> chunk_ids;

    // Pattern matches: code_TYPE_number where TYPE is DOC|CHAT|CODE|FIX|FEAT
    // Also matches legacy format: code.number and CH### format
    regex chunk_id_pattern(R"(([a-zA-Z0-9]+_(?:DOC|CHAT|CODE|FIX|FEAT)_\d+)|([a-zA-Z0-9]+\.\d+)|(CH\d+))");

    auto begin = sregex_iterator(text.begin(), text.end(), chunk_id_pattern);
    auto end = sregex_iterator();

    for (auto it = begin; it != end; ++it) {
        string match = (*it).str();
        // Avoid duplicates
        if (find(chunk_ids.begin(), chunk_ids.end(), match) == chunk_ids.end()) {
            chunk_ids.push_back(match);
        }
    }

    return chunk_ids;
}

// Feature 3: Reconstruct full context from chunk IDs
// Returns the combined content of all referenced chunks
string reconstruct_context_from_ids(const vector<string>& chunk_ids) {
    string context;

    for (const string& chunk_id : chunk_ids) {
        auto [content, score] = get_chunk_by_id(chunk_id);
        if (score >= 0 && !content.empty()) {
            context += "[" + chunk_id + "]\n" + content + "\n\n---\n\n";
        }
    }

    return context;
}

// Feature 3: Format response with chunk references
// Includes source IDs for 667:1 compression ratio
string format_response_with_refs(const string& answer,
                                 const vector<ChunkReference>& refs,
                                 const string& turn_chunk_id) {
    string formatted = answer;

    // Add source references at the end
    if (!refs.empty()) {
        formatted += "\n\nSources: ";
        for (size_t i = 0; i < refs.size(); i++) {
            formatted += refs[i].chunk_id;
            if (i < refs.size() - 1) formatted += ", ";
        }
    }

    // Add this response's chunk ID
    if (!turn_chunk_id.empty()) {
        formatted += "\nThis response: " + turn_chunk_id;
    }

    return formatted;
}

// Feature 3: Calculate compression ratio
// chunk_ids vs full content size
double calculate_compression_ratio(const vector<string>& chunk_ids, const string& full_context) {
    if (full_context.empty()) return 0.0;

    size_t ids_size = 0;
    for (const auto& id : chunk_ids) {
        ids_size += id.length() + 2;  // +2 for ", " separator
    }

    return (double)full_context.length() / (double)max(ids_size, (size_t)1);
}

// Search engine (BM25 TAAT, stemming, keyword extraction)
#include "search_engine.hpp"

// v8.0: global mmap index pointer — set during startup if stems.idx exists
MmapIndex* g_mmap_idx = nullptr;
MmapIndex g_mmap_idx_storage;  // actual storage (pointer points here when loaded)

ShardEngine global_shards;  // v4.4: shard-parallel search tentacles

// v9.1: Multi-segment index for per-segment BM25
MultiSegmentIndex g_multi_seg;
// segment storage paths for chunk decompression
vector<string> g_segment_storage_paths;

// v9.1: Decompress a chunk from the correct segment's storage.bin
string decompress_segment_chunk(uint32_t global_doc_idx) {
    if (!g_multi_seg.loaded) return "";
    int seg_idx = find_segment_for_doc(g_multi_seg, global_doc_idx);
    if (seg_idx < 0 || seg_idx >= (int)g_multi_seg.segments.size()) return "";
    if (seg_idx >= (int)g_segment_storage_paths.size()) return "";

    // for now, use global_corpus docs if available (monolithic + segments share manifest)
    if ((uint32_t)global_doc_idx < global_corpus.docs.size()) {
        const auto& doc = global_corpus.docs[global_doc_idx];
        return decompress_chunk(global_storage_path, doc.offset, doc.length);
    }
    return "";
}

// v4.2: Ocean Biological Scaling Taxonomy — tentacle count per creature tier
struct CreatureTier {
    const char* name;
    const char* tag;
    size_t min_chunks;
    size_t max_chunks;
    int tentacles;
};

static const CreatureTier CREATURE_TIERS[] = {
    {"Seahorse",    "seahorse",    0,            99,           3},
    {"Starfish",    "starfish",    100,          999,          8},
    {"Jellyfish",   "jellyfish",   1000,         9999,         10},
    {"Squid",       "squid",       10000,        99999,        13},
    {"Octopus",     "octopus",     100000,       999999,       17},
    {"Giant Squid", "giant_squid", 1000000,      4999999,      24},
    {"Kraken",      "kraken",      5000000,      9999999,      30},
    {"Leviathan",   "leviathan",   10000000,     99999999,     40},
    {"Poseidon",    "poseidon",    100000000,    999999999,    70},
    {"Oceanus",     "oceanus",     1000000000,   9999999999,   100},
    {"GodMode",     "godmode",     10000000000,  (size_t)-1,   150},
};

static const int NUM_CREATURE_TIERS = sizeof(CREATURE_TIERS) / sizeof(CREATURE_TIERS[0]);

const CreatureTier& get_creature_tier(size_t corpus_size) {
    for (int i = NUM_CREATURE_TIERS - 1; i >= 0; i--) {
        if (corpus_size >= CREATURE_TIERS[i].min_chunks) {
            return CREATURE_TIERS[i];
        }
    }
    return CREATURE_TIERS[0];
}

int calculate_dynamic_topk() {
    shared_lock<shared_mutex> lock(corpus_mutex);
    size_t corpus_size = global_corpus.docs.size();
    if (corpus_size == 0) return g_config.search.top_k;
    const CreatureTier& tier = get_creature_tier(corpus_size);
    return tier.tentacles;
}

string get_creature_name() {
    shared_lock<shared_mutex> lock(corpus_mutex);
    return string(get_creature_tier(global_corpus.docs.size()).name);
}

int get_tentacle_count() {
    shared_lock<shared_mutex> lock(corpus_mutex);
    return get_creature_tier(global_corpus.docs.size()).tentacles;
}

// v4.2: Apply score threshold cutoff — drop results below threshold fraction of top score
void apply_score_threshold(vector<Hit>& hits, double threshold_fraction) {
    if (hits.empty() || threshold_fraction <= 0) return;

    double top_score = hits[0].score;
    double cutoff = top_score * threshold_fraction;

    auto it = remove_if(hits.begin(), hits.end(),
        [cutoff](const Hit& h) { return h.score < cutoff; });
    hits.erase(it, hits.end());
}

// CURL callback
// LLM client (CURL, query with retry + exponential backoff)
#include "llm_client.hpp"

// v4.2: Reranker client — calls Python sidecar to rerank BM25 results
// Returns reranked hits, or original hits if reranker is unavailable
vector<Hit> rerank_hits(const string& query, vector<Hit>& hits,
                        const vector<string>& decompressed_contents, int final_topk) {
    if (!g_config.reranker.enabled || hits.empty()) return hits;

    // Build request JSON
    json request;
    request["query"] = query;
    request["top_k"] = final_topk;
    json docs = json::array();

    // Map hit_idx to doc info
    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        for (size_t i = 0; i < hits.size() && i < decompressed_contents.size(); i++) {
            json doc;
            if (hits[i].doc_idx < global_corpus.docs.size()) {
                doc["chunk_id"] = global_corpus.docs[hits[i].doc_idx].id;
            }
            doc["content"] = decompressed_contents[i].substr(0, 512);
            doc["score"] = hits[i].score;
            doc["original_idx"] = (int)i;
            docs.push_back(doc);
        }
    }
    request["documents"] = docs;

    // Call reranker sidecar via CURL
    CURL* curl = curl_easy_init();
    if (!curl) return hits;

    curl_easy_setopt(curl, CURLOPT_URL, g_config.reranker.url.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)g_config.reranker.timeout_ms);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    string body = request.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());

    string response_str;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_str);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        DEBUG_ERR("Reranker sidecar unavailable: " << curl_easy_strerror(res));
        return hits;  // fall back to BM25 order
    }

    try {
        json resp = json::parse(response_str);
        if (!resp.contains("results")) return hits;

        // Rebuild hits in reranked order
        vector<Hit> reranked;
        for (const auto& r : resp["results"]) {
            int orig_idx = r.value("original_idx", -1);
            if (orig_idx >= 0 && orig_idx < (int)hits.size()) {
                Hit h = hits[orig_idx];
                h.score = r.value("rerank_score", h.score);
                reranked.push_back(h);
            }
        }

        if (!reranked.empty()) {
            DEBUG_LOG("Reranked " << hits.size() << " -> " << reranked.size()
                      << " results in " << resp.value("rerank_time_ms", 0.0) << "ms");
            return reranked;
        }
    } catch (const exception& e) {
        DEBUG_ERR("Reranker response parse error: " << e.what());
    }

    return hits;  // fall back to BM25 order
}

// Step 22: Generate keywords with term frequencies for conversation turn
// Returns pair<keywords, freq_map> where freq_map[keyword] = count
pair<vector<string>, unordered_map<string, uint16_t>> generate_chat_keywords_with_tf(
    const string& user_msg, const string& system_response) {

    unordered_map<string, uint16_t> word_freq;
    stringstream ss(user_msg + " " + system_response);
    string word;

    while (ss >> word) {
        // Simple keyword extraction - remove punctuation and convert to lowercase
        string clean_word;
        for (char c : word) {
            if (isalnum(c)) clean_word += tolower(c);
        }
        if (clean_word.length() >= 3 && clean_word.length() <= 20) {
            word_freq[clean_word]++;
        }
    }

    // Sort by frequency for top-20 selection
    vector<pair<string, uint16_t>> sorted_words(word_freq.begin(), word_freq.end());
    sort(sorted_words.begin(), sorted_words.end(),
         [](const pair<string, uint16_t>& a, const pair<string, uint16_t>& b) {
             return a.second > b.second;  // Higher frequency first
         });

    // Take top 20 keywords
    vector<string> keywords;
    unordered_map<string, uint16_t> tf_map;
    for (size_t i = 0; i < min(sorted_words.size(), (size_t)20); i++) {
        keywords.push_back(sorted_words[i].first);
        tf_map[sorted_words[i].first] = sorted_words[i].second;
    }

    return {keywords, tf_map};
}

// Legacy wrapper for backward compatibility
vector<string> generate_chat_keywords(const string& user_msg, const string& system_response) {
    auto [keywords, tf] = generate_chat_keywords_with_tf(user_msg, system_response);
    return keywords;
}

// Save conversation turn as a chunk in the main binary database
void save_conversation_turn(const ConversationTurn& turn) {
    // Create chunk content
    string chunk_content = "User: " + turn.user_message + "\n\nAssistant: " + turn.system_response;

    // Generate summary
    string summary = turn.user_message.substr(0, 100);
    if (turn.user_message.length() > 100) summary += "...";

    // Generate keywords with term frequencies (Step 22)
    auto [keywords, tf_map] = generate_chat_keywords_with_tf(turn.user_message, turn.system_response);

    // Compress the chunk content
    string compressed_data = compress_chunk(chunk_content);

    // Get current storage file size to determine offset
    ifstream storage_check(global_storage_path, ios::binary | ios::ate);
    uint64_t offset = 0;
    if (storage_check.is_open()) {
        offset = static_cast<uint64_t>(storage_check.tellg());
        storage_check.close();
    }

    // Append compressed data to main storage file
    ofstream storage_file(global_storage_path, ios::binary | ios::app);
    if (storage_file.is_open()) {
        storage_file.write(compressed_data.data(), compressed_data.size());
        storage_file.close();
    }

    // Create manifest entry for main manifest
    json manifest_entry;
    manifest_entry["code"] = "guten9m";
    manifest_entry["title"] = "Gutenberg 9M Tokens";
    manifest_entry["chunk_id"] = turn.chunk_id;
    manifest_entry["type"] = "CHAT";  // Feature 2: Mark as conversation type
    manifest_entry["index"] = global_corpus.docs.size();  // Index in corpus
    manifest_entry["token_start"] = 0;  // Conversation chunks don't have token positions
    manifest_entry["token_end"] = 0;
    manifest_entry["offset"] = offset;
    manifest_entry["length"] = compressed_data.size();
    manifest_entry["compression"] = "zstd";
    manifest_entry["summary"] = summary;
    manifest_entry["keywords"] = keywords;
    manifest_entry["user_message"] = turn.user_message;
    manifest_entry["system_response"] = turn.system_response;
    manifest_entry["timestamp"] = chrono::duration_cast<chrono::seconds>(turn.timestamp.time_since_epoch()).count();

    // Feature 2: Store full source references with scores and snippets
    json source_refs_json = json::array();
    for (const auto& ref : turn.source_refs) {
        source_refs_json.push_back(ref.to_json());
    }
    manifest_entry["source_refs"] = source_refs_json;

    // Append to main manifest
    ofstream manifest_file(MANIFEST, ios::app);
    if (manifest_file.is_open()) {
        manifest_file << manifest_entry.dump() << "\n";
        manifest_file.close();
    }

    // Add to in-memory corpus for immediate search availability
    // Thread safety: acquire exclusive lock for corpus mutation
    {
        unique_lock<shared_mutex> lock(corpus_mutex);

        DocMeta doc;
        doc.id = turn.chunk_id;
        // low-mem: don't store summary in RAM
        // low-mem: intern keyword strings to IDs
        for (const string& kw : keywords) {
            doc.keyword_ids.push_back(intern_keyword(global_corpus, kw));
        }
        doc.offset = offset;
        doc.length = compressed_data.size();
        doc.start = 0;
        doc.end = 0;
        doc.timestamp = chrono::duration_cast<chrono::seconds>(turn.timestamp.time_since_epoch()).count();
        global_corpus.docs.push_back(doc);

        // Update inverted index
        uint32_t doc_idx = global_corpus.docs.size() - 1;
        for (const string& kw : keywords) {
            global_corpus.inverted_index[kw].push_back(doc_idx);
        }

        // Step 22: Update tf_index with term frequencies for this doc
        for (const auto& [kw, freq] : tf_map) {
            global_corpus.tf_index[kw][doc_idx] = freq;
        }

        // Also update chunk_id_to_index under the same lock
        chunk_id_to_index[turn.chunk_id] = global_corpus.docs.size() - 1;
    }
}

// Load chat history from main manifest
void load_chat_history() {
    cout << "Loading conversation history from main manifest..." << endl;

    // Find the highest CH chunk number from the global corpus
    for (const auto& doc : global_corpus.docs) {
        if (has_prefix(doc.id, "CH") && doc.id.length() > 2) {
            try {
                string chunk_num = doc.id.substr(2);  // Remove "CH" prefix
                int num = stoi(chunk_num);
                if (num > chat_chunk_counter) chat_chunk_counter = num;
            } catch (const exception& e) {
                // Skip invalid chunk IDs
            }
        }
    }

    cout << "Found " << chat_chunk_counter << " conversation chunks in main database." << endl;
}

// Clear all conversation chunks from database
// v4.2: Hold corpus_mutex for entire operation to prevent race with save_conversation_turn()
bool clear_conversation_database() {
    try {
        // Thread safety: acquire exclusive lock for entire operation
        // This blocks search and save during clear, which is acceptable for a rare operation
        unique_lock<shared_mutex> lock(corpus_mutex);

        // Remove all CH chunks from in-memory corpus
        auto& docs = global_corpus.docs;
        docs.erase(remove_if(docs.begin(), docs.end(),
            [](const DocMeta& doc) {
                return doc.id.length() >= 2 && has_prefix(doc.id, "CH");
            }), docs.end());

        // Rebuild inverted index and chunk_id_to_index from scratch
        // This is necessary because erasing docs shifts all indices
        global_corpus.inverted_index.clear();
        global_corpus.tf_index.clear();
        chunk_id_to_index.clear();

        for (uint32_t i = 0; i < docs.size(); i++) {
            for (uint32_t kid : docs[i].keyword_ids) {
                global_corpus.inverted_index[resolve_keyword(global_corpus, kid)].push_back(i);
            }
            chunk_id_to_index[docs[i].id] = i;
        }

        // Recalculate corpus stats
        global_corpus.total_tokens = 0;
        for (const auto& doc : docs) {
            global_corpus.total_tokens += (doc.end - doc.start);
        }
        global_corpus.avgdl = docs.empty() ? 0 :
            static_cast<double>(global_corpus.total_tokens) / docs.size();

        // Rebuild manifest without conversation chunks (still under lock)
        ifstream input_file(MANIFEST);
        ofstream temp_file(MANIFEST + ".tmp");

        if (!input_file.is_open() || !temp_file.is_open()) {
            return false;
        }

        string line;
        while (getline(input_file, line)) {
            if (line.empty()) continue;

            try {
                json obj = json::parse(line);
                string chunk_id = obj.value("chunk_id", "");

                // Skip conversation chunks (CH*) when rebuilding manifest
                if (!has_prefix(chunk_id, "CH")) {
                    temp_file << line << "\n";
                }
            } catch (...) {
                // Keep non-JSON lines or corrupted entries (document chunks)
                temp_file << line << "\n";
            }
        }

        input_file.close();
        temp_file.close();

        // Replace original manifest with cleaned version
        if (rename((MANIFEST + ".tmp").c_str(), MANIFEST.c_str()) != 0) {
            return false;
        }

        // Reset conversation counter
        chat_chunk_counter = 0;

        cout << "Database cleared: All conversation chunks removed from memory and manifest." << endl;
        return true;

    } catch (const exception& e) {
        cerr << "Error clearing database: " << e.what() << endl;
        return false;
    }
}

// Counter for uploaded file chunks
atomic<int> uploaded_chunk_counter{0};

// Make string safe for JSON (keep only printable ASCII)
string make_json_safe(const string& input) {
    string result;
    result.reserve(input.size());
    for (unsigned char c : input) {
        if (c >= 32 && c < 127) {
            result += c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            result += ' ';
        }
        // Skip all other characters (non-ASCII, control chars)
    }
    return result;
}

// Structure to hold chunk processing results
struct ChunkData {
    size_t start_pos;
    size_t end_pos;
    string chunk_text;
    string chunk_id;
    string summary;
    string compressed;
    vector<string> keywords;
    unordered_map<string, uint16_t> tf_map;  // Step 22: term frequencies
    int token_start;
    int token_end;
    string source_file;  // v4.2: original filename for cross-references
};

// Add file content to index (PARALLELIZED)
json add_file_to_index(const string& filename, const string& content) {
    DEBUG_LOG("add_file_to_index called with filename: " << filename << ", content size: " << content.size());

    const int CHUNK_SIZE = 5000;  // ~1250 tokens worth of characters
    size_t content_len = content.length();
    DEBUG_ERR("Content length: " << content_len << " bytes");

    // Extract base filename without extension for chunk IDs
    string base_name = filename;
    size_t dot_pos = base_name.rfind('.');
    if (dot_pos != string::npos) {
        base_name = base_name.substr(0, dot_pos);
    }
    // Clean filename for chunk ID (ASCII alphanumeric only)
    string clean_name;
    for (unsigned char c : base_name) {
        if (c < 128 && isalnum(c)) clean_name += (char)tolower(c);
    }
    if (clean_name.empty()) clean_name = "file";

    // Determine content type from filename
    string content_type = "DOC";
    if (filename.find(".cpp") != string::npos || filename.find(".py") != string::npos ||
        filename.find(".js") != string::npos || filename.find(".ts") != string::npos ||
        filename.find(".go") != string::npos || filename.find(".rs") != string::npos) {
        content_type = "CODE";
    }

    // v4.2: Paragraph-safe chunking — NEVER split mid-paragraph
    // Strategy: split on \n\n boundaries, merge small paragraphs into chunks
    // up to CHUNK_SIZE, but never split a paragraph even if it exceeds CHUNK_SIZE
    DEBUG_ERR("v4.2 paragraph-safe chunking (target " << CHUNK_SIZE << " chars)");

    // Phase 1: Split content into paragraphs
    vector<pair<size_t, size_t>> paragraphs;  // (start, end) positions
    {
        size_t pos = 0;
        while (pos < content_len) {
            // Find next paragraph break (\n\n)
            size_t para_end = content.find("\n\n", pos);
            if (para_end == string::npos) {
                para_end = content_len;
            } else {
                para_end += 2;  // Include the \n\n
            }
            // Skip empty paragraphs (just whitespace)
            size_t text_start = pos;
            while (text_start < para_end && (content[text_start] == '\n' || content[text_start] == ' ' || content[text_start] == '\t'))
                text_start++;
            if (text_start < para_end) {
                paragraphs.push_back({pos, para_end});
            }
            pos = para_end;
        }
    }
    DEBUG_ERR("Found " << paragraphs.size() << " paragraphs");

    // Phase 2: Merge consecutive small paragraphs into chunks (parallel)
    int num_threads = omp_get_max_threads();
    vector<ChunkData> chunks;
    atomic<int> global_chunk_counter(uploaded_chunk_counter.load());

    // Single-pass merge: accumulate paragraphs until we'd exceed CHUNK_SIZE
    {
        size_t current_start = 0;
        size_t current_end = 0;
        bool started = false;

        for (size_t pi = 0; pi < paragraphs.size(); pi++) {
            auto [pstart, pend] = paragraphs[pi];
            size_t para_len = pend - pstart;

            if (!started) {
                current_start = pstart;
                current_end = pend;
                started = true;
                continue;
            }

            size_t current_len = current_end - current_start;

            // If adding this paragraph would exceed target AND we already have content,
            // emit current chunk and start new one
            if (current_len + para_len > CHUNK_SIZE && current_len > 0) {
                // Emit chunk
                ChunkData chunk;
                chunk.start_pos = current_start;
                chunk.end_pos = current_end;
                chunk.chunk_text = content.substr(current_start, current_end - current_start);
                int chunk_num = ++global_chunk_counter;
                chunk.chunk_id = clean_name + "_" + content_type + "_" + to_string(chunk_num);
                auto [kws, tf] = extract_text_keywords_with_tf(chunk.chunk_text);
                chunk.keywords = kws;
                chunk.tf_map = tf;
                chunk.summary = make_json_safe(chunk.chunk_text.substr(0, 150)).substr(0, 100);
                if (chunk.chunk_text.length() > 100) chunk.summary += "...";
                chunk.compressed = compress_chunk(chunk.chunk_text);
                chunks.push_back(move(chunk));

                // Start new chunk with this paragraph
                current_start = pstart;
                current_end = pend;
            } else {
                // Extend current chunk to include this paragraph
                current_end = pend;
            }
        }

        // Emit final chunk
        if (started && current_end > current_start) {
            ChunkData chunk;
            chunk.start_pos = current_start;
            chunk.end_pos = current_end;
            chunk.chunk_text = content.substr(current_start, current_end - current_start);
            int chunk_num = ++global_chunk_counter;
            chunk.chunk_id = clean_name + "_" + content_type + "_" + to_string(chunk_num);
            auto [kws, tf] = extract_text_keywords_with_tf(chunk.chunk_text);
            chunk.keywords = kws;
            chunk.tf_map = tf;
            chunk.summary = make_json_safe(chunk.chunk_text.substr(0, 150)).substr(0, 100);
            if (chunk.chunk_text.length() > 100) chunk.summary += "...";
            chunk.compressed = compress_chunk(chunk.chunk_text);
            chunks.push_back(move(chunk));
        }
    }

    // Compress remaining chunks in parallel
    #pragma omp parallel for schedule(dynamic)
    for (size_t i = 0; i < chunks.size(); i++) {
        // Keywords and compression already done above
        (void)i;
    }

    uploaded_chunk_counter = global_chunk_counter.load();
    DEBUG_ERR("Total chunks: " << chunks.size());
    DEBUG_ERR("Paragraph-safe chunking complete");

    // v4.2: Build cross-references (prev/next chunk linkage)
    string safe_filename = make_json_safe(filename);
    for (size_t i = 0; i < chunks.size(); i++) {
        chunks[i].source_file = safe_filename;
    }

    // PHASE 3: Write to storage and update in-memory index
    // v4.3 fix: single lock acquisition for entire batch (was per-chunk, caused lock contention)
    // v4.3 fix: update stem index on ingestion (was only built at startup)
    DEBUG_ERR("Phase 3: Writing to storage...");

    // Get current storage offset
    ifstream storage_check(global_storage_path, ios::binary | ios::ate);
    uint64_t current_offset = 0;
    if (storage_check.is_open()) {
        current_offset = static_cast<uint64_t>(storage_check.tellg());
        storage_check.close();
    }

    ofstream storage_file(global_storage_path, ios::binary | ios::app);
    ofstream manifest_file(MANIFEST, ios::app);
    if (!storage_file.is_open() || !manifest_file.is_open()) {
        return {{"error", "Failed to open storage files"}, {"success", false}};
    }

    int chunks_added = 0;
    uint64_t total_tokens = 0;  // v4.2: was int, overflows on multi-GB files

    // Write all chunks to disk first (no lock needed for file I/O)
    // Build manifest entries and track offsets
    struct ChunkMeta {
        uint64_t offset;
        uint64_t length;
        uint64_t token_start;
        uint64_t token_end;
    };
    vector<ChunkMeta> chunk_metas;
    chunk_metas.reserve(chunks.size());

    for (size_t ci = 0; ci < chunks.size(); ci++) {
        auto& chunk = chunks[ci];

        // Write compressed data to storage
        storage_file.write(chunk.compressed.data(), chunk.compressed.size());

        // Track offset/length for in-memory update
        chunk_metas.push_back({current_offset, (uint64_t)chunk.compressed.size(),
                               total_tokens, total_tokens + (uint64_t)(chunk.chunk_text.length() / 4)});

        // Create manifest entry
        json entry;
        entry["code"] = "guten9m";
        entry["chunk_id"] = make_json_safe(chunk.chunk_id);
        entry["type"] = content_type;
        entry["source_file"] = safe_filename;
        entry["token_start"] = total_tokens;
        entry["token_end"] = total_tokens + (uint64_t)(chunk.chunk_text.length() / 4);
        entry["offset"] = (uint64_t)current_offset;
        entry["length"] = (uint64_t)chunk.compressed.size();
        entry["compression"] = "zstd";
        entry["summary"] = chunk.summary;

        // v4.2: Cross-references
        if (ci > 0) entry["prev_chunk_id"] = make_json_safe(chunks[ci - 1].chunk_id);
        if (ci + 1 < chunks.size()) entry["next_chunk_id"] = make_json_safe(chunks[ci + 1].chunk_id);

        json kw_array = json::array();
        for (const auto& kw : chunk.keywords) {
            kw_array.push_back(make_json_safe(kw));
        }
        entry["keywords"] = kw_array;

        manifest_file << entry.dump() << "\n";

        current_offset += chunk.compressed.size();
        total_tokens += chunk.chunk_text.length() / 4;
        chunks_added++;

        if (chunks_added % 10000 == 0) {
            DEBUG_ERR("Written " << chunks_added << " / " << chunks.size() << " chunks");
        }
    }

    storage_file.close();
    manifest_file.close();

    // v4.3 fix: single lock acquisition for entire batch update
    // previously locked per-chunk which caused severe contention at ~480+ docs
    {
        unique_lock<shared_mutex> lock(corpus_mutex);

        for (size_t ci = 0; ci < chunks.size(); ci++) {
            auto& chunk = chunks[ci];
            auto& meta = chunk_metas[ci];

            DocMeta doc;
            doc.id = chunk.chunk_id;
            for (const string& kw : chunk.keywords) {
                doc.keyword_ids.push_back(intern_keyword(global_corpus, kw));
            }
            doc.offset = meta.offset;
            doc.length = meta.length;
            doc.start = meta.token_start;
            doc.end = meta.token_end;
            doc.source_file = safe_filename;
            if (ci > 0) doc.prev_chunk_id = chunks[ci - 1].chunk_id;
            if (ci + 1 < chunks.size()) doc.next_chunk_id = chunks[ci + 1].chunk_id;
            global_corpus.docs.push_back(doc);

            uint32_t doc_idx = global_corpus.docs.size() - 1;
            for (const string& kw : chunk.keywords) {
                global_corpus.inverted_index[kw].push_back(doc_idx);
            }
            chunk_id_to_index[chunk.chunk_id] = doc_idx;

            for (const auto& [kw, freq] : chunk.tf_map) {
                global_corpus.tf_index[kw][doc_idx] = freq;
            }
        }
    }

    // v4.3 fix: update stem index for newly ingested keywords
    // previously only built at startup, so dynamically added docs were unsearchable
    {
        unique_lock<shared_mutex> wlock(g_stem_mutex);
        for (const auto& chunk : chunks) {
            for (const string& kw : chunk.keywords) {
                if (g_stem_cache.find(kw) == g_stem_cache.end()) {
                    string stemmed = porter::stem(kw);
                    g_stem_cache[kw] = stemmed;
                    g_stem_to_keywords[stemmed].push_back(kw);
                }
            }
        }
    }

    DEBUG_ERR("Write complete: " << chunks_added << " chunks");

    // Update chapter guide (with defensive checks)
    {
        lock_guard<mutex> lock(chapter_guide_mutex);
        try {
            if (chapter_guide.contains("chunks") && chapter_guide["chunks"].contains("by_type")) {
                chapter_guide["chunks"]["total"] = (int)global_corpus.docs.size();
                auto& by_type = chapter_guide["chunks"]["by_type"];
                if (content_type == "DOC" && by_type.contains("DOC") && by_type["DOC"].is_number()) {
                    by_type["DOC"] = by_type["DOC"].get<int>() + chunks_added;
                } else if (content_type == "CODE" && by_type.contains("CODE") && by_type["CODE"].is_number()) {
                    by_type["CODE"] = by_type["CODE"].get<int>() + chunks_added;
                }
            }
            // Save inline (don't call save_chapter_guide() - would deadlock)
            try {
                ofstream guide_file(chapter_guide_path);
                if (guide_file.is_open()) {
                    guide_file << chapter_guide.dump(2);
                }
            } catch (const exception& e) {
                cerr << "Warning: Failed to save chapter guide: " << e.what() << endl;
            }
        } catch (const exception& e) {
            cerr << "Warning: Failed to update chapter guide: " << e.what() << endl;
        }
    }

    // Recalculate average document length
    size_t total_doc_length = 0;
    for (const auto& doc : global_corpus.docs) {
        total_doc_length += doc.end - doc.start;
    }
    global_corpus.avgdl = global_corpus.docs.empty() ? 0 :
        static_cast<double>(total_doc_length) / global_corpus.docs.size();

    DEBUG_ERR("Building result JSON...");
    // collect chunk IDs for original file tracking
    json chunk_id_list = json::array();
    for (const auto& chunk : chunks) {
        chunk_id_list.push_back(chunk.chunk_id);
    }

    json result;
    result["success"] = true;
    result["filename"] = make_json_safe(filename);
    result["chunks_added"] = chunks_added;
    result["tokens_added"] = (uint64_t)total_tokens;
    result["total_chunks"] = (uint64_t)global_corpus.docs.size();
    result["total_tokens"] = (uint64_t)(global_corpus.total_tokens + total_tokens);
    result["chunk_ids"] = chunk_id_list;
    cout << "Result JSON built successfully" << endl;
    return result;
}

// Detect if query is asking about user/self (triggers conversation-first search)
bool is_self_referential_query(const string& query) {
    string lower_query = query;
    transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);

    // Self-referential patterns - comprehensive list
    vector<string> self_patterns = {
        // Basic self-reference
        "my ", "i ", "me ", "myself", "mine",

        // Question patterns about self
        "am i", "do i", "did i", "have i", "will i", "can i", "should i", "would i",
        "was i", "were i", "could i", "may i", "might i",

        // "What" questions about self
        "what is my", "what's my", "whats my", "what are my", "what was my", "what were my",
        "what do i", "what did i", "what am i", "what was i", "what will i",
        "tell me about my", "about my", "regarding my",

        // "Who" questions
        "who am i", "who was i", "who is my", "who are my", "who did i",

        // "Where" questions
        "where am i", "where was i", "where do i", "where did i", "where is my",

        // "When" questions
        "when did i", "when do i", "when am i", "when was i", "when will i",

        // "How" questions
        "how did i", "how do i", "how am i", "how was i", "how will i", "how can i",

        // "Why" questions
        "why did i", "why do i", "why am i", "why was i", "why will i",

        // Memory/recall patterns
        "remember me", "recall me", "what do you know about me", "tell me what you know",
        "what did we talk about", "what did we discuss", "our conversation", "we talked",
        "you said i", "i told you", "i mentioned", "i said",

        // Specific personal items (common things people track)
        "my name", "my age", "my birthday", "my address", "my phone", "my email",
        "my job", "my work", "my career", "my profession", "my occupation",
        "my family", "my parents", "my children", "my kids", "my spouse",
        "my wife", "my husband", "my partner", "my boyfriend", "my girlfriend",
        "my friend", "my friends", "my brother", "my sister", "my mother", "my father",
        "my mom", "my dad", "my son", "my daughter",

        // Pets and animals
        "my cat", "my dog", "my pet", "my pets", "my bird", "my fish", "my rabbit",
        "my hamster", "my guinea pig", "my horse", "my turtle",

        // Possessions
        "my car", "my house", "my home", "my apartment", "my room", "my computer",
        "my phone", "my laptop", "my book", "my books", "my favorite",

        // Activities and interests
        "my hobby", "my hobbies", "my interest", "my interests", "my sport", "my music",
        "my movie", "my show", "my game", "my food", "my restaurant",

        // Health and body
        "my health", "my condition", "my illness", "my medication", "my doctor",
        "my weight", "my height", "my allergies",

        // Personal traits
        "my personality", "my character", "my mood", "my feelings", "my thoughts",
        "my opinion", "my preference", "my style", "my type",

        // Past actions and experiences
        "i went", "i visited", "i traveled", "i bought", "i sold", "i read", "i watched",
        "i played", "i learned", "i studied", "i worked", "i lived", "i moved",
        "i called", "i texted", "i emailed", "i met", "i saw", "i heard"
    };

    for (const string& pattern : self_patterns) {
        size_t pos = lower_query.find(pattern);
        if (pos != string::npos) {
            // v4.2: Ensure match is at word boundary to avoid false positives
            // e.g., "i " should not match inside "anti " or "alibi "
            if (pos == 0 || !isalpha((unsigned char)lower_query[pos - 1])) {
                return true;
            }
        }
    }

    return false;
}

// Search conversation chunks specifically
// NOTE: Caller must hold corpus_mutex (shared or unique)
vector<Hit> search_conversation_chunks_only_locked(const string& query, int max_results) {
    vector<Hit> hits;

    for (size_t i = 0; i < global_corpus.docs.size(); ++i) {
        const auto& doc = global_corpus.docs[i];

        // Only search conversation chunks (CH prefix)
        if (!has_prefix(doc.id, "CH")) continue;

        // Simple keyword matching for conversation chunks
        bool has_match = false;
        string query_lower = query;
        transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

        // Check if any keywords from the conversation match the query
        for (uint32_t kid : doc.keyword_ids) {
            const string& keyword = resolve_keyword(global_corpus, kid);
            string kw_lower = keyword;
            transform(kw_lower.begin(), kw_lower.end(), kw_lower.begin(), ::tolower);

            // Bidirectional matching: query contains keyword OR keyword contains query term
            if (query_lower.find(kw_lower) != string::npos || kw_lower.find(query_lower) != string::npos) {
                has_match = true;
                break;
            }
        }

        // low-mem: summary not stored in RAM, skip summary matching

        if (has_match) {
            Hit hit;
            hit.doc_idx = i;
            hit.score = 15.0;  // High score for conversation matches

            // Boost recent conversations with timestamp-based scoring
            if (doc.timestamp > 0) {
                long long current_time = chrono::duration_cast<chrono::seconds>(
                    chrono::system_clock::now().time_since_epoch()).count();

                // Calculate age in hours
                double age_hours = (current_time - doc.timestamp) / 3600.0;

                // Apply exponential decay: newer conversations get higher scores
                // Decay factor: score multiplier drops by half every 24 hours
                double time_boost = exp(-age_hours / 24.0);
                hit.score += time_boost * 10.0;  // Up to 10 point boost for very recent
            }

            hits.push_back(hit);
        }
    }

    // Sort by score (most relevant first)
    sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        return a.score > b.score;
    });

    // Limit results
    if (hits.size() > max_results) {
        hits.resize(max_results);
    }

    return hits;
}

// Feature 2: Handle "show me sources" query - returns sources for a given turn
json handle_source_query(const string& turn_id) {
    json response;

    // Check cache first
    // v4.2: Proper LRU — move accessed item to front on cache hit
    {
        lock_guard<mutex> lock(cache_mutex);
        auto it = recent_turns_cache.find(turn_id);
        if (it != recent_turns_cache.end()) {
            // LRU promotion: move to front of list
            recent_turns_list.splice(recent_turns_list.begin(), recent_turns_list, it->second);
            const ConversationTurn& turn = it->second->second;

            json sources = json::array();
            for (const auto& ref : turn.source_refs) {
                sources.push_back(ref.to_json());
            }

            response["success"] = true;
            response["turn_id"] = turn_id;
            response["source_count"] = turn.source_refs.size();
            response["sources"] = sources;
            return response;
        }
    }

    // Not in cache - search manifest for turn_id (get_chunk_by_id has its own lock)
    auto [content, score] = get_chunk_by_id(turn_id);
    if (score < 0) {
        response["success"] = false;
        response["error"] = "Turn ID not found: " + turn_id;
        return response;
    }

    if (content.empty()) {
        response["success"] = false;
        response["error"] = "Could not retrieve turn content";
        return response;
    }

    response["success"] = true;
    response["turn_id"] = turn_id;
    response["content"] = content;
    response["note"] = "Full source_refs available in cached turns only";
    return response;
}

// Feature 2: Handle "tell me more" - uses cached refs, no re-search needed
json handle_tell_me_more(const string& prev_turn_id, const string& aspect) {
    json response;

    // Check cache for previous turn and extract what we need
    // v4.2: Proper LRU — promote on access
    ConversationTurn prev_turn;
    {
        lock_guard<mutex> lock(cache_mutex);
        auto it = recent_turns_cache.find(prev_turn_id);
        if (it == recent_turns_cache.end()) {
            response["success"] = false;
            response["error"] = "Previous turn not in cache. Use regular search instead.";
            response["prev_turn_id"] = prev_turn_id;
            return response;
        }
        // LRU promotion: move to front of list
        recent_turns_list.splice(recent_turns_list.begin(), recent_turns_list, it->second);
        prev_turn = it->second->second;  // Copy the turn data
    }

    // Build context from cached source_refs - no BM25 search needed!
    string context;
    for (const auto& ref : prev_turn.source_refs) {
        auto [content, _] = get_chunk_by_id(ref.chunk_id);
        if (!content.empty()) {
            context += "[" + ref.chunk_id + " (score: " + to_string(ref.relevance_score) + ")]\n";
            context += content + "\n\n---\n\n";
        }
    }

    if (context.empty()) {
        response["success"] = false;
        response["error"] = "No source content available from cached references";
        return response;
    }

    // v4.2: Truncate context to max size
    if (context.length() > (size_t)g_config.search.max_context_chars) {
        context = context.substr(0, g_config.search.max_context_chars) + "\n\n[Context truncated]";
    }

    // Build prompt focused on the requested aspect
    string prompt = "Based on the context below (same sources as the previous answer), "
                   "provide more detail about: " + aspect + "\n\n"
                   "Previous question: " + prev_turn.user_message + "\n"
                   "Previous answer summary: " + prev_turn.system_response.substr(0, 200) + "...\n\n"
                   "Context:\n" + context +
                   "\n\nProvide more detail about: " + aspect + "\n\nAnswer:";

    // Query LLM
    auto [answer, llm_ms] = query_llm(prompt);

    // Create new turn reusing the same sources
    ConversationTurn new_turn;
    new_turn.user_message = "Tell me more about: " + aspect;
    new_turn.system_response = answer;
    new_turn.source_refs = prev_turn.source_refs;  // Reuse same sources - no re-search!
    new_turn.timestamp = chrono::system_clock::now();
    new_turn.chunk_id = "CH" + to_string(++chat_chunk_counter);

    // Save and cache the new turn
    save_conversation_turn(new_turn);

    // Update cache
    // Thread safety: lock cache for write
    {
        lock_guard<mutex> lock(cache_mutex);
        if (recent_turns_cache.size() >= MAX_CACHED_TURNS) {
            // Evict oldest entry (back of list)
            auto& oldest = recent_turns_list.back();
            recent_turns_cache.erase(oldest.first);
            recent_turns_list.pop_back();
        }
        recent_turns_list.push_front({new_turn.chunk_id, new_turn});
        recent_turns_cache[new_turn.chunk_id] = recent_turns_list.begin();
    }

    // Feature 3: Format answer with chunk references
    string formatted_answer = format_response_with_refs(answer, new_turn.source_refs, new_turn.chunk_id);

    response["success"] = true;
    response["answer"] = answer;
    response["formatted_answer"] = formatted_answer;  // Feature 3
    response["turn_id"] = new_turn.chunk_id;
    response["sources_reused"] = prev_turn.source_refs.size();
    response["llm_time_ms"] = llm_ms;
    response["search_time_ms"] = 0;  // No search needed!
    response["note"] = "Sources reused from previous turn - no BM25 search performed";

    return response;
}

// Get system stats
json get_system_stats() {
    json stats_json;

    // CPU usage (simple approximation)
    static long last_idle = 0, last_total = 0;
    ifstream stat_file("/proc/stat");
    string line;
    getline(stat_file, line);
    stat_file.close();

    stringstream ss(line);
    string cpu;
    long user, nice, system, idle, iowait, irq, softirq;
    ss >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq;

    long total = user + nice + system + idle + iowait + irq + softirq;
    long total_diff = total - last_total;
    long idle_diff = idle - last_idle;

    double cpu_usage = 0;
    if (total_diff > 0) {
        cpu_usage = 100.0 * (1.0 - (double)idle_diff / total_diff);
    }

    last_idle = idle;
    last_total = total;

    // RAM usage
    struct sysinfo si;
    sysinfo(&si);
    double ram_total_gb = si.totalram / (1024.0 * 1024.0 * 1024.0);
    double ram_used_gb = (si.totalram - si.freeram) / (1024.0 * 1024.0 * 1024.0);
    double ram_percent = (ram_used_gb / ram_total_gb) * 100.0;

    // Database size
    struct stat st;
    size_t db_size = 0;
    if (stat(global_storage_path.c_str(), &st) == 0) {
        db_size = st.st_size / (1024 * 1024);
    }

    stats_json["cpu_usage"] = round(cpu_usage * 10) / 10.0;
    stats_json["ram_usage"] = round(ram_percent * 10) / 10.0;
    stats_json["ram_used_gb"] = round(ram_used_gb * 10) / 10.0;
    stats_json["ram_total_gb"] = round(ram_total_gb * 10) / 10.0;

    // low-memory: report process RSS
    {
        long rss_kb = 0;
        ifstream proc_status("/proc/self/status");
        string pline;
        while (getline(proc_status, pline)) {
            if (pline.substr(0, 6) == "VmRSS:") {
                stringstream pss(pline.substr(6));
                pss >> rss_kb;
                break;
            }
        }
        stats_json["process_rss_mb"] = rss_kb / 1024;
    }

    // Thread safety: lock stats and corpus for read
    {
        lock_guard<mutex> slock(stats_mutex);
        shared_lock<shared_mutex> clock(corpus_mutex);

        stats_json["total_tokens"] = global_corpus.total_tokens;
        stats_json["db_size_mb"] = db_size;
        stats_json["chunks_loaded"] = global_corpus.docs.size();
        stats_json["creature_tier"] = get_creature_name();
        stats_json["tentacles"] = get_tentacle_count();
        stats_json["total_queries"] = stats.total_queries;
        stats_json["avg_search_ms"] = stats.total_queries > 0 ? stats.total_search_time_ms / stats.total_queries : 0;
        stats_json["avg_llm_ms"] = stats.total_queries > 0 ? stats.total_llm_time_ms / stats.total_queries : 0;
        stats_json["unique_keywords"] = global_corpus.inverted_index.size();
        stats_json["stem_cache_size"] = g_stem_cache.size();
        stats_json["stem_reverse_size"] = g_stem_to_keywords.size();
    }

    return stats_json;
}

// Feature 4: Load chapter guide from file
void load_chapter_guide() {
    ifstream file(chapter_guide_path);
    if (file.is_open()) {
        try {
            file >> chapter_guide;
            cout << "Loaded chapter guide v" << chapter_guide.value("version", "unknown") << endl;
        } catch (const exception& e) {
            cerr << "Failed to parse chapter guide: " << e.what() << endl;
            // Create minimal guide
            chapter_guide = {
                {"version", "3.0"},
                {"title", "Unknown"},
                {"code", "unknown"},
                {"chunks", {{"total", global_corpus.docs.size()}}},
                {"conversations", {{"count", 0}, {"summaries", json::array()}}},
                {"code_files", {{"count", 0}, {"files", json::array()}}},
                {"fixes", {{"count", 0}, {"entries", json::array()}}},
                {"features", {{"count", 0}, {"entries", json::array()}}}
            };
        }
    } else {
        // Create default guide from corpus
        chapter_guide = {
            {"version", "3.0"},
            {"title", "Loaded Corpus"},
            {"code", "corpus"},
            {"chunks", {{"total", global_corpus.docs.size()}}},
            {"conversations", {{"count", 0}, {"summaries", json::array()}}},
            {"code_files", {{"count", 0}, {"files", json::array()}}},
            {"fixes", {{"count", 0}, {"entries", json::array()}}},
            {"features", {{"count", 0}, {"entries", json::array()}}}
        };
    }
}

// Feature 4: Update chapter guide with new conversation
void update_chapter_guide_conversation(const ConversationTurn& turn) {
    lock_guard<mutex> lock(chapter_guide_mutex);

    // Ensure conversations is an object (may be null from empty chapter guide)
    if (!chapter_guide.contains("conversations") || chapter_guide["conversations"].is_null()) {
        chapter_guide["conversations"] = json::object();
    }

    // Update conversation count
    int conv_count = chapter_guide["conversations"].value("count", 0) + 1;
    chapter_guide["conversations"]["count"] = conv_count;

    // Add summary entry
    json summary_entry = {
        {"chunk_id", turn.chunk_id},
        {"summary", turn.user_message.substr(0, 100)},
        {"source_refs", json::array()}
    };

    for (const auto& ref : turn.source_refs) {
        summary_entry["source_refs"].push_back(ref.chunk_id);
    }

    chapter_guide["conversations"]["summaries"].push_back(summary_entry);

    // Limit to last 100 conversations in guide
    auto& summaries = chapter_guide["conversations"]["summaries"];
    if (summaries.size() > 100) {
        summaries.erase(summaries.begin());
    }
}

// Feature 4: Save chapter guide to file
void save_chapter_guide() {
    lock_guard<mutex> lock(chapter_guide_mutex);

    ofstream file(chapter_guide_path);
    if (file.is_open()) {
        file << chapter_guide.dump(2);
    }
}

// Feature 4: Query discussions about a code chunk
json query_code_discussions(const string& code_chunk_id) {
    json result = {
        {"chunk_id", code_chunk_id},
        {"discussions", json::array()}
    };

    // Search through conversation summaries for references to this chunk
    lock_guard<mutex> lock(chapter_guide_mutex);
    if (chapter_guide.contains("conversations") &&
        chapter_guide["conversations"].contains("summaries")) {

        for (const auto& conv : chapter_guide["conversations"]["summaries"]) {
            if (conv.contains("source_refs")) {
                for (const auto& ref : conv["source_refs"]) {
                    if (ref.get<string>() == code_chunk_id) {
                        result["discussions"].push_back({
                            {"conversation_id", conv["chunk_id"]},
                            {"summary", conv["summary"]}
                        });
                        break;
                    }
                }
            }
        }
    }

    result["count"] = result["discussions"].size();
    return result;
}

// Feature 4: Query fixes for a file
json query_fixes_for_file(const string& filename) {
    json result = {
        {"filename", filename},
        {"fixes", json::array()}
    };

    lock_guard<mutex> lock(chapter_guide_mutex);

    // Find chunk IDs for this file
    vector<string> file_chunk_ids;
    if (chapter_guide.contains("code_files") &&
        chapter_guide["code_files"].contains("files")) {

        for (const auto& file : chapter_guide["code_files"]["files"]) {
            if (file["filename"].get<string>().find(filename) != string::npos) {
                for (const auto& chunk_id : file["chunk_ids"]) {
                    file_chunk_ids.push_back(chunk_id.get<string>());
                }
            }
        }
    }

    // Find fix entries related to these chunks
    if (chapter_guide.contains("fixes") &&
        chapter_guide["fixes"].contains("entries")) {

        for (const auto& fix : chapter_guide["fixes"]["entries"]) {
            if (fix.contains("affected_code_chunks")) {
                for (const auto& chunk : fix["affected_code_chunks"]) {
                    string chunk_str = chunk.get<string>();
                    if (find(file_chunk_ids.begin(), file_chunk_ids.end(), chunk_str) != file_chunk_ids.end()) {
                        result["fixes"].push_back(fix);
                        break;
                    }
                }
            }
        }
    }

    result["count"] = result["fixes"].size();
    result["chunk_ids"] = file_chunk_ids;
    return result;
}

// Feature 4: Query feature implementation
json query_feature_implementation(const string& feature_id) {
    json result = {
        {"feature_id", feature_id},
        {"code_chunks", json::array()},
        {"conversations", json::array()}
    };

    lock_guard<mutex> lock(chapter_guide_mutex);

    if (chapter_guide.contains("features") &&
        chapter_guide["features"].contains("entries")) {

        for (const auto& feat : chapter_guide["features"]["entries"]) {
            if (feat.contains("feature_id") &&
                feat["feature_id"].get<string>().find(feature_id) != string::npos) {

                if (feat.contains("code_chunks")) {
                    result["code_chunks"] = feat["code_chunks"];
                }
                if (feat.contains("conversation_refs")) {
                    result["conversations"] = feat["conversation_refs"];
                }
                break;
            }
        }
    }

    return result;
}

// v9.1: LLM Scanner — filter BM25 candidates with a fast/cheap scanner LLM
// At 50B scale, BM25 returns many noise candidates. The scanner does YES/NO
// relevance filtering before passing survivors to the main LLM.
// Uses curl_multi batching for ~1s latency over 64 candidates.
vector<Hit> scanner_filter_chunks(const string& question, const vector<Hit>& candidates) {
    if (!g_config.scanner.enabled || candidates.empty()) return candidates;
    if (g_config.llm.api_key.empty()) {
        cerr << "[scanner] No API key set (OCEAN_API_KEY), skipping scanner" << endl;
        return candidates;
    }

    int max_scan = min((int)candidates.size(), g_config.scanner.max_candidates);
    int max_keep = g_config.scanner.max_keep;
    int batch_size = g_config.scanner.batch_size;

    auto scan_start = chrono::high_resolution_clock::now();

    // collect chunk texts for candidates (decompress under lock)
    struct CandidateInfo {
        int orig_idx;
        string text;
        uint64_t offset;
        uint64_t length;
    };
    vector<CandidateInfo> cands;
    cands.reserve(max_scan);

    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        for (int i = 0; i < max_scan; i++) {
            if (candidates[i].doc_idx < global_corpus.docs.size()) {
                const auto& doc = global_corpus.docs[candidates[i].doc_idx];
                cands.push_back({i, "", doc.offset, doc.length});
            }
        }
    }

    // decompress outside lock
    for (auto& c : cands) {
        string text = decompress_chunk(global_storage_path, c.offset, c.length);
        // truncate to ~2000 chars for scanner prompt
        if (text.size() > 2000) text.resize(2000);
        c.text = make_json_safe(text);
    }

    // build scanner prompts
    vector<string> prompts;
    prompts.reserve(cands.size());
    for (const auto& c : cands) {
        prompts.push_back(
            "Question: " + question + "\n\n"
            "Chunk:\n" + c.text + "\n\n"
            "Does this chunk contain information relevant to answering the question? Reply YES or NO only.");
    }

    // process in batches using curl_multi
    vector<Hit> filtered;
    filtered.reserve(max_keep);
    double total_scanner_ms = 0;

    for (int batch_start = 0; batch_start < (int)prompts.size() && (int)filtered.size() < max_keep; batch_start += batch_size) {
        int batch_end = min(batch_start + batch_size, (int)prompts.size());
        vector<string> batch_prompts(prompts.begin() + batch_start, prompts.begin() + batch_end);

        auto batch_results = query_scanner_llm_batch(batch_prompts);

        for (int j = 0; j < (int)batch_results.size(); j++) {
            int cand_idx = batch_start + j;
            const auto& [response, ms] = batch_results[j];
            total_scanner_ms = max(total_scanner_ms, ms);

            // check for YES (case-insensitive, handles "Yes", "YES", "yes", "Yes.")
            string upper_resp = response;
            transform(upper_resp.begin(), upper_resp.end(), upper_resp.begin(), ::toupper);
            if (upper_resp.find("YES") != string::npos) {
                int orig_idx = cands[cand_idx].orig_idx;
                filtered.push_back(candidates[orig_idx]);
                if ((int)filtered.size() >= max_keep) break;
            }
        }
    }

    auto scan_end = chrono::high_resolution_clock::now();
    double scan_ms = chrono::duration<double, milli>(scan_end - scan_start).count();

    cerr << "[scanner] " << filtered.size() << "/" << max_scan << " candidates passed ("
         << (int)scan_ms << "ms, " << prompts.size() << " scanned)" << endl;

    // fallback: if scanner filtered everything out, return original top-K
    if (filtered.empty()) {
        cerr << "[scanner] All candidates filtered — falling back to BM25 top-" << max_keep << endl;
        vector<Hit> fallback(candidates.begin(),
                            candidates.begin() + min((int)candidates.size(), max_keep));
        return fallback;
    }

    return filtered;
}

// v4.4: LLM-guided search refinement loop
// search → LLM judges results → FOUND (return) or REFINE (search with better terms)
// binary loop: keeps going until LLM says FOUND or max iterations reached
struct RefinementResult {
    vector<Hit> hits;
    string final_query;
    int iterations = 0;
    double refinement_ms = 0;
};

// ask LLM to judge if search results answer the question
string judge_chunks(const string& question, const vector<Hit>& hits) {
    string chunks_summary;
    int max_preview = min((int)hits.size(), 8);

    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        for (int i = 0; i < max_preview && i < (int)hits.size(); i++) {
            if (hits[i].doc_idx < global_corpus.docs.size()) {
                const auto& doc = global_corpus.docs[hits[i].doc_idx];
                string text = decompress_chunk(global_storage_path, doc.offset, doc.length);
                string preview = make_json_safe(text.substr(0, 300));
                chunks_summary += "[Chunk " + to_string(i+1) + "] " + preview + "\n\n";
            }
        }
    }

    string judge_prompt =
        "You are a search quality judge. A user asked a question and the search engine returned these chunks.\n\n"
        "Question: " + question + "\n\n"
        "Retrieved chunks:\n" + chunks_summary + "\n"
        "Do these chunks contain the information needed to answer the question?\n\n"
        "If YES: Reply with exactly: FOUND\n"
        "If NO: Reply with exactly: REFINE: <better search keywords that would find the right answer>\n\n"
        "Reply with FOUND or REFINE only. No explanation.";

    auto [response, ms] = query_llm(judge_prompt);
    return response;
}

// parse LLM refinement response
pair<bool, string> parse_refinement_response(const string& response) {
    if (response.find("FOUND") != string::npos) return {true, ""};

    size_t refine_pos = response.find("REFINE:");
    if (refine_pos != string::npos) {
        string terms = response.substr(refine_pos + 7);
        size_t start = terms.find_first_not_of(" \t\n\r");
        if (start != string::npos) terms = terms.substr(start);
        size_t end = terms.find_last_not_of(" \t\n\r.\"'");
        if (end != string::npos) terms = terms.substr(0, end + 1);
        return {false, terms};
    }
    return {true, ""};  // couldn't parse — treat as found
}

// v4.6 refinement loop: search → judge → refine → merge best results across iterations
RefinementResult search_with_refinement(const string& question, int topk, int max_iter = 3) {
    RefinementResult result;
    auto t0 = chrono::high_resolution_clock::now();
    string current_query = question;

    // track best hits across all iterations (merge, don't replace)
    unordered_map<uint32_t, Hit> best_hits;  // doc_idx -> best Hit

    auto merge_hits = [&](const vector<Hit>& new_hits) {
        for (const auto& h : new_hits) {
            auto it = best_hits.find(h.doc_idx);
            if (it == best_hits.end() || h.score > it->second.score) {
                best_hits[h.doc_idx] = h;
            }
        }
    };

    auto get_merged_topk = [&]() -> vector<Hit> {
        vector<Hit> merged;
        merged.reserve(best_hits.size());
        for (auto& [idx, hit] : best_hits) merged.push_back(hit);
        sort(merged.begin(), merged.end(), [](const Hit& a, const Hit& b) {
            return a.score > b.score;
        });
        if ((int)merged.size() > topk) merged.resize(topk);
        return merged;
    };

    for (int iter = 0; iter <= max_iter; iter++) {
        vector<Hit> hits;
        {
            shared_lock<shared_mutex> lock(corpus_mutex);
            // v9.1: prefer multi-segment search if loaded
            if (g_multi_seg.loaded) {
                hits = search_multi_segment(g_multi_seg, current_query, topk);
            } else if (global_shards.shard_count > 0) {
                hits = search_unified_twophase(global_shards, global_corpus, current_query, topk);
            } else {
                hits = search_bm25_twophase(global_corpus, current_query, topk);
            }
            if (!g_multi_seg.loaded) {
                apply_source_coherence(hits, global_corpus);
            }
        }

        merge_hits(hits);

        // last iteration — return merged best
        if (iter == max_iter) {
            result.hits = get_merged_topk();
            result.final_query = current_query;
            result.iterations = iter;
            break;
        }

        // ask LLM to judge using current merged best
        auto current_best = get_merged_topk();
        string judgment = judge_chunks(question, current_best);
        auto [found, new_terms] = parse_refinement_response(judgment);

        if (found) {
            result.hits = move(current_best);
            result.final_query = current_query;
            result.iterations = iter;
            break;
        }

        // REFINE: use new search terms but keep all previous results
        if (!new_terms.empty()) {
            current_query = new_terms;
        } else {
            current_query = question + " " + question;
        }
    }

    result.refinement_ms = chrono::duration<double, milli>(
        chrono::high_resolution_clock::now() - t0).count();
    return result;
}

// Handle chat query
json handle_chat(const string& question) {
    json response;

    auto search_start = chrono::high_resolution_clock::now();

    // v4.2: Dynamic top_k based on corpus size
    int effective_topk = calculate_dynamic_topk();

    // v4.4: LLM refinement loop — search → judge → refine → re-search
    vector<Hit> hits;
    int refinement_iterations = 0;
    double refinement_ms = 0;

    // BM25 search — oversample if scanner is enabled
    int search_topk = g_config.scanner.enabled ? g_config.scanner.max_candidates : effective_topk;
    auto refine_result = search_with_refinement(question, search_topk);
    hits = move(refine_result.hits);
    refinement_iterations = refine_result.iterations;
    refinement_ms = refine_result.refinement_ms;

    // v9.1: scanner filter for chat
    if (g_config.scanner.enabled && !hits.empty()) {
        hits = scanner_filter_chunks(question, hits);
    }
    if ((int)hits.size() > effective_topk) hits.resize(effective_topk);

    // Apply score threshold
    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        apply_score_threshold(hits, g_config.search.score_threshold);
    }

    auto search_end = chrono::high_resolution_clock::now();
    double search_ms = chrono::duration<double, milli>(search_end - search_start).count();

    // Build context from search results
    // Thread safety: extract doc metadata under lock, then decompress outside lock
    string context;
    bool has_conversation_context = false;
    bool has_document_context = false;

    struct HitMeta {
        string id;
        uint64_t offset;
        uint64_t length;
        double score;
    };
    vector<HitMeta> hit_metas;

    {
        shared_lock<shared_mutex> lock(corpus_mutex);
        for (const auto& hit : hits) {
            if (hit.doc_idx < global_corpus.docs.size()) {
                const auto& doc = global_corpus.docs[hit.doc_idx];
                hit_metas.push_back({doc.id, doc.offset, doc.length, hit.score});
            }
        }
    }

    // Cache decompressed chunks to avoid double decompression (Step 18 fix)
    vector<string> decompressed_chunks;
    decompressed_chunks.reserve(hit_metas.size());

    // v4.2: Decompress all chunks first (needed for reranking)
    for (const auto& meta : hit_metas) {
        string chunk_text = decompress_chunk(global_storage_path, meta.offset, meta.length);
        decompressed_chunks.push_back(make_json_safe(chunk_text));
    }

    // v4.2: Rerank using Python sidecar if enabled
    if (g_config.reranker.enabled && !hits.empty()) {
        vector<Hit> reranked = rerank_hits(question, hits, decompressed_chunks, effective_topk);
        if (reranked.size() != hits.size() || &reranked != &hits) {
            // Rebuild hit_metas and decompressed_chunks in reranked order
            vector<HitMeta> new_metas;
            vector<string> new_decomp;
            {
                shared_lock<shared_mutex> lock(corpus_mutex);
                for (const auto& hit : reranked) {
                    if (hit.doc_idx < global_corpus.docs.size()) {
                        const auto& doc = global_corpus.docs[hit.doc_idx];
                        new_metas.push_back({doc.id, doc.offset, doc.length, hit.score});
                        // Find the original decompressed content
                        for (size_t j = 0; j < hits.size(); j++) {
                            if (hits[j].doc_idx == hit.doc_idx) {
                                new_decomp.push_back(decompressed_chunks[j]);
                                break;
                            }
                        }
                    }
                }
            }
            hit_metas = move(new_metas);
            decompressed_chunks = move(new_decomp);
            hits = move(reranked);
        }
    }

    for (size_t hi = 0; hi < hit_metas.size() && hi < decompressed_chunks.size(); hi++) {
        const auto& meta = hit_metas[hi];
        const string& chunk_text = decompressed_chunks[hi];

        // Track what types of context we have
        if (has_prefix(meta.id, "CH")) {
            context += "[Previous conversation]\n" + chunk_text + "\n\n---\n\n";
            has_conversation_context = true;
        } else {
            context += chunk_text + "\n\n";

            // v4.2: Adjacent chunk retrieval — expand context with neighboring chunks
            int cw = g_config.search.context_window;
            if (cw > 0) {
                auto adjacent = get_adjacent_chunks(meta.id, cw);
                for (const auto& [adj_id, adj_content] : adjacent) {
                    string safe_adj = make_json_safe(adj_content);
                    context += safe_adj + "\n\n";
                }
            }

            context += "---\n\n";
            has_document_context = true;
        }
    }

    // If we have both types and it's a self-referential query, let LLM know to prioritize personal info
    string context_note = "";
    if (is_self_referential_query(question) && has_conversation_context && has_document_context) {
        context_note = "Note: This appears to be a personal question. Prioritize information from [Previous conversation] sections over document content.\n\n";
    }

    // v4.2: Truncate context to configurable max size to prevent exceeding LLM token limits
    if (context.length() > (size_t)g_config.search.max_context_chars) {
        // Truncate at a sentence boundary if possible
        size_t cut = g_config.search.max_context_chars;
        size_t last_period = context.rfind('.', cut);
        if (last_period != string::npos && last_period > cut * 0.8) {
            cut = last_period + 1;
        }
        context = context.substr(0, cut) + "\n\n[Context truncated]";
    }

    // Build LLM prompt with context guidance
    string prompt = "Based on the context below, answer the question concisely.\n\n" + context_note + "Context:\n" + context +
                   "\n\nQuestion: " + question + "\n\nAnswer:";

    // Query LLM
    auto [answer, llm_ms] = query_llm(prompt);

    // Save conversation turn
    ConversationTurn turn;
    turn.user_message = question;
    turn.system_response = answer;
    turn.timestamp = chrono::system_clock::now();
    turn.chunk_id = "CH" + to_string(++chat_chunk_counter);

    // Feature 2: Build full ChunkReference objects with scores and snippets
    // Use cached decompressed content (no double decompression!)
    for (size_t i = 0; i < hit_metas.size(); i++) {
        ChunkReference ref;
        ref.chunk_id = hit_metas[i].id;
        ref.relevance_score = hit_metas[i].score;
        ref.snippet = create_snippet(decompressed_chunks[i]);
        turn.source_refs.push_back(ref);
    }

    // Save conversation turn to main database
    save_conversation_turn(turn);

    // Feature 4: Update chapter guide with new conversation
    update_chapter_guide_conversation(turn);

    // Feature 2: Cache the turn for "tell me more" queries (proper LRU eviction)
    // Thread safety: lock cache before mutation
    {
        lock_guard<mutex> lock(cache_mutex);
        if (recent_turns_cache.size() >= MAX_CACHED_TURNS) {
            auto& oldest = recent_turns_list.back();
            recent_turns_cache.erase(oldest.first);
            recent_turns_list.pop_back();
        }
        recent_turns_list.push_front({turn.chunk_id, turn});
        recent_turns_cache[turn.chunk_id] = recent_turns_list.begin();
    }

    // Note: chunk_id_to_index is now updated inside save_conversation_turn()

    // Update stats
    // Thread safety: lock stats before mutation
    {
        lock_guard<mutex> lock(stats_mutex);
        stats.total_queries++;
        stats.total_search_time_ms += search_ms;
        stats.total_llm_time_ms += llm_ms;
    }

    // Feature 3: Format answer with chunk references for infinite context
    string formatted_answer = format_response_with_refs(answer, turn.source_refs, turn.chunk_id);

    response["answer"] = make_json_safe(answer);
    response["formatted_answer"] = make_json_safe(formatted_answer);
    response["search_time_ms"] = search_ms;
    response["llm_time_ms"] = llm_ms;
    response["total_time_ms"] = search_ms + llm_ms;
    response["chunks_retrieved"] = hits.size();
    response["creature_tier"] = get_creature_name();
    response["tentacles"] = get_tentacle_count();
    // v4.4: refinement loop tracking
    response["refinement_iterations"] = refinement_iterations;
    response["refinement_ms"] = refinement_ms;
    response["final_query"] = refine_result.final_query;
    response["turn_id"] = turn.chunk_id;

    // Include source IDs with original file info
    json source_ids = json::array();
    for (const auto& ref : turn.source_refs) {
        json src = {
            {"chunk_id", make_json_safe(ref.chunk_id)},
            {"score", ref.relevance_score}
        };
        // attach original file info if this chunk has one
        auto oit = chunk_to_original.find(ref.chunk_id);
        if (oit != chunk_to_original.end()) {
            src["original_file_id"] = oit->second;
            // find the original entry for name and category
            lock_guard<mutex> olock(originals_mutex);
            for (const auto& entry : originals_catalog) {
                if (entry.value("file_id", "") == oit->second) {
                    src["original_name"] = entry.value("original_name", "");
                    src["category"] = entry.value("category", "");
                    break;
                }
            }
        }
        source_ids.push_back(src);
    }
    response["sources"] = source_ids;

    // Compression ratio
    vector<string> all_source_ids;
    for (const auto& ref : turn.source_refs) {
        all_source_ids.push_back(ref.chunk_id);
    }
    response["compression_ratio"] = calculate_compression_ratio(all_source_ids, context);

    return response;
}

// Rate limiter (token bucket per IP)
class RateLimiter {
    struct Bucket {
        double tokens;
        chrono::steady_clock::time_point last_refill;
    };
    mutex mtx;
    unordered_map<string, Bucket> buckets;
    int max_tokens;       // requests_per_minute
    double refill_rate;   // tokens per second

public:
    RateLimiter(int requests_per_minute)
        : max_tokens(requests_per_minute),
          refill_rate(requests_per_minute / 60.0) {}

    bool allow(const string& ip) {
        lock_guard<mutex> lock(mtx);
        auto now = chrono::steady_clock::now();
        auto& b = buckets[ip];
        if (b.last_refill == chrono::steady_clock::time_point{}) {
            // New bucket: start full
            b.tokens = max_tokens;
            b.last_refill = now;
        } else {
            // Refill based on elapsed time
            double elapsed = chrono::duration<double>(now - b.last_refill).count();
            b.tokens = min((double)max_tokens, b.tokens + elapsed * refill_rate);
            b.last_refill = now;
        }
        if (b.tokens >= 1.0) {
            b.tokens -= 1.0;
            return true;
        }
        return false;
    }
};

// HTTP server using cpp-httplib
void run_http_server() {
    httplib::Server svr;
    g_server_ptr = &svr;  // For graceful shutdown

    // CORS middleware
    // Rate limiter instance (persists across requests)
    auto rate_limiter = make_shared<RateLimiter>(g_config.rate_limit.requests_per_minute);

    svr.set_pre_routing_handler([rate_limiter](const httplib::Request& req, httplib::Response& res) {
        // CORS headers on all responses
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type, X-API-Key");
        if (req.method == "OPTIONS") {
            res.status = 204;
            return httplib::Server::HandlerResponse::Handled;
        }

        // Skip auth/rate-limit for health check
        if (req.path == "/health") {
            return httplib::Server::HandlerResponse::Unhandled;
        }

        // Auth check (if enabled)
        if (g_config.auth.enabled) {
            string client_key = req.get_header_value("X-API-Key");
            if (client_key != g_config.auth.api_key) {
                res.status = 401;
                res.set_content(R"({"error":"Unauthorized: invalid or missing X-API-Key"})", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        // Rate limit check (if enabled)
        if (g_config.rate_limit.enabled) {
            string client_ip = req.remote_addr;
            if (!rate_limiter->allow(client_ip)) {
                res.status = 429;
                res.set_content(R"({"error":"Rate limit exceeded. Try again later."})", "application/json");
                return httplib::Server::HandlerResponse::Handled;
            }
        }

        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Request logging
    svr.set_logger([](const httplib::Request& req, const httplib::Response& res) {
        if (req.method == "OPTIONS") return;  // Skip CORS preflight noise
        auto now = chrono::system_clock::now();
        auto time_t_now = chrono::system_clock::to_time_t(now);
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime(&time_t_now));
        cout << "[" << buf << "] " << req.method << " " << req.path
             << " -> " << res.status << " (" << res.body.size() << "B)" << endl;
    });

    // GET /stats
    svr.Get("/stats", [](const httplib::Request&, httplib::Response& res) {
        json stats_json = get_system_stats();
        res.set_content(stats_json.dump(), "application/json");
    });

    // GET /health
    svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
        json health = {{"status", "ok"}, {"version", "4.8"}, {"creature_tier", get_creature_name()}, {"tentacles", get_tentacle_count()}};
        res.set_content(health.dump(), "application/json");
    });

    // GET /chunk/:id — return decompressed chunk text for accuracy testing
    svr.Get(R"(/chunk/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        try {
            string chunk_id = req.matches[1];
            shared_lock<shared_mutex> lock(corpus_mutex);
            for (size_t i = 0; i < global_corpus.docs.size(); i++) {
                if (global_corpus.docs[i].id == chunk_id) {
                    const auto& doc = global_corpus.docs[i];
                    string text = decompress_chunk(global_storage_path, doc.offset, doc.length);
                    json result = {
                        {"chunk_id", chunk_id},
                        {"content", make_json_safe(text)},
                        {"source_file", doc.source_file}
                    };
                    res.set_content(result.dump(), "application/json");
                    return;
                }
            }
            res.status = 404;
            res.set_content(json({{"error", "Chunk not found"}}).dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    // POST /search — raw BM25 search without LLM, for latency benchmarks
    svr.Post("/search", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string query = body.value("query", body.value("question", ""));
            int topk = body.value("top_k", TOPK);
            bool use_refinement = body.value("refine", false);  // off by default, accuracy test passes true
            if (query.empty()) {
                res.status = 400;
                res.set_content(json({{"error", "Missing query"}}).dump(), "application/json");
                return;
            }

            auto t0 = chrono::high_resolution_clock::now();
            vector<Hit> hits;
            int refinement_iterations = 0;
            double refinement_ms = 0;

            bool use_scanner = body.value("scanner", g_config.scanner.enabled);
            double scanner_ms = 0;
            int scanner_passed = 0;
            int scanner_scanned = 0;

            if (use_refinement && !g_config.llm.api_key.empty()) {
                // v4.4: LLM refinement loop
                int max_refine = body.value("max_refine", 1);
                auto refine_result = search_with_refinement(query, topk, max_refine);
                hits = move(refine_result.hits);
                refinement_iterations = refine_result.iterations;
                refinement_ms = refine_result.refinement_ms;
            } else {
                // BM25 search — oversample if scanner is enabled
                int search_topk = (use_scanner && g_config.scanner.enabled)
                    ? g_config.scanner.max_candidates : topk;
                {
                    shared_lock<shared_mutex> lock(corpus_mutex);
                    if (g_multi_seg.loaded) {
                        hits = search_multi_segment(g_multi_seg, query, search_topk);
                    } else if (global_shards.shard_count > 0) {
                        hits = search_unified_twophase(global_shards, global_corpus, query, search_topk);
                    } else {
                        hits = search_bm25_twophase(global_corpus, query, search_topk);
                    }
                    if (!g_multi_seg.loaded) {
                        apply_source_coherence(hits, global_corpus);
                    }
                }
            }

            // v9.1: scanner filter — runs after BM25, before returning
            if (use_scanner && g_config.scanner.enabled && !hits.empty()) {
                auto scan_t0 = chrono::high_resolution_clock::now();
                scanner_scanned = min((int)hits.size(), g_config.scanner.max_candidates);
                hits = scanner_filter_chunks(query, hits);
                scanner_passed = (int)hits.size();
                auto scan_t1 = chrono::high_resolution_clock::now();
                scanner_ms = chrono::duration<double, milli>(scan_t1 - scan_t0).count();
            }

            // trim to requested topk after scanner
            if ((int)hits.size() > topk) hits.resize(topk);

            auto t1 = chrono::high_resolution_clock::now();
            double search_ms = chrono::duration<double, milli>(t1 - t0).count();

            json results = json::array();
            {
                shared_lock<shared_mutex> lock(corpus_mutex);
                for (const auto& hit : hits) {
                    if (hit.doc_idx < global_corpus.docs.size()) {
                        const auto& doc = global_corpus.docs[hit.doc_idx];
                        results.push_back({{"chunk_id", doc.id}, {"score", hit.score}});
                    }
                }
            }

            json response_json = {
                {"results", results},
                {"search_time_ms", search_ms},
                {"total_results", results.size()},
                {"refinement_iterations", refinement_iterations},
                {"refinement_ms", refinement_ms}
            };
            if (use_scanner && g_config.scanner.enabled) {
                response_json["scanner_ms"] = scanner_ms;
                response_json["scanner_scanned"] = scanner_scanned;
                response_json["scanner_passed"] = scanner_passed;
            }
            res.set_content(response_json.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    // POST /chat
    svr.Post("/chat", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            if (!body.contains("question")) {
                res.status = 400;
                res.set_content(json({{"error", "Missing 'question' field"}}).dump(), "application/json");
                return;
            }
            string question = body["question"];
            json response = handle_chat(question);
            res.set_content(response.dump(), "application/json");
        } catch (const exception& e) {
            res.status = 500;
            cerr << "[ERROR] /chat exception: " << e.what() << endl;
            res.set_content(json({{"error", string("Chat error: ") + e.what()}}).dump(), "application/json");
        }
    });

    // Step 25: POST /chat/stream - SSE streaming endpoint
    svr.Post("/chat/stream", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            if (!body.contains("question")) {
                res.set_content(json({{"error", "Missing 'question' field"}}).dump(), "application/json");
                return;
            }
            string question = body["question"];

            auto search_start = chrono::high_resolution_clock::now();

            // v4.2: Dynamic top_k for streaming endpoint
            int effective_topk = calculate_dynamic_topk();
            // oversample for scanner
            int stream_search_topk = g_config.scanner.enabled
                ? g_config.scanner.max_candidates : effective_topk;

            // Same search logic as handle_chat
            vector<Hit> hits;
            {
                shared_lock<shared_mutex> lock(corpus_mutex);

                if (is_self_referential_query(question)) {
                    const int CHAT_TENTACLES = 3;
                    const int DOC_TENTACLES = max(stream_search_topk - CHAT_TENTACLES, 5);
                    vector<Hit> conv_hits = search_conversation_chunks_only_locked(question, CHAT_TENTACLES);
                    vector<Hit> doc_hits = global_shards.shard_count > 0
                        ? search_unified_twophase(global_shards, global_corpus, question, DOC_TENTACLES)
                        : search_bm25_twophase(global_corpus, question, DOC_TENTACLES);
                    for (const auto& hit : conv_hits) hits.push_back(hit);
                    for (const auto& hit : doc_hits) {
                        if ((int)hits.size() >= stream_search_topk) break;
                        if (hit.doc_idx < global_corpus.docs.size() &&
                            !has_prefix(global_corpus.docs[hit.doc_idx].id, "CH")) {
                            hits.push_back(hit);
                        }
                    }
                } else {
                    if (g_multi_seg.loaded) {
                        hits = search_multi_segment(g_multi_seg, question, stream_search_topk);
                    } else {
                        hits = global_shards.shard_count > 0
                            ? search_unified_twophase(global_shards, global_corpus, question, stream_search_topk)
                            : search_bm25_twophase(global_corpus, question, stream_search_topk);
                    }
                    for (auto& hit : hits) {
                        if (hit.doc_idx < global_corpus.docs.size() &&
                            has_prefix(global_corpus.docs[hit.doc_idx].id, "CH")) {
                            hit.score *= 1.5;
                        }
                    }
                    sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                        return a.score > b.score;
                    });
                }

                // v4.4: Source coherence boost
                apply_source_coherence(hits, global_corpus);

                // v4.2: Apply score threshold cutoff
                apply_score_threshold(hits, g_config.search.score_threshold);
            }

            // v9.1: scanner filter for streaming
            if (g_config.scanner.enabled && !hits.empty()) {
                hits = scanner_filter_chunks(question, hits);
            }
            if ((int)hits.size() > effective_topk) hits.resize(effective_topk);

            auto search_end = chrono::high_resolution_clock::now();
            double search_ms = chrono::duration<double, milli>(search_end - search_start).count();

            // Extract hit metadata under lock
            struct StreamHitMeta { string id; uint64_t offset; uint64_t length; double score; };
            vector<StreamHitMeta> hit_metas;
            {
                shared_lock<shared_mutex> lock(corpus_mutex);
                for (const auto& hit : hits) {
                    if (hit.doc_idx < global_corpus.docs.size()) {
                        const auto& doc = global_corpus.docs[hit.doc_idx];
                        hit_metas.push_back({doc.id, doc.offset, doc.length, hit.score});
                    }
                }
            }

            // Decompress chunks and build context
            string context;
            vector<string> decompressed_chunks;
            bool has_conversation_context = false;
            bool has_document_context = false;
            for (const auto& meta : hit_metas) {
                string chunk_text = decompress_chunk(global_storage_path, meta.offset, meta.length);
                chunk_text = make_json_safe(chunk_text);
                decompressed_chunks.push_back(chunk_text);
                if (has_prefix(meta.id, "CH")) {
                    context += "[Previous conversation]\n" + chunk_text + "\n\n---\n\n";
                    has_conversation_context = true;
                } else {
                    context += chunk_text + "\n\n---\n\n";
                    has_document_context = true;
                }
            }

            string context_note = "";
            if (is_self_referential_query(question) && has_conversation_context && has_document_context) {
                context_note = "Note: This appears to be a personal question. Prioritize information from [Previous conversation] sections over document content.\n\n";
            }

            // v4.2: Truncate context for streaming endpoint too
            if (context.length() > (size_t)g_config.search.max_context_chars) {
                size_t cut = g_config.search.max_context_chars;
                size_t last_period = context.rfind('.', cut);
                if (last_period != string::npos && last_period > cut * 0.8) {
                    cut = last_period + 1;
                }
                context = context.substr(0, cut) + "\n\n[Context truncated]";
            }

            string prompt = "Based on the context below, answer the question concisely.\n\n" + context_note + "Context:\n" + context +
                           "\n\nQuestion: " + question + "\n\nAnswer:";

            // Get turn_id for this response
            string turn_id = "CH" + to_string(++chat_chunk_counter);

            // Set SSE headers
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("X-Accel-Buffering", "no");  // Disable nginx buffering

            // True SSE streaming using chunked content provider
            // Shared state for the streaming callback and content provider
            struct StreamState {
                mutex mtx;
                condition_variable cv;
                queue<string> event_queue;
                bool done = false;
                string full_response;
            };
            auto state = make_shared<StreamState>();

            // Queue search metadata event first
            json search_event = {{"search_time_ms", search_ms}, {"chunks_retrieved", hits.size()}, {"creature_tier", get_creature_name()}, {"tentacles", get_tentacle_count()}};
            {
                lock_guard<mutex> lock(state->mtx);
                state->event_queue.push("event: search\ndata: " + search_event.dump() + "\n\n");
            }

            // Launch LLM streaming in a background thread
            // Capture hit_metas and decompressed_chunks by value to avoid dangling references
            // (the handler's stack may unwind before the detached thread finishes)
            auto stream_thread = thread([state, prompt, hit_metas, decompressed_chunks,
                                          search_ms, turn_id, question]() {
                auto [answer, llm_ms] = query_llm_streaming(prompt, [&state](const string& token) {
                    json token_event = {{"token", token}};
                    lock_guard<mutex> lock(state->mtx);
                    state->event_queue.push("data: " + token_event.dump() + "\n\n");
                    state->cv.notify_one();
                });

                state->full_response = answer;

                // Build source info
                json sources = json::array();
                for (const auto& meta : hit_metas) {
                    sources.push_back({{"chunk_id", meta.id}, {"score", meta.score}});
                }

                // Queue done event
                json done_event = {
                    {"turn_id", turn_id},
                    {"total_time_ms", search_ms + llm_ms},
                    {"llm_time_ms", llm_ms},
                    {"sources", sources}
                };

                {
                    lock_guard<mutex> lock(state->mtx);
                    state->event_queue.push("event: done\ndata: " + done_event.dump() + "\n\n");
                    state->done = true;
                }
                state->cv.notify_one();

                // Save conversation turn
                if (answer.substr(0, 5) != "ERROR") {
                    ConversationTurn turn;
                    turn.user_message = question;
                    turn.system_response = answer;
                    turn.timestamp = chrono::system_clock::now();
                    turn.chunk_id = turn_id;
                    for (size_t i = 0; i < hit_metas.size(); i++) {
                        ChunkReference ref;
                        ref.chunk_id = hit_metas[i].id;
                        ref.relevance_score = hit_metas[i].score;
                        ref.snippet = create_snippet(decompressed_chunks[i]);
                        turn.source_refs.push_back(ref);
                    }
                    save_conversation_turn(turn);
                    update_chapter_guide_conversation(turn);
                    {
                        lock_guard<mutex> lock_c(cache_mutex);
                        if (recent_turns_cache.size() >= MAX_CACHED_TURNS) {
                            auto& oldest = recent_turns_list.back();
                            recent_turns_cache.erase(oldest.first);
                            recent_turns_list.pop_back();
                        }
                        recent_turns_list.push_front({turn.chunk_id, turn});
                        recent_turns_cache[turn.chunk_id] = recent_turns_list.begin();
                    }
                }

                // Update stats
                {
                    lock_guard<mutex> lock_s(stats_mutex);
                    stats.total_queries++;
                    stats.total_search_time_ms += search_ms;
                    stats.total_llm_time_ms += llm_ms;
                }
            });
            // v4.2: Track thread for clean shutdown instead of detaching
            {
                lock_guard<mutex> tlock(g_stream_threads_mutex);
                g_stream_threads.push_back(move(stream_thread));
            }

            // Use chunked content provider to stream SSE events as they arrive
            res.set_chunked_content_provider("text/event-stream",
                [state](size_t /*offset*/, httplib::DataSink& sink) -> bool {
                    while (true) {
                        unique_lock<mutex> lock(state->mtx);
                        state->cv.wait_for(lock, chrono::milliseconds(100), [&state] {
                            return !state->event_queue.empty() || state->done;
                        });

                        // Drain all queued events
                        while (!state->event_queue.empty()) {
                            string event = state->event_queue.front();
                            state->event_queue.pop();
                            lock.unlock();
                            sink.write(event.data(), event.size());
                            lock.lock();
                        }

                        if (state->done && state->event_queue.empty()) {
                            lock.unlock();
                            sink.done();
                            return false;  // Signal completion
                        }
                    }
                });

            // v4.2: Periodically clean up finished streaming threads
            {
                lock_guard<mutex> tlock(g_stream_threads_mutex);
                g_stream_threads.erase(
                    remove_if(g_stream_threads.begin(), g_stream_threads.end(),
                        [](thread& t) {
                            // Can't query if a std::thread is done, so we don't join here.
                            // Cleanup happens at shutdown. This just removes non-joinable entries.
                            if (!t.joinable()) return true;
                            return false;
                        }),
                    g_stream_threads.end());
            }

        } catch (const exception& e) {
            res.set_content("event: error\ndata: {\"error\":\"" + string(e.what()) + "\"}\n\n", "text/event-stream");
        }
    });

    // POST /clear-database
    svr.Post("/clear-database", [](const httplib::Request&, httplib::Response& res) {
        try {
            bool success = clear_conversation_database();
            if (success) {
                res.set_content(json({{"success", true}, {"message", "Database cleared successfully"}}).dump(), "application/json");
            } else {
                res.set_content(json({{"success", false}, {"error", "Failed to clear database"}}).dump(), "application/json");
            }
        } catch (const exception& e) {
            res.set_content(json({{"success", false}, {"error", e.what()}}).dump(), "application/json");
        }
    });

    // POST /sources
    svr.Post("/sources", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string turn_id = body["turn_id"];
            json response = handle_source_query(turn_id);
            if (!response.value("success", false)) res.status = 404;
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need turn_id"}}).dump(), "application/json");
        }
    });

    // POST /tell-me-more
    svr.Post("/tell-me-more", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string prev_turn_id = body["prev_turn_id"];
            string aspect = body.value("aspect", "the topic");
            json response = handle_tell_me_more(prev_turn_id, aspect);
            if (!response.value("success", false)) res.status = 404;
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need prev_turn_id"}}).dump(), "application/json");
        }
    });

    // GET /chunk/:id - use regex pattern matching
    // v4.2: supports ?context_window=N parameter for adjacent chunk retrieval
    svr.Get(R"(/chunk/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        string chunk_id = req.matches[1];
        int context_window = 0;
        if (req.has_param("context_window")) {
            try { context_window = min(stoi(req.get_param_value("context_window")), 3); }
            catch (...) { context_window = 0; }
        }

        auto [content, score] = get_chunk_by_id(chunk_id);
        if (score >= 0) {
            json response = {{"success", true}, {"chunk_id", chunk_id}, {"content", content}};

            // v4.2: Include cross-reference metadata
            {
                shared_lock<shared_mutex> lock(corpus_mutex);
                auto it = chunk_id_to_index.find(chunk_id);
                if (it != chunk_id_to_index.end() && it->second < global_corpus.docs.size()) {
                    const auto& doc = global_corpus.docs[it->second];
                    if (!doc.prev_chunk_id.empty()) response["prev_chunk_id"] = doc.prev_chunk_id;
                    if (!doc.next_chunk_id.empty()) response["next_chunk_id"] = doc.next_chunk_id;
                    if (!doc.source_file.empty()) response["source_file"] = doc.source_file;
                }
            }

            // v4.2: Include adjacent chunks if requested
            if (context_window > 0) {
                auto adjacent = get_adjacent_chunks(chunk_id, context_window);
                json adj_json = json::array();
                for (const auto& [adj_id, adj_content] : adjacent) {
                    adj_json.push_back({{"chunk_id", adj_id}, {"content", adj_content}});
                }
                response["adjacent_chunks"] = adj_json;
            }

            res.set_content(response.dump(), "application/json");
        } else {
            res.status = 404;
            json error = {{"success", false}, {"error", "Chunk not found: " + chunk_id}};
            res.set_content(error.dump(), "application/json");
        }
    });

    // POST /reconstruct
    svr.Post("/reconstruct", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            vector<string> chunk_ids;
            if (body.contains("chunk_ids") && body["chunk_ids"].is_array()) {
                for (const auto& id : body["chunk_ids"]) {
                    chunk_ids.push_back(id.get<string>());
                }
            } else if (body.contains("text")) {
                chunk_ids = extract_chunk_ids_from_context(body["text"].get<string>());
            }
            string context = reconstruct_context_from_ids(chunk_ids);
            double compression = calculate_compression_ratio(chunk_ids, context);
            json response = {
                {"success", true}, {"chunk_ids", chunk_ids},
                {"context", context}, {"context_length", context.length()},
                {"compression_ratio", compression}
            };
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need chunk_ids array or text"}}).dump(), "application/json");
        }
    });

    // POST /extract-ids
    svr.Post("/extract-ids", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string text = body["text"].get<string>();
            vector<string> chunk_ids = extract_chunk_ids_from_context(text);
            json response = {{"success", true}, {"chunk_ids", chunk_ids}, {"count", chunk_ids.size()}};
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need text field"}}).dump(), "application/json");
        }
    });

    // GET /guide
    svr.Get("/guide", [](const httplib::Request&, httplib::Response& res) {
        lock_guard<mutex> lock(chapter_guide_mutex);
        res.set_content(chapter_guide.dump(2), "application/json");
    });

    // v4.2: GET /catalog — hierarchical corpus catalog with per-chunk type/source/section info
    svr.Get("/catalog", [](const httplib::Request& req, httplib::Response& res) {
        string filter_type = req.has_param("type") ? req.get_param_value("type") : "";
        string filter_source = req.has_param("source") ? req.get_param_value("source") : "";
        int page = 0, page_size = 100;
        if (req.has_param("page")) {
            try { page = stoi(req.get_param_value("page")); } catch (...) {}
        }
        if (req.has_param("page_size")) {
            try { page_size = min(stoi(req.get_param_value("page_size")), 1000); } catch (...) {}
        }

        json catalog;

        // Build hierarchical view under shared lock
        {
            shared_lock<shared_mutex> lock(corpus_mutex);

            // Type counts
            unordered_map<string, int> type_counts;
            // Source file -> chunk count
            unordered_map<string, int> source_counts;
            // Section paths
            unordered_map<string, int> section_counts;

            for (const auto& doc : global_corpus.docs) {
                // Determine type from ID prefix
                string type = "DOC";
                if (has_prefix(doc.id, "CH")) type = "CHAT";
                else if (doc.id.find("_CODE_") != string::npos) type = "CODE";
                else if (doc.id.find("_FIX_") != string::npos) type = "FIX";
                else if (doc.id.find("_FEAT_") != string::npos) type = "FEAT";
                type_counts[type]++;

                if (!doc.source_file.empty()) {
                    source_counts[doc.source_file]++;
                }

                if (!doc.doc_section.empty()) {
                    section_counts[doc.doc_section]++;
                }
            }

            catalog["total_chunks"] = global_corpus.docs.size();
            catalog["total_tokens"] = global_corpus.total_tokens;
            catalog["avgdl"] = global_corpus.avgdl;

            // Types
            json types = json::object();
            for (const auto& [t, c] : type_counts) types[t] = c;
            catalog["types"] = types;

            // Source files (sorted by chunk count)
            json sources = json::array();
            vector<pair<string, int>> sorted_sources(source_counts.begin(), source_counts.end());
            sort(sorted_sources.begin(), sorted_sources.end(),
                [](const pair<string, int>& a, const pair<string, int>& b) {
                    return a.second > b.second;
                });
            for (const auto& [s, c] : sorted_sources) {
                sources.push_back({{"source_file", s}, {"chunk_count", c}});
            }
            catalog["source_files"] = sources;

            // Paginated chunk listing
            json chunk_list = json::array();
            int start_idx = page * page_size;
            int added = 0;
            int skipped = 0;
            for (size_t i = 0; i < global_corpus.docs.size() && added < page_size; i++) {
                const auto& doc = global_corpus.docs[i];

                // Apply filters
                if (!filter_type.empty()) {
                    string type = "DOC";
                    if (has_prefix(doc.id, "CH")) type = "CHAT";
                    else if (doc.id.find("_CODE_") != string::npos) type = "CODE";
                    if (type != filter_type) continue;
                }
                if (!filter_source.empty() && doc.source_file != filter_source) continue;

                if (skipped < start_idx) { skipped++; continue; }

                json entry = {
                    {"chunk_id", doc.id},
                    {"summary", doc.summary},
                    {"keywords_count", doc.keyword_ids.size()}
                };
                if (!doc.source_file.empty()) entry["source_file"] = doc.source_file;
                if (!doc.prev_chunk_id.empty()) entry["prev_chunk_id"] = doc.prev_chunk_id;
                if (!doc.next_chunk_id.empty()) entry["next_chunk_id"] = doc.next_chunk_id;
                chunk_list.push_back(entry);
                added++;
            }
            catalog["chunks"] = chunk_list;
            catalog["page"] = page;
            catalog["page_size"] = page_size;
        }

        res.set_content(catalog.dump(2), "application/json");
    });

    // POST /query/code-discussions
    svr.Post("/query/code-discussions", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string chunk_id = body["chunk_id"].get<string>();
            json result = query_code_discussions(chunk_id);
            res.set_content(result.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need chunk_id"}}).dump(), "application/json");
        }
    });

    // POST /query/fixes
    svr.Post("/query/fixes", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string filename = body["filename"].get<string>();
            json result = query_fixes_for_file(filename);
            res.set_content(result.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need filename"}}).dump(), "application/json");
        }
    });

    // POST /query/feature
    svr.Post("/query/feature", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string feature_id = body["feature_id"].get<string>();
            json result = query_feature_implementation(feature_id);
            res.set_content(result.dump(), "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content(json({{"error", "Invalid request - need feature_id"}}).dump(), "application/json");
        }
    });

    // POST /add-file-path (large files from disk)
    svr.Post("/add-file-path", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            DEBUG_LOG("add-file-path received body: " << req.body.substr(0, 200));
            string filepath = body.value("path", "");
            if (filepath.empty()) {
                res.set_content(json({{"error", "Missing 'path' in request body"}, {"success", false}}).dump(), "application/json");
                return;
            }

            // v4.2: Security: Block path traversal attacks with strict directory allowlist
            if (filepath.find("..") != string::npos) {
                res.status = 403;
                res.set_content(json({{"error", "Path traversal not allowed"}, {"success", false}}).dump(), "application/json");
                return;
            }
            // Resolve symlinks and canonicalize path
            char resolved[PATH_MAX];
            if (realpath(filepath.c_str(), resolved) == nullptr) {
                res.status = 400;
                res.set_content(json({{"error", "Cannot resolve path: " + filepath}, {"success", false}}).dump(), "application/json");
                return;
            }
            string resolved_str(resolved);
            // Get current working directory
            char cwd[PATH_MAX];
            if (getcwd(cwd, PATH_MAX) == nullptr) {
                res.status = 500;
                res.set_content(json({{"error", "Server error resolving working directory"}, {"success", false}}).dump(), "application/json");
                return;
            }
            string cwd_str(cwd);
            // v4.2: Strict directory boundary check — prevent prefix collisions
            // e.g., /home/user must not match /home/user2/evil
            bool under_cwd = (resolved_str == cwd_str) ||
                             (resolved_str.length() > cwd_str.length() &&
                              resolved_str.substr(0, cwd_str.length()) == cwd_str &&
                              resolved_str[cwd_str.length()] == '/');
            bool under_tmp = (resolved_str.length() > 4 &&
                              resolved_str.substr(0, 5) == "/tmp/");
            if (!under_cwd && !under_tmp) {
                res.status = 403;
                res.set_content(json({{"error", "Access denied: path must be under working directory or /tmp"}, {"success", false}}).dump(), "application/json");
                return;
            }

            ifstream file(filepath);
            if (!file.is_open()) {
                res.set_content(json({{"error", "Cannot open file: " + filepath}, {"success", false}}).dump(), "application/json");
                return;
            }

            file.seekg(0, ios::end);
            size_t file_size = file.tellg();
            file.seekg(0, ios::beg);
            cout << "Reading file from path: " << filepath << " (" << file_size << " bytes)" << endl;

            string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
            file.close();

            // Sanitize UTF-8
            for (size_t i = 0; i < content.size(); i++) {
                unsigned char c = static_cast<unsigned char>(content[i]);
                if (c > 127 && c < 192) content[i] = ' ';
                else if (c >= 192 && c < 224) {
                    if (i + 1 >= content.size() || (static_cast<unsigned char>(content[i+1]) & 0xC0) != 0x80) content[i] = ' ';
                } else if (c >= 224 && c < 240) {
                    if (i + 2 >= content.size()) content[i] = ' ';
                } else if (c >= 240) {
                    if (i + 3 >= content.size()) content[i] = ' ';
                }
            }

            string filename = filepath;
            size_t slash_pos = filepath.rfind('/');
            if (slash_pos != string::npos) filename = filepath.substr(slash_pos + 1);

            json result = add_file_to_index(filename, content);
            res.set_content(result.dump(), "application/json");
        } catch (const exception& e) {
            res.set_content(json({{"error", string("Failed to add file: ") + e.what()}, {"success", false}}).dump(), "application/json");
        }
    });

    // POST /add-file (small files, content in body)
    svr.Post("/add-file", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string filename = body["filename"].get<string>();
            string content = body["content"].get<string>();
            cout << "Adding file to index: " << filename << " (" << content.length() << " chars)" << endl;
            json result = add_file_to_index(filename, content);
            res.set_content(result.dump(), "application/json");
        } catch (const exception& e) {
            res.set_content(json({{"error", string("Failed to add file: ") + e.what()}, {"success", false}}).dump(), "application/json");
        }
    });

    // GET /original/{file_id} — serve an original file back to the user
    svr.Get(R"(/original/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        string file_id = req.matches[1];

        lock_guard<mutex> olock(originals_mutex);
        for (const auto& entry : originals_catalog) {
            if (entry.value("file_id", "") == file_id) {
                string stored_path = entry.value("stored_path", "");
                if (stored_path.empty()) {
                    res.status = 404;
                    res.set_content(json({{"error", "Original file path not recorded"}}).dump(), "application/json");
                    return;
                }

                // stored_path is relative to corpus dir — prepend "corpus/"
                string full_path = "corpus/" + stored_path;
                ifstream file(full_path, ios::binary);
                if (!file.is_open()) {
                    // try as-is (in case stored_path is already absolute or correct)
                    file.open(stored_path, ios::binary);
                    if (!file.is_open()) {
                        res.status = 404;
                        res.set_content(json({{"error", "Original file not found on disk: " + stored_path}}).dump(), "application/json");
                        return;
                    }
                }

                // read entire file
                string body((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
                file.close();

                // set content type based on format
                string fmt = entry.value("format", "bin");
                string content_type = "application/octet-stream";
                if (fmt == "pdf") content_type = "application/pdf";
                else if (fmt == "docx") content_type = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
                else if (fmt == "xlsx") content_type = "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet";
                else if (fmt == "pptx") content_type = "application/vnd.openxmlformats-officedocument.presentationml.presentation";
                else if (fmt == "csv") content_type = "text/csv";
                else if (fmt == "txt" || fmt == "md") content_type = "text/plain";
                else if (fmt == "html" || fmt == "htm") content_type = "text/html";
                else if (fmt == "png") content_type = "image/png";
                else if (fmt == "jpg" || fmt == "jpeg") content_type = "image/jpeg";
                else if (fmt == "py" || fmt == "js" || fmt == "cpp" || fmt == "java" ||
                         fmt == "sql" || fmt == "r" || fmt == "go" || fmt == "rs") content_type = "text/plain";

                string original_name = entry.value("original_name", "file." + fmt);
                res.set_header("Content-Disposition", "attachment; filename=\"" + original_name + "\"");
                res.set_content(body, content_type);
                return;
            }
        }

        res.status = 404;
        res.set_content(json({{"error", "Original file not found: " + file_id}}).dump(), "application/json");
    });

    // GET /originals — list all original files in the catalog
    svr.Get("/originals", [](const httplib::Request&, httplib::Response& res) {
        lock_guard<mutex> olock(originals_mutex);
        json result;
        result["total"] = originals_catalog.size();
        result["files"] = originals_catalog;
        res.set_content(result.dump(), "application/json");
    });

    // POST /reload-catalog — hot-reload originals.json from disk
    svr.Post("/reload-catalog", [](const httplib::Request&, httplib::Response& res) {
        lock_guard<mutex> olock(originals_mutex);
        load_originals_catalog();
        json result;
        result["success"] = true;
        result["files"] = originals_catalog.size();
        result["chunk_mappings"] = chunk_to_original.size();
        res.set_content(result.dump(), "application/json");
    });

    // v9.1: POST /reload-segments — hot-reload meta.json and any new segments
    svr.Post("/reload-segments", [](const httplib::Request&, httplib::Response& res) {
        try {
            string corpus_dir;
            size_t sl = MANIFEST.rfind('/');
            corpus_dir = (sl != string::npos) ? MANIFEST.substr(0, sl) : ".";

            string meta_path = corpus_dir + "/meta.json";
            if (!fs::exists(meta_path)) {
                res.set_content(json({{"error", "No meta.json found"}}).dump(), "application/json");
                return;
            }

            ifstream mf(meta_path);
            json meta = json::parse(mf);

            if (!meta.contains("segments") || !meta["segments"].is_array()) {
                res.set_content(json({{"error", "No segments in meta.json"}}).dump(), "application/json");
                return;
            }

            int new_segments = 0;
            uint64_t cum_offset = g_multi_seg.global_doc_count;

            for (const auto& seg_def : meta["segments"]) {
                string seg_dir = seg_def.value("dir", "");
                int seg_id = seg_def.value("id", -1);

                if (!seg_dir.empty() && seg_dir[0] != '/') {
                    seg_dir = corpus_dir + "/" + seg_dir;
                }

                // check if already loaded
                bool already_loaded = false;
                for (const auto& existing : g_multi_seg.segments) {
                    if (existing.dir == seg_dir) { already_loaded = true; break; }
                }
                if (already_loaded) continue;

                if (!fs::exists(seg_dir + "/stems.idx")) continue;

                SegmentIndex seg;
                seg.id = (seg_id >= 0) ? seg_id : (int)g_multi_seg.segments.size();
                seg.dir = seg_dir;
                seg.doc_offset = cum_offset;

                if (!seg.mmap_idx.load(seg_dir)) continue;

                seg.doc_count = seg.mmap_idx.doc_count;
                seg.avg_dl = seg.mmap_idx.avg_dl;

                string seg_mbin = seg_dir + "/manifest.bin";
                if (fs::exists(seg_mbin)) {
                    BinaryCorpus bc = load_binary_manifest_mmap(seg_mbin, nullptr, true);
                    seg.keyword_dict = move(bc.keyword_dict);
                }

                cum_offset += seg.doc_count;
                g_segment_storage_paths.push_back(seg_dir + "/storage.bin");

                cerr << "[reload] New segment " << seg.id << ": " << seg.doc_count
                     << " docs from " << seg_dir << endl;

                g_multi_seg.segments.push_back(move(seg));
                new_segments++;
            }

            if (new_segments > 0) {
                g_multi_seg.global_doc_count = cum_offset;
                double sum_dl = 0;
                for (const auto& seg : g_multi_seg.segments) {
                    sum_dl += seg.avg_dl * seg.doc_count;
                }
                g_multi_seg.global_avg_dl = (cum_offset > 0) ? sum_dl / cum_offset : 1.0;
                g_multi_seg.loaded = true;
            }

            res.set_content(json({
                {"success", true},
                {"new_segments", new_segments},
                {"total_segments", g_multi_seg.segments.size()},
                {"total_docs", g_multi_seg.global_doc_count}
            }).dump(), "application/json");
        } catch (const exception& e) {
            res.set_content(json({{"error", e.what()}}).dump(), "application/json");
        }
    });

    cout << "\nOceanEterna Chat Server running on http://localhost:" << HTTP_PORT << endl;
    cout << "Open ocean_chat.html in your browser to start chatting!\n" << endl;

    cout << "Creature Tier: " << get_creature_name() << " (" << get_tentacle_count() << " tentacles)" << endl;
    svr.listen(g_config.server.host.c_str(), g_config.server.port);
}

int main(int argc, char** argv) {
    // Install signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    cout << "OceanEterna Chat Server v4.8 (Cap-100 + Hapax Filter + TF BM25)" << endl;
    cout << "============================================================\n" << endl;

    // Load configuration from config.json + environment variables
    g_config = load_config("config.json");
    MANIFEST = g_config.corpus.manifest;
    STORAGE = g_config.corpus.storage;
    chapter_guide_path = g_config.corpus.chapter_guide;

    // Check for API key
    if (g_config.llm.api_key.empty() && g_config.llm.use_external) {
        cerr << "WARNING: OCEAN_API_KEY environment variable not set!" << endl;
        cerr << "Set it with: export OCEAN_API_KEY=your_key_here" << endl;
        cerr << "LLM calls will fail without a valid API key.\n" << endl;
    }

    // v9.1: Scanner status
    if (g_config.scanner.enabled) {
        cout << "[scanner] Enabled — model: " << g_config.scanner.model
             << ", max_candidates: " << g_config.scanner.max_candidates
             << ", max_keep: " << g_config.scanner.max_keep
             << ", batch_size: " << g_config.scanner.batch_size << endl;
        if (g_config.llm.api_key.empty()) {
            cerr << "[scanner] WARNING: Scanner enabled but OCEAN_API_KEY not set — scanner will be skipped" << endl;
        }
    }

    // Load manifest - try binary format first for faster loading
    global_storage_path = STORAGE;
    string binary_manifest_path = get_binary_manifest_path(MANIFEST);

    // detect mmap index before loading manifest — skip building inverted_index in memory
    string corpus_dir;
    {
        size_t sl = MANIFEST.rfind('/');
        corpus_dir = (sl != string::npos) ? MANIFEST.substr(0, sl) : ".";
    }
    bool has_mmap_idx = fs::exists(corpus_dir + "/stems.idx");
    if (has_mmap_idx) {
        cout << "mmap index detected — skipping in-memory inverted index build" << endl;
    }

    bool loaded = false;
    if (binary_manifest_is_current(binary_manifest_path, MANIFEST)) {
        cout << "Using binary manifest (mmap fast loading)..." << endl;
        BinaryCorpus bc = load_binary_manifest_mmap(binary_manifest_path, &chunk_id_to_index, has_mmap_idx);
        if (!bc.docs.empty()) {
            global_corpus.docs = std::move(bc.docs);
            global_corpus.inverted_index = std::move(bc.inverted_index);
            global_corpus.keyword_dict = std::move(bc.keyword_dict);
            global_corpus.keyword_to_id = std::move(bc.keyword_to_id);
            global_corpus.total_tokens = bc.total_tokens;
            global_corpus.avgdl = bc.avgdl;
            loaded = true;
        } else {
            cout << "Binary manifest load failed, falling back to JSONL..." << endl;
        }
    }
    if (!loaded) {
        cout << "Loading from JSONL (this may take a while for large corpora)..." << endl;
        global_corpus = load_manifest(MANIFEST);
    }

    if (global_corpus.docs.empty()) {
        cout << "Corpus is empty — starting with 0 chunks (add data via /add-file or /add-file-path)" << endl;
    }

    // low-memory: report RSS after corpus load
    {
        long rss_kb = 0;
        ifstream proc_status("/proc/self/status");
        string line;
        while (getline(proc_status, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                stringstream ss(line.substr(6));
                ss >> rss_kb;
                break;
            }
        }
        cout << "[mem] after corpus load: " << rss_kb / 1024 << " MB RSS, "
             << global_corpus.docs.size() << " chunks, "
             << global_corpus.inverted_index.size() << " keywords" << endl;
    }

    // v8.0: check for mmap index files (stems.idx, doc_keywords.idx, keywords.idx)
    // if present, skip building inverted_index/stem_cache/hapax in memory
    {
        // extract corpus directory from manifest path
        string corpus_dir;
        size_t last_slash = MANIFEST.rfind('/');
        if (last_slash != string::npos) {
            corpus_dir = MANIFEST.substr(0, last_slash);
        } else {
            corpus_dir = ".";
        }

        string stems_idx_path = corpus_dir + "/stems.idx";
        if (fs::exists(stems_idx_path)) {
            cout << "Found mmap index files in " << corpus_dir << ", using fast loading path..." << endl;
            if (g_mmap_idx_storage.load(corpus_dir)) {
                g_mmap_idx = &g_mmap_idx_storage;

                // free the in-memory inverted index + stem maps — mmap replaces them
                {
                    decltype(global_corpus.inverted_index) empty_inv;
                    global_corpus.inverted_index.swap(empty_inv);
                }
                {
                    decltype(global_corpus.tf_index) empty_tf;
                    global_corpus.tf_index.swap(empty_tf);
                }

                // use avgdl from mmap index
                global_corpus.avgdl = g_mmap_idx->avg_dl;

                cout << "[mmap] inverted_index and tf_index freed — using mmap for search" << endl;

                // report RSS
                {
                    long rss_kb = 0;
                    ifstream proc_status("/proc/self/status");
                    string line;
                    while (getline(proc_status, line)) {
                        if (line.substr(0, 6) == "VmRSS:") {
                            stringstream ss(line.substr(6));
                            ss >> rss_kb;
                            break;
                        }
                    }
                    cout << "[mem] after mmap index load: " << rss_kb / 1024 << " MB RSS" << endl;
                }
            } else {
                cout << "mmap index load failed, falling back to standard loading..." << endl;
                g_mmap_idx = nullptr;
            }
        }
    }

    if (!g_mmap_idx) {
        // legacy path: build stemmed inverted index for improved recall
        build_stemmed_index(global_corpus);

        // v4.7: Remove hapax legomena (DF=1 terms) — saves ~40-60% RAM with negligible recall loss
        apply_hapax_filter(global_corpus);
    }

    // report RSS
    {
        long rss_kb = 0;
        ifstream proc_status("/proc/self/status");
        string line;
        while (getline(proc_status, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                stringstream ss(line.substr(6));
                ss >> rss_kb;
                break;
            }
        }
        if (g_mmap_idx) {
            cout << "[mem] mmap path ready: " << rss_kb / 1024 << " MB RSS, "
                 << g_mmap_idx->stem_count << " stems (mmap), "
                 << g_mmap_idx->doc_count << " docs" << endl;
        } else {
            cout << "[mem] after hapax filter: " << rss_kb / 1024 << " MB RSS, "
                 << global_corpus.inverted_index.size() << " keywords in index, "
                 << g_stem_to_keywords.size() << " stems" << endl;
        }
    }

    // v4.4: Build shard-parallel search tentacles
    // OE_SHARDS env: 0=disabled, N=force N shards, unset=auto (100K per shard)
    {
        const char* shard_env = getenv("OE_SHARDS");
        int force_shards = shard_env ? atoi(shard_env) : -1;
        if (force_shards == 0) {
            cout << "Shards disabled (OE_SHARDS=0)" << endl;
        } else if (force_shards > 0) {
            size_t n = global_corpus.docs.size();
            global_shards.target_shard_size = max((size_t)1, n / force_shards);
            global_shards.build(global_corpus);
        } else {
            global_shards.target_shard_size = 100000;
            global_shards.build(global_corpus);
        }
    }

    // low-memory: report RSS after shards
    {
        long rss_kb = 0;
        ifstream proc_status("/proc/self/status");
        string line;
        while (getline(proc_status, line)) {
            if (line.substr(0, 6) == "VmRSS:") {
                stringstream ss(line.substr(6));
                ss >> rss_kb;
                break;
            }
        }
        if (g_mmap_idx) {
            cout << "[mem] after shards (mmap): " << rss_kb / 1024 << " MB RSS" << endl;
        } else {
            cout << "[mem] after stem index: " << rss_kb / 1024 << " MB RSS, "
                 << g_stem_cache.size() << " stem entries, "
                 << g_stem_to_keywords.size() << " reverse entries" << endl;
        }
    }

    // v9.1: Multi-segment detection and loading
    // If corpus_dir/meta.json exists with segment definitions, load as multi-segment
    {
        string corpus_dir;
        size_t sl = MANIFEST.rfind('/');
        corpus_dir = (sl != string::npos) ? MANIFEST.substr(0, sl) : ".";

        // check for meta.json (segment registry)
        string meta_path = corpus_dir + "/meta.json";
        if (fs::exists(meta_path)) {
            try {
                ifstream mf(meta_path);
                json meta = json::parse(mf);
                if (meta.contains("segments") && meta["segments"].is_array()) {
                    cout << "\n[multi-segment] Loading " << meta["segments"].size()
                         << " segments from " << meta_path << endl;

                    uint64_t cum_offset = 0;
                    for (const auto& seg_def : meta["segments"]) {
                        string seg_dir = seg_def.value("dir", "");
                        int seg_id = seg_def.value("id", (int)g_multi_seg.segments.size());

                        // resolve relative paths
                        if (!seg_dir.empty() && seg_dir[0] != '/') {
                            seg_dir = corpus_dir + "/" + seg_dir;
                        }

                        if (!fs::exists(seg_dir + "/stems.idx")) {
                            cerr << "[multi-segment] WARNING: stems.idx not found in " << seg_dir
                                 << ", skipping segment " << seg_id << endl;
                            continue;
                        }

                        SegmentIndex seg;
                        seg.id = seg_id;
                        seg.dir = seg_dir;
                        seg.doc_offset = cum_offset;

                        if (!seg.mmap_idx.load(seg_dir)) {
                            cerr << "[multi-segment] Failed to load segment " << seg_id
                                 << " from " << seg_dir << endl;
                            continue;
                        }

                        seg.doc_count = seg.mmap_idx.doc_count;
                        seg.avg_dl = seg.mmap_idx.avg_dl;

                        // load keyword dict from segment's manifest.bin
                        string seg_manifest_bin = seg_dir + "/manifest.bin";
                        if (fs::exists(seg_manifest_bin)) {
                            // read keyword dict from binary manifest header
                            BinaryCorpus bc = load_binary_manifest_mmap(seg_manifest_bin, nullptr, true);
                            seg.keyword_dict = move(bc.keyword_dict);
                        }

                        cum_offset += seg.doc_count;
                        g_segment_storage_paths.push_back(seg_dir + "/storage.bin");

                        cout << "[multi-segment] Segment " << seg.id << ": " << seg.doc_count
                             << " docs, dir=" << seg_dir << endl;

                        g_multi_seg.segments.push_back(move(seg));
                    }

                    if (!g_multi_seg.segments.empty()) {
                        g_multi_seg.global_doc_count = cum_offset;
                        // weighted avg_dl
                        double sum_dl = 0;
                        for (const auto& seg : g_multi_seg.segments) {
                            sum_dl += seg.avg_dl * seg.doc_count;
                        }
                        g_multi_seg.global_avg_dl = sum_dl / cum_offset;
                        g_multi_seg.loaded = true;

                        cout << "[multi-segment] " << g_multi_seg.segments.size()
                             << " segments loaded, " << cum_offset << " total docs" << endl;
                    }
                }
            } catch (const exception& e) {
                cerr << "[multi-segment] Error loading meta.json: " << e.what() << endl;
            }
        }
    }

    // Load conversation history
    cout << "Loading conversation history..." << endl;
    load_chat_history();
    // Count conversation chunks in main corpus
    int chat_chunks = 0;
    for (const auto& doc : global_corpus.docs) {
        if (has_prefix(doc.id, "CH")) chat_chunks++;
    }
    cout << "Current conversation chunks in database: " << chat_chunks << endl;

    // Feature 4: Load chapter guide for navigation
    cout << "Loading chapter guide..." << endl;
    load_chapter_guide();

    // Load original files catalog
    originals_catalog_path = "corpus/originals.json";
    cout << "Loading originals catalog..." << endl;
    load_originals_catalog();

    // Speed Improvement #2 & #3: Build BM25S pre-computed index with BlockWeakAnd
    // v3: Using original BM25 search (500ms) - no BM25S overhead
    cout << "\nReady! Using original BM25 search (~500ms)" << endl;
    // v4.2: Print configuration summary
    cout << "Search: dynamic top_k (base " << g_config.search.top_k
         << "), context_window=" << g_config.search.context_window
         << ", max_context=" << g_config.search.max_context_chars << " chars" << endl;
    cout << "Score threshold: " << g_config.search.score_threshold << " (fraction of top score)" << endl;
    if (g_config.reranker.enabled)
        cout << "Reranker: ENABLED (" << g_config.reranker.url << ")" << endl;
    else
        cout << "Reranker: disabled (enable in config.json)" << endl;
    if (g_config.auth.enabled)
        cout << "Auth: ENABLED (X-API-Key required)" << endl;
    if (g_config.rate_limit.enabled)
        cout << "Rate limit: ENABLED (" << g_config.rate_limit.requests_per_minute << " req/min per IP)" << endl;

    // Start HTTP server (blocks until shutdown signal)
    run_http_server();

    // v4.2: Join all active streaming threads before destroying globals
    {
        lock_guard<mutex> tlock(g_stream_threads_mutex);
        for (auto& t : g_stream_threads) {
            if (t.joinable()) t.join();
        }
        g_stream_threads.clear();
    }

    // Cleanup after shutdown
    cout << "Server shut down cleanly." << endl;
    return 0;
}
