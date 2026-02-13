#pragma once
// OceanEterna v4 Search Engine
// BM25 TAAT search with Porter stemming and keyword extraction
// Requires: porter_stemmer.hpp, Corpus/Hit structs, g_config, g_stem_cache, g_stem_to_keywords

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>

// 2-char abbreviation whitelist (kept as keywords despite being short)
static const std::unordered_set<std::string> ABBREV_WHITELIST = {
    "ai", "ml", "us", "uk", "eu", "un", "os", "io", "db", "ip",
    "id", "ui", "ux", "qa", "hr", "it", "pc", "tv", "dj", "dc",
    "nj", "ny", "la", "sf", "co", "vs", "ph", "gp", "bp", "pr"
};

// Common English stop words to filter out
static const std::unordered_set<std::string> STOP_WORDS = {
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

// Extract keywords from text content
inline std::vector<std::string> extract_text_keywords(const std::string& text) {
    std::unordered_map<std::string, int> word_freq;
    std::string current_word;

    for (unsigned char c : text) {
        if (c < 128 && isalnum(c)) {
            current_word += (char)tolower(c);
        } else if (!current_word.empty()) {
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

    std::vector<std::pair<std::string, int>> sorted_words(word_freq.begin(), word_freq.end());
    std::sort(sorted_words.begin(), sorted_words.end(),
         [](const std::pair<std::string,int>& a, const std::pair<std::string,int>& b) {
             if (a.second != b.second) return a.second > b.second;
             return a.first < b.first;
         });

    std::vector<std::string> keywords;
    keywords.reserve(sorted_words.size());
    for (const auto& word_pair : sorted_words) {
        keywords.push_back(word_pair.first);
    }
    return keywords;
}

// Build stem cache and reverse mapping from corpus inverted index
inline void build_stemmed_index(const Corpus& corpus) {
    auto start = std::chrono::high_resolution_clock::now();
    std::cout << "Building stemmed index..." << std::flush;

    for (const auto& [keyword, doc_list] : corpus.inverted_index) {
        std::string stemmed = porter::stem(keyword);
        g_stem_cache[keyword] = stemmed;
        g_stem_to_keywords[stemmed].push_back(keyword);
    }

    auto elapsed = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
    std::cout << " Done! (" << g_stem_cache.size() << " keywords -> "
         << g_stem_to_keywords.size() << " stems in " << elapsed << "s)" << std::endl;
}

// Get stem for a keyword (uses cache, falls back to computing)
inline std::string get_stem(const std::string& word) {
    auto it = g_stem_cache.find(word);
    if (it != g_stem_cache.end()) return it->second;
    std::string stemmed = porter::stem(word);
    g_stem_cache[word] = stemmed;
    return stemmed;
}

// BM25 search - TAAT (Term-At-A-Time) with inverted index
inline std::vector<Hit> search_bm25(const Corpus& corpus, const std::string& query, int topk) {
    std::vector<std::string> raw_terms = extract_text_keywords(query);

    std::vector<std::string> query_terms;
    for (const std::string& t : raw_terms) {
        std::string stemmed = get_stem(t);
        if (std::find(query_terms.begin(), query_terms.end(), stemmed) == query_terms.end())
            query_terms.push_back(stemmed);
    }

    const double k1 = g_config.search.k1;
    const double b = g_config.search.b;
    const size_t N = corpus.docs.size();

    std::unordered_map<uint32_t, double> doc_scores;

    for (const std::string& term : query_terms) {
        auto stk_it = g_stem_to_keywords.find(term);
        if (stk_it == g_stem_to_keywords.end()) continue;

        int df = 0;
        for (const std::string& orig_kw : stk_it->second) {
            auto inv_it = corpus.inverted_index.find(orig_kw);
            if (inv_it != corpus.inverted_index.end())
                df += inv_it->second.size();
        }
        if (df == 0) continue;

        double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);

        for (const std::string& orig_kw : stk_it->second) {
            auto inv_it = corpus.inverted_index.find(orig_kw);
            if (inv_it == corpus.inverted_index.end()) continue;

            for (uint32_t doc_idx : inv_it->second) {
                int doc_len = corpus.docs[doc_idx].keywords.size();
                double norm = k1 * (1.0 - b + b * doc_len / corpus.avgdl);
                double score = idf * (k1 + 1.0) / (1.0 + norm);
                doc_scores[doc_idx] += score;
            }
        }
    }

    std::vector<std::pair<uint32_t, double>> scored_docs(doc_scores.begin(), doc_scores.end());
    int result_count = std::min((int)scored_docs.size(), topk);
    if (result_count > 0) {
        std::partial_sort(scored_docs.begin(),
                     scored_docs.begin() + result_count,
                     scored_docs.end(),
                     [](const std::pair<uint32_t, double>& a, const std::pair<uint32_t, double>& b) {
                         return a.second > b.second;
                     });
    }

    std::vector<Hit> hits;
    for (int i = 0; i < result_count; i++) {
        Hit hit;
        hit.doc_idx = scored_docs[i].first;
        hit.score = scored_docs[i].second;
        hits.push_back(hit);
    }
    return hits;
}

// Wrapper for compatibility with existing call sites
inline std::vector<Hit> search_bm25_fast(const Corpus& corpus, const std::string& query, int topk) {
    return search_bm25(corpus, query, topk);
}
