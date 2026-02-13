// ocean_build.cpp
// Build full chunked dataset with metadata for retrieval (C++17)
// - Streams input, splits into token-count chunks (optional overlap)
// - Computes extractive summaries (~50-100 tokens) and keywords per chunk
// - Compresses chunks in parallel (zlib|zstd|lz4)
// - Writes a single storage file (code.bin) and a JSONL manifest with offsets
// - Writes a chapter_guide.json with high-level mapping
//
// Goal: keep heavy work in C++, leave retrieval/orchestration to Python.

#include <zlib.h>
#ifdef ZSTD
#include <zstd.h>
#endif
#ifdef LZ4
#include <lz4frame.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <regex>

// ============= CONTENT TYPE SYSTEM (Feature 1: Smart Adaptive Chunking) =============
enum class ContentType {
    DOCUMENT,   // DOC - Books, articles, prose
    CHAT,       // CHAT - Conversation turns
    CODE,       // CODE - Source code files
    FIX,        // FIX - Bug fixes
    FEATURE     // FEAT - Feature implementations
};

static std::string content_type_to_string(ContentType type) {
    switch(type) {
        case ContentType::DOCUMENT: return "DOC";
        case ContentType::CHAT: return "CHAT";
        case ContentType::CODE: return "CODE";
        case ContentType::FIX: return "FIX";
        case ContentType::FEATURE: return "FEAT";
        default: return "DOC";
    }
}

// Detect content type from text and filename
static ContentType detect_content_type(const std::string& text, const std::string& filename) {
    // Check filename patterns first
    std::string fname_lower = filename;
    for (auto& c : fname_lower) c = std::tolower(static_cast<unsigned char>(c));

    // Code file extensions
    if (fname_lower.find(".cpp") != std::string::npos ||
        fname_lower.find(".hpp") != std::string::npos ||
        fname_lower.find(".c") != std::string::npos ||
        fname_lower.find(".h") != std::string::npos ||
        fname_lower.find(".py") != std::string::npos ||
        fname_lower.find(".js") != std::string::npos ||
        fname_lower.find(".ts") != std::string::npos ||
        fname_lower.find(".java") != std::string::npos ||
        fname_lower.find(".rs") != std::string::npos ||
        fname_lower.find(".go") != std::string::npos) {
        return ContentType::CODE;
    }

    // Check content patterns for chat
    if (text.find("User:") != std::string::npos &&
        text.find("Assistant:") != std::string::npos) {
        return ContentType::CHAT;
    }

    // Check for code indicators in content (first 2000 chars)
    std::string sample = text.substr(0, std::min(text.size(), (size_t)2000));
    int code_indicators = 0;
    if (sample.find("#include") != std::string::npos) code_indicators++;
    if (sample.find("def ") != std::string::npos) code_indicators++;
    if (sample.find("function ") != std::string::npos) code_indicators++;
    if (sample.find("class ") != std::string::npos) code_indicators++;
    if (sample.find("import ") != std::string::npos) code_indicators++;
    if (sample.find("const ") != std::string::npos) code_indicators++;
    if (sample.find("let ") != std::string::npos) code_indicators++;
    if (sample.find("var ") != std::string::npos) code_indicators++;
    if (sample.find("public ") != std::string::npos) code_indicators++;
    if (sample.find("private ") != std::string::npos) code_indicators++;
    if (sample.find("return ") != std::string::npos) code_indicators++;
    if (sample.find("if (") != std::string::npos || sample.find("if(") != std::string::npos) code_indicators++;
    if (sample.find("for (") != std::string::npos || sample.find("for(") != std::string::npos) code_indicators++;
    if (sample.find("while (") != std::string::npos || sample.find("while(") != std::string::npos) code_indicators++;
    if (sample.find("};") != std::string::npos) code_indicators++;
    if (sample.find("();") != std::string::npos) code_indicators++;

    if (code_indicators >= 3) {
        return ContentType::CODE;
    }

    return ContentType::DOCUMENT;
}

// Find semantic boundary for documents (paragraph or sentence)
static size_t find_document_boundary(const std::string& text, size_t target_pos, size_t min_pos, size_t max_pos) {
    // Clamp positions
    if (max_pos > text.size()) max_pos = text.size();
    if (min_pos > max_pos) min_pos = max_pos;
    if (target_pos < min_pos) target_pos = min_pos;
    if (target_pos > max_pos) target_pos = max_pos;

    // Priority 1: Double newline (paragraph boundary)
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (i+1 < text.size() && text[i] == '\n' && text[i+1] == '\n') {
            return i + 2;
        }
    }

    // Priority 2: Period followed by space or newline (sentence boundary)
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') {
            if (i+1 < text.size() && (text[i+1] == ' ' || text[i+1] == '\n' || text[i+1] == '\r')) {
                return i + 1;
            }
        }
    }

    // Priority 3: Newline
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (text[i] == '\n') {
            return i + 1;
        }
    }

    return target_pos;
}

