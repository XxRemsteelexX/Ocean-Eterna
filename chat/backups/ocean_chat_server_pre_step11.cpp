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
#include <atomic>
#include <omp.h>
#include <lz4frame.h>
#include <curl/curl.h>
#include <regex>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <signal.h>
#include "httplib.h"
#include "json.hpp"
#include "binary_manifest.hpp"
#include "porter_stemmer.hpp"
// BM25S removed in v3 - using original BM25 search which is faster

using json = nlohmann::json;
using namespace std;

// Graceful shutdown support
atomic<bool> g_shutdown_requested(false);
httplib::Server* g_server_ptr = nullptr;

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

// Helper: read environment variable with fallback
string get_env_or(const char* name, const string& fallback) {
    const char* val = getenv(name);
    return val ? string(val) : fallback;
}

// Configuration struct -- loaded from config.json with env var overrides
struct Config {
    struct {
        int port = 8888;
        string host = "0.0.0.0";
    } server;

    struct {
        bool use_external = true;
        string external_url = "https://routellm.abacus.ai/v1/chat/completions";
        string external_model = "gpt-5-mini";
        string local_url = "http://127.0.0.1:1234/v1/chat/completions";
        string local_model = "qwen/qwen3-32b";
        string api_key;  // from env OCEAN_API_KEY
        int timeout_sec = 30;
        int max_retries = 3;
        int retry_backoff_ms = 1000;
    } llm;

    struct {
        int top_k = 8;
        double k1 = 1.5;
        double b = 0.75;
    } search;

    struct {
        string manifest = "guten_9m_build/manifest_guten9m.jsonl";
        string storage = "guten_9m_build/storage/guten9m.bin";
        string chapter_guide = "guten_9m_build/chapter_guide.json";
    } corpus;

    struct {
        bool enabled = false;
        string api_key = "";  // Server-side API key clients must provide via X-API-Key header
    } auth;

    struct {
        bool enabled = false;
        int requests_per_minute = 60;
    } rate_limit;
};

Config load_config(const string& path) {
    Config cfg;
    ifstream f(path);
    if (f.is_open()) {
        try {
            json j = json::parse(f);
            if (j.contains("server")) {
                cfg.server.port = j["server"].value("port", cfg.server.port);
                cfg.server.host = j["server"].value("host", cfg.server.host);
            }
            if (j.contains("llm")) {
                cfg.llm.use_external = j["llm"].value("use_external", cfg.llm.use_external);
                cfg.llm.external_url = j["llm"].value("external_url", cfg.llm.external_url);
                cfg.llm.external_model = j["llm"].value("external_model", cfg.llm.external_model);
                cfg.llm.local_url = j["llm"].value("local_url", cfg.llm.local_url);
                cfg.llm.local_model = j["llm"].value("local_model", cfg.llm.local_model);
                cfg.llm.timeout_sec = j["llm"].value("timeout_sec", cfg.llm.timeout_sec);
                cfg.llm.max_retries = j["llm"].value("max_retries", cfg.llm.max_retries);
                cfg.llm.retry_backoff_ms = j["llm"].value("retry_backoff_ms", cfg.llm.retry_backoff_ms);
            }
            if (j.contains("search")) {
                cfg.search.top_k = j["search"].value("top_k", cfg.search.top_k);
                cfg.search.k1 = j["search"].value("bm25_k1", cfg.search.k1);
                cfg.search.b = j["search"].value("bm25_b", cfg.search.b);
            }
            if (j.contains("corpus")) {
                cfg.corpus.manifest = j["corpus"].value("manifest", cfg.corpus.manifest);
                cfg.corpus.storage = j["corpus"].value("storage", cfg.corpus.storage);
                cfg.corpus.chapter_guide = j["corpus"].value("chapter_guide", cfg.corpus.chapter_guide);
            }
            if (j.contains("auth")) {
                cfg.auth.enabled = j["auth"].value("enabled", cfg.auth.enabled);
                cfg.auth.api_key = j["auth"].value("api_key", cfg.auth.api_key);
            }
            if (j.contains("rate_limit")) {
                cfg.rate_limit.enabled = j["rate_limit"].value("enabled", cfg.rate_limit.enabled);
                cfg.rate_limit.requests_per_minute = j["rate_limit"].value("requests_per_minute", cfg.rate_limit.requests_per_minute);
            }
            cout << "Loaded config from " << path << endl;
        } catch (const exception& e) {
            cerr << "Warning: Failed to parse " << path << ": " << e.what() << endl;
            cerr << "Using default configuration." << endl;
        }
    } else {
        cout << "No config.json found, using defaults." << endl;
    }

    // Environment variable overrides (highest priority)
    cfg.llm.api_key = get_env_or("OCEAN_API_KEY", "");
    string env_url = get_env_or("OCEAN_API_URL", "");
    if (!env_url.empty()) cfg.llm.external_url = env_url;
    string env_model = get_env_or("OCEAN_MODEL", "");
    if (!env_model.empty()) cfg.llm.external_model = env_model;
    string env_server_key = get_env_or("OCEAN_SERVER_API_KEY", "");
    if (!env_server_key.empty()) cfg.auth.api_key = env_server_key;

    return cfg;
}

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
    size_t total_tokens = 0;
    double avgdl = 0;
};

// Stemming support: stem cache and reverse mapping
unordered_map<string, string> g_stem_cache;              // keyword -> stem
unordered_map<string, vector<string>> g_stem_to_keywords; // stem -> original keywords

