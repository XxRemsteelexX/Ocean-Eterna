#pragma once
// OceanEterna v8.0 Search Engine — mmap index + legacy fallback
// BM25 TAAT search with Porter stemming, shard-parallel search, and keyword extraction
// Requires: porter_stemmer.hpp, Corpus/Hit structs, g_config, g_stem_cache, g_stem_to_keywords
// Optional: mmap_index.hpp for zero-copy mmap-based search (v8.0+)

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <iostream>
#include <shared_mutex>
#include <omp.h>
#include "mmap_index.hpp"

// global mmap index — set by server if stems.idx exists
extern MmapIndex* g_mmap_idx;

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

// Step 22: Extract keywords with term frequencies from text content
// Returns pair<keywords, freq_map> where freq_map[keyword] = count
inline std::pair<std::vector<std::string>, std::unordered_map<std::string, uint16_t>>
extract_text_keywords_with_tf(const std::string& text) {
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
    std::unordered_map<std::string, uint16_t> tf_map;
    keywords.reserve(sorted_words.size());
    for (const auto& word_pair : sorted_words) {
        keywords.push_back(word_pair.first);
        tf_map[word_pair.first] = static_cast<uint16_t>(std::min(word_pair.second, 65535));
    }
    return {keywords, tf_map};
}