// Find semantic boundary for code (function/class end)
static size_t find_code_boundary(const std::string& text, size_t target_pos, size_t min_pos, size_t max_pos) {
    // Clamp positions
    if (max_pos > text.size()) max_pos = text.size();
    if (min_pos > max_pos) min_pos = max_pos;
    if (target_pos < min_pos) target_pos = min_pos;
    if (target_pos > max_pos) target_pos = max_pos;

    // Priority 1: Closing brace followed by newline (end of function/class)
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (text[i] == '}') {
            if (i+1 >= text.size() || text[i+1] == '\n' || text[i+1] == '\r') {
                return i + 1;
            }
        }
    }

    // Priority 2: Blank line (double newline)
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (i+1 < text.size() && text[i] == '\n' && text[i+1] == '\n') {
            return i + 2;
        }
    }

    // Priority 3: Single newline
    for (size_t i = max_pos; i > min_pos && i > 0; i--) {
        if (text[i] == '\n') {
            return i + 1;
        }
    }

    return target_pos;
}

// Generate unified chunk ID: {code}_{TYPE}_{index}
static std::string generate_chunk_id(const std::string& code, ContentType type, uint64_t index) {
    return code + "_" + content_type_to_string(type) + "_" + std::to_string(index);
}

// ============= END CONTENT TYPE SYSTEM =============

struct Args {
    std::string input;
    std::string method = "lz4"; // zlib|zstd|lz4
    int level = 3;
    int threads = std::thread::hardware_concurrency();
    int chunk_tokens = 2000;
    int overlap_tokens = 0;
    std::string code;   // short code like drac or DM101
    std::string title;  // human-friendly title
    std::string output_dir = "build_out";
};

static void usage(const char* prog) {
    std::cerr << "Usage: " << prog << " --input FILE [--method zlib|zstd|lz4] [--level N] [--threads N]"
              << " [--chunk-tokens N] [--overlap N] [--code CODE] [--title TITLE] [--output-dir DIR]\n";
}

static std::string sanitize_code(const std::string& base, size_t maxlen=8) {
    std::string out;
    out.reserve(base.size());
    for (char c : base) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out.push_back(std::tolower(static_cast<unsigned char>(c)));
        }
    }
    if (out.empty()) out = "doc";
    if (out.size() > maxlen) out.resize(maxlen);
    return out;
}

static std::optional<Args> parse(int argc, char** argv) {
    Args a;
    for (int i=1;i<argc;++i) {
        std::string arg = argv[i];
        auto need = [&](int i){ if (i+1>=argc) { usage(argv[0]); return false; } return true; };
        if (arg=="--input") { if(!need(i)) return std::nullopt; a.input = argv[++i]; }
        else if (arg=="--method") { if(!need(i)) return std::nullopt; a.method = argv[++i]; }
        else if (arg=="--level") { if(!need(i)) return std::nullopt; a.level = std::stoi(argv[++i]); }
        else if (arg=="--threads") { if(!need(i)) return std::nullopt; a.threads = std::stoi(argv[++i]); }
        else if (arg=="--chunk-tokens") { if(!need(i)) return std::nullopt; a.chunk_tokens = std::stoi(argv[++i]); }
        else if (arg=="--overlap") { if(!need(i)) return std::nullopt; a.overlap_tokens = std::stoi(argv[++i]); }
        else if (arg=="--code") { if(!need(i)) return std::nullopt; a.code = argv[++i]; }
        else if (arg=="--title") { if(!need(i)) return std::nullopt; a.title = argv[++i]; }
        else if (arg=="--output-dir") { if(!need(i)) return std::nullopt; a.output_dir = argv[++i]; }
        else { std::cerr << "Unknown arg: " << arg << "\n"; usage(argv[0]); return std::nullopt; }
    }
    if (a.input.empty()) { usage(argv[0]); return std::nullopt; }
    if (a.threads <= 0) a.threads = 1;
    // Derive code/title from filename if missing
    if (a.code.empty()) {
        std::string base = a.input;
        size_t slash = base.find_last_of("/\\"); if (slash!=std::string::npos) base = base.substr(slash+1);
        size_t dot = base.find('.'); if (dot!=std::string::npos) base = base.substr(0,dot);
        a.code = sanitize_code(base, 8);
    }
    if (a.title.empty()) a.title = a.code;
    return a;
}

struct ChunkIn {
    uint64_t index;           // 1-based chunk number
    uint64_t token_start;     // inclusive
    uint64_t token_end;       // exclusive
    std::vector<std::string> tokens;  // chunk tokens (compute text/summary/keywords in worker)
    ContentType type = ContentType::DOCUMENT;  // Content type for smart chunking
    std::string source_file;  // Source filename (for CODE type)
};

struct ChunkOut {
    uint64_t index;
    uint64_t token_start;
    uint64_t token_end;
    uint64_t offset;          // in storage file
    uint64_t length;          // compressed length
    std::string summary;
    std::vector<std::string> keywords;
    ContentType type = ContentType::DOCUMENT;  // Content type
    std::string chunk_id;     // Unified chunk ID: {code}_{TYPE}_{index}
    std::string source_file;  // Source filename
};