struct Hit {
    uint32_t doc_idx;
    double score;
    string context;
};

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
int chat_chunk_counter = 0;

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
unordered_map<string, ConversationTurn> recent_turns_cache;
const size_t MAX_CACHED_TURNS = 100;

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
            doc.summary = obj.value("summary", "");
            doc.offset = obj.value("offset", 0ULL);
            doc.length = obj.value("length", 0ULL);
            doc.start = obj.value("token_start", 0U);
            doc.end = obj.value("token_end", 0U);
            doc.timestamp = obj.value("timestamp", 0LL);

            if (obj.contains("keywords") && obj["keywords"].is_array()) {
                for (const auto& kw : obj["keywords"]) {
                    doc.keywords.push_back(kw.get<string>());
                }
            }

            corpus.docs.push_back(doc);

            // Build inverted index
            for (const string& kw : doc.keywords) {
                corpus.inverted_index[kw].push_back(corpus.docs.size() - 1);
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

// Compress chunk with LZ4
string compress_chunk(const string& input) {
    LZ4F_preferences_t prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.compressionLevel = 3;
    prefs.frameInfo.contentSize = input.size();

    size_t out_cap = LZ4F_compressFrameBound(input.size(), &prefs);
    string out(out_cap, '\0');

    size_t written = LZ4F_compressFrame(out.data(), out.size(),
                                       input.data(), input.size(), &prefs);
    if (LZ4F_isError(written)) {
        throw runtime_error("LZ4 compression failed");
    }

    out.resize(written);
    return out;
}

// Decompress chunk
string decompress_chunk(const string& storage_path, uint64_t offset, uint64_t length) {
    ifstream file(storage_path, ios::binary);
    if (!file.is_open()) return "";

    vector<char> compressed(length);
    file.seekg(offset);
    file.read(compressed.data(), length);
    file.close();

    size_t decomp_size = length * 10;
    vector<char> decompressed(decomp_size);

    LZ4F_decompressionContext_t ctx;
    LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION);

    size_t src_size = length;
    size_t dst_size = decomp_size;
    LZ4F_decompress(ctx, decompressed.data(), &dst_size, compressed.data(), &src_size, nullptr);
    LZ4F_freeDecompressionContext(ctx);

    return string(decompressed.data(), dst_size);
}

// Feature 2: O(1) chunk lookup by ID
// Returns pair<content, score> - score is 0 if not from a search result
pair<string, double> get_chunk_by_id(const string& chunk_id) {
    auto it = chunk_id_to_index.find(chunk_id);
    if (it == chunk_id_to_index.end()) {
        return {"", -1.0};  // Not found
    }

    uint32_t idx = it->second;
    if (idx >= global_corpus.docs.size()) {
        return {"", -1.0};  // Index out of bounds
    }

    const DocMeta& doc = global_corpus.docs[idx];
    string content = decompress_chunk(global_storage_path, doc.offset, doc.length);
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

// Feature 3: Extract chunk IDs from context text
// Looks for patterns like: 📎 chunk_id or [chunk_id] or guten9m_DOC_123
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
        formatted += "\n\n📎 Sources: ";
        for (size_t i = 0; i < refs.size(); i++) {
            formatted += refs[i].chunk_id;
            if (i < refs.size() - 1) formatted += ", ";
        }
    }

    // Add this response's chunk ID
    if (!turn_chunk_id.empty()) {
        formatted += "\n📎 This response: " + turn_chunk_id;
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

// Forward declaration
vector<string> extract_text_keywords(const string& text);

// Build stem cache and reverse mapping from corpus inverted index
void build_stemmed_index(const Corpus& corpus) {
    auto start = chrono::high_resolution_clock::now();
    cout << "Building stemmed index..." << flush;

    // Stem each unique keyword and build cache + reverse mapping
    for (const auto& [keyword, doc_list] : corpus.inverted_index) {
        string stemmed = porter::stem(keyword);
        g_stem_cache[keyword] = stemmed;
        g_stem_to_keywords[stemmed].push_back(keyword);
    }

    auto elapsed = chrono::duration<double>(chrono::high_resolution_clock::now() - start).count();
    cout << " Done! (" << g_stem_cache.size() << " keywords -> "
         << g_stem_to_keywords.size() << " stems in " << elapsed << "s)" << endl;
}

// Get stem for a keyword (uses cache, falls back to computing)
inline string get_stem(const string& word) {
    auto it = g_stem_cache.find(word);
    if (it != g_stem_cache.end()) return it->second;
    string stemmed = porter::stem(word);
    g_stem_cache[word] = stemmed;
    return stemmed;
}

// BM25 search with stemming support
vector<Hit> search_bm25(const Corpus& corpus, const string& query, int topk) {
    // Extract query terms using same method as keyword extraction
    vector<string> raw_terms = extract_text_keywords(query);

    // Stem query terms and deduplicate
    vector<string> query_terms;
    for (const string& t : raw_terms) {
        string stemmed = get_stem(t);
        if (find(query_terms.begin(), query_terms.end(), stemmed) == query_terms.end())
            query_terms.push_back(stemmed);
    }

    // Pre-compute df for each stemmed query term from original inverted index
    unordered_map<string, int> stem_df;
    for (const string& term : query_terms) {
        auto stk_it = g_stem_to_keywords.find(term);
        if (stk_it == g_stem_to_keywords.end()) continue;
        int df = 0;
        for (const string& orig_kw : stk_it->second) {
            auto inv_it = corpus.inverted_index.find(orig_kw);
            if (inv_it != corpus.inverted_index.end())
                df += inv_it->second.size();
        }
        stem_df[term] = df;
    }

    const double k1 = g_config.search.k1;
    const double b = g_config.search.b;
    const size_t N = corpus.docs.size();

    vector<pair<uint32_t, double>> scores;

    #pragma omp parallel
    {
        vector<pair<uint32_t, double>> local_scores;

        #pragma omp for
        for (size_t i = 0; i < corpus.docs.size(); i++) {
            const DocMeta& doc = corpus.docs[i];
            unordered_map<string, int> term_freq;

            for (const string& kw : doc.keywords) {
                string kw_lower = kw;
                transform(kw_lower.begin(), kw_lower.end(), kw_lower.begin(), ::tolower);
                // Look up stem from cache (read-only, populated at startup)
                auto it = g_stem_cache.find(kw_lower);
                string stemmed = (it != g_stem_cache.end()) ? it->second : porter::stem(kw_lower);
                term_freq[stemmed]++;
            }

            double score = 0.0;
            for (const string& term : query_terms) {
                auto df_it = stem_df.find(term);
                if (df_it == stem_df.end()) continue;

                int df = df_it->second;
                int tf = term_freq[term];

                if (tf > 0) {
                    double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);
                    int doc_len = doc.keywords.size();
                    double norm = k1 * (1.0 - b + b * doc_len / corpus.avgdl);
                    score += idf * (tf * (k1 + 1.0)) / (tf + norm);
                }
            }

            if (score > 0) {
                local_scores.push_back({i, score});
            }
        }

        #pragma omp critical
        scores.insert(scores.end(), local_scores.begin(), local_scores.end());
    }

    sort(scores.begin(), scores.end(), [](auto& a, auto& b) { return a.second > b.second; });

    vector<Hit> hits;
    for (int i = 0; i < min((int)scores.size(), topk); i++) {
        Hit hit;
        hit.doc_idx = scores[i].first;
        hit.score = scores[i].second;
        hits.push_back(hit);
    }

    return hits;
}

