// OceanEterna Chat Server - HTTP server for chat interface
// Based on ocean_benchmark_fast.cpp with added HTTP endpoints

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "json.hpp"

using json = nlohmann::json;
using namespace std;

// Configuration
const bool USE_EXTERNAL_API = true;
const string LOCAL_LLM_URL = "http://127.0.0.1:1234/v1/chat/completions";
const string LOCAL_MODEL = "qwen/qwen3-32b";
const string EXTERNAL_API_URL = "https://routellm.abacus.ai/v1/chat/completions";
const string EXTERNAL_API_KEY = "s2_1b3816923a5f4e72af9a3e4e1895ae12";
const string EXTERNAL_MODEL = "gpt-5-mini";

string MANIFEST = "guten_9m_build/manifest_guten9m.jsonl";
string STORAGE = "guten_9m_build/storage/guten9m.bin";
const int TOPK = 8;
const int LLM_TIMEOUT_SEC = 30;
const int HTTP_PORT = 8888;

struct DocMeta {
    string id;
    string summary;
    vector<string> keywords;
    uint64_t offset;
    uint64_t length;
    uint32_t start;
    uint32_t end;
    long long timestamp = 0;  // For conversation chunks
};

struct Corpus {
    vector<DocMeta> docs;
    unordered_map<string, vector<uint32_t>> inverted_index;
    size_t total_tokens = 0;
    double avgdl = 0;
};

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

// BM25 search
vector<Hit> search_bm25(const Corpus& corpus, const string& query, int topk) {
    // Extract query terms using same method as keyword extraction
    // This ensures query terms match the format of indexed keywords
    vector<string> query_terms = extract_text_keywords(query);

    const double k1 = 1.5;
    const double b = 0.75;
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
                term_freq[kw_lower]++;
            }

            double score = 0.0;
            for (const string& term : query_terms) {
                if (corpus.inverted_index.find(term) == corpus.inverted_index.end()) continue;

                int df = corpus.inverted_index.at(term).size();
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

// CURL callback
size_t curl_write_cb(void* contents, size_t size, size_t nmemb, string* s) {
    size_t new_length = size * nmemb;
    s->append((char*)contents, new_length);
    return new_length;
}

// Query LLM
pair<string, double> query_llm(const string& prompt) {
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

    // Debug: Log raw response
    cout << "===== RAW RESPONSE =====" << endl;
    cout << response_string.substr(0, 500) << endl;
    cout << "========================" << endl;

    try {
        json response = json::parse(response_string);

        // Debug: Log the response
        cout << "LLM Response parsed: " << response.dump().substr(0, 500) << endl;
        cout << "Has choices: " << response.contains("choices") << endl;
        if (response.contains("choices")) {
            cout << "Choices is array: " << response["choices"].is_array() << endl;
            cout << "Choices empty: " << response["choices"].empty() << endl;
        }

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
vector<string> extract_text_keywords(const string& text) {
    // Count word frequencies
    unordered_map<string, int> word_freq;
    string current_word;

    for (unsigned char c : text) {
        // Only process ASCII alphanumeric (skip UTF-8 high bytes)
        if (c < 128 && isalnum(c)) {
            current_word += (char)tolower(c);
        } else if (!current_word.empty()) {
            if (current_word.length() >= 3 && current_word.length() <= 30) {
                word_freq[current_word]++;
            }
            current_word.clear();
        }
    }
    if (!current_word.empty() && current_word.length() >= 3 && current_word.length() <= 30) {
        word_freq[current_word]++;
    }

    // Sort by frequency (descending), then alphabetically for ties
    vector<pair<string, int>> sorted_words(word_freq.begin(), word_freq.end());
    sort(sorted_words.begin(), sorted_words.end(),
         [](const pair<string,int>& a, const pair<string,int>& b) {
             if (a.second != b.second) return a.second > b.second;  // Higher freq first
             return a.first < b.first;  // Alphabetical for ties
         });

    // Keep all keywords (no limit - storage is cheap with compression)
    // This ensures proper nouns and rare but important words are captured
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
    cout << "add_file_to_index called with filename: " << filename << ", content size: " << content.size() << endl;

    const int CHUNK_SIZE = 2000;  // ~500 tokens worth of characters
    size_t content_len = content.length();
    cerr << "Content length: " << content_len << " bytes" << endl << flush;

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

    cerr << "Processing with " << num_threads << " threads, section size: " << section_size << endl << flush;

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
            cerr << "Thread " << tid << " processed " << my_chunks.size() << " chunks" << endl;
        }
    }

    // Merge all thread chunks
    vector<ChunkData> chunks;
    for (auto& tc : thread_chunks) {
        chunks.insert(chunks.end(), make_move_iterator(tc.begin()), make_move_iterator(tc.end()));
    }
    uploaded_chunk_counter = global_chunk_counter.load();
    cerr << "Total chunks: " << chunks.size() << endl << flush;
    // chunk counter already updated in parallel section
    cerr << "Parallel processing complete" << endl << flush;

    // PHASE 3: Sequential writes (single-threaded for consistency)
    cerr << "Phase 3: Writing to storage..." << endl << flush;

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
            cerr << "Written " << chunks_added << " / " << chunks.size() << " chunks" << endl << flush;
        }
    }
    cerr << "Write complete: " << chunks_added << " chunks" << endl << flush;

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

    cerr << "Building result JSON..." << endl << flush;
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
        cout << "[DEBUG] Self-referential query detected, prioritizing conversation search" << endl;

        // Allocate tentacles: 3 for conversation, 5 for documents
        const int CHAT_TENTACLES = 3;
        const int DOC_TENTACLES = TOPK - CHAT_TENTACLES;

        // Search conversation chunks first with dedicated tentacles
        vector<Hit> conv_hits = search_conversation_chunks_only(question, CHAT_TENTACLES);

        // Search document corpus with remaining tentacles
        vector<Hit> doc_hits = search_bm25(global_corpus, question, DOC_TENTACLES);

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
        cout << "[DEBUG] General query, using unified search with conversation boost" << endl;

        // Standard unified search for non-self-referential queries
        hits = search_bm25(global_corpus, question, TOPK);

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
void send_http_response(int client_socket, const string& content_type, const string& body) {
    string response = "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: " + content_type + "\r\n";
    response += "Access-Control-Allow-Origin: *\r\n";
    response += "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response += "Access-Control-Allow-Headers: Content-Type\r\n";
    response += "Content-Length: " + to_string(body.length()) + "\r\n";
    response += "\r\n";
    response += body;

    write(client_socket, response.c_str(), response.length());
}