// --- Compression helpers ---
static std::string compress_zlib(const std::string& input, int level) {
    uLong src_len = static_cast<uLong>(input.size());
    uLong bound = compressBound(src_len);
    std::string out(bound, '\0');
    uLongf dest_len = bound;
    int ret = compress2(reinterpret_cast<Bytef*>(&out[0]), &dest_len,
                        reinterpret_cast<const Bytef*>(input.data()), src_len,
                        level);
    if (ret != Z_OK) throw std::runtime_error("zlib compress2 failed");
    out.resize(dest_len);
    return out;
}
#ifdef ZSTD
static std::string compress_zstd(const std::string& input, int level) {
    size_t bound = ZSTD_compressBound(input.size());
    std::string out(bound, '\0');
    size_t written = ZSTD_compress(out.data(), out.size(), input.data(), input.size(), level);
    if (ZSTD_isError(written)) throw std::runtime_error("zstd compress failed");
    out.resize(written);
    return out;
}
#endif
#ifdef LZ4
static std::string compress_lz4(const std::string& input) {
    LZ4F_preferences_t prefs; memset(&prefs, 0, sizeof(prefs));
    size_t out_cap = LZ4F_compressFrameBound(input.size(), &prefs);
    std::string out(out_cap, '\0');
    size_t written = LZ4F_compressFrame(out.data(), out.size(), input.data(), input.size(), &prefs);
    if (LZ4F_isError(written)) throw std::runtime_error("lz4 compress failed");
    out.resize(written);
    return out;
}
#endif

static inline bool is_alpha_str(const std::string& s) {
    // Accept alphabetic characters including UTF-8 (accented chars)
    for (unsigned char c : s) {
        // ASCII alpha: a-z, A-Z
        // UTF-8 multibyte: high bit set (>=128)
        // Reject: digits, punctuation, spaces
        if (std::isalpha(c) || c >= 128) {
            continue;  // Valid char
        } else if (std::isdigit(c) || std::ispunct(c) || std::isspace(c)) {
            return false;  // Reject numbers, punctuation, spaces
        }
    }
    return !s.empty();
}

static std::vector<std::string> split_tokens(const std::string& text) {
    std::vector<std::string> out; out.reserve(256);
    std::string tok; tok.reserve(16);
    for (size_t i=0;i<text.size();++i) {
        unsigned char c = text[i];
        if (std::isspace(c)) {
            if (!tok.empty()) { out.push_back(tok); tok.clear(); }
        } else {
            tok.push_back(static_cast<char>(c));
        }
    }
    if (!tok.empty()) out.push_back(tok);
    return out;
}

static std::vector<std::string> split_sentences(const std::string& text) {
    std::vector<std::string> out; out.reserve(32);
    std::string cur; cur.reserve(128);
    auto flush = [&](){
        std::string s = cur;
        // trim
        size_t a=0; while (a<s.size() && std::isspace((unsigned char)s[a])) ++a;
        size_t b=s.size(); while (b>a && std::isspace((unsigned char)s[b-1])) --b;
        if (b>a) out.push_back(s.substr(a,b-a));
        cur.clear();
    };
    for (char ch : text) {
        cur.push_back(ch);
        if (ch=='.' || ch=='!' || ch=='?') flush();
    }
    if (!cur.empty()) flush();
    return out;
}

static std::vector<std::string> top_keywords(const std::vector<std::string>& tokens, size_t scan_tokens=200, size_t max_k=20) {
    // Common English stop words to filter out
    static const std::set<std::string> stop_words = {
        "the", "and", "for", "are", "but", "not", "you", "all", "can", "her",
        "was", "one", "our", "out", "day", "get", "has", "him", "his", "how",
        "man", "new", "now", "old", "see", "two", "way", "who", "boy", "did",
        "its", "let", "put", "say", "she", "too", "use", "may", "any", "got",
        "had", "yet", "own", "off", "big", "few", "run", "set", "six", "yes",
        "ago", "cry", "far", "fun", "lot", "red", "try", "also", "back", "been",
        "call", "came", "come", "down", "each", "even", "find", "from", "gave",
        "good", "have", "here", "into", "just", "know", "last", "like", "long",
        "look", "made", "make", "many", "more", "most", "much", "must", "name",
        "next", "only", "open", "over", "part", "same", "seem", "seen", "show",
        "side", "some", "such", "take", "than", "that", "them", "then", "they",
        "this", "time", "upon", "very", "want", "well", "went", "were", "what",
        "when", "will", "with", "your", "about", "after", "again", "before", "being",
        "below", "could", "doing", "every", "first", "found", "great", "later",
        "least", "never", "other", "place", "right", "since", "still", "their",
        "there", "these", "thing", "think", "those", "three", "under", "until",
        "where", "which", "while", "would", "years", "though", "through"
    };

    std::map<std::string,int> freq;
    size_t n = std::min(tokens.size(), scan_tokens);
    for (size_t i=0;i<n;++i) {
        std::string t = tokens[i];
        for (auto& c : t) c = std::tolower(static_cast<unsigned char>(c));
        // Filter: >2 chars, alpha only, not a stop word
        if (t.size()>2 && is_alpha_str(t) && stop_words.find(t) == stop_words.end()) {
            freq[t]++;
        }
    }
    std::vector<std::pair<int,std::string>> items; items.reserve(freq.size());
    for (auto& kv : freq) items.emplace_back(kv.second, kv.first);
    std::sort(items.begin(), items.end(), [](auto& a, auto& b){ if (a.first!=b.first) return a.first>b.first; return a.second<b.second; });
    std::vector<std::string> out; out.reserve(max_k);
    for (size_t i=0;i<items.size() && out.size()<max_k; ++i) out.push_back(items[i].second);
    return out;
}