// v3: Use original BM25 search (proven 500ms performance)
// Wrapper for compatibility with existing call sites
vector<Hit> search_bm25_fast(const Corpus& corpus, const string& query, int topk) {
    return search_bm25(corpus, query, topk);
}

// CURL callback
size_t curl_write_cb(void* contents, size_t size, size_t nmemb, string* s) {
    size_t new_length = size * nmemb;
    s->append((char*)contents, new_length);
    return new_length;
}

// Single LLM call (no retry)
pair<string, double> query_llm_once(const string& prompt) {
    CURL* curl = curl_easy_init();
    if (!curl) return {"ERROR", 0};

    auto start = chrono::high_resolution_clock::now();

    json request;
    try {
        if (USE_EXTERNAL_API) {
            request["model"] = EXTERNAL_MODEL;
        } else {
            request["model"] = LOCAL_MODEL;
        }
        request["messages"] = {{{"role", "user"}, {"content", prompt}}};
        request["temperature"] = 0.3;
        request["max_tokens"] = 500;
        request["stream"] = false;
        request["stop"] = json::array({"\n\n", "Question:", "Context:"});
    } catch (...) {
        return {"ERROR", 0};
    }

    const string& api_url = USE_EXTERNAL_API ? EXTERNAL_API_URL : LOCAL_LLM_URL;
    curl_easy_setopt(curl, CURLOPT_URL, api_url.c_str());

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    if (USE_EXTERNAL_API) {
        string auth_header = "Authorization: Bearer " + EXTERNAL_API_KEY;
        headers = curl_slist_append(headers, auth_header.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    string request_body = request.dump();
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body.c_str());

    string response_string;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, LLM_TIMEOUT_SEC);

    CURLcode res = curl_easy_perform(curl);

    auto end = chrono::high_resolution_clock::now();
    double elapsed_ms = chrono::duration<double, milli>(end - start).count();

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return {"ERROR: " + string(curl_easy_strerror(res)), elapsed_ms};
    }

    DEBUG_LOG("===== RAW RESPONSE =====");
    DEBUG_LOG(response_string.substr(0, 500));
    DEBUG_LOG("========================");

    try {
        json response = json::parse(response_string);

#ifdef DEBUG_MODE
        cout << "LLM Response parsed: " << response.dump().substr(0, 500) << endl;
        cout << "Has choices: " << response.contains("choices") << endl;
        if (response.contains("choices")) {
            cout << "Choices is array: " << response["choices"].is_array() << endl;
            cout << "Choices empty: " << response["choices"].empty() << endl;
        }
#endif

        // Check if content exists and is not null
        if (!response.contains("choices") || response["choices"].empty()) {
            return {"ERROR: No choices in LLM response. Response: " + response.dump().substr(0, 200), elapsed_ms};
        }

        auto& choice = response["choices"][0];
        if (!choice.contains("message") || !choice["message"].contains("content")) {
            return {"ERROR: No content in LLM response", elapsed_ms};
        }

        // Handle null content
        if (choice["message"]["content"].is_null()) {
            return {"ERROR: LLM returned null content", elapsed_ms};
        }

        string answer = choice["message"]["content"].get<string>();

        // Trim whitespace
        answer.erase(0, answer.find_first_not_of(" \n\r\t"));
        answer.erase(answer.find_last_not_of(" \n\r\t") + 1);

        if (answer.empty()) {
            return {"ERROR: LLM returned empty response", elapsed_ms};
        }

        return {answer, elapsed_ms};
    } catch (const exception& e) {
        return {string("ERROR: Failed to parse LLM response: ") + e.what() + "\nResponse: " + response_string.substr(0, 200), elapsed_ms};
    }
}

