#pragma once
// OceanEterna v4.3 Entity Extractor
// extracts multi-token named entities and acronyms from text
// entities become high-signal keywords for BM25 search

#include <string>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace oe_entity {

// connector words allowed inside named entities ("United States of America")
static const std::unordered_set<std::string> CONNECTORS = {
    "of", "the", "and", "de", "von", "van", "del", "di", "la", "le",
    "el", "al", "bin", "ibn", "mac", "mc", "st", "for", "in", "on"
};

// common titlecase words that are NOT entities (sentence starters, etc.)
static const std::unordered_set<std::string> FALSE_POSITIVES = {
    "the", "this", "that", "these", "those", "there", "their", "they",
    "what", "when", "where", "which", "while", "who", "whom", "whose",
    "how", "here", "have", "has", "had", "been", "being", "would",
    "could", "should", "will", "shall", "may", "might", "must", "can",
    "some", "many", "most", "much", "very", "also", "just", "only",
    "with", "from", "into", "over", "under", "after", "before",
    "about", "above", "below", "between", "through", "during",
    "each", "every", "both", "either", "neither", "other",
    "then", "than", "but", "and", "for", "not", "are", "was", "were"
};

struct Entity {
    std::string text;       // original text ("Jean-Luc Picard")
    std::string normalized; // index key ("jean_luc_picard")
    int token_count;        // number of tokens
};

inline bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }
inline bool is_lower(char c) { return c >= 'a' && c <= 'z'; }
inline bool is_alpha(char c) { return is_upper(c) || is_lower(c); }
inline bool is_alnum(char c) { return is_alpha(c) || (c >= '0' && c <= '9'); }

// check if a word is titlecase (first char upper, rest has at least one lower)
inline bool is_titlecase(const std::string& word) {
    if (word.empty() || !is_upper(word[0])) return false;
    bool has_lower = false;
    for (size_t i = 1; i < word.size(); i++) {
        if (is_lower(word[i])) has_lower = true;
        // allow hyphens and apostrophes in names
        if (!is_alnum(word[i]) && word[i] != '-' && word[i] != '\'') return false;
    }
    return has_lower || word.size() == 1;
}

// check if a word is ALL CAPS (acronym candidate)
inline bool is_all_caps(const std::string& word) {
    if (word.size() < 2 || word.size() > 10) return false;
    for (char c : word) {
        if (!is_upper(c) && c != '.' && c != '-') return false;
    }
    return true;
}

// normalize entity text to index key: lowercase, spaces/hyphens → underscore
inline std::string normalize_entity(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == ' ' || c == '-' || c == '\'') {
            if (!out.empty() && out.back() != '_') out += '_';
        } else if (is_alnum(c)) {
            out += (char)tolower(c);
        }
    }
    // trim trailing underscore
    while (!out.empty() && out.back() == '_') out.pop_back();
    return out;
}

inline std::string to_lower_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
}

// tokenize text into words (split on whitespace/punctuation, keep hyphens in words)
inline std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string current;
    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (is_alnum(c) || (c == '-' && !current.empty() && i + 1 < text.size() && is_alpha(text[i+1])) ||
            (c == '\'' && !current.empty() && i + 1 < text.size() && is_alpha(text[i+1]))) {
            current += c;
        } else {
            if (!current.empty()) {
                tokens.push_back(current);
                current.clear();
            }
        }
    }
    if (!current.empty()) tokens.push_back(current);
    return tokens;
}

// extract named entities from text
// returns vector of Entity structs with original text, normalized form, and token count
inline std::vector<Entity> extract_entities(const std::string& text) {
    std::vector<Entity> entities;
    std::unordered_set<std::string> seen; // dedup by normalized form

    auto tokens = tokenize(text);
    if (tokens.empty()) return entities;

    // pass 1: multi-token proper nouns (titlecase runs with connectors)
    size_t i = 0;
    while (i < tokens.size()) {
        if (!is_titlecase(tokens[i]) || FALSE_POSITIVES.count(to_lower_str(tokens[i]))) {
            i++;
            continue;
        }

        // start a potential entity run
        std::vector<std::string> run;
        run.push_back(tokens[i]);
        size_t j = i + 1;

        while (j < tokens.size()) {
            std::string lower_tok = to_lower_str(tokens[j]);

            if (is_titlecase(tokens[j]) && !FALSE_POSITIVES.count(lower_tok)) {
                run.push_back(tokens[j]);
                j++;
            } else if (CONNECTORS.count(lower_tok) && j + 1 < tokens.size() &&
                       is_titlecase(tokens[j + 1]) && !FALSE_POSITIVES.count(to_lower_str(tokens[j + 1]))) {
                // connector followed by another titlecase word
                run.push_back(tokens[j]);
                run.push_back(tokens[j + 1]);
                j += 2;
            } else {
                break;
            }
        }

        // only keep multi-token entities (single titlecase words are too noisy)
        if (run.size() >= 2) {
            std::string full_text;
            for (size_t k = 0; k < run.size(); k++) {
                if (k > 0) full_text += " ";
                full_text += run[k];
            }
            std::string norm = normalize_entity(full_text);
            if (norm.size() >= 3 && !seen.count(norm)) {
                seen.insert(norm);
                entities.push_back({full_text, norm, (int)run.size()});
            }
        }

        i = j;
    }

    // pass 2: acronyms (2-10 char all-caps)
    for (const auto& token : tokens) {
        if (is_all_caps(token)) {
            std::string clean;
            for (char c : token) {
                if (is_upper(c)) clean += (char)tolower(c);
            }
            if (clean.size() >= 2 && clean.size() <= 10 && !seen.count(clean)) {
                seen.insert(clean);
                entities.push_back({token, clean, 1});
            }
        }
    }

    return entities;
}

} // namespace oe_entity