// --- Lightweight entity extraction (people, orgs, places, events, dates, numbers) ---
static inline std::string strip_punct(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::ispunct((unsigned char)s[a])) ++a;
    while (b > a && std::ispunct((unsigned char)s[b-1])) --b;
    return s.substr(a, b - a);
}

static inline bool is_titlecase(const std::string& s) {
    if (s.size() < 2) return false;
    if (!std::isalpha((unsigned char)s[0]) || !std::isupper((unsigned char)s[0])) return false;
    // allow internal hyphen/apostrophe cases like O'Neil, Jean-Luc
    for (size_t i=1;i<s.size();++i) {
        unsigned char c = s[i];
        if (!(std::islower(c) || c=='-' || c=='\'')) return false;
    }
    return true;
}

static inline bool is_acronym(const std::string& s) {
    if (s.size() < 2 || s.size() > 10) return false;
    for (unsigned char c : s) {
        if (!(std::isupper(c) || std::isdigit(c) || c=='&' || c=='-')) return false;
    }
    return true;
}

static inline bool is_connector(const std::string& s) {
    static const char* words[] = {"of","and","for","the","de","di","da","van","von","y","la","le","&"};
    for (auto* w : words) if (s == w) return true;
    return false;
}

static inline bool is_month(const std::string& s) {
    static const char* months[] = {"january","february","march","april","may","june","july","august","september","october","november","december",
                                   "jan","feb","mar","apr","jun","jul","aug","sep","sept","oct","nov","dec"};
    for (auto* m : months) if (s == m) return true;
    return false;
}

static inline bool is_weekday(const std::string& s) {
    static const char* days[] = {"monday","tuesday","wednesday","thursday","friday","saturday","sunday","mon","tue","tues","wed","thu","thur","thu","fri","sat","sun"};
    for (auto* d : days) if (s == d) return true;
    return false;
}

static inline bool looks_like_year(const std::string& s) {
    if (s.size()!=4) return false;
    for (unsigned char c : s) if (!std::isdigit(c)) return false;
    int y = std::stoi(s);
    return y >= 1800 && y <= 2100;
}

static inline bool looks_like_money(const std::string& tok) {
    // $1,234.56 or 1.2m, 3bn, etc.
    std::string s = tok;
    for (auto& c : s) c = std::tolower((unsigned char)c);
    if (!s.empty() && (s[0]=='$' || s.back()=='$')) return true;
    if (s.find("usd")!=std::string::npos || s.find("eur")!=std::string::npos || s.find("gbp")!=std::string::npos) return true;
    if (!s.empty() && (s.back()=='m' || s.back()=='b' || s.back()=='k')) {
        // number + suffix
        bool any=false; for (char c : s) if (std::isdigit((unsigned char)c)) { any=true; break; }
        if (any) return true;
    }
    return false;
}

static inline bool looks_like_percent(const std::string& tok) {
    if (tok.empty()) return false;
    if (tok.back()=='%') return true;
    std::string s = tok; for (auto& c : s) c = std::tolower((unsigned char)c);
    return s.find("percent")!=std::string::npos;
}

static std::vector<std::string> extract_entities(const std::vector<std::string>& tokens) {
    std::vector<std::string> entities; entities.reserve(64);
    std::set<std::string> seen_lower;

    auto push_unique = [&](const std::string& e){
        if (e.empty()) return;
        std::string low=e; for (auto& c:low) c = std::tolower((unsigned char)c);
        if (!seen_lower.count(low)) { seen_lower.insert(low); entities.push_back(e); }
    };

    // 1) Multi-token proper-noun sequences
    size_t i=0; while (i<tokens.size()) {
        std::string t = strip_punct(tokens[i]);
        if (t.empty()) { ++i; continue; }
        std::string low=t; for (auto& c:low) c = std::tolower((unsigned char)c);
        bool start = is_titlecase(t) || is_acronym(t);
        if (!start) { ++i; continue; }
        // collect run
        std::vector<std::string> parts; parts.push_back(t);
        size_t j=i+1;
        while (j<tokens.size()) {
            std::string u = strip_punct(tokens[j]); if (u.empty()) break;
            std::string ul=u; for (auto& c:ul) c = std::tolower((unsigned char)c);
            if (is_connector(ul)) { parts.push_back(ul); ++j; continue; }
            if (is_titlecase(u) || is_acronym(u)) { parts.push_back(u); ++j; continue; }
            break;
        }
        if (!parts.empty()) {
            // Avoid sequences that are only connectors or single very short tokens
            int real=0; for (auto& p:parts){ std::string pl=p; for(auto& c:pl) c=std::tolower((unsigned char)c); if (!is_connector(pl)) real++; }
            if (real>=1) {
                std::ostringstream oss; for (size_t k=0;k<parts.size();++k){ if(k)oss<<' '; oss<<parts[k]; }
                push_unique(oss.str());
            }
        }
        i = (j>i? j : i+1);
    }

    // 2) Dates, months, weekdays, years
    for (const auto& raw : tokens) {
        std::string t = strip_punct(raw); if (t.empty()) continue;
        std::string low=t; for (auto& c:low) c = std::tolower((unsigned char)c);
        if (is_month(low) || is_weekday(low) || looks_like_year(t)) push_unique(t);
    }

    // 3) Money and percents
    for (const auto& raw : tokens) {
        std::string t = strip_punct(raw); if (t.empty()) continue;
        if (looks_like_money(t) || looks_like_percent(t)) push_unique(t);
    }

    // Limit to a reasonable number
    if (entities.size() > 50) entities.resize(50);
    return entities;
}