// Legacy wrapper for backward compatibility
inline std::vector<std::string> extract_text_keywords(const std::string& text) {
    auto [keywords, tf] = extract_text_keywords_with_tf(text);
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
// Thread-safe: uses shared lock for reads, unique lock for writes
extern std::shared_mutex g_stem_mutex;

inline std::string get_stem(const std::string& word) {
    // Fast path: read-only lookup under shared lock
    {
        std::shared_lock<std::shared_mutex> rlock(g_stem_mutex);
        auto it = g_stem_cache.find(word);
        if (it != g_stem_cache.end()) return it->second;
    }
    // Slow path: compute stem and insert under unique lock
    std::string stemmed = porter::stem(word);
    {
        std::unique_lock<std::shared_mutex> wlock(g_stem_mutex);
        g_stem_cache[word] = stemmed;
    }
    return stemmed;
}

// BM25 search - TAAT (Term-At-A-Time) with inverted index
inline std::vector<Hit> search_bm25(const Corpus& corpus, const std::string& query, int topk) {
    // v4.2: Guard against empty corpus (division by zero on avgdl)
    if (corpus.docs.empty() || corpus.avgdl <= 0) {
        return {};
    }

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

            // Step 23: Look up tf from tf_index if available
            auto tf_kw_it = corpus.tf_index.find(orig_kw);

            for (uint32_t doc_idx : inv_it->second) {
                int doc_len = corpus.docs[doc_idx].keyword_ids.size();

                // Get term frequency: use tf_index if available, else assume tf=1 for legacy docs
                double tf = 1.0;
                if (tf_kw_it != corpus.tf_index.end()) {
                    auto tf_doc_it = tf_kw_it->second.find(doc_idx);
                    if (tf_doc_it != tf_kw_it->second.end()) {
                        tf = tf_doc_it->second;
                    }
                }

                // Full BM25 formula with proper tf handling
                // For tf=1 (legacy docs), this reduces to the original formula
                double dl_ratio = (double)doc_len / corpus.avgdl;
                double score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl_ratio));
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

// v4.4: Two-phase BM25 search with keyword coverage rescoring
// Phase 1: standard BM25 to get top-64 candidates
// Phase 2: rescore by query term coverage density → rewards chunks matching more query terms
inline std::vector<Hit> search_bm25_twophase(const Corpus& corpus, const std::string& query, int topk) {
    if (corpus.docs.empty() || corpus.avgdl <= 0) return {};

    // phase 1: oversample — get top-256 BM25 candidates
    int oversample = std::max(topk * 32, 256);
    std::vector<Hit> candidates = search_bm25(corpus, query, oversample);
    if ((int)candidates.size() <= topk) return candidates;

    // extract query stems for coverage scoring
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = get_stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return candidates;

    // phase 2: rescore each candidate by keyword coverage
    for (auto& hit : candidates) {
        int matches = 0;
        const auto& doc = corpus.docs[hit.doc_idx];
        for (const auto& qstem : query_stems) {
            // check if any keyword in this doc matches the query stem
            for (uint32_t kid : doc.keyword_ids) {
                const std::string& kw = corpus.keyword_dict[kid];
                std::string kstem;
                {
                    std::shared_lock<std::shared_mutex> rlock(g_stem_mutex);
                    auto it = g_stem_cache.find(kw);
                    if (it != g_stem_cache.end()) kstem = it->second;
                }
                if (kstem.empty()) kstem = porter::stem(kw);
                if (kstem == qstem) { matches++; break; }
            }
        }
        double coverage = (double)matches / query_stems.size();
        double coverage_boost = 0.8 * coverage * coverage;
        hit.score *= (1.0 + coverage_boost);

        // exact match bonus (25% per match, max 75%)
        int exact_matches = 0;
        for (const auto& raw_term : raw_terms) {
            if (raw_term.find('_') != std::string::npos) continue;
            for (uint32_t kid : doc.keyword_ids) {
                const std::string& kw = corpus.keyword_dict[kid];
                if (kw == raw_term) { exact_matches++; break; }
            }
        }
        double exact_boost = std::min(exact_matches * 0.25, 0.75);
        hit.score *= (1.0 + exact_boost);
    }

    // re-sort by rescored value
    std::sort(candidates.begin(), candidates.end(),
        [](const Hit& a, const Hit& b) { return a.score > b.score; });

    candidates.resize(std::min((int)candidates.size(), topk));
    return candidates;
}

// v4.4: Source coherence boost — multiple hits from same source file get boosted
// rationale: if BM25 finds multiple relevant chunks from the same document,
// that document is likely highly relevant
inline void apply_source_coherence(std::vector<Hit>& hits, const Corpus& corpus,
                                    double boost_per_extra = 0.05, double max_boost = 0.30) {
    if (hits.size() < 2) return;

    // count hits per source file
    std::unordered_map<std::string, int> source_counts;
    for (const auto& hit : hits) {
        if (hit.doc_idx < corpus.docs.size()) {
            const auto& sf = corpus.docs[hit.doc_idx].source_file;
            if (!sf.empty()) source_counts[sf]++;
        }
    }

    // boost hits from sources with multiple hits
    bool modified = false;
    for (auto& hit : hits) {
        if (hit.doc_idx < corpus.docs.size()) {
            const auto& sf = corpus.docs[hit.doc_idx].source_file;
            if (!sf.empty()) {
                int count = source_counts[sf];
                if (count > 1) {
                    double boost = std::min((count - 1) * boost_per_extra, max_boost);
                    hit.score *= (1.0 + boost);
                    modified = true;
                }
            }
        }
    }

    if (modified) {
        std::sort(hits.begin(), hits.end(),
            [](const Hit& a, const Hit& b) { return a.score > b.score; });
    }
}

// ============================================================================
// v4.4: Shard-parallel BM25 — split corpus into tentacles, search in parallel
// ============================================================================

// v8.0: modular shards — no per-shard inverted index!
// shard assignment: doc_idx % shard_count. search filters global posting lists.
// eliminates massive string duplication (56GB → 0 at 50B scale).
struct Shard {
    int shard_id;
    int shard_count;
    size_t doc_count;
};

struct ShardEngine {
    std::vector<Shard> shards;
    int shard_count = 0;
    size_t target_shard_size = 100000;  // ~100K chunks per shard

    void build(const Corpus& corpus) {
        if (corpus.docs.empty()) return;

        size_t N = corpus.docs.size();
        shard_count = std::max(1, (int)((N + target_shard_size - 1) / target_shard_size));
        if (shard_count > 32) shard_count = 32;
        shards.resize(shard_count);

        auto start = std::chrono::high_resolution_clock::now();
        std::cout << "Building " << shard_count << " search shards ("
                  << N << " chunks)..." << std::flush;

        // modular sharding: doc i goes to shard i % shard_count
        // this distributes documents evenly (topics spread across shards)
        for (int s = 0; s < shard_count; s++) {
            shards[s].shard_id = s;
            shards[s].shard_count = shard_count;
            shards[s].doc_count = (N / shard_count) + (s < (int)(N % shard_count) ? 1 : 0);
        }

        auto elapsed = std::chrono::duration<double>(
            std::chrono::high_resolution_clock::now() - start).count();
        std::cout << " Done! (" << elapsed << "s)" << std::endl;
        for (int i = 0; i < shard_count; i++) {
            std::cout << "  shard " << i << ": " << shards[i].doc_count << " chunks (modular)" << std::endl;
        }
    }
};

// v8.0: search a modular shard — filter global posting lists by doc_idx % shard_count
inline std::vector<Hit> search_shard_bm25(const Shard& shard, const Corpus& corpus,
                                           const std::vector<std::string>& query_stems,
                                           int topk) {
    if (shard.doc_count == 0) return {};

    const double k1 = g_config.search.k1;
    const double b = g_config.search.b;
    const size_t N = corpus.docs.size();
    const int sid = shard.shard_id;
    const int scnt = shard.shard_count;

    std::unordered_map<uint32_t, double> scores;  // global doc_idx → score

    for (const std::string& stem : query_stems) {
        auto stk_it = g_stem_to_keywords.find(stem);
        if (stk_it == g_stem_to_keywords.end()) continue;

        // compute GLOBAL df for IDF
        int df = 0;
        for (const std::string& orig_kw : stk_it->second) {
            auto inv_it = corpus.inverted_index.find(orig_kw);
            if (inv_it != corpus.inverted_index.end())
                df += inv_it->second.size();
        }
        if (df == 0) continue;
        double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);

        // score only docs belonging to THIS shard (doc_idx % shard_count == shard_id)
        for (const std::string& orig_kw : stk_it->second) {
            auto inv_it = corpus.inverted_index.find(orig_kw);
            if (inv_it == corpus.inverted_index.end()) continue;

            const auto& postings = inv_it->second;
            auto tf_kw_it = corpus.tf_index.find(orig_kw);

            for (uint32_t global_idx : postings) {
                if ((int)(global_idx % scnt) != sid) continue;  // not our shard

                int doc_len = corpus.docs[global_idx].keyword_ids.size();

                double tf = 1.0;
                if (tf_kw_it != corpus.tf_index.end()) {
                    auto tf_doc_it = tf_kw_it->second.find(global_idx);
                    if (tf_doc_it != tf_kw_it->second.end()) tf = tf_doc_it->second;
                }

                double dl_ratio = (double)doc_len / corpus.avgdl;
                double score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b + b * dl_ratio));
                scores[global_idx] += score;
            }
        }
    }

    // get top-K from this shard
    std::vector<std::pair<uint32_t, double>> scored(scores.begin(), scores.end());
    int result_count = std::min((int)scored.size(), topk);
    if (result_count > 0) {
        std::partial_sort(scored.begin(), scored.begin() + result_count, scored.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    std::vector<Hit> hits;
    for (int i = 0; i < result_count; i++) {
        Hit hit;
        hit.doc_idx = scored[i].first;
        hit.score = scored[i].second;
        hits.push_back(hit);
    }
    return hits;
}

// parallel search across all shards — each shard returns top-16, merge to global top-K
inline std::vector<Hit> search_sharded(const ShardEngine& engine, const Corpus& corpus,
                                        const std::string& query, int topk) {
    if (engine.shard_count <= 0 || corpus.docs.empty()) return {};

    // extract and stem query terms once
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = get_stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return {};

    int per_shard_topk = std::max(topk * 2, 16);

    // search all shards in parallel
    std::vector<std::vector<Hit>> shard_results(engine.shard_count);

    #pragma omp parallel for schedule(dynamic)
    for (int s = 0; s < engine.shard_count; s++) {
        shard_results[s] = search_shard_bm25(engine.shards[s], corpus, query_stems, per_shard_topk);
    }

    // merge all shard results
    std::vector<Hit> all_hits;
    for (const auto& sr : shard_results) {
        all_hits.insert(all_hits.end(), sr.begin(), sr.end());
    }

    // global sort + truncate
    int result_count = std::min((int)all_hits.size(), topk);
    if (result_count > 0) {
        std::partial_sort(all_hits.begin(), all_hits.begin() + result_count, all_hits.end(),
            [](const Hit& a, const Hit& b) { return a.score > b.score; });
    }
    all_hits.resize(result_count);
    return all_hits;
}

// v5.21: Selective hapax filter — remove DF=1 bigrams only from inverted index
// Preserves unigrams (single words) even at DF=1, only prunes compound/bigram terms
// v4.7 bug fix: do NOT prune g_stem_to_keywords, so two-phase coverage
// scoring can still match query stems to doc keywords via the reverse lookup.
inline void apply_hapax_filter(Corpus& corpus) {
    auto start = std::chrono::high_resolution_clock::now();

    size_t before_inv = corpus.inverted_index.size();

    // collect bigram keywords to remove (DF == 1 AND contains underscore = bigram)
    std::vector<std::string> hapax_keywords;
    for (const auto& [kw, doc_list] : corpus.inverted_index) {
        if (doc_list.size() <= 1 && kw.find('_') != std::string::npos) {
            hapax_keywords.push_back(kw);
        }
    }

    // remove from inverted index only — leave g_stem_to_keywords intact
    for (const auto& kw : hapax_keywords) {
        corpus.inverted_index.erase(kw);
    }

    auto elapsed = std::chrono::duration<double>(
        std::chrono::high_resolution_clock::now() - start).count();

    std::cout << "[v5.21] hapax filter (selective/bigrams-only): removed " << hapax_keywords.size()
              << " / " << before_inv << " keywords (DF=1 bigrams) from inverted_index, "
              << "g_stem_to_keywords preserved (" << g_stem_to_keywords.size()
              << " stems) (" << elapsed << "s)" << std::endl;
}

// v4.4: Sharded two-phase search — shard-parallel BM25 + coverage rescoring
inline std::vector<Hit> search_sharded_twophase(const ShardEngine& engine, const Corpus& corpus,
                                                 const std::string& query, int topk) {
    if (engine.shard_count <= 0) return search_bm25_twophase(corpus, query, topk);

    // phase 1: shard-parallel BM25
    int oversample = std::max(topk * 32, 256);
    std::vector<Hit> candidates = search_sharded(engine, corpus, query, oversample);
    if ((int)candidates.size() <= topk) return candidates;

    // phase 2: rescore by keyword coverage (same as two-phase)
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = get_stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return candidates;

    for (auto& hit : candidates) {
        int matches = 0;
        const auto& doc = corpus.docs[hit.doc_idx];
        for (const auto& qstem : query_stems) {
            for (uint32_t kid : doc.keyword_ids) {
                const std::string& kw = corpus.keyword_dict[kid];
                std::string kstem;
                {
                    std::shared_lock<std::shared_mutex> rlock(g_stem_mutex);
                    auto it = g_stem_cache.find(kw);
                    if (it != g_stem_cache.end()) kstem = it->second;
                }
                if (kstem.empty()) kstem = porter::stem(kw);
                if (kstem == qstem) { matches++; break; }
            }
        }
        double coverage = (double)matches / query_stems.size();
        double coverage_boost = 0.8 * coverage * coverage;
        hit.score *= (1.0 + coverage_boost);

        // exact match bonus (25% per match, max 75%)
        int exact_matches = 0;
        for (const auto& raw_term : raw_terms) {
            if (raw_term.find('_') != std::string::npos) continue;
            for (uint32_t kid : doc.keyword_ids) {
                const std::string& kw = corpus.keyword_dict[kid];
                if (kw == raw_term) { exact_matches++; break; }
            }
        }
        double exact_boost = std::min(exact_matches * 0.25, 0.75);
        hit.score *= (1.0 + exact_boost);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const Hit& a, const Hit& b) { return a.score > b.score; });
    candidates.resize(std::min((int)candidates.size(), topk));
    return candidates;
}