// HTTP server (simplified - for production use proper library like cpp-httplib)
void run_http_server() {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        cerr << "Socket creation failed" << endl;
        return;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        cerr << "Bind failed" << endl;
        return;
    }

    if (listen(server_fd, 10) < 0) {
        cerr << "Listen failed" << endl;
        return;
    }

    cout << "\n🌊 OceanEterna Chat Server running on http://localhost:" << HTTP_PORT << endl;
    cout << "Open ocean_chat.html in your browser to start chatting!\n" << endl;

    while (true) {
        int client_socket;
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        client_socket = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) continue;

        // Read request - use larger buffer and multiple reads if needed
        char buffer[65536] = {0};  // 64KB buffer
        string request;

        // Set socket timeout to prevent hanging
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // First read
        int n = read(client_socket, buffer, sizeof(buffer) - 1);
        if (n > 0) {
            request.append(buffer, n);

            // Check if we need more data (Content-Length vs received)
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                // Find Content-Length (case-insensitive search)
                size_t cl_pos = request.find("Content-Length: ");
                if (cl_pos == string::npos) cl_pos = request.find("content-length: ");
                if (cl_pos == string::npos) cl_pos = request.find("Content-length: ");

                if (cl_pos != string::npos && cl_pos < body_start) {
                    int content_length = atoi(request.c_str() + cl_pos + 16);
                    int body_received = request.length() - body_start - 4;

                    // Read more if needed
                    while (body_received < content_length && body_received < 100000) {
                        memset(buffer, 0, sizeof(buffer));
                        int m = read(client_socket, buffer, sizeof(buffer) - 1);
                        if (m <= 0) break;
                        request.append(buffer, m);
                        body_received = request.length() - body_start - 4;
                    }
                }
            }
        }

        // Parse request
        if (request.find("GET /stats") != string::npos) {
            // Return system stats
            json stats_json = get_system_stats();
            send_http_response(client_socket, "application/json", stats_json.dump());
        }
        else if (request.find("POST /chat") != string::npos) {
            // Extract JSON body
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    if (!req.contains("question")) {
                        json error = {{"error", "Missing 'question' field"}};
                        send_http_response(client_socket, "application/json", error.dump());
                    } else {
                        string question = req["question"];
                        json response = handle_chat(question);
                        send_http_response(client_socket, "application/json", response.dump());
                    }
                } catch (const exception& e) {
                    json error = {{"error", "Invalid request"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            } else {
                json error = {{"error", "Invalid request"}};
                send_http_response(client_socket, "application/json", error.dump());
            }
        }
        else if (request.find("POST /clear-database") != string::npos) {
            // Clear conversation database
            try {
                bool success = clear_conversation_database();
                if (success) {
                    json response = {{"success", true}, {"message", "Database cleared successfully"}};
                    send_http_response(client_socket, "application/json", response.dump());
                } else {
                    json error = {{"success", false}, {"error", "Failed to clear database"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            } catch (const exception& e) {
                json error = {{"success", false}, {"error", e.what()}};
                send_http_response(client_socket, "application/json", error.dump());
            }
        }
        else if (request.find("POST /sources") != string::npos) {
            // Feature 2: Return sources for a given turn
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string turn_id = req["turn_id"];
                    json response = handle_source_query(turn_id);
                    send_http_response(client_socket, "application/json", response.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need turn_id"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /tell-me-more") != string::npos) {
            // Feature 2: Handle "tell me more" without re-searching
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string prev_turn_id = req["prev_turn_id"];
                    string aspect = req.value("aspect", "the topic");
                    json response = handle_tell_me_more(prev_turn_id, aspect);
                    send_http_response(client_socket, "application/json", response.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need prev_turn_id"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("GET /chunk/") != string::npos) {
            // Feature 2: Direct chunk retrieval by ID
            size_t id_start = request.find("GET /chunk/") + 11;
            size_t id_end = request.find(" ", id_start);
            string chunk_id = request.substr(id_start, id_end - id_start);

            auto [content, score] = get_chunk_by_id(chunk_id);
            if (score >= 0) {
                json response = {
                    {"success", true},
                    {"chunk_id", chunk_id},
                    {"content", content}
                };
                send_http_response(client_socket, "application/json", response.dump());
            } else {
                json error = {{"success", false}, {"error", "Chunk not found: " + chunk_id}};
                send_http_response(client_socket, "application/json", error.dump());
            }
        }
        else if (request.find("POST /reconstruct") != string::npos) {
            // Feature 3: Reconstruct context from chunk IDs
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);

                    vector<string> chunk_ids;
                    if (req.contains("chunk_ids") && req["chunk_ids"].is_array()) {
                        for (const auto& id : req["chunk_ids"]) {
                            chunk_ids.push_back(id.get<string>());
                        }
                    } else if (req.contains("text")) {
                        // Extract chunk IDs from text
                        chunk_ids = extract_chunk_ids_from_context(req["text"].get<string>());
                    }

                    string context = reconstruct_context_from_ids(chunk_ids);
                    double compression = calculate_compression_ratio(chunk_ids, context);

                    json response = {
                        {"success", true},
                        {"chunk_ids", chunk_ids},
                        {"context", context},
                        {"context_length", context.length()},
                        {"compression_ratio", compression}
                    };
                    send_http_response(client_socket, "application/json", response.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need chunk_ids array or text"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /extract-ids") != string::npos) {
            // Feature 3: Extract chunk IDs from text
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string text = req["text"].get<string>();

                    vector<string> chunk_ids = extract_chunk_ids_from_context(text);

                    json response = {
                        {"success", true},
                        {"chunk_ids", chunk_ids},
                        {"count", chunk_ids.size()}
                    };
                    send_http_response(client_socket, "application/json", response.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need text field"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("GET /guide") != string::npos) {
            // Feature 4: Return full chapter guide
            lock_guard<mutex> lock(chapter_guide_mutex);
            send_http_response(client_socket, "application/json", chapter_guide.dump(2));
        }
        else if (request.find("POST /query/code-discussions") != string::npos) {
            // Feature 4: Find discussions about a code chunk
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string chunk_id = req["chunk_id"].get<string>();
                    json result = query_code_discussions(chunk_id);
                    send_http_response(client_socket, "application/json", result.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need chunk_id"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /query/fixes") != string::npos) {
            // Feature 4: Find fixes for a file
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string filename = req["filename"].get<string>();
                    json result = query_fixes_for_file(filename);
                    send_http_response(client_socket, "application/json", result.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need filename"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /query/feature") != string::npos) {
            // Feature 4: Get feature implementation details
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string feature_id = req["feature_id"].get<string>();
                    json result = query_feature_implementation(feature_id);
                    send_http_response(client_socket, "application/json", result.dump());
                } catch (...) {
                    json error = {{"error", "Invalid request - need feature_id"}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /add-file-path") != string::npos) {
            // Add file from local path (for large files) - must come before /add-file
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                cout << "add-file-path received body: " << body.substr(0, 200) << endl;
                try {
                    json req = json::parse(body);
                    cout << "Parsed JSON: " << req.dump() << endl;
                    string filepath = req.value("path", "");
                    if (filepath.empty()) {
                        json error = {{"error", "Missing 'path' in request body"}, {"success", false}};
                        send_http_response(client_socket, "application/json", error.dump());
                        continue;
                    }

                    // Read file from disk
                    ifstream file(filepath);
                    if (!file.is_open()) {
                        json error = {{"error", "Cannot open file: " + filepath}, {"success", false}};
                        send_http_response(client_socket, "application/json", error.dump());
                    } else {
                        // Get file size
                        file.seekg(0, ios::end);
                        size_t file_size = file.tellg();
                        file.seekg(0, ios::beg);

                        cout << "Reading file from path: " << filepath << " (" << file_size << " bytes)" << endl;

                        // Read content
                        string content((istreambuf_iterator<char>(file)), istreambuf_iterator<char>());
                        file.close();

                        // Sanitize UTF-8: replace invalid bytes with space
                        for (size_t i = 0; i < content.size(); i++) {
                            unsigned char c = static_cast<unsigned char>(content[i]);
                            if (c > 127 && c < 192) {
                                // Invalid UTF-8 continuation byte without start
                                content[i] = ' ';
                            } else if (c >= 192 && c < 224) {
                                // 2-byte sequence
                                if (i + 1 >= content.size() || (static_cast<unsigned char>(content[i+1]) & 0xC0) != 0x80) {
                                    content[i] = ' ';
                                }
                            } else if (c >= 224 && c < 240) {
                                // 3-byte sequence
                                if (i + 2 >= content.size()) content[i] = ' ';
                            } else if (c >= 240) {
                                // 4-byte sequence
                                if (i + 3 >= content.size()) content[i] = ' ';
                            }
                        }

                        // Extract filename from path
                        string filename = filepath;
                        size_t slash_pos = filepath.rfind('/');
                        if (slash_pos != string::npos) {
                            filename = filepath.substr(slash_pos + 1);
                        }

                        json result = add_file_to_index(filename, content);
                        send_http_response(client_socket, "application/json", result.dump());
                    }
                } catch (const exception& e) {
                    json error = {{"error", string("Failed to add file: ") + e.what()}, {"success", false}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else if (request.find("POST /add-file") != string::npos) {
            // Add file to index (content in body - for small files)
            size_t body_start = request.find("\r\n\r\n");
            if (body_start != string::npos) {
                string body = request.substr(body_start + 4);
                try {
                    json req = json::parse(body);
                    string filename = req["filename"].get<string>();
                    string content = req["content"].get<string>();
                    cout << "Adding file to index: " << filename << " (" << content.length() << " chars)" << endl;
                    json result = add_file_to_index(filename, content);
                    send_http_response(client_socket, "application/json", result.dump());
                } catch (const exception& e) {
                    json error = {{"error", string("Failed to add file: ") + e.what()}, {"success", false}};
                    send_http_response(client_socket, "application/json", error.dump());
                }
            }
        }
        else {
            // OPTIONS or unknown request
            send_http_response(client_socket, "text/plain", "OK");
        }

        close(client_socket);
    }
}

int main(int argc, char** argv) {
    cout << "🌊 OceanEterna Chat Server" << endl;
    cout << "============================\n" << endl;

    // Load manifest
    global_corpus = load_manifest(MANIFEST);
    global_storage_path = STORAGE;

    if (global_corpus.docs.empty()) {
        cerr << "Failed to load corpus!" << endl;
        return 1;
    }

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

    // Start HTTP server
    run_http_server();

    return 0;
}