// Query LLM with retry + exponential backoff
pair<string, double> query_llm(const string& prompt) {
    int max_retries = g_config.llm.max_retries;
    int backoff_ms = g_config.llm.retry_backoff_ms;
    double total_elapsed = 0;

    for (int attempt = 0; attempt <= max_retries; attempt++) {
        auto [answer, elapsed_ms] = query_llm_once(prompt);
        total_elapsed += elapsed_ms;

        // Success: answer doesn't start with "ERROR"
        if (answer.substr(0, 5) != "ERROR") {
            return {answer, total_elapsed};
        }

        // Last attempt -- return the error
        if (attempt == max_retries) {
            return {answer, total_elapsed};
        }

        // Check if error is retryable
        bool retryable = (answer.find("timeout") != string::npos ||
                         answer.find("TIMEOUT") != string::npos ||
                         answer.find("rate limit") != string::npos ||
                         answer.find("Rate limit") != string::npos ||
                         answer.find("502") != string::npos ||
                         answer.find("503") != string::npos ||
                         answer.find("CURLE_OPERATION_TIMEDOUT") != string::npos ||
                         answer.find("CURLE_COULDNT_CONNECT") != string::npos);

        if (!retryable) {
            return {answer, total_elapsed};  // Non-retryable error
        }

        int delay = backoff_ms * (1 << attempt);  // Exponential: 1s, 2s, 4s
        cerr << "LLM call failed (attempt " << (attempt + 1) << "/" << (max_retries + 1)
             << "), retrying in " << delay << "ms: " << answer.substr(0, 80) << endl;
        this_thread::sleep_for(chrono::milliseconds(delay));
    }

    return {"ERROR: Max retries exceeded", total_elapsed};
}

// Generate keywords for conversation turn
vector<string> generate_chat_keywords(const string& user_msg, const string& system_response) {
    vector<string> keywords;
    stringstream ss(user_msg + " " + system_response);
    string word;

    while (ss >> word) {
        // Simple keyword extraction - remove punctuation and convert to lowercase
        string clean_word;
        for (char c : word) {
            if (isalnum(c)) clean_word += tolower(c);
        }
        if (clean_word.length() >= 3 && clean_word.length() <= 20) {
            keywords.push_back(clean_word);
        }
    }

    // Remove duplicates and limit to 20 keywords
    sort(keywords.begin(), keywords.end());
    keywords.erase(unique(keywords.begin(), keywords.end()), keywords.end());
    if (keywords.size() > 20) keywords.resize(20);

    return keywords;
}

// Save conversation turn as a chunk in the main binary database
void save_conversation_turn(const ConversationTurn& turn) {
    // Create chunk content
    string chunk_content = "User: " + turn.user_message + "\n\nAssistant: " + turn.system_response;

    // Generate summary
    string summary = turn.user_message.substr(0, 100);
    if (turn.user_message.length() > 100) summary += "...";

    // Generate keywords
    auto keywords = generate_chat_keywords(turn.user_message, turn.system_response);

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
    manifest_entry["compression"] = "lz4";
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
    DocMeta doc;
    doc.id = turn.chunk_id;
    doc.summary = summary;
    doc.keywords = keywords;
    doc.offset = offset;
    doc.length = compressed_data.size();
    doc.start = 0;
    doc.end = 0;
    doc.timestamp = chrono::duration_cast<chrono::seconds>(turn.timestamp.time_since_epoch()).count();
    global_corpus.docs.push_back(doc);

    // Update inverted index
    for (const string& kw : keywords) {
        global_corpus.inverted_index[kw].push_back(global_corpus.docs.size() - 1);
    }
}