static std::string build_extractive_summary(const std::string& text, const std::vector<std::string>& kw, size_t target_tokens_low=50, size_t target_tokens_high=100) {
    auto sents = split_sentences(text);
    if (sents.empty()) {
        // fallback to first N tokens
        auto toks = split_tokens(text);
        size_t take = std::min(toks.size(), target_tokens_high);
        std::ostringstream oss; for (size_t i=0;i<take;++i){ if(i)oss<<' '; oss<<toks[i]; }
        return oss.str();
    }
    auto kwset = std::set<std::string>(kw.begin(), kw.end());
    // score sentences by keyword hits
    struct Sc { double score; size_t idx; };
    std::vector<Sc> scored; scored.reserve(sents.size());
    for (size_t i=0;i<sents.size();++i) {
        auto toks = split_tokens(sents[i]);
        int hits=0;
        for (auto& t : toks) {
            std::string tl=t; for (auto& c:tl) c=std::tolower((unsigned char)c);
            if (kwset.count(tl)) hits++;
        }
        double len = std::max<size_t>(toks.size(),1);
        double score = hits / len; // normalized
        // small bonus for early sentences
        score += 0.05 * (1.0 / double(i+1));
        scored.push_back({score, i});
    }
    std::sort(scored.begin(), scored.end(), [](const Sc& a, const Sc& b){ return a.score>b.score; });
    // pick top sentences until within token range
    std::vector<bool> picked(sents.size(), false);
    std::vector<size_t> order;
    size_t total_tokens=0;
    for (auto& sc : scored) {
        if (total_tokens >= target_tokens_low) break;
        picked[sc.idx] = true;
        order.push_back(sc.idx);
        total_tokens += split_tokens(sents[sc.idx]).size();
    }
    // sort selected sentences by original order
    std::sort(order.begin(), order.end());
    std::ostringstream oss;
    for (size_t j=0;j<order.size();++j) {
        if (j) oss << ' ';
        oss << sents[order[j]];
    }
    // ensure not exceeding upper bound too much
    auto summary = oss.str();
    auto sumtoks = split_tokens(summary);
    if (sumtoks.size() > target_tokens_high) {
        std::ostringstream cut; for (size_t i=0;i<target_tokens_high;++i){ if(i)cut<<' '; cut<<sumtoks[i]; }
        return cut.str();
    }
    if (summary.empty()) {
        // final fallback
        auto toks = split_tokens(text);
        size_t take = std::min(toks.size(), target_tokens_high);
        std::ostringstream o2; for (size_t i=0;i<take;++i){ if(i)o2<<' '; o2<<toks[i]; }
        return o2.str();
    }
    return summary;
}

static std::string json_escape(const std::string& s) {
    std::ostringstream o; o<<'"';
    for (unsigned char c : s) {
        switch(c) {
            case '"': o << "\\\""; break;
            case '\\': o << "\\\\"; break;
            case '\n': o << "\\n"; break;
            case '\r': o << "\\r"; break;
            case '\t': o << "\\t"; break;
            default:
                if (c < 0x20) { o << "\\u" << std::hex << std::setw(4) << std::setfill('0') << int(c); }
                else { o << (char)c; }
        }
    }
    o<<'"'; return o.str();
}

