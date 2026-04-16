// bulk_build.cpp — Two-pass disk-backed corpus builder for Ocean Eterna v9
// v9: Two-pass architecture for 50B+ token corpora under 10GB RAM.
//
// Pass 1 (Streaming):
//   - Stream files, chunk, extract keywords + TF
//   - Write storage.bin + manifest.jsonl immediately (unchanged)
//   - Write keyword data to keywords.tmp (flat binary on disk)
//   - Keep only ChunkSlim (~60 bytes/chunk) + KeywordIntern + doc_freq in RAM
//
// Pass 2 (IDF Sort + Index Generation):
//   - mmap keywords.tmp (read-only, zero RAM cost)
//   - Neighbor propagation via mmap'd sliding window → keywords_propagated.tmp
//   - TF²*IDF re-sort → keywords_sorted.tmp
//   - Generate stems.idx, keywords.idx, doc_keywords.idx, manifest.bin from sorted data
//
// Memory budget at 50B tokens (~5.5M chunks):
//   KeywordIntern: ~2GB | ChunkSlim vector: ~330MB | doc_freq: ~200MB
//   Working buffers: ~500MB | Total: ~3-4GB RAM (vs 30-40GB in v8)
//
// compile:
//   g++ -O3 -std=c++17 -march=native -fopenmp -o bulk_build bulk_build.cpp -lzstd -lpthread
//
// usage:
//   ./bulk_build --dirs corpus-sources/gutenberg corpus-sources/wikipedia \
//                --out corpus/ --max-tokens 1000000000

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <cstring>
#include <cmath>
#include <omp.h>
#include <zstd.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include "porter_stemmer.hpp"

using namespace std;
namespace fs = std::filesystem;

// ===================== RSS monitor =====================

static size_t get_rss_mb() {
    ifstream f("/proc/self/status");
    string line;
    while (getline(f, line)) {
        if (line.substr(0, 6) == "VmRSS:") {
            size_t kb = 0;
            for (char c : line) { if (isdigit(c)) kb = kb * 10 + (c - '0'); }
            return kb / 1024;
        }
    }
    return 0;
}

// ===================== global keyword intern =====================

struct KeywordIntern {
    unordered_map<string, uint32_t> str_to_id;
    vector<string> id_to_str;

    uint32_t intern(const string& s) {
        auto it = str_to_id.find(s);
        if (it != str_to_id.end()) return it->second;
        uint32_t id = id_to_str.size();
        str_to_id[s] = id;
        id_to_str.push_back(s);
        return id;
    }

    const string& lookup(uint32_t id) const { return id_to_str[id]; }
    size_t size() const { return id_to_str.size(); }
};

// ===================== lightweight per-chunk metadata =====================
// ~60 bytes per chunk — at 5.5M chunks (50B tokens) this is ~330MB total.
// NO keyword_ids or tf_values in memory — those live on disk in keywords.tmp.

struct ChunkSlim {
    string chunk_id;         // ~12 bytes (chapter_code + "_" + number)
    string source_file;      // ~30 bytes
    string chapter_code;     // ~8 bytes
    uint64_t kw_file_offset; // position in keywords.tmp
    uint16_t num_keywords;   // keyword count for this chunk
    uint64_t storage_offset; // position in storage.bin
    uint64_t storage_length; // compressed size in storage.bin
    uint64_t token_start;
    uint64_t token_end;
};

// ===================== keyword extraction =====================

static const unordered_set<string> ABBREV_WHITELIST = {
    "ai", "ml", "us", "uk", "eu", "un", "os", "io", "db", "ip",
    "id", "ui", "ux", "qa", "hr", "it", "pc", "tv", "dj", "dc",
    "nj", "ny", "la", "sf", "co", "vs", "ph", "gp", "bp", "pr"
};

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

static inline bool is_valid_token(const string& w) {
    return w.length() <= 30 &&
           (w.length() >= 3 || (w.length() == 2 && ABBREV_WHITELIST.count(w)));
}

pair<vector<string>, unordered_map<string, uint16_t>>
extract_keywords_tf(const string& text) {
    unordered_map<string, int> word_freq;
    string current_word;
    vector<string> token_stream;

    for (unsigned char c : text) {
        if (c < 128 && isalnum(c)) {
            current_word += (char)tolower(c);
        } else if (!current_word.empty()) {
            if (is_valid_token(current_word) && !STOP_WORDS.count(current_word)) {
                word_freq[current_word]++;
                token_stream.push_back(current_word);
            }
            current_word.clear();
        }
    }
    if (!current_word.empty() && is_valid_token(current_word) && !STOP_WORDS.count(current_word)) {
        word_freq[current_word]++;
        token_stream.push_back(current_word);
    }

    for (size_t i = 0; i + 1 < token_stream.size(); i++) {
        string bigram = token_stream[i] + "_" + token_stream[i + 1];
        if (bigram.length() <= 61) word_freq[bigram]++;
    }

    vector<pair<string, int>> sorted_words(word_freq.begin(), word_freq.end());
    sort(sorted_words.begin(), sorted_words.end(),
         [](const pair<string,int>& a, const pair<string,int>& b) {
             if (a.second != b.second) return a.second > b.second;
             return a.first < b.first;
         });

    vector<string> keywords;
    unordered_map<string, uint16_t> tf_map;
    size_t bigram_count = 0;
    const size_t max_bigrams = 50;
    keywords.reserve(sorted_words.size());
    for (const auto& wp : sorted_words) {
        bool is_bigram = (wp.first.find('_') != string::npos);
        if (is_bigram) {
            if (bigram_count >= max_bigrams) continue;
            bigram_count++;
        }
        keywords.push_back(wp.first);
        tf_map[wp.first] = static_cast<uint16_t>(min(wp.second, 65535));
    }
    return {keywords, tf_map};
}

// ===================== chapter code generation =====================

string generate_chapter_code(const string& filename) {
    string name = filename;
    size_t dot = name.rfind('.');
    if (dot != string::npos) name = name.substr(0, dot);

    int digits = 0, letters = 0;
    for (char c : name) {
        if (isdigit(c)) digits++;
        else if (isalpha(c)) letters++;
    }

    string code;
    if (digits > letters) {
        for (char c : name) { if (isalnum(c)) code += tolower(c); }
    } else {
        static const string vowels = "aeiouAEIOU";
        for (char c : name) {
            if (isalpha(c) && vowels.find(c) == string::npos) code += tolower(c);
            else if (isdigit(c)) code += c;
        }
    }

    if (code.empty()) code = "unk";
    if (code.length() > 8) code = code.substr(0, 8);
    return code;
}

struct ChapterRegistry {
    unordered_map<string, string> file_to_code;
    unordered_map<string, string> code_to_file;

    string assign(const string& filename) {
        auto it = file_to_code.find(filename);
        if (it != file_to_code.end()) return it->second;

        string base = generate_chapter_code(filename);
        string code = base;

        if (code_to_file.count(code) && code_to_file[code] != filename) {
            int suffix = 2;
            while (code_to_file.count(base + to_string(suffix))) suffix++;
            code = base + to_string(suffix);
        }

        file_to_code[filename] = code;
        code_to_file[code] = filename;
        return code;
    }
};

// ===================== chunking =====================

// heavy struct — only lives during batch processing, then discarded
struct ChunkOut {
    string chunk_id;
    string chunk_text;
    string compressed;
    vector<string> keywords;
    unordered_map<string, uint16_t> tf_map;
    string summary;
    string source_file;
    string chapter_code;
    string prev_id;
    string next_id;
};

string make_json_safe(const string& s) {
    string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32) continue;
        else if (c > 127) out += ' ';
        else out += (char)c;
    }
    return out;
}