// ============================================================================
// v8.0: mmap-based search functions — zero-copy, ~5GB RAM at 50B scale
// These replace the hash-map-based functions when stems.idx is present.
// ============================================================================

// mmap BM25 search for a single shard — uses MmapIndex::lookup_stem() instead of hash maps
inline std::vector<Hit> search_shard_bm25_mmap(const Shard& shard, const MmapIndex& midx,
                                                const Corpus& corpus,
                                                const std::vector<std::string>& query_stems,
                                                int topk) {
    if (shard.doc_count == 0) return {};

    const double k1 = g_config.search.k1;
    const double b_param = g_config.search.b;
    const size_t N = midx.doc_count;
    const double avgdl = midx.avg_dl;
    const int sid = shard.shard_id;
    const int scnt = shard.shard_count;

    std::unordered_map<uint32_t, double> scores;

    for (const std::string& stem : query_stems) {
        auto result = midx.lookup_stem(stem);
        if (!result.postings || result.count == 0) continue;

        int df = result.df;
        double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);

        for (uint32_t pi = 0; pi < result.count; pi++) {
            uint32_t doc_idx = result.postings[pi];
            if ((int)(doc_idx % scnt) != sid) continue;  // not our shard

            // doc length from mmap doc_keywords index
            auto dk = midx.get_doc_keywords(doc_idx);
            int doc_len = dk.count;

            // tf=1 for mmap path (TF stored at keyword level, not per-stem;
            // BM25 with tf=1 is the baseline and matches legacy behavior)
            double tf = 1.0;
            double dl_ratio = (double)doc_len / avgdl;
            double score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b_param + b_param * dl_ratio));
            scores[doc_idx] += score;
        }
    }

    std::vector<std::pair<uint32_t, double>> scored(scores.begin(), scores.end());
    int result_count = std::min((int)scored.size(), topk);
    if (result_count > 0) {
        std::partial_sort(scored.begin(), scored.begin() + result_count, scored.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    std::vector<Hit> hits;
    for (int i = 0; i < result_count; i++) {
        Hit hit;
        hit.doc_idx = scored[i].first;
        hit.score = scored[i].second;
        hits.push_back(hit);
    }
    return hits;
}

// mmap parallel search across all shards
inline std::vector<Hit> search_sharded_mmap(const ShardEngine& engine, const MmapIndex& midx,
                                             const Corpus& corpus,
                                             const std::string& query, int topk) {
    if (engine.shard_count <= 0) return {};

    // extract and stem query terms once
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = porter::stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return {};

    int per_shard_topk = std::max(topk * 2, 16);

    std::vector<std::vector<Hit>> shard_results(engine.shard_count);

    #pragma omp parallel for schedule(dynamic)
    for (int s = 0; s < engine.shard_count; s++) {
        shard_results[s] = search_shard_bm25_mmap(engine.shards[s], midx, corpus, query_stems, per_shard_topk);
    }

    std::vector<Hit> all_hits;
    for (const auto& sr : shard_results) {
        all_hits.insert(all_hits.end(), sr.begin(), sr.end());
    }

    int result_count = std::min((int)all_hits.size(), topk);
    if (result_count > 0) {
        std::partial_sort(all_hits.begin(), all_hits.begin() + result_count, all_hits.end(),
            [](const Hit& a, const Hit& b) { return a.score > b.score; });
    }
    all_hits.resize(result_count);
    return all_hits;
}

// mmap sharded two-phase search: BM25 via mmap + coverage rescoring via mmap doc_keywords
inline std::vector<Hit> search_sharded_twophase_mmap(const ShardEngine& engine, const MmapIndex& midx,
                                                      const Corpus& corpus,
                                                      const std::string& query, int topk) {
    if (engine.shard_count <= 0) return {};

    // phase 1: shard-parallel BM25 via mmap
    int oversample = std::max(topk * 32, 256);
    std::vector<Hit> candidates = search_sharded_mmap(engine, midx, corpus, query, oversample);
    if ((int)candidates.size() <= topk) return candidates;

    // extract query stems and raw terms for Phase 2
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = porter::stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return candidates;

    // phase 2: rescore by keyword coverage using mmap doc_keywords + keywords.idx
    for (auto& hit : candidates) {
        int matches = 0;
        auto dk = midx.get_doc_keywords(hit.doc_idx);

        for (const auto& qstem : query_stems) {
            // check if any keyword in this doc maps to the query stem
            for (uint16_t ki = 0; ki < dk.count; ki++) {
                uint32_t kid = dk.ids[ki];
                uint32_t stem_id = midx.get_keyword_stem(kid);
                if (stem_id == UINT32_MAX) continue;
                // read the stem string from the stem directory to compare
                auto se = midx.read_stem_entry(stem_id);
                const char* stem_str = (const char*)(midx.stem_strings + se.str_offset);
                if (se.str_len == qstem.size() && memcmp(stem_str, qstem.data(), qstem.size()) == 0) {
                    matches++;
                    break;
                }
            }
        }
        double coverage = (double)matches / query_stems.size();
        double coverage_boost = 0.8 * coverage * coverage;
        hit.score *= (1.0 + coverage_boost);

        // exact match bonus using keyword_dict (still in RAM for the slim corpus)
        int exact_matches = 0;
        for (const auto& raw_term : raw_terms) {
            if (raw_term.find('_') != std::string::npos) continue;
            for (uint16_t ki = 0; ki < dk.count; ki++) {
                uint32_t kid = dk.ids[ki];
                if (kid < corpus.keyword_dict.size() && corpus.keyword_dict[kid] == raw_term) {
                    exact_matches++;
                    break;
                }
            }
        }
        double exact_boost = std::min(exact_matches * 0.25, 0.75);
        hit.score *= (1.0 + exact_boost);
    }

    std::sort(candidates.begin(), candidates.end(),
        [](const Hit& a, const Hit& b) { return a.score > b.score; });
    candidates.resize(std::min((int)candidates.size(), topk));
    return candidates;
}

// unified search dispatcher: uses mmap if available, falls back to legacy hash maps
inline std::vector<Hit> search_unified_twophase(const ShardEngine& engine, const Corpus& corpus,
                                                 const std::string& query, int topk) {
    if (g_mmap_idx && g_mmap_idx->loaded) {
        return search_sharded_twophase_mmap(engine, *g_mmap_idx, corpus, query, topk);
    }
    return search_sharded_twophase(engine, corpus, query, topk);
}

// ============================================================================
// v9.1: Multi-Segment Search
// Each segment has independent stems.idx + doc_keywords.idx + keywords.idx.
// BM25 searches each segment independently with per-segment IDF,
// then merges results and applies phase-2 coverage rescoring.
// ============================================================================

struct SegmentIndex {
    int id = -1;
    std::string dir;
    MmapIndex mmap_idx;
    std::vector<std::string> keyword_dict;
    uint64_t doc_count = 0;
    double avg_dl = 0;
    uint64_t doc_offset = 0;  // cumulative offset for global doc IDs
};

struct MultiSegmentIndex {
    std::vector<SegmentIndex> segments;
    uint64_t global_doc_count = 0;
    double global_avg_dl = 0;
    bool loaded = false;
};

// BM25 search within a single segment (no sharding — segment is the shard)
inline std::vector<Hit> search_segment_bm25(const SegmentIndex& seg,
                                              const std::vector<std::string>& query_stems,
                                              int topk) {
    const MmapIndex& midx = seg.mmap_idx;
    if (!midx.loaded || midx.doc_count == 0) return {};

    const double k1 = g_config.search.k1;
    const double b_param = g_config.search.b;
    const size_t N = midx.doc_count;
    const double avgdl = midx.avg_dl;

    std::unordered_map<uint32_t, double> scores;

    for (const std::string& stem : query_stems) {
        auto result = midx.lookup_stem(stem);
        if (!result.postings || result.count == 0) continue;

        int df = result.df;
        double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);

        for (uint32_t pi = 0; pi < result.count; pi++) {
            uint32_t doc_idx = result.postings[pi];

            auto dk = midx.get_doc_keywords(doc_idx);
            int doc_len = dk.count;
            double tf = 1.0;
            double dl_ratio = (double)doc_len / avgdl;
            double score = idf * (tf * (k1 + 1.0)) / (tf + k1 * (1.0 - b_param + b_param * dl_ratio));
            scores[doc_idx] += score;
        }
    }

    std::vector<std::pair<uint32_t, double>> scored(scores.begin(), scores.end());
    int result_count = std::min((int)scored.size(), topk);
    if (result_count > 0) {
        std::partial_sort(scored.begin(), scored.begin() + result_count, scored.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
    }

    std::vector<Hit> hits;
    for (int i = 0; i < result_count; i++) {
        Hit hit;
        // translate local doc_idx to global by adding segment's doc_offset
        hit.doc_idx = scored[i].first + seg.doc_offset;
        hit.score = scored[i].second;
        hits.push_back(hit);
    }
    return hits;
}

// Phase-2 coverage rescoring for a segment hit (using segment's mmap index)
inline void rescore_segment_hit(Hit& hit, const SegmentIndex& seg,
                                 const std::vector<std::string>& query_stems,
                                 const std::vector<std::string>& raw_terms) {
    const MmapIndex& midx = seg.mmap_idx;
    // translate global doc_idx back to segment-local
    uint32_t local_idx = hit.doc_idx - seg.doc_offset;

    int matches = 0;
    auto dk = midx.get_doc_keywords(local_idx);

    for (const auto& qstem : query_stems) {
        for (uint16_t ki = 0; ki < dk.count; ki++) {
            uint32_t kid = dk.ids[ki];
            uint32_t stem_id = midx.get_keyword_stem(kid);
            if (stem_id == UINT32_MAX) continue;
            auto se = midx.read_stem_entry(stem_id);
            const char* stem_str = (const char*)(midx.stem_strings + se.str_offset);
            if (se.str_len == qstem.size() && memcmp(stem_str, qstem.data(), qstem.size()) == 0) {
                matches++;
                break;
            }
        }
    }
    double coverage = (double)matches / query_stems.size();
    double coverage_boost = 0.8 * coverage * coverage;
    hit.score *= (1.0 + coverage_boost);

    // exact match bonus
    int exact_matches = 0;
    for (const auto& raw_term : raw_terms) {
        if (raw_term.find('_') != std::string::npos) continue;
        for (uint16_t ki = 0; ki < dk.count; ki++) {
            uint32_t kid = dk.ids[ki];
            if (kid < seg.keyword_dict.size() && seg.keyword_dict[kid] == raw_term) {
                exact_matches++;
                break;
            }
        }
    }
    double exact_boost = std::min(exact_matches * 0.25, 0.75);
    hit.score *= (1.0 + exact_boost);
}

// Multi-segment two-phase search
// 1) BM25 each segment independently → per-segment top-K
// 2) Merge all segment results
// 3) Phase-2 coverage rescoring on merged candidates → final top-K
inline std::vector<Hit> search_multi_segment(const MultiSegmentIndex& msi,
                                               const std::string& query, int topk) {
    if (!msi.loaded || msi.segments.empty()) return {};

    // extract and stem query terms once
    std::vector<std::string> raw_terms = extract_text_keywords(query);
    std::vector<std::string> query_stems;
    for (const auto& t : raw_terms) {
        std::string stemmed = porter::stem(t);
        if (std::find(query_stems.begin(), query_stems.end(), stemmed) == query_stems.end())
            query_stems.push_back(stemmed);
    }
    if (query_stems.empty()) return {};

    int per_seg_topk = std::max(topk, 8);
    int nseg = (int)msi.segments.size();

    // phase 1: search each segment in parallel
    std::vector<std::vector<Hit>> seg_results(nseg);

    #pragma omp parallel for schedule(dynamic)
    for (int s = 0; s < nseg; s++) {
        seg_results[s] = search_segment_bm25(msi.segments[s], query_stems, per_seg_topk);
    }

    // merge all segment results
    std::vector<std::pair<Hit, int>> all_hits;  // (hit, segment_index)
    for (int s = 0; s < nseg; s++) {
        for (auto& h : seg_results[s]) {
            all_hits.push_back({h, s});
        }
    }

    if (all_hits.empty()) return {};

    // phase 2: coverage rescoring (needs segment info for doc_keywords lookup)
    for (auto& [hit, seg_idx] : all_hits) {
        rescore_segment_hit(hit, msi.segments[seg_idx], query_stems, raw_terms);
    }

    // sort by score, return top-K
    int result_count = std::min((int)all_hits.size(), topk);
    std::partial_sort(all_hits.begin(), all_hits.begin() + result_count, all_hits.end(),
        [](const auto& a, const auto& b) { return a.first.score > b.first.score; });

    std::vector<Hit> results;
    for (int i = 0; i < result_count; i++) {
        results.push_back(all_hits[i].first);
    }
    return results;
}

// Find which segment a global doc_idx belongs to
inline int find_segment_for_doc(const MultiSegmentIndex& msi, uint32_t global_doc_idx) {
    for (int i = (int)msi.segments.size() - 1; i >= 0; i--) {
        if (global_doc_idx >= msi.segments[i].doc_offset) return i;
    }
    return 0;
}