int main(int argc, char** argv) {
    auto pargs = parse(argc, argv); if (!pargs) return 2; Args args = *pargs;

    struct stat st; if (stat(args.input.c_str(), &st)!=0) { std::cerr<<"❌ cannot stat input\n"; return 2; }
    double file_mb = double(st.st_size)/(1024.0*1024.0);

    // Prepare output directory
    std::string outdir = args.output_dir;
    std::string mkcmd = std::string("mkdir -p ") + outdir + "/storage";
    std::ignore = system(mkcmd.c_str());

    // Storage and manifests
    std::string storage_path = outdir + "/storage/" + args.code + ".bin";
    std::string manifest_path = outdir + "/manifest_" + args.code + ".jsonl";
    std::string guide_path = outdir + "/chapter_guide.json";

    std::ofstream storage(storage_path, std::ios::binary);
    if (!storage) { std::cerr<<"❌ failed to open storage file\n"; return 2; }
    std::ofstream manifest(manifest_path);
    if (!manifest) { std::cerr<<"❌ failed to open manifest file\n"; return 2; }

    std::cout << "🛠️ Building chunked dataset\n";
    std::cout << "Code: "<<args.code<<" | Title: "<<args.title<<"\n";
    std::cout << "Input: "<<args.input<<" ("<<std::fixed<<std::setprecision(1)<<file_mb<<"MB)\n";
    std::cout << "Threads: "<<args.threads<<" | Chunk tokens: "<<args.chunk_tokens<<" | Overlap: "<<args.overlap_tokens<<"\n";
    std::cout << "Compression: "<<args.method<<" (level "<<args.level<<")\n";

    // Threading structures
    struct Work { ChunkIn in; };
    struct Done { ChunkOut out; std::string compressed; };
    
    std::queue<Work> q; std::mutex qmu; std::condition_variable qcv; bool qdone=false;
    std::queue<Done> dq; std::mutex dqmu; std::condition_variable dqcv; bool dqd=false;

    std::atomic<uint64_t> total_tokens{0};
    std::atomic<uint64_t> total_chunks{0};
    std::atomic<uint64_t> total_bytes{0};
    // Counters by content type for chapter guide
    std::atomic<uint64_t> doc_count{0};
    std::atomic<uint64_t> chat_count{0};
    std::atomic<uint64_t> code_count{0};
    std::atomic<uint64_t> fix_count{0};
    std::atomic<uint64_t> feat_count{0};

    // Feature 4: Track code files and their chunks for enhanced chapter guide
    std::map<std::string, std::vector<std::string>> code_file_chunks;  // filename -> [chunk_ids]
    std::mutex code_file_mutex;

    // Workers
    std::vector<std::thread> workers;
    workers.reserve(args.threads);
    
    auto worker_fn = [&](){
        try {
            while (true) {
                Work w; {
                    std::unique_lock<std::mutex> lk(qmu);
                    qcv.wait(lk, [&]{ return !q.empty() || qdone; });
                    if (q.empty()) break;
                    w = std::move(q.front());
                    q.pop();
                }
                
                // Build text from tokens (moved from producer)
                std::ostringstream text_builder;
                for (size_t i = 0; i < w.in.tokens.size(); ++i) {
                    if (i > 0) text_builder << ' ';
                    text_builder << w.in.tokens[i];
                }
                std::string text = text_builder.str();

                // Debug output (only first chunk, disabled in production)
                #ifdef DEBUG_BUILD
                if (w.in.index == 1) {
                    std::cerr << "DEBUG chunk 1: text length=" << text.size() << "\n";
                    std::cerr << "DEBUG chunk 1: first 200 chars: " << text.substr(0, 200) << "\n";
                }
                #endif

                // Create summary: first 100 chars + last 100 chars (like working Python version)
                std::string summary;
                if (text.size() <= 200) {
                    summary = text;
                } else {
                    // First 100 chars
                    std::string first_part = text.substr(0, 100);
                    // Last 100 chars
                    std::string last_part = text.substr(text.size() - 100);
                    summary = first_part + " ... " + last_part;
                }

                // Ensure summary doesn't exceed 200 chars (plus ellipsis)
                if (summary.size() > 210) {
                    summary = summary.substr(0, 210);
                }

                #ifdef DEBUG_BUILD
                if (w.in.index == 1) {
                    std::cerr << "DEBUG chunk 1: summary length=" << summary.size() << "\n";
                    std::cerr << "DEBUG chunk 1: summary: " << summary.substr(0, 200) << "\n";
                }
                #endif

                // Compress the FULL TEXT, not the summary
                
                // compress
                std::string comp;
                if (args.method=="zlib") comp = compress_zlib(text, args.level);
                else if (args.method=="zstd") {
                #ifdef ZSTD
                    comp = compress_zstd(text, args.level);
                #else
                    throw std::runtime_error("zstd not compiled");
                #endif
                }
                else if (args.method=="lz4") {
                #ifdef LZ4
                    comp = compress_lz4(text);
                #else
                    throw std::runtime_error("lz4 not compiled");
                #endif
                } else throw std::runtime_error("unknown method");

                Done d;
                d.out.index = w.in.index;
                d.out.token_start = w.in.token_start;
                d.out.token_end = w.in.token_end;
                d.out.length = comp.size(); // offset later by writer
                d.out.offset = 0; // fill later
                d.out.summary = std::move(summary);
                // Extract keywords for inverted index (top 40, >2 chars, alpha only, stop words filtered)
                d.out.keywords = top_keywords(w.in.tokens, 500, 40);
                // Content type detection and unified chunk ID
                d.out.type = detect_content_type(text, w.in.source_file);
                d.out.source_file = w.in.source_file;
                // chunk_id will be generated by writer thread with correct args.code
                d.compressed = std::move(comp);

                {
                    std::lock_guard<std::mutex> lk(dqmu);
                    dq.push(std::move(d));
                }
                dqcv.notify_one();
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lk(dqmu);
            std::cerr << "Worker error: "<< e.what() <<"\n";
            dqd = true;
            dqcv.notify_all();
        }
    };
    
    // Start workers
    for (int i = 0; i < args.threads; ++i) {
        workers.emplace_back(worker_fn);
    }

    // Writer
    uint64_t write_offset = 0;
    std::mutex writemu;
    auto write_thread = std::thread([&]{
        std::map<uint64_t, ChunkOut> tracker;
        while (true) {
            Done d; bool got=false;
            {
                std::unique_lock<std::mutex> lk(dqmu);
                dqcv.wait(lk, [&]{ return !dq.empty() || (qdone && (dq.empty())) || dqd; });
                if (!dq.empty()) { d = std::move(dq.front()); dq.pop(); got=true; }
                else if (qdone && dq.empty()) break;
            }
            if (!got) continue;
            // write compressed bytes to storage
            uint64_t my_off;
            {
                std::lock_guard<std::mutex> lk(writemu);
                my_off = write_offset;
                storage.write(d.compressed.data(), d.compressed.size());
                write_offset += d.compressed.size();
            }
            // emit manifest line
            d.out.offset = my_off;
            total_bytes += d.out.length;
            total_chunks += 1;
            total_tokens += (d.out.token_end - d.out.token_start);

            // Increment type-specific counter
            switch(d.out.type) {
                case ContentType::DOCUMENT: doc_count++; break;
                case ContentType::CHAT: chat_count++; break;
                case ContentType::CODE: code_count++; break;
                case ContentType::FIX: fix_count++; break;
                case ContentType::FEATURE: feat_count++; break;
            }

            // Generate unified chunk ID: {code}_{TYPE}_{index}
            std::string chunk_id = generate_chunk_id(args.code, d.out.type, d.out.index);
            d.out.chunk_id = chunk_id;

            // Feature 4: Track code files and their chunks
            if (d.out.type == ContentType::CODE && !d.out.source_file.empty()) {
                std::lock_guard<std::mutex> lock(code_file_mutex);
                code_file_chunks[d.out.source_file].push_back(chunk_id);
            }

            // Serialize keywords array
            std::ostringstream keywords_json;
            keywords_json << '[';
            for (size_t i = 0; i < d.out.keywords.size(); ++i) {
                if (i > 0) keywords_json << ',';
                keywords_json << json_escape(d.out.keywords[i]);
            }
            keywords_json << ']';

            manifest << '{'
                     << "\"code\":" << json_escape(args.code) << ','
                     << "\"title\":" << json_escape(args.title) << ','
                     << "\"chunk_id\":" << json_escape(chunk_id) << ','
                     << "\"type\":" << json_escape(content_type_to_string(d.out.type)) << ','
                     << "\"index\":" << d.out.index << ','
                     << "\"token_start\":" << d.out.token_start << ','
                     << "\"token_end\":" << d.out.token_end << ','
                     << "\"offset\":" << d.out.offset << ','
                     << "\"length\":" << d.out.length << ','
                     << "\"compression\":" << json_escape(args.method) << ','
                     << "\"summary\":" << json_escape(d.out.summary) << ','
                     << "\"keywords\":" << keywords_json.str();

            // Add source_file if present (for CODE type)
            if (!d.out.source_file.empty()) {
                manifest << ',' << "\"source_file\":" << json_escape(d.out.source_file);
            }

            manifest << "}\n";
        }
    });

    // Producer: stream and chunk with summaries/keywords
    auto t0 = std::chrono::steady_clock::now();
    std::ifstream in(args.input, std::ios::in | std::ios::binary);
    if (!in) { std::cerr<<"❌ failed to open input\n"; return 2; }
    const size_t BUF = 1<<20; std::vector<char> buf(BUF);

    uint64_t global_token_index = 0;
    uint64_t chunk_idx = 0;
    uint64_t current_tokens = 0;
    std::vector<std::string> current_tokens_vec; current_tokens_vec.reserve(args.chunk_tokens + 100);
    std::deque<std::string> overlap_ring;
    bool in_tok=false; std::string tok; tok.reserve(32);

    // Document boundary detection function
    auto is_document_boundary = [](const std::string& line) {
        if (line.empty()) return false;
        
        // Check for separator lines like "===== putWindowclemens.lab21.org.v34.shell-extension.zip ====="
        if (line.find("=====") != std::string::npos && line.length() > 10) {
            return true;
        }
        
        // Check for Project Gutenberg markers
        if (line.find("*** START OF") != std::string::npos ||
            line.find("*** END OF") != std::string::npos ||
            line.find("PROJECT GUTENBERG") != std::string::npos) {
            return true;
        }
        
        // Check for new book markers
        if ((line.find("BOOK") != std::string::npos || 
             line.find("Chapter") != std::string::npos ||
             line.find("CHAPTER") != std::string::npos) && 
            line.length() < 100) { // Avoid matching text that just contains these words
            return true;
        }
        
        // Check for JSON block starts (like extension metadata)
        if (line[0] == '{' && line.find('"') != std::string::npos) {
            return true;
        }
        
        return false;
    };

    auto flush_chunk = [&](){
        if (current_tokens==0) return;
        ++chunk_idx;
        // Just pass tokens to worker (lightweight producer)
        ChunkIn inobj;
        inobj.index = chunk_idx;
        inobj.token_start = global_token_index - current_tokens;
        inobj.token_end = global_token_index;
        inobj.tokens = std::move(current_tokens_vec);
        inobj.source_file = args.input;  // Track source file for type detection
        inobj.type = ContentType::DOCUMENT;  // Default, will be refined by worker

        {
            std::lock_guard<std::mutex> lk(qmu);
            q.push(Work{std::move(inobj)});
        }
        qcv.notify_one();

        current_tokens_vec.clear();
        current_tokens_vec.reserve(args.chunk_tokens + 100);
        current_tokens = 0;
    };

    // Line-by-line processing to detect document boundaries
    std::string current_line;
    current_line.reserve(1024);
    
    while (in) {
        in.read(buf.data(), BUF); std::streamsize got = in.gcount(); if (got<=0) break;
        for (std::streamsize i=0;i<got;++i) {
            unsigned char c = buf[i];
            
            // Build current line for boundary detection
            if (c == '\n') {
                // Check for document boundary at line end
                if (is_document_boundary(current_line)) {
                    // Flush current chunk if it has content
                    if (current_tokens > 0) {
                        flush_chunk();
                    }
                }
                current_line.clear();
            } else if (c != '\r') { // Skip carriage returns
                current_line.push_back((char)c);
            }
            
            // Original token processing logic
            if (std::isspace(c)) {
                if (in_tok) {
                    // end token - add to vector instead of building text
                    current_tokens_vec.push_back(tok);
                    if (args.overlap_tokens>0) {
                        overlap_ring.push_back(tok);
                        if (overlap_ring.size()> (size_t)args.overlap_tokens) overlap_ring.pop_front();
                    }
                    tok.clear(); in_tok=false;
                    current_tokens++; global_token_index++;
                    if (current_tokens >= (uint64_t)args.chunk_tokens) flush_chunk();
                }
            } else {
                in_tok=true; tok.push_back((char)c);
            }
        }
    }
    if (in_tok && !tok.empty()) {
        current_tokens_vec.push_back(tok);
        tok.clear();
        current_tokens++; global_token_index++;
    }
    flush_chunk();

    // finish queues
    {
        std::lock_guard<std::mutex> lk(qmu); qdone=true;
    }
    qcv.notify_all();
    
    // Join workers
    for (auto& w : workers) {
        w.join();
    }

    write_thread.join();

    auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1-t0).count();

    storage.close(); manifest.close();

    // Chapter guide (version 3.0 with enhanced structure - Feature 4)
    {
        std::ofstream guide(guide_path);
        if (guide) {
            guide << "{\n";
            guide << "  \"version\": \"3.0\",\n";
            guide << "  \"title\": "<< json_escape(args.title) << ",\n";
            guide << "  \"code\": "<< json_escape(args.code) << ",\n";
            guide << "  \"chunks\": {\n";
            guide << "    \"total\": " << total_chunks.load() << ",\n";
            guide << "    \"by_type\": {\n";
            guide << "      \"DOC\": " << doc_count.load() << ",\n";
            guide << "      \"CHAT\": " << chat_count.load() << ",\n";
            guide << "      \"CODE\": " << code_count.load() << ",\n";
            guide << "      \"FIX\": " << fix_count.load() << ",\n";
            guide << "      \"FEAT\": " << feat_count.load() << "\n";
            guide << "    }\n";
            guide << "  },\n";
            guide << "  \"chunk_id_format\": \"{code}_{TYPE}_{N}\",\n";

            // Feature 4: Code files section
            guide << "  \"code_files\": {\n";
            guide << "    \"count\": " << code_file_chunks.size() << ",\n";
            guide << "    \"files\": [\n";
            bool first_file = true;
            for (const auto& [filename, chunks] : code_file_chunks) {
                if (!first_file) guide << ",\n";
                first_file = false;
                guide << "      {\"filename\": " << json_escape(filename) << ", \"chunk_ids\": [";
                bool first_chunk = true;
                for (const auto& chunk_id : chunks) {
                    if (!first_chunk) guide << ", ";
                    first_chunk = false;
                    guide << json_escape(chunk_id);
                }
                guide << "]}";
            }
            guide << "\n    ]\n";
            guide << "  },\n";

            // Placeholder sections for runtime-populated data
            guide << "  \"conversations\": {\n";
            guide << "    \"count\": 0,\n";
            guide << "    \"summaries\": []\n";
            guide << "  },\n";
            guide << "  \"fixes\": {\n";
            guide << "    \"count\": 0,\n";
            guide << "    \"entries\": []\n";
            guide << "  },\n";
            guide << "  \"features\": {\n";
            guide << "    \"count\": 0,\n";
            guide << "    \"entries\": []\n";
            guide << "  },\n";

            guide << "  \"manifest\": "<< json_escape(std::string("manifest_")+args.code+".jsonl") << ",\n";
            guide << "  \"storage\": "<< json_escape(std::string("storage/")+args.code+".bin") << "\n";
            guide << "}\n";
        }
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n✅ Build complete\n";
    std::cout << "  Chunks: "<< total_chunks.load() << " | Tokens: "<< total_tokens.load() << "\n";
    std::cout << "  Time: "<< secs << " s | Rate: "<< (total_tokens.load()/secs/1e6) << " M tokens/s\n";
    std::cout << "  Output: "<< manifest_path << " and " << storage_path << "\n";
    std::cout << "  Guide:  "<< guide_path << "\n";

    return 0;
}