string compress_zstd(const string& input) {
    size_t bound = ZSTD_compressBound(input.size());
    string out(bound, '\0');
    size_t written = ZSTD_compress(out.data(), out.size(),
                                   input.data(), input.size(), 3);
    if (ZSTD_isError(written)) {
        cerr << "zstd error: " << ZSTD_getErrorName(written) << endl;
        return "";
    }
    out.resize(written);
    return out;
}

vector<ChunkOut> chunk_file(const string& filename, const string& content,
                            const string& chapter_code) {
    const int CHUNK_SIZE = 15000;
    vector<ChunkOut> chunks;
    int file_chunk_num = 0;

    vector<pair<size_t, size_t>> paragraphs;
    size_t pos = 0;
    size_t content_len = content.length();
    while (pos < content_len) {
        size_t para_end = content.find("\n\n", pos);
        if (para_end == string::npos) para_end = content_len;
        else para_end += 2;

        size_t text_start = pos;
        while (text_start < para_end &&
               (content[text_start] == '\n' || content[text_start] == ' ' || content[text_start] == '\t'))
            text_start++;
        if (text_start < para_end) paragraphs.push_back({pos, para_end});
        pos = para_end;
    }

    size_t current_start = 0, current_end = 0;
    bool started = false;

    auto emit_chunk = [&](size_t start, size_t end) {
        ChunkOut c;
        file_chunk_num++;
        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "%03d", file_chunk_num);
        c.chunk_id = chapter_code + "_" + string(num_buf);
        c.chunk_text = content.substr(start, end - start);
        c.source_file = make_json_safe(filename);
        c.chapter_code = chapter_code;
        auto [kws, tf] = extract_keywords_tf(c.chunk_text);
        c.keywords = kws;
        c.tf_map = tf;
        string summary = make_json_safe(c.chunk_text.substr(0, 150)).substr(0, 100);
        if (c.chunk_text.length() > 100) summary += "...";
        c.summary = summary;
        c.compressed = compress_zstd(c.chunk_text);
        chunks.push_back(move(c));
    };

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
        if (current_len + para_len > CHUNK_SIZE && current_len > 0) {
            emit_chunk(current_start, current_end);
            current_start = pstart;
            current_end = pend;
        } else {
            current_end = pend;
        }
    }
    if (started && current_end > current_start) {
        emit_chunk(current_start, current_end);
    }

    for (size_t i = 0; i < chunks.size(); i++) {
        if (i > 0) chunks[i].prev_id = chunks[i-1].chunk_id;
        if (i + 1 < chunks.size()) chunks[i].next_id = chunks[i+1].chunk_id;
    }

    return chunks;
}

// streaming version for large files (>50MB): reads in segments to avoid RAM spikes
vector<ChunkOut> chunk_file_streaming(const string& filepath, const string& filename,
                                       const string& chapter_code) {
    const size_t SEGMENT_SIZE = 50 * 1024 * 1024; // 50MB
    vector<ChunkOut> all_chunks;
    int file_chunk_num = 0;

    ifstream f(filepath, ios::binary);
    if (!f.is_open()) return {};

    string leftover;

    while (f.good() || !leftover.empty()) {
        // read segment
        string segment(SEGMENT_SIZE, '\0');
        f.read(segment.data(), SEGMENT_SIZE);
        size_t bytes_read = f.gcount();
        segment.resize(bytes_read);

        if (segment.empty() && leftover.empty()) break;

        string content = leftover + segment;
        leftover.clear();

        if (f.good() && !f.eof()) {
            // find last paragraph boundary to avoid splitting mid-paragraph
            size_t last_break = content.rfind("\n\n");
            if (last_break != string::npos && last_break > content.size() / 2) {
                leftover = content.substr(last_break + 2);
                content.resize(last_break + 2);
            }
        }

        // sanitize
        for (size_t j = 0; j < content.size(); j++) {
            unsigned char c = static_cast<unsigned char>(content[j]);
            if (c > 127 && c < 192) content[j] = ' ';
        }

        // chunk this segment
        auto segment_chunks = chunk_file(filename, content, chapter_code);

        // renumber chunk IDs sequentially across segments
        for (auto& c : segment_chunks) {
            file_chunk_num++;
            char num_buf[16];
            snprintf(num_buf, sizeof(num_buf), "%03d", file_chunk_num);
            c.chunk_id = chapter_code + "_" + string(num_buf);
        }

        // fix cross-references between segments
        if (!all_chunks.empty() && !segment_chunks.empty()) {
            all_chunks.back().next_id = segment_chunks.front().chunk_id;
            segment_chunks.front().prev_id = all_chunks.back().chunk_id;
        }

        all_chunks.insert(all_chunks.end(),
            make_move_iterator(segment_chunks.begin()),
            make_move_iterator(segment_chunks.end()));
    }

    return all_chunks;
}

// ===================== manifest writing =====================

string manifest_line(const ChunkOut& c, uint64_t offset, uint64_t length,
                     uint64_t token_start, uint64_t token_end) {
    string s = "{";
    s += "\"chunk_id\":\"" + make_json_safe(c.chunk_id) + "\"";
    s += ",\"type\":\"DOC\"";
    s += ",\"source_file\":\"" + c.source_file + "\"";
    s += ",\"chapter_code\":\"" + c.chapter_code + "\"";
    s += ",\"token_start\":" + to_string(token_start);
    s += ",\"token_end\":" + to_string(token_end);
    s += ",\"offset\":" + to_string(offset);
    s += ",\"length\":" + to_string(length);
    s += ",\"compression\":\"zstd\"";
    s += ",\"summary\":\"" + make_json_safe(c.summary) + "\"";
    if (!c.prev_id.empty()) s += ",\"prev_chunk_id\":\"" + make_json_safe(c.prev_id) + "\"";
    if (!c.next_id.empty()) s += ",\"next_chunk_id\":\"" + make_json_safe(c.next_id) + "\"";

    s += ",\"keywords\":[";
    for (size_t i = 0; i < c.keywords.size(); i++) {
        if (i) s += ",";
        s += "\"" + make_json_safe(c.keywords[i]) + "\"";
    }
    s += "]";

    if (!c.tf_map.empty()) {
        s += ",\"tf\":{";
        bool first_tf = true;
        for (const auto& [kw, tf] : c.tf_map) {
            if (tf <= 1) continue;
            if (!first_tf) s += ",";
            first_tf = false;
            s += "\"" + make_json_safe(kw) + "\":" + to_string(tf);
        }
        s += "}";
    }

    s += "}";
    return s;
}

// ===================== mmap helper =====================

struct MmapFile {
    const uint8_t* data = nullptr;
    size_t size = 0;
    int fd = -1;

    bool open(const string& path) {
        fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { cerr << "mmap: cannot open " << path << endl; return false; }
        struct stat st;
        fstat(fd, &st);
        size = st.st_size;
        if (size == 0) { ::close(fd); fd = -1; return true; }
        data = (const uint8_t*)mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (data == MAP_FAILED) {
            data = nullptr;
            ::close(fd); fd = -1;
            cerr << "mmap: mapping failed for " << path << endl;
            return false;
        }
        // advise sequential access (pages loaded on demand, not prefaulted)
        madvise((void*)data, size, MADV_SEQUENTIAL);
        return true;
    }

    // drop cached pages from memory (reduces RSS without unmapping)
    void drop_pages() {
        if (data && size > 0) {
            madvise((void*)data, size, MADV_DONTNEED);
        }
    }

    void close() {
        if (data && size > 0) {
            madvise((void*)data, size, MADV_DONTNEED);
            munmap((void*)data, size);
            data = nullptr;
        }
        if (fd >= 0) { ::close(fd); fd = -1; }
        size = 0;
    }