// Load chat history from main manifest
void load_chat_history() {
    cout << "Loading conversation history from main manifest..." << endl;

    // Find the highest CH chunk number from the global corpus
    for (const auto& doc : global_corpus.docs) {
        if (doc.id.substr(0, 2) == "CH") {
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
bool clear_conversation_database() {
    try {
        // Remove all CH chunks from in-memory corpus
        auto& docs = global_corpus.docs;
        docs.erase(remove_if(docs.begin(), docs.end(),
            [](const DocMeta& doc) { return doc.id.substr(0, 2) == "CH"; }), docs.end());

        // Clear conversation chunks from inverted index
        for (auto it = global_corpus.inverted_index.begin(); it != global_corpus.inverted_index.end();) {
            auto& indices = it->second;
            indices.erase(remove_if(indices.begin(), indices.end(),
                [&docs](uint32_t idx) {
                    return idx >= docs.size() || docs[idx].id.substr(0, 2) == "CH";
                }), indices.end());

            if (indices.empty()) {
                it = global_corpus.inverted_index.erase(it);
            } else {
                ++it;
            }
        }

        // Rebuild manifest without conversation chunks
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
                if (chunk_id.substr(0, 2) != "CH") {
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

// Extract keywords from text content (for file uploads)
// 2-char abbreviation whitelist (kept as keywords despite being short)
static const unordered_set<string> ABBREV_WHITELIST = {
    "ai", "ml", "us", "uk", "eu", "un", "os", "io", "db", "ip",
    "id", "ui", "ux", "qa", "hr", "it", "pc", "tv", "dj", "dc",
    "nj", "ny", "la", "sf", "co", "vs", "ph", "gp", "bp", "pr"
};

// Common English stop words to filter out
static const unordered_set<string> STOP_WORDS = {
    "the", "and", "for", "are", "but", "not", "you", "all", "can",
    "had", "her", "was", "one", "our", "out", "has", "his", "how",
    "its", "may", "new", "now", "old", "see", "way", "who", "did",
    "get", "let", "say", "she", "too", "use", "been", "each", "have",
    "this", "that", "with", "they", "will", "from", "what", "been",
    "more", "when", "some", "them", "than", "many", "very", "just",
    "also", "into", "over", "such", "only", "your", "most", "then",
    "make", "like", "does", "much", "here", "both", "made", "well",
    "were", "about", "would", "there", "their", "which", "could",
    "other", "these", "after", "those", "being", "should", "where"
};

vector<string> extract_text_keywords(const string& text) {
    // Count word frequencies
    unordered_map<string, int> word_freq;
    string current_word;

    for (unsigned char c : text) {
        // Only process ASCII alphanumeric (skip UTF-8 high bytes)
        if (c < 128 && isalnum(c)) {
            current_word += (char)tolower(c);
        } else if (!current_word.empty()) {
            // Accept: 3-30 char words, OR 2-char words in abbreviation whitelist
            if (current_word.length() <= 30 &&
                (current_word.length() >= 3 ||
                 (current_word.length() == 2 && ABBREV_WHITELIST.count(current_word)))) {
                if (!STOP_WORDS.count(current_word)) {
                    word_freq[current_word]++;
                }
            }
            current_word.clear();
        }
    }
    if (!current_word.empty() && current_word.length() <= 30 &&
        (current_word.length() >= 3 ||
         (current_word.length() == 2 && ABBREV_WHITELIST.count(current_word)))) {
        if (!STOP_WORDS.count(current_word)) {
            word_freq[current_word]++;
        }
    }

    // Sort by frequency (descending), then alphabetically for ties
    vector<pair<string, int>> sorted_words(word_freq.begin(), word_freq.end());
    sort(sorted_words.begin(), sorted_words.end(),
         [](const pair<string,int>& a, const pair<string,int>& b) {
             if (a.second != b.second) return a.second > b.second;
             return a.first < b.first;
         });

    // Keep all keywords (no limit - storage is cheap with compression)
    vector<string> keywords;
    keywords.reserve(sorted_words.size());
    for (const auto& word_pair : sorted_words) {
        keywords.push_back(word_pair.first);
    }

    return keywords;
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
    int token_start;
    int token_end;
};

// Add file content to index (PARALLELIZED)
json add_file_to_index(const string& filename, const string& content) {
    DEBUG_LOG("add_file_to_index called with filename: " << filename << ", content size: " << content.size());

    const int CHUNK_SIZE = 2000;  // ~500 tokens worth of characters
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

    // Divide file into sections for parallel processing
    int num_threads = omp_get_max_threads();
    size_t section_size = content_len / num_threads;
    if (section_size < CHUNK_SIZE * 10) section_size = content_len;  // Don't divide if file is small

    DEBUG_ERR("Processing with " << num_threads << " threads, section size: " << section_size);

    // Each thread will process its section and produce chunks
    vector<vector<ChunkData>> thread_chunks(num_threads);
    atomic<int> global_chunk_counter(uploaded_chunk_counter.load());

    #pragma omp parallel
    {
        int tid = omp_get_thread_num();
        size_t section_start = tid * section_size;
        size_t section_end = (tid == num_threads - 1) ? content_len : (tid + 1) * section_size;

        // Adjust section_start to paragraph boundary (except for first thread)
        if (tid > 0 && section_start < content_len) {
            size_t para = content.find("\n\n", section_start);
            if (para != string::npos && para < section_start + CHUNK_SIZE) {
                section_start = para + 2;
            }
        }

        vector<ChunkData>& my_chunks = thread_chunks[tid];
        size_t pos = section_start;

        while (pos < section_end && pos < content_len) {
            size_t target_end = min(pos + CHUNK_SIZE, content_len);
            size_t end_pos = target_end;

            // Find best chunk boundary
            if (end_pos < content_len) {
                size_t search_end = min(end_pos + 500, content_len);  // Larger search window
                size_t search_start = (pos + CHUNK_SIZE/2 < end_pos) ? pos + CHUNK_SIZE/2 : pos;

                // Priority 1: Look for article boundary (3+ newlines) - strongest signal
                size_t best_break = string::npos;
                for (size_t i = search_start; i < search_end - 2; i++) {
                    if (content[i] == '\n' && content[i+1] == '\n' && content[i+2] == '\n') {
                        best_break = i + 3;
                        break;
                    }
                }

                // Priority 2: Look for paragraph break (\n\n) near target
                if (best_break == string::npos) {
                    // First look forward
                    for (size_t i = target_end; i < search_end - 1; i++) {
                        if (content[i] == '\n' && content[i+1] == '\n') {
                            best_break = i + 2;
                            break;
                        }
                    }
                    // Then look backward if nothing forward
                    if (best_break == string::npos) {
                        for (size_t i = target_end; i > search_start; i--) {
                            if (content[i] == '\n' && i+1 < content_len && content[i+1] == '\n') {
                                best_break = i + 2;
                                break;
                            }
                        }
                    }
                }

                // Priority 3: Sentence boundary
                if (best_break == string::npos) {
                    for (size_t i = target_end; i > search_start; i--) {
                        if ((content[i-1] == '.' || content[i-1] == '!' || content[i-1] == '?') &&
                            (content[i] == ' ' || content[i] == '\n')) {
                            best_break = i;
                            break;
                        }
                    }
                }

                if (best_break != string::npos && best_break > pos) {
                    end_pos = best_break;
                }
            }

            if (end_pos <= pos) end_pos = min(pos + CHUNK_SIZE, content_len);
            if (end_pos <= pos) break;

            ChunkData chunk;
            chunk.start_pos = pos;
            chunk.end_pos = end_pos;
            chunk.chunk_text = content.substr(pos, end_pos - pos);

            int chunk_num = ++global_chunk_counter;
            chunk.chunk_id = clean_name + "_" + content_type + "_" + to_string(chunk_num);
            chunk.keywords = extract_text_keywords(chunk.chunk_text);
            chunk.summary = make_json_safe(chunk.chunk_text.substr(0, 150)).substr(0, 100);
            if (chunk.chunk_text.length() > 100) chunk.summary += "...";
            chunk.compressed = compress_chunk(chunk.chunk_text);

            my_chunks.push_back(move(chunk));
            pos = end_pos;
        }

        #pragma omp critical
        {
            DEBUG_ERR("Thread " << tid << " processed " << my_chunks.size() << " chunks");
        }
    }

    // Merge all thread chunks
    vector<ChunkData> chunks;
    for (auto& tc : thread_chunks) {
        chunks.insert(chunks.end(), make_move_iterator(tc.begin()), make_move_iterator(tc.end()));
    }
    uploaded_chunk_counter = global_chunk_counter.load();
    DEBUG_ERR("Total chunks: " << chunks.size());
    // chunk counter already updated in parallel section
    DEBUG_ERR("Parallel processing complete");

    // PHASE 3: Sequential writes (single-threaded for consistency)
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
    int total_tokens = 0;
    string safe_filename = make_json_safe(filename);

    for (auto& chunk : chunks) {
        // Write compressed data
        storage_file.write(chunk.compressed.data(), chunk.compressed.size());

        // Create manifest entry
        json entry;
        entry["code"] = "guten9m";
        entry["chunk_id"] = make_json_safe(chunk.chunk_id);
        entry["type"] = content_type;
        entry["source_file"] = safe_filename;
        entry["index"] = (int)global_corpus.docs.size();
        entry["token_start"] = total_tokens;
        entry["token_end"] = total_tokens + (int)(chunk.chunk_text.length() / 4);
        entry["offset"] = (long long)current_offset;
        entry["length"] = (int)chunk.compressed.size();
        entry["compression"] = "lz4";
        entry["summary"] = chunk.summary;

        json kw_array = json::array();
        for (const auto& kw : chunk.keywords) {
            kw_array.push_back(make_json_safe(kw));
        }
        entry["keywords"] = kw_array;

        manifest_file << entry.dump() << "\n";

        // Add to in-memory corpus
        DocMeta doc;
        doc.id = chunk.chunk_id;
        doc.summary = chunk.summary;
        doc.keywords = chunk.keywords;
        doc.offset = current_offset;
        doc.length = chunk.compressed.size();
        doc.start = total_tokens;
        doc.end = total_tokens + (chunk.chunk_text.length() / 4);
        global_corpus.docs.push_back(doc);

        // Update inverted index
        for (const string& kw : chunk.keywords) {
            global_corpus.inverted_index[kw].push_back(global_corpus.docs.size() - 1);
        }
        chunk_id_to_index[chunk.chunk_id] = global_corpus.docs.size() - 1;

        current_offset += chunk.compressed.size();
        total_tokens += chunk.chunk_text.length() / 4;
        chunks_added++;

        if (chunks_added % 10000 == 0) {
            DEBUG_ERR("Written " << chunks_added << " / " << chunks.size() << " chunks");
        }
    }
    DEBUG_ERR("Write complete: " << chunks_added << " chunks");

    storage_file.close();
    manifest_file.close();

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
    json result;
    result["success"] = true;
    result["filename"] = make_json_safe(filename);
    result["chunks_added"] = (int)chunks_added;
    result["tokens_added"] = (int)total_tokens;
    result["total_chunks"] = (int)global_corpus.docs.size();
    result["total_tokens"] = (long long)(global_corpus.total_tokens + total_tokens);
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
        if (lower_query.find(pattern) != string::npos) {
            return true;
        }
    }

    return false;
}

// Search conversation chunks specifically
vector<Hit> search_conversation_chunks_only(const string& query, int max_results) {
    vector<Hit> hits;

    for (size_t i = 0; i < global_corpus.docs.size(); ++i) {
        const auto& doc = global_corpus.docs[i];

        // Only search conversation chunks (CH prefix)
        if (doc.id.substr(0, 2) != "CH") continue;

        // Simple keyword matching for conversation chunks
        bool has_match = false;
        string query_lower = query;
        transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);

        // Check if any keywords from the conversation match the query
        for (const string& keyword : doc.keywords) {
            string kw_lower = keyword;
            transform(kw_lower.begin(), kw_lower.end(), kw_lower.begin(), ::tolower);

            // Bidirectional matching: query contains keyword OR keyword contains query term
            if (query_lower.find(kw_lower) != string::npos || kw_lower.find(query_lower) != string::npos) {
                has_match = true;
                break;
            }
        }

        // Also check summary for direct matches (in case keywords miss something)
        string summary_lower = doc.summary;
        transform(summary_lower.begin(), summary_lower.end(), summary_lower.begin(), ::tolower);
        if (summary_lower.find(query_lower) != string::npos) {
            has_match = true;
        }

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
    auto it = recent_turns_cache.find(turn_id);
    if (it != recent_turns_cache.end()) {
        const ConversationTurn& turn = it->second;

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

    // Not in cache - search manifest for turn_id
    auto idx_it = chunk_id_to_index.find(turn_id);
    if (idx_it == chunk_id_to_index.end()) {
        response["success"] = false;
        response["error"] = "Turn ID not found: " + turn_id;
        return response;
    }

    // Load the turn from manifest to get source_refs
    // For now, return the chunk content directly
    auto [content, _] = get_chunk_by_id(turn_id);
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

    // Check cache for previous turn
    auto it = recent_turns_cache.find(prev_turn_id);
    if (it == recent_turns_cache.end()) {
        response["success"] = false;
        response["error"] = "Previous turn not in cache. Use regular search instead.";
        response["prev_turn_id"] = prev_turn_id;
        return response;
    }

    const ConversationTurn& prev_turn = it->second;

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
    if (recent_turns_cache.size() >= MAX_CACHED_TURNS) {
        recent_turns_cache.erase(recent_turns_cache.begin());
    }
    recent_turns_cache[new_turn.chunk_id] = new_turn;

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
    if (stat(global_storage_path.c_str(), &st) == 0) {
        stats.db_size_mb = st.st_size / (1024 * 1024);
    }

    stats_json["cpu_usage"] = round(cpu_usage * 10) / 10.0;
    stats_json["ram_usage"] = round(ram_percent * 10) / 10.0;
    stats_json["ram_used_gb"] = round(ram_used_gb * 10) / 10.0;
    stats_json["ram_total_gb"] = round(ram_total_gb * 10) / 10.0;
    stats_json["total_tokens"] = global_corpus.total_tokens;
    stats_json["db_size_mb"] = stats.db_size_mb;
    stats_json["chunks_loaded"] = global_corpus.docs.size();
    stats_json["total_queries"] = stats.total_queries;
    stats_json["avg_search_ms"] = stats.total_queries > 0 ? stats.total_search_time_ms / stats.total_queries : 0;
    stats_json["avg_llm_ms"] = stats.total_queries > 0 ? stats.total_llm_time_ms / stats.total_queries : 0;

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

// Handle chat query
json handle_chat(const string& question) {
    json response;

    auto search_start = chrono::high_resolution_clock::now();

    // Smart tentacle allocation based on query type
    vector<Hit> hits;

    if (is_self_referential_query(question)) {
        DEBUG_LOG("[DEBUG] Self-referential query detected, prioritizing conversation search");

        // Allocate tentacles: 3 for conversation, 5 for documents
        const int CHAT_TENTACLES = 3;
        const int DOC_TENTACLES = TOPK - CHAT_TENTACLES;

        // Search conversation chunks first with dedicated tentacles
        vector<Hit> conv_hits = search_conversation_chunks_only(question, CHAT_TENTACLES);

        // Search document corpus with remaining tentacles
        vector<Hit> doc_hits = search_bm25_fast(global_corpus, question, DOC_TENTACLES);

        // Merge results with conversation taking priority
        for (const auto& hit : conv_hits) {
            hits.push_back(hit);
        }

        // Add document hits to fill remaining slots
        for (const auto& hit : doc_hits) {
            if (hits.size() >= TOPK) break;

            // Only add if it's not a conversation chunk (avoid duplicates)
            if (hit.doc_idx < global_corpus.docs.size() &&
                global_corpus.docs[hit.doc_idx].id.substr(0, 2) != "CH") {
                hits.push_back(hit);
            }
        }
    } else {
        DEBUG_LOG("[DEBUG] General query, using unified search with conversation boost");

        // Standard unified search for non-self-referential queries
        hits = search_bm25_fast(global_corpus, question, TOPK);

        // Boost scores for conversation chunks
        for (auto& hit : hits) {
            if (hit.doc_idx < global_corpus.docs.size() &&
                global_corpus.docs[hit.doc_idx].id.substr(0, 2) == "CH") {
                hit.score *= 1.5;  // 50% boost for conversation chunks
            }
        }

        // Re-sort by score after boosting
        sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            return a.score > b.score;
        });
    }

    auto search_end = chrono::high_resolution_clock::now();
    double search_ms = chrono::duration<double, milli>(search_end - search_start).count();

    // Build context from search results
    string context;
    bool has_conversation_context = false;
    bool has_document_context = false;

    for (auto& hit : hits) {
        if (hit.doc_idx < global_corpus.docs.size()) {
            string chunk_text = decompress_chunk(global_storage_path,
                                                global_corpus.docs[hit.doc_idx].offset,
                                                global_corpus.docs[hit.doc_idx].length);

            // Sanitize UTF-8 to prevent JSON errors
            chunk_text = make_json_safe(chunk_text);

            // Track what types of context we have
            if (global_corpus.docs[hit.doc_idx].id.substr(0, 2) == "CH") {
                context += "[Previous conversation]\n" + chunk_text + "\n\n---\n\n";
                has_conversation_context = true;
            } else {
                context += chunk_text + "\n\n---\n\n";
                has_document_context = true;
            }
        }
    }

    // If we have both types and it's a self-referential query, let LLM know to prioritize personal info
    string context_note = "";
    if (is_self_referential_query(question) && has_conversation_context && has_document_context) {
        context_note = "Note: This appears to be a personal question. Prioritize information from [Previous conversation] sections over document content.\n\n";
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
    for (const auto& hit : hits) {
        if (hit.doc_idx < global_corpus.docs.size()) {
            ChunkReference ref;
            ref.chunk_id = global_corpus.docs[hit.doc_idx].id;
            ref.relevance_score = hit.score;

            // Get snippet from the decompressed context (already loaded above)
            string chunk_content = decompress_chunk(global_storage_path,
                                                   global_corpus.docs[hit.doc_idx].offset,
                                                   global_corpus.docs[hit.doc_idx].length);
            ref.snippet = create_snippet(make_json_safe(chunk_content));

            turn.source_refs.push_back(ref);
        }
    }

    // Save conversation turn to main database
    save_conversation_turn(turn);

    // Feature 4: Update chapter guide with new conversation
    update_chapter_guide_conversation(turn);

    // Feature 2: Cache the turn for "tell me more" queries
    if (recent_turns_cache.size() >= MAX_CACHED_TURNS) {
        recent_turns_cache.erase(recent_turns_cache.begin());
    }
    recent_turns_cache[turn.chunk_id] = turn;

    // Feature 2: Also update the chunk_id_to_index for the new chunk
    chunk_id_to_index[turn.chunk_id] = global_corpus.docs.size() - 1;

    // Update stats
    stats.total_queries++;
    stats.total_search_time_ms += search_ms;
    stats.total_llm_time_ms += llm_ms;

    // Feature 3: Format answer with chunk references for infinite context
    string formatted_answer = format_response_with_refs(answer, turn.source_refs, turn.chunk_id);

    response["answer"] = make_json_safe(answer);
    response["formatted_answer"] = make_json_safe(formatted_answer);
    response["search_time_ms"] = search_ms;
    response["llm_time_ms"] = llm_ms;
    response["total_time_ms"] = search_ms + llm_ms;
    response["chunks_retrieved"] = hits.size();
    response["turn_id"] = turn.chunk_id;

    // Include source IDs
    json source_ids = json::array();
    for (const auto& ref : turn.source_refs) {
        source_ids.push_back({
            {"chunk_id", make_json_safe(ref.chunk_id)},
            {"score", ref.relevance_score}
        });
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

// Simple HTTP server response
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
        json health = {{"status", "ok"}, {"version", "4.0"}};
        res.set_content(health.dump(), "application/json");
    });

    // POST /chat
    svr.Post("/chat", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            if (!body.contains("question")) {
                res.set_content(json({{"error", "Missing 'question' field"}}).dump(), "application/json");
                return;
            }
            string question = body["question"];
            json response = handle_chat(question);
            res.set_content(response.dump(), "application/json");
        } catch (const exception& e) {
            res.set_content(json({{"error", "Invalid request"}}).dump(), "application/json");
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
            res.set_content(response.dump(), "application/json");
        } catch (...) {
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
            res.set_content(response.dump(), "application/json");
        } catch (...) {
            res.set_content(json({{"error", "Invalid request - need prev_turn_id"}}).dump(), "application/json");
        }
    });

    // GET /chunk/:id - use regex pattern matching
    svr.Get(R"(/chunk/(.+))", [](const httplib::Request& req, httplib::Response& res) {
        string chunk_id = req.matches[1];
        auto [content, score] = get_chunk_by_id(chunk_id);
        if (score >= 0) {
            json response = {{"success", true}, {"chunk_id", chunk_id}, {"content", content}};
            res.set_content(response.dump(), "application/json");
        } else {
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
            res.set_content(json({{"error", "Invalid request - need text field"}}).dump(), "application/json");
        }
    });

    // GET /guide
    svr.Get("/guide", [](const httplib::Request&, httplib::Response& res) {
        lock_guard<mutex> lock(chapter_guide_mutex);
        res.set_content(chapter_guide.dump(2), "application/json");
    });

    // POST /query/code-discussions
    svr.Post("/query/code-discussions", [](const httplib::Request& req, httplib::Response& res) {
        try {
            json body = json::parse(req.body);
            string chunk_id = body["chunk_id"].get<string>();
            json result = query_code_discussions(chunk_id);
            res.set_content(result.dump(), "application/json");
        } catch (...) {
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

    cout << "\n🌊 OceanEterna Chat Server running on http://localhost:" << HTTP_PORT << endl;
    cout << "Open ocean_chat.html in your browser to start chatting!\n" << endl;

    svr.listen(g_config.server.host.c_str(), g_config.server.port);
}

int main(int argc, char** argv) {
    // Install signal handlers for graceful shutdown
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    cout << "🌊 OceanEterna Chat Server v4.0 (Binary Manifest + Fast BM25)" << endl;
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

    // Load manifest - try binary format first for faster loading
    global_storage_path = STORAGE;
    string binary_manifest_path = get_binary_manifest_path(MANIFEST);

    if (binary_manifest_is_current(binary_manifest_path, MANIFEST)) {
        cout << "Using binary manifest (fast loading)..." << endl;
        BinaryCorpus bc = load_binary_manifest(binary_manifest_path, &chunk_id_to_index);
        global_corpus.docs = std::move(bc.docs);
        global_corpus.inverted_index = std::move(bc.inverted_index);
        global_corpus.total_tokens = bc.total_tokens;
        global_corpus.avgdl = bc.avgdl;
    } else {
        cout << "Binary manifest not found or outdated, using JSONL (slow)..." << endl;
        cout << "Run 'convert_manifest " << MANIFEST << "' to create binary manifest" << endl;
        global_corpus = load_manifest(MANIFEST);
    }

    if (global_corpus.docs.empty()) {
        cerr << "Failed to load corpus!" << endl;
        return 1;
    }

    // Build stemmed inverted index for improved recall
    build_stemmed_index(global_corpus);

    // Load conversation history
    cout << "Loading conversation history..." << endl;
    load_chat_history();
    // Count conversation chunks in main corpus
    int chat_chunks = 0;
    for (const auto& doc : global_corpus.docs) {
        if (doc.id.substr(0, 2) == "CH") chat_chunks++;
    }
    cout << "Current conversation chunks in database: " << chat_chunks << endl;

    // Feature 4: Load chapter guide for navigation
    cout << "Loading chapter guide..." << endl;
    load_chapter_guide();

    // Speed Improvement #2 & #3: Build BM25S pre-computed index with BlockWeakAnd
    // v3: Using original BM25 search (500ms) - no BM25S overhead
    cout << "\nReady! Using original BM25 search (~500ms)" << endl;
    if (g_config.auth.enabled)
        cout << "Auth: ENABLED (X-API-Key required)" << endl;
    if (g_config.rate_limit.enabled)
        cout << "Rate limit: ENABLED (" << g_config.rate_limit.requests_per_minute << " req/min per IP)" << endl;

    // Start HTTP server (blocks until shutdown signal)
    run_http_server();

    // Cleanup after shutdown
    cout << "Server shut down cleanly." << endl;
    return 0;
}