    ~MmapFile() { close(); }
};

// ===================== read keywords from mmap'd file =====================

static inline vector<pair<uint32_t, uint16_t>>
read_chunk_keywords(const uint8_t* mmap_base, uint64_t offset) {
    const uint8_t* p = mmap_base + offset;
    uint32_t nkw;
    memcpy(&nkw, p, 4); p += 4;
    vector<pair<uint32_t, uint16_t>> result(nkw);
    for (uint32_t k = 0; k < nkw; k++) {
        memcpy(&result[k].first, p, 4); p += 4;
        memcpy(&result[k].second, p, 2); p += 2;
    }
    return result;
}

// ===================== main =====================

int main(int argc, char** argv) {
    vector<string> dirs;
    string out_dir = "corpus/";
    uint64_t max_tokens = 0;
    uint64_t skip_tokens = 0;  // v9.1: skip this many tokens before ingesting (for segments)
    int threads = omp_get_max_threads();
    int neighbor_top_n = 5;
    int signature_size = 64;

    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "--dirs" || arg == "-d") {
            while (i + 1 < argc && argv[i+1][0] != '-') {
                dirs.push_back(argv[++i]);
            }
        } else if (arg == "--out" || arg == "-o") {
            if (i + 1 < argc) out_dir = argv[++i];
        } else if (arg == "--max-tokens" || arg == "-t") {
            if (i + 1 < argc) max_tokens = stoull(argv[++i]);
        } else if (arg == "--skip-tokens") {
            if (i + 1 < argc) skip_tokens = stoull(argv[++i]);
        } else if (arg == "--threads" || arg == "-j") {
            if (i + 1 < argc) threads = stoi(argv[++i]);
        } else if (arg == "--neighbor-top" || arg == "-n") {
            if (i + 1 < argc) neighbor_top_n = stoi(argv[++i]);
        } else if (arg == "--signature-size") {
            if (i + 1 < argc) signature_size = stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            cout << "usage: bulk_build --dirs DIR1 [DIR2 ...] --out OUTDIR --max-tokens N\n";
            cout << "  --skip-tokens N     skip first N tokens (for segment builds)\n";
            cout << "  --neighbor-top N    keywords to propagate from neighbors (default: 5)\n";
            cout << "  --signature-size N  keywords per chapter signature (default: 64)\n";
            return 0;
        }
    }

    if (dirs.empty()) { cerr << "error: specify --dirs\n"; return 1; }
    if (max_tokens == 0) { cerr << "error: specify --max-tokens\n"; return 1; }

    omp_set_num_threads(threads);
    cout << "bulk_build v9.1 (two-pass disk-backed + hash-partition): " << threads << " threads, target "
         << max_tokens << " tokens" << endl;
    if (skip_tokens > 0) {
        cout << "  segment mode: skipping first " << skip_tokens << " tokens" << endl;
    }
    cout << "  neighbor propagation: top-" << neighbor_top_n << endl;
    cout << "  [mem] start: " << get_rss_mb() << " MB" << endl;

    // ================================================================
    // PHASE 1: collect files
    // ================================================================
    auto t0 = chrono::high_resolution_clock::now();
    vector<pair<string, uintmax_t>> files;
    for (const auto& dir : dirs) {
        if (!fs::exists(dir)) { cerr << "  dir not found: " << dir << endl; continue; }
        size_t before = files.size();
        for (const auto& entry : fs::recursive_directory_iterator(dir,
                fs::directory_options::skip_permission_denied)) {
            if (!entry.is_regular_file()) continue;
            string ext = entry.path().extension().string();
            if (ext == ".txt" || ext == ".md")
                files.push_back({entry.path().string(), entry.file_size()});
        }
        cout << "  " << dir << ": " << (files.size() - before) << " files" << endl;
    }
    sort(files.begin(), files.end(),
         [](const auto& a, const auto& b) { return a.second < b.second; });

    uint64_t total_bytes = 0;
    for (const auto& [p, s] : files) total_bytes += s;
    cout << "total: " << files.size() << " files (" << (total_bytes / 1000000000ULL) << " GB)" << endl;

    // ================================================================
    // PHASE 2: register chapters
    // ================================================================
    ChapterRegistry registry;
    for (const auto& [filepath, sz] : files) {
        string fname = fs::path(filepath).filename().string();
        registry.assign(fname);
    }
    cout << "  chapters: " << registry.file_to_code.size() << " unique codes" << endl;

    // ================================================================
    // PASS 1: chunk + STREAM to disk + write keywords.tmp
    // ================================================================
    // storage.bin and manifest.jsonl written immediately per chunk.
    // keyword data written to keywords.tmp (flat binary on disk).
    // only ChunkSlim (~60 bytes/chunk) kept in memory.
    // doc_freq accumulated during streaming.
    cout << "\n=== PASS 1: Streaming + keyword extraction ===" << endl;

    fs::create_directories(out_dir);
    string manifest_path = out_dir + "/manifest.jsonl";
    string storage_path = out_dir + "/storage.bin";
    string kw_tmp_path = out_dir + "/keywords.tmp";
    string binary_manifest_path = out_dir + "/manifest.bin";
    string chapter_guide_path = out_dir + "/chapter_guide.json";

    KeywordIntern kw_intern;
    vector<ChunkSlim> all_slim;
    vector<uint32_t> doc_freq;  // flat array indexed by keyword ID, grows dynamically
    atomic<uint64_t> bytes_processed(0);
    atomic<size_t> files_done(0);
    uint64_t running_tokens = 0;
    bool target_reached = false;

    auto t1 = chrono::high_resolution_clock::now();
    auto last_report = t1;
    // adaptive batch size: small files → big batches, big files → small batches
    // prevents RAM spikes from loading 128 × 220MB CommonCrawl files simultaneously
    auto get_batch_size = [&](size_t fi) -> size_t {
        if (fi >= files.size()) return 1;
        uintmax_t file_sz = files[fi].second;
        if (file_sz > 50000000) return max(threads / 4, 4);       // >50MB: 8 at a time
        if (file_sz > 10000000) return max(threads, 16);           // >10MB: 32
        return (size_t)(threads * 4);                               // small: 128
    };

    // open output files before the loop
    ofstream storage_out(storage_path, ios::binary);
    ofstream manifest_out(manifest_path, ios::binary);
    ofstream kw_tmp_out(kw_tmp_path, ios::binary);
    uint64_t storage_offset = 0;
    uint64_t tok_running = 0;
    uint64_t kw_file_offset = 0;

    for (size_t fi = 0; fi < files.size() && !target_reached; ) {
        size_t batch_sz = get_batch_size(fi);
        size_t batch_end = min(fi + batch_sz, files.size());
        size_t batch_count = batch_end - fi;

        // parallel: chunk files (heavy ChunkOut lives only in this batch)
        vector<vector<ChunkOut>> batch_results(batch_count);

        #pragma omp parallel for schedule(dynamic)
        for (size_t bi = 0; bi < batch_count; bi++) {
            if (target_reached) continue;
            const string& filepath = files[fi + bi].first;
            uintmax_t file_sz = files[fi + bi].second;
            string fname = fs::path(filepath).filename().string();
            string chapter_code = registry.file_to_code[fname];

            if (file_sz > 50000000) {
                // large file: stream in 50MB segments to avoid RAM spikes
                batch_results[bi] = chunk_file_streaming(filepath, fname, chapter_code);
            } else {
                // small file: load whole (fast path)
                ifstream f(filepath, ios::binary);
                if (!f.is_open()) continue;
                string content((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
                f.close();

                for (size_t j = 0; j < content.size(); j++) {
                    unsigned char c = static_cast<unsigned char>(content[j]);
                    if (c > 127 && c < 192) content[j] = ' ';
                }

                batch_results[bi] = chunk_file(fname, content, chapter_code);
            }
            bytes_processed += files[fi + bi].second;
        }

        // sequential: stream to disk, write keywords.tmp, keep only ChunkSlim
        for (size_t bi = 0; bi < batch_count && !target_reached; bi++) {
            for (auto& chunk : batch_results[bi]) {
                if (chunk.compressed.empty()) continue;

                uint64_t chunk_tokens = chunk.chunk_text.length() / 4;

                // v9.1: skip-tokens for segment builds — skip chunks until we've
                // passed skip_tokens, then ingest up to max_tokens after that
                if (skip_tokens > 0 && running_tokens + chunk_tokens <= skip_tokens) {
                    running_tokens += chunk_tokens;
                    tok_running += chunk_tokens;
                    continue;
                }

                uint64_t off = storage_offset;
                uint64_t len = chunk.compressed.size();
                uint64_t ts = tok_running;
                uint64_t te = tok_running + chunk_tokens;

                // STREAM: write compressed data + manifest line to disk NOW
                storage_out.write(chunk.compressed.data(), chunk.compressed.size());
                manifest_out << manifest_line(chunk, off, len, ts, te) << "\n";

                // write keyword data to keywords.tmp
                // format: [uint32_t num_keywords] [keyword_id: uint32_t, tf: uint16_t] x N
                uint64_t this_kw_offset = kw_file_offset;
                uint32_t nkw = chunk.keywords.size();
                kw_tmp_out.write((char*)&nkw, 4);
                kw_file_offset += 4;

                for (const auto& kw : chunk.keywords) {
                    uint32_t kid = kw_intern.intern(kw);
                    kw_tmp_out.write((char*)&kid, 4);
                    auto it = chunk.tf_map.find(kw);
                    uint16_t tf = (it != chunk.tf_map.end()) ? it->second : 1;
                    kw_tmp_out.write((char*)&tf, 2);
                    kw_file_offset += 6;

                    // accumulate doc_freq (keywords are unique per chunk)
                    if (kid >= doc_freq.size()) doc_freq.resize(kid + 1, 0);
                    doc_freq[kid]++;
                }

                // create lightweight ChunkSlim (no keyword data in memory)
                ChunkSlim slim;
                slim.chunk_id = move(chunk.chunk_id);
                slim.source_file = move(chunk.source_file);
                slim.chapter_code = move(chunk.chapter_code);
                slim.kw_file_offset = this_kw_offset;
                slim.num_keywords = (uint16_t)min((uint32_t)65535, nkw);
                slim.storage_offset = off;
                slim.storage_length = len;
                slim.token_start = ts;
                slim.token_end = te;
                all_slim.push_back(move(slim));

                storage_offset += len;
                tok_running += chunk_tokens;
                running_tokens += chunk_tokens;

                if (running_tokens >= skip_tokens + max_tokens) { target_reached = true; break; }
            }
            files_done++;
        }

        fi = batch_end;

        auto now = chrono::high_resolution_clock::now();
        double secs = chrono::duration<double>(now - last_report).count();
        if (secs >= 5.0 || target_reached) {
            double elapsed = chrono::duration<double>(now - t1).count();
            double pct = (double)running_tokens / max_tokens * 100.0;
            double tok_s = running_tokens / elapsed;
            double mb_s = bytes_processed / elapsed / 1e6;
            cout << "  [" << (int)pct << "%] "
                 << files_done << " files, "
                 << running_tokens << " tok, "
                 << all_slim.size() << " chunks, "
                 << kw_intern.size() << " kw, "
                 << (int)tok_s << " tok/s, "
                 << (int)mb_s << " MB/s, "
                 << get_rss_mb() << " MB RSS" << endl;
            last_report = now;
        }
    }

    storage_out.close();
    manifest_out.close();
    kw_tmp_out.close();
    malloc_trim(0);

    auto t2 = chrono::high_resolution_clock::now();
    double chunk_secs = chrono::duration<double>(t2 - t1).count();
    size_t NM = all_slim.size();
    size_t NKW = kw_intern.size();
    // ensure doc_freq covers all keywords
    if (doc_freq.size() < NKW) doc_freq.resize(NKW, 0);

    cout << "  pass 1 done: " << NM << " chunks, " << NKW << " unique keywords in "
         << chunk_secs << "s" << endl;
    cout << "  keywords.tmp: " << (kw_file_offset / 1000000) << " MB on disk" << endl;
    cout << "  [mem] after pass 1: " << get_rss_mb() << " MB" << endl;

    // ================================================================
    // PASS 2: mmap keywords.tmp, neighbor propagation, IDF re-sort
    // ================================================================
    cout << "\n=== PASS 2: Neighbor propagation + IDF sort (disk-backed) ===" << endl;

    // mmap keywords.tmp for reading
    MmapFile kw_mmap;
    if (!kw_mmap.open(kw_tmp_path)) {
        cerr << "FATAL: cannot mmap keywords.tmp" << endl;
        return 1;
    }
    cout << "  mmap'd keywords.tmp: " << (kw_mmap.size / 1000000) << " MB" << endl;
    cout << "  [mem] after mmap: " << get_rss_mb() << " MB" << endl;

    // ----------------------------------------------------------------
    // phase 4: neighbor keyword propagation (from mmap'd keywords.tmp)
    // writes keywords_propagated.tmp with original + propagated keywords
    // ----------------------------------------------------------------
    cout << "  neighbor propagation..." << endl;
    string kw_prop_path = out_dir + "/keywords_propagated.tmp";
    {
        auto tp0 = chrono::high_resolution_clock::now();

        // for neighbor propagation, we only need unigram doc_freq (already computed)
        // we'll use the existing doc_freq array

        // batched neighbor propagation: process in chunks of PROP_BATCH to limit memory
        // IMPORTANT: we must not modify all_slim[].kw_file_offset until after ALL
        // batches are computed, because neighbor lookups reference original offsets.
        // So we store new offsets in a separate vector and apply after writing.
        ofstream kw_prop_out(kw_prop_path, ios::binary);
        uint64_t prop_offset = 0;
        const size_t PROP_BATCH = 100000;

        // new offsets for propagated file (applied after all batches written)
        vector<uint64_t> new_kw_offsets(NM);
        vector<uint16_t> new_num_keywords(NM);

        for (size_t batch_start = 0; batch_start < NM; batch_start += PROP_BATCH) {
            size_t batch_end_idx = min(batch_start + PROP_BATCH, NM);
            size_t batch_sz = batch_end_idx - batch_start;

            struct PropEntry { vector<pair<uint32_t, uint16_t>> additions; };
            vector<PropEntry> props(batch_sz);

            #pragma omp parallel for schedule(static)
            for (size_t bi = 0; bi < batch_sz; bi++) {
                size_t i = batch_start + bi;
                const auto& chunk = all_slim[i];

                // read this chunk's keywords from ORIGINAL mmap (offsets unchanged)
                auto my_kws = read_chunk_keywords(kw_mmap.data, chunk.kw_file_offset);
                unordered_set<uint32_t> existing;
                for (const auto& [kid, tf] : my_kws) existing.insert(kid);

                auto get_top_idf = [&](size_t ni) -> vector<uint32_t> {
                    if (ni >= NM) return {};
                    const auto& neighbor = all_slim[ni];
                    if (neighbor.source_file != chunk.source_file) return {};
                    // always read from ORIGINAL mmap using original offsets
                    auto n_kws = read_chunk_keywords(kw_mmap.data, neighbor.kw_file_offset);
                    vector<pair<uint32_t, double>> scored;
                    for (const auto& [kid, tf] : n_kws) {
                        if (kw_intern.lookup(kid).find('_') != string::npos) continue;
                        double idf = log((double)NM / (1 + doc_freq[kid]));
                        scored.push_back({kid, idf});
                    }
                    sort(scored.begin(), scored.end(),
                         [](const auto& a, const auto& b) { return a.second > b.second; });
                    vector<uint32_t> result;
                    for (int j = 0; j < neighbor_top_n && j < (int)scored.size(); j++)
                        result.push_back(scored[j].first);
                    return result;
                };

                auto prev_kws = (i > 0) ? get_top_idf(i - 1) : vector<uint32_t>{};
                auto next_kws = (i + 1 < NM) ? get_top_idf(i + 1) : vector<uint32_t>{};

                auto& p = props[bi];
                for (uint32_t kid : prev_kws) {
                    if (!existing.count(kid)) { p.additions.push_back({kid, 1}); existing.insert(kid); }
                }
                for (uint32_t kid : next_kws) {
                    if (!existing.count(kid)) { p.additions.push_back({kid, 1}); existing.insert(kid); }
                }
            }

            // write this batch sequentially: original keywords + additions
            for (size_t bi = 0; bi < batch_sz; bi++) {
                size_t i = batch_start + bi;
                // read from ORIGINAL mmap (kw_file_offset still points to keywords.tmp)
                auto orig = read_chunk_keywords(kw_mmap.data, all_slim[i].kw_file_offset);
                uint32_t total_kw = orig.size() + props[bi].additions.size();

                // store new offset/count for later application
                new_kw_offsets[i] = prop_offset;
                new_num_keywords[i] = (uint16_t)min((uint32_t)65535, total_kw);

                kw_prop_out.write((char*)&total_kw, 4);
                prop_offset += 4;

                // write original keywords
                for (const auto& [kid, tf] : orig) {
                    kw_prop_out.write((char*)&kid, 4);
                    kw_prop_out.write((char*)&tf, 2);
                    prop_offset += 6;
                }
                // write propagated additions (TF=1)
                for (const auto& [kid, tf] : props[bi].additions) {
                    kw_prop_out.write((char*)&kid, 4);
                    kw_prop_out.write((char*)&tf, 2);
                    prop_offset += 6;
                }
            }
            // props freed here at end of batch iteration
        }
        kw_prop_out.close();

        // now apply new offsets (pointing to keywords_propagated.tmp)
        for (size_t i = 0; i < NM; i++) {
            all_slim[i].kw_file_offset = new_kw_offsets[i];
            all_slim[i].num_keywords = new_num_keywords[i];
        }

        auto tp1 = chrono::high_resolution_clock::now();
        cout << "  neighbor propagation done: " << chrono::duration<double>(tp1-tp0).count() << "s" << endl;
        cout << "  keywords_propagated.tmp: " << (prop_offset / 1000000) << " MB" << endl;
    }

    // close original keywords.tmp mmap, open propagated version
    kw_mmap.close();
    malloc_trim(0);

    MmapFile kw_prop_mmap;
    if (!kw_prop_mmap.open(kw_prop_path)) {
        cerr << "FATAL: cannot mmap keywords_propagated.tmp" << endl;
        return 1;
    }
    cout << "  [mem] after neighbor prop: " << get_rss_mb() << " MB" << endl;

    // ----------------------------------------------------------------
    // phase 4b: TF²*IDF keyword re-sort
    // reads from mmap'd keywords_propagated.tmp
    // writes keywords_sorted.tmp with re-ordered keywords
    // ----------------------------------------------------------------
    cout << "  IDF re-sort (TF²*IDF)..." << endl;
    string kw_sorted_path = out_dir + "/keywords_sorted.tmp";
    {
        auto ti0 = chrono::high_resolution_clock::now();

        // recompute doc_freq including propagated keywords (all kw, unigrams + bigrams)
        // first zero it out
        fill(doc_freq.begin(), doc_freq.end(), 0);
        for (size_t i = 0; i < NM; i++) {
            auto kws = read_chunk_keywords(kw_prop_mmap.data, all_slim[i].kw_file_offset);
            for (const auto& [kid, tf] : kws) {
                if (kid < doc_freq.size()) doc_freq[kid]++;
            }
        }

        double log_N = log((double)NM);

        // streaming IDF re-sort: process chunks in batches to limit memory
        // each batch is sorted in parallel, then written to disk sequentially
        ofstream kw_sorted_out(kw_sorted_path, ios::binary);
        uint64_t sorted_offset = 0;
        const size_t SORT_BATCH = 100000;  // process 100K chunks at a time

        for (size_t batch_start = 0; batch_start < NM; batch_start += SORT_BATCH) {
            size_t batch_end_idx = min(batch_start + SORT_BATCH, NM);
            size_t batch_sz = batch_end_idx - batch_start;

            // read + sort this batch in parallel
            vector<vector<pair<uint32_t, uint16_t>>> batch_sorted(batch_sz);

            #pragma omp parallel for schedule(static)
            for (size_t bi = 0; bi < batch_sz; bi++) {
                size_t i = batch_start + bi;
                auto kws = read_chunk_keywords(kw_prop_mmap.data, all_slim[i].kw_file_offset);
                size_t K = kws.size();
                if (K <= 1) {
                    batch_sorted[bi] = move(kws);
                    continue;
                }

                // score each keyword
                vector<pair<double, size_t>> scores(K);
                for (size_t k = 0; k < K; k++) {
                    double tf = (double)kws[k].second;
                    double idf = log_N - log(1.0 + doc_freq[kws[k].first]);
                    scores[k] = {tf * tf * idf, k};  // TF^2 * IDF
                }

                // sort descending, break ties by original position
                sort(scores.begin(), scores.end(),
                     [](const auto& a, const auto& b) {
                         if (a.first != b.first) return a.first > b.first;
                         return a.second < b.second;
                     });

                // rebuild in new order
                vector<pair<uint32_t, uint16_t>> new_kws(K);
                for (size_t k = 0; k < K; k++) {
                    size_t orig = scores[k].second;
                    new_kws[k] = kws[orig];
                }
                batch_sorted[bi] = move(new_kws);
            }

            // write this batch sequentially, then free
            for (size_t bi = 0; bi < batch_sz; bi++) {
                size_t i = batch_start + bi;
                all_slim[i].kw_file_offset = sorted_offset;
                uint32_t nkw = batch_sorted[bi].size();
                all_slim[i].num_keywords = (uint16_t)min((uint32_t)65535, nkw);

                kw_sorted_out.write((char*)&nkw, 4);
                sorted_offset += 4;

                for (const auto& [kid, tf] : batch_sorted[bi]) {
                    kw_sorted_out.write((char*)&kid, 4);
                    kw_sorted_out.write((char*)&tf, 2);
                    sorted_offset += 6;
                }
            }
            // batch_sorted freed here
        }
        kw_sorted_out.close();

        auto ti1 = chrono::high_resolution_clock::now();
        cout << "  IDF re-sort done: " << chrono::duration<double>(ti1-ti0).count() << "s" << endl;
        cout << "  keywords_sorted.tmp: " << (sorted_offset / 1000000) << " MB" << endl;
    }

    // close propagated mmap, open sorted version
    kw_prop_mmap.close();
    malloc_trim(0);

    MmapFile kw_sorted_mmap;
    if (!kw_sorted_mmap.open(kw_sorted_path)) {
        cerr << "FATAL: cannot mmap keywords_sorted.tmp" << endl;
        return 1;
    }
    cout << "  [mem] after IDF re-sort: " << get_rss_mb() << " MB" << endl;

    // ================================================================
    // phase 5: write binary manifest (keywords from sorted mmap)
    // batched: process MANIFEST_BATCH chunks at a time to limit memory
    // ================================================================
    cout << "  writing binary manifest..." << endl;
    {
        auto tb0 = chrono::high_resolution_clock::now();

        // write header first
        ofstream out(binary_manifest_path, ios::binary);
        out.write("OEM1", 4);
        uint32_t version = 1;
        out.write((char*)&version, 4);
        uint64_t chunk_count = NM;
        out.write((char*)&chunk_count, 8);
        uint64_t kw_count = NKW;
        out.write((char*)&kw_count, 8);

        // keyword dictionary (from intern, same order)
        for (size_t k = 0; k < NKW; k++) {
            const auto& kw = kw_intern.id_to_str[k];
            uint16_t len = min((size_t)65535, kw.size());
            out.write((char*)&len, 2);
            out.write(kw.data(), len);
        }

        // write chunk entries in batches
        const size_t MANIFEST_BATCH = 100000;
        for (size_t batch_start = 0; batch_start < NM; batch_start += MANIFEST_BATCH) {
            size_t batch_end_idx = min(batch_start + MANIFEST_BATCH, NM);
            size_t batch_sz = batch_end_idx - batch_start;

            vector<string> chunk_bufs(batch_sz);

            #pragma omp parallel for schedule(static)
            for (size_t bi = 0; bi < batch_sz; bi++) {
                size_t i = batch_start + bi;
                const auto& c = all_slim[i];
                auto kws = read_chunk_keywords(kw_sorted_mmap.data, c.kw_file_offset);

                string buf;
                buf.reserve(c.chunk_id.size() + kws.size() * 4 + 40);

                uint16_t id_len = min((size_t)65535, c.chunk_id.size());
                buf.append((char*)&id_len, 2);
                buf.append(c.chunk_id.data(), id_len);

                uint16_t sum_len = 0;
                buf.append((char*)&sum_len, 2);

                buf.append((char*)&c.storage_offset, 8);
                buf.append((char*)&c.storage_length, 8);
                uint32_t ts = (uint32_t)c.token_start;
                uint32_t te = (uint32_t)c.token_end;
                buf.append((char*)&ts, 4);
                buf.append((char*)&te, 4);
                int64_t timestamp = 0;
                buf.append((char*)&timestamp, 8);

                uint16_t kw_cnt = min((size_t)65535, kws.size());
                buf.append((char*)&kw_cnt, 2);
                for (size_t k = 0; k < kw_cnt; k++) {
                    uint32_t kid = kws[k].first;
                    buf.append((char*)&kid, 4);
                }
                chunk_bufs[bi] = move(buf);
            }

            for (const auto& buf : chunk_bufs) {
                out.write(buf.data(), buf.size());
            }
            // chunk_bufs freed here
        }
        out.close();

        auto tb1 = chrono::high_resolution_clock::now();
        cout << "  binary manifest: " << binary_manifest_path
             << " (" << (fs::file_size(binary_manifest_path) / 1000000) << " MB) in "
             << chrono::duration<double>(tb1-tb0).count() << "s" << endl;
    }
    malloc_trim(0);
    cout << "  [mem] after binary manifest: " << get_rss_mb() << " MB" << endl;

    // ================================================================
    // phase 6: chapter guide (reads keywords from sorted mmap)
    // ================================================================
    cout << "  writing chapter guide..." << endl;
    {
        struct ChapterInfo {
            string code;
            string source_file;
            uint32_t first_chunk = UINT32_MAX;
            uint32_t last_chunk = 0;
            uint32_t chunk_count = 0;
            vector<uint32_t> chunk_indices;
        };

        unordered_map<string, ChapterInfo> chapters;
        for (size_t i = 0; i < NM; i++) {
            const auto& c = all_slim[i];
            auto& ch = chapters[c.chapter_code];
            if (ch.code.empty()) { ch.code = c.chapter_code; ch.source_file = c.source_file; }
            if ((uint32_t)i < ch.first_chunk) ch.first_chunk = i;
            if ((uint32_t)i > ch.last_chunk) ch.last_chunk = i;
            ch.chunk_count++;
            ch.chunk_indices.push_back((uint32_t)i);
        }

        ofstream out(chapter_guide_path);
        out << "{\"chapters\":[" << endl;
        bool first = true;
        for (const auto& [code, ch] : chapters) {
            if (!first) out << "," << endl;
            first = false;

            // aggregate keywords from all chunks in this chapter
            unordered_map<uint32_t, int> keyword_freq;
            for (uint32_t ci : ch.chunk_indices) {
                auto kws = read_chunk_keywords(kw_sorted_mmap.data, all_slim[ci].kw_file_offset);
                for (const auto& [kid, tf] : kws) {
                    if (kw_intern.lookup(kid).find('_') != string::npos) continue;
                    keyword_freq[kid]++;
                }
            }

            vector<pair<uint32_t, double>> scored_kws;
            scored_kws.reserve(keyword_freq.size());
            for (const auto& [kid, freq] : keyword_freq) {
                scored_kws.push_back({kid, log(1.0 + freq)});
            }
            sort(scored_kws.begin(), scored_kws.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

            out << "{\"code\":\"" << ch.code << "\"";
            out << ",\"source_file\":\"" << ch.source_file << "\"";
            out << ",\"chunk_count\":" << ch.chunk_count;
            out << ",\"first_chunk\":" << ch.first_chunk;
            out << ",\"last_chunk\":" << ch.last_chunk;
            out << ",\"signature\":[";
            int sig_count = min((int)scored_kws.size(), signature_size);
            for (int i = 0; i < sig_count; i++) {
                if (i) out << ",";
                out << "\"" << make_json_safe(kw_intern.lookup(scored_kws[i].first)) << "\"";
            }
            out << "]}";
        }
        out << "]}" << endl;
    }

    // ================================================================
    // phase 7: generate stems.idx + keywords.idx (reads from sorted mmap)
    // BUCKETED BUILD: partition stems by hash into N_BUCKETS buckets.
    // Each bucket processes only 1/N of stems, keeping memory bounded.
    // At 50B: ~50M unique stems / 16 buckets = ~3M stems per bucket
    //         posting lists per bucket: ~1GB instead of ~15GB total
    // ================================================================
    cout << "  generating stems.idx..." << endl;
    {
        auto ts0 = chrono::high_resolution_clock::now();

        // step 1: compute keyword_id -> stem string + hash mapping
        vector<string> kw_stems(NKW);
        vector<uint64_t> kw_stem_hashes(NKW);
        #pragma omp parallel for schedule(static)
        for (size_t k = 0; k < NKW; k++) {
            kw_stems[k] = porter::stem(kw_intern.id_to_str[k]);
            uint64_t h = 0xcbf29ce484222325ULL;
            for (char c : kw_stems[k]) { h ^= (uint8_t)c; h *= 0x100000001b3ULL; }
            kw_stem_hashes[k] = h;
        }

        // step 2: compute per-keyword doc frequency for hapax filter
        vector<uint32_t> kw_df(NKW, 0);
        for (size_t i = 0; i < NM; i++) {
            auto kws = read_chunk_keywords(kw_sorted_mmap.data, all_slim[i].kw_file_offset);
            for (const auto& [kid, tf] : kws) {
                kw_df[kid]++;
            }
        }

        // step 3: compute avgdl for header
        double avgdl = 0;
        {
            size_t total_kw_count = 0;
            for (size_t i = 0; i < NM; i++) total_kw_count += all_slim[i].num_keywords;
            avgdl = (NM > 0) ? (double)total_kw_count / NM : 1.0;
        }

        // ================================================================
        // v9.1: Hash-partition stems.idx generation (low-memory)
        // Replaces unordered_map-based stem collection with 256 hash-partition
        // bucket files on disk. Reduces peak RAM from ~15GB to ~3-5GB at 50B.
        // ================================================================

        // --- Phase A: Hash-partition stems into 256 bucket files ---
        // Record format: [2-byte stem_len][stem_bytes][4-byte chunk_idx]
        // Bucket selection: top 8 bits of FNV-1a hash → contiguous hash ranges
        // so processing buckets 0-255 in order yields globally hash-sorted stems.
        cout << "    [stems] Phase A: partitioning into 256 hash buckets..." << endl;
        const int NUM_STEM_BUCKETS = 256;
        string bucket_dir = out_dir + "/_tmp_stem_buckets";
        fs::create_directories(bucket_dir);

        {
            vector<ofstream> bfiles(NUM_STEM_BUCKETS);
            for (int b = 0; b < NUM_STEM_BUCKETS; b++) {
                bfiles[b].open(bucket_dir + "/b" + to_string(b), ios::binary);
            }
            for (size_t i = 0; i < NM; i++) {
                auto kws = read_chunk_keywords(kw_sorted_mmap.data, all_slim[i].kw_file_offset);
                unordered_set<string> seen;  // deduplicate stems per chunk
                for (const auto& [kid, tf] : kws) {
                    const string& kw = kw_intern.id_to_str[kid];
                    if (kw_df[kid] <= 1 && kw.find('_') != string::npos) continue;
                    const string& stem = kw_stems[kid];
                    if (seen.insert(stem).second) {
                        uint64_t h = 0xcbf29ce484222325ULL;
                        for (char c : stem) { h ^= (uint8_t)c; h *= 0x100000001b3ULL; }
                        int bid = (h >> 56) & 0xFF;
                        uint16_t slen = (uint16_t)stem.size();
                        uint32_t cidx = (uint32_t)i;
                        bfiles[bid].write((char*)&slen, 2);
                        bfiles[bid].write(stem.data(), slen);
                        bfiles[bid].write((char*)&cidx, 4);
                    }
                }
            }
            for (int b = 0; b < NUM_STEM_BUCKETS; b++) bfiles[b].close();
        }
        cout << "    [stems] Phase A done, " << get_rss_mb() << " MB RSS" << endl;

        // --- Phase B: Collect stems + DF from bucket files ---
        // Process each bucket independently: read file, group by stem, count DF.
        // Accumulate into a flat vector (2-3GB) instead of a hash map (15GB+).
        // Buckets are in hash-range order, so the accumulated vector is globally sorted.
        cout << "    [stems] Phase B: collecting stem metadata from buckets..." << endl;
        struct StemInfo {
            string stem;
            uint64_t hash;
            uint32_t df;
        };
        vector<StemInfo> all_stems;
        all_stems.reserve(10000000);  // ~10M initial, grows as needed

        for (int b = 0; b < NUM_STEM_BUCKETS; b++) {
            string bp = bucket_dir + "/b" + to_string(b);
            size_t bsz = fs::exists(bp) ? fs::file_size(bp) : 0;
            if (bsz == 0) { if (fs::exists(bp)) fs::remove(bp); continue; }

            // read entire bucket into memory (~120MB avg at 50B)
            vector<char> raw(bsz);
            { ifstream bf(bp, ios::binary); bf.read(raw.data(), bsz); }

            // count DF per stem (one entry per chunk-stem pair in bucket)
            unordered_map<string, uint32_t> stem_df;
            size_t pos = 0;
            while (pos + 6 <= bsz) {
                uint16_t slen; memcpy(&slen, &raw[pos], 2); pos += 2;
                if (pos + slen + 4 > bsz) break;
                string stem(&raw[pos], slen); pos += slen;
                pos += 4;  // skip chunk_idx (only counting DF here)
                stem_df[stem]++;
            }

            // convert to sorted StemInfo entries
            vector<StemInfo> bstems;
            bstems.reserve(stem_df.size());
            for (auto& [stem, df] : stem_df) {
                uint64_t h = 0xcbf29ce484222325ULL;
                for (char c : stem) { h ^= (uint8_t)c; h *= 0x100000001b3ULL; }
                bstems.push_back({move(stem), h, df});
            }
            stem_df.clear();

            sort(bstems.begin(), bstems.end(),
                 [](const StemInfo& a, const StemInfo& b) { return a.hash < b.hash; });

            for (auto& si : bstems) all_stems.push_back(move(si));

            if ((b + 1) % 64 == 0)
                cout << "    bucket " << (b+1) << "/256, " << all_stems.size()
                     << " stems, " << get_rss_mb() << " MB RSS" << endl;
        }

        uint64_t sc = all_stems.size();
        cout << "    " << sc << " unique stems, " << get_rss_mb() << " MB RSS" << endl;

        // --- Phase C: Write keywords.idx (binary search, no hash map) ---
        // For each keyword, binary search all_stems by hash to find sorted index.
        // Replaces 10GB+ unordered_map with O(log n) lookups on the sorted vector.
        cout << "    [stems] Phase C: writing keywords.idx..." << endl;
        vector<uint32_t> kw_stem_sorted_idx(NKW, UINT32_MAX);
        {
            #pragma omp parallel for schedule(dynamic, 1000)
            for (size_t k = 0; k < NKW; k++) {
                const string& stem = kw_stems[k];
                uint64_t h = 0xcbf29ce484222325ULL;
                for (char c : stem) { h ^= (uint8_t)c; h *= 0x100000001b3ULL; }
                // binary search by hash
                auto lo = lower_bound(all_stems.begin(), all_stems.end(), h,
                    [](const StemInfo& si, uint64_t hv) { return si.hash < hv; });
                // linear scan for exact stem match (handles hash collisions)
                for (auto it = lo; it != all_stems.end() && it->hash == h; ++it) {
                    if (it->stem == stem) {
                        kw_stem_sorted_idx[k] = (uint32_t)(it - all_stems.begin());
                        break;
                    }
                }
            }
        }
        string kw_path = out_dir + "/keywords.idx";
        {
            ofstream kf(kw_path, ios::binary);
            kf.write("OEK1", 4);
            uint32_t kver = 1; kf.write((char*)&kver, 4);
            uint64_t nkw = NKW; kf.write((char*)&nkw, 8);
            kf.write((char*)kw_stem_sorted_idx.data(), NKW * 4);
            kf.close();
        }
        // free kw_stems — no longer needed
        { vector<string>().swap(kw_stems); }

        // --- Phase D: Write stems.idx header + directory + strings ---
        cout << "    [stems] Phase D: writing stems.idx header + directory..." << endl;
        string stems_path = out_dir + "/stems.idx";
        ofstream sf(stems_path, ios::binary);

        // header (32 bytes)
        sf.write("OES1", 4);
        uint32_t ver = 1; sf.write((char*)&ver, 4);
        sf.write((char*)&sc, 8);
        uint64_t ps_placeholder = 0; sf.write((char*)&ps_placeholder, 8); // patched later
        uint64_t avgdl_bits; memcpy(&avgdl_bits, &avgdl, 8);
        sf.write((char*)&avgdl_bits, 8);

        // compute string offsets
        uint32_t str_offset = 0;
        vector<uint32_t> str_offsets(sc);
        for (size_t i = 0; i < sc; i++) {
            str_offsets[i] = str_offset;
            str_offset += all_stems[i].stem.size();
        }

        // compute posting offsets from cumulative df
        uint32_t cum_post_offset = 0;
        vector<uint32_t> post_offsets(sc);
        for (size_t i = 0; i < sc; i++) {
            post_offsets[i] = cum_post_offset;
            cum_post_offset += all_stems[i].df * 4;
        }

        // write directory entries (28 bytes each)
        for (size_t i = 0; i < sc; i++) {
            sf.write((char*)&all_stems[i].hash, 8);
            sf.write((char*)&str_offsets[i], 4);
            uint16_t slen = all_stems[i].stem.size();
            sf.write((char*)&slen, 2);
            sf.write((char*)&post_offsets[i], 4);
            uint32_t pcnt = all_stems[i].df;
            sf.write((char*)&pcnt, 4);
            sf.write((char*)&all_stems[i].df, 4);
            uint16_t pad = 0;
            sf.write((char*)&pad, 2);
        }
        str_offsets.clear();
        post_offsets.clear();

        // write stem strings
        for (size_t i = 0; i < sc; i++) {
            sf.write(all_stems[i].stem.data(), all_stems[i].stem.size());
        }

        // patch postings_start in header
        uint64_t postings_start = sf.tellp();
        sf.seekp(16);
        sf.write((char*)&postings_start, 8);
        sf.seekp(0, ios::end);

        // --- Phase E: Write posting lists from bucket files ---
        // Re-read each bucket file, build posting lists per stem, write in order.
        // Each bucket produces posting lists for stems with hash in that bucket's range.
        // Since buckets are processed in order (0-255) and stems within each bucket
        // are hash-sorted, posting lists are written in the same order as the directory.
        cout << "    [stems] Phase E: writing posting lists from bucket files..." << endl;

        // build a mapping: for each bucket, which all_stems indices belong to it
        // (needed to write posting lists in the correct directory order)
        size_t stems_written = 0;
        for (int b = 0; b < NUM_STEM_BUCKETS; b++) {
            string bp = bucket_dir + "/b" + to_string(b);
            size_t bsz = fs::exists(bp) ? fs::file_size(bp) : 0;
            if (bsz == 0) { if (fs::exists(bp)) fs::remove(bp); continue; }

            // read bucket file
            vector<char> raw(bsz);
            { ifstream bf(bp, ios::binary); bf.read(raw.data(), bsz); }
            fs::remove(bp);  // delete after reading

            // group by stem → posting list (chunk indices)
            unordered_map<string, vector<uint32_t>> stem_postings;
            size_t pos = 0;
            while (pos + 6 <= bsz) {
                uint16_t slen; memcpy(&slen, &raw[pos], 2); pos += 2;
                if (pos + slen + 4 > bsz) break;
                string stem(&raw[pos], slen); pos += slen;
                uint32_t cidx; memcpy(&cidx, &raw[pos], 4); pos += 4;
                stem_postings[stem].push_back(cidx);
            }
            raw.clear();

            // find which all_stems entries belong to this bucket and write in order
            // all_stems is globally sorted by hash, and this bucket's stems have
            // hash values in [(b << 56), ((b+1) << 56)) — contiguous range
            // scan all_stems from stems_written forward
            size_t bucket_stem_count = 0;
            while (stems_written < sc) {
                int stem_bucket = (all_stems[stems_written].hash >> 56) & 0xFF;
                if (stem_bucket != b) break;

                auto it = stem_postings.find(all_stems[stems_written].stem);
                if (it != stem_postings.end()) {
                    auto& posts = it->second;
                    sort(posts.begin(), posts.end());
                    posts.erase(unique(posts.begin(), posts.end()), posts.end());
                    sf.write((char*)posts.data(), posts.size() * 4);
                }
                stems_written++;
                bucket_stem_count++;
            }

            if ((b + 1) % 32 == 0) {
                cout << "    posting bucket " << (b+1) << "/256 ("
                     << bucket_stem_count << " stems), " << get_rss_mb() << " MB RSS" << endl;
            }
        }
        sf.close();

        // cleanup bucket directory
        fs::remove_all(bucket_dir);

        auto ts1 = chrono::high_resolution_clock::now();
        cout << "  stems.idx: " << stems_path << " (" << (fs::file_size(stems_path) / 1000000) << " MB), "
             << sc << " stems in " << chrono::duration<double>(ts1-ts0).count() << "s" << endl;
        cout << "  keywords.idx: " << kw_path << " (" << (fs::file_size(kw_path) / 1000000) << " MB)" << endl;
        cout << "  [mem] after stems.idx: " << get_rss_mb() << " MB" << endl;
    }
    malloc_trim(0);

    // ================================================================
    // phase 8: generate doc_keywords.idx (reads from sorted mmap)
    // ================================================================
    cout << "  generating doc_keywords.idx..." << endl;
    {
        auto td0 = chrono::high_resolution_clock::now();
        string dkw_path = out_dir + "/doc_keywords.idx";
        ofstream dkf(dkw_path, ios::binary);

        // header
        dkf.write("OED1", 4);
        uint32_t dver = 1; dkf.write((char*)&dver, 4);
        uint64_t dc = NM; dkf.write((char*)&dc, 8);

        // compute offsets for keyword data
        uint32_t data_offset = 0;
        for (size_t i = 0; i < NM; i++) {
            uint32_t off = data_offset;
            uint16_t cnt = all_slim[i].num_keywords;
            uint16_t pad = 0;
            dkf.write((char*)&off, 4);
            dkf.write((char*)&cnt, 2);
            dkf.write((char*)&pad, 2);
            data_offset += cnt * 4;
        }

        // write keyword data from mmap'd sorted file
        for (size_t i = 0; i < NM; i++) {
            auto kws = read_chunk_keywords(kw_sorted_mmap.data, all_slim[i].kw_file_offset);
            for (const auto& [kid, tf] : kws) {
                dkf.write((char*)&kid, 4);
            }
        }
        dkf.close();

        auto td1 = chrono::high_resolution_clock::now();
        cout << "  doc_keywords.idx: " << dkw_path << " (" << (fs::file_size(dkw_path) / 1000000) << " MB) in "
             << chrono::duration<double>(td1-td0).count() << "s" << endl;
    }

    // ================================================================
    // cleanup: remove temporary files
    // ================================================================
    kw_sorted_mmap.close();
    cout << "  cleaning up temp files..." << endl;
    fs::remove(kw_tmp_path);
    fs::remove(kw_prop_path);
    fs::remove(kw_sorted_path);

    auto t3 = chrono::high_resolution_clock::now();
    double total_secs = chrono::duration<double>(t3 - t1).count();

    cout << "\n=== BUILD COMPLETE (v9.1 — two-pass disk-backed) ===" << endl;
    cout << "  files: " << files_done << endl;
    cout << "  chapters: " << registry.file_to_code.size() << endl;
    cout << "  chunks: " << NM << endl;
    cout << "  keywords: " << NKW << " unique" << endl;
    cout << "  tokens: " << tok_running << endl;
    cout << "  time: " << total_secs << "s" << endl;
    cout << "  speed: " << (uint64_t)(tok_running / total_secs) << " tok/s" << endl;
    cout << "  speed: " << (int)(bytes_processed / total_secs / 1e6) << " MB/s" << endl;
    cout << "  manifest: " << manifest_path << " (" << (fs::file_size(manifest_path) / 1000000) << " MB)" << endl;
    cout << "  binary: " << binary_manifest_path << " (" << (fs::file_size(binary_manifest_path) / 1000000) << " MB)" << endl;
    cout << "  storage: " << storage_path << " (" << (fs::file_size(storage_path) / 1000000) << " MB)" << endl;
    cout << "  chapter_guide: " << chapter_guide_path << endl;
    cout << "  [mem] final: " << get_rss_mb() << " MB" << endl;

    return 0;
}
