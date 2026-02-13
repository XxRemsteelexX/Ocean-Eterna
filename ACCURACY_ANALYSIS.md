# OceanEterna Accuracy Analysis

**Date:** January 29, 2026

---

## Current Accuracy: 90%

9/10 test questions passed. The one failure was "BBQ class Missoula cost" - even though the answer ($35) is in the indexed data.

---

## Issues Found

### 1. CRITICAL: Mixed Content Chunks

**Problem:** Chunks contain multiple unrelated articles merged together.

**Example:** Chunk `c4500mtokens_DOC_191422` contains:
- Tony Balay BBQ Class article (first 700 chars)
- Mac OS X Carbon Copy Cloner forum post (next 1000 chars)

**Root Cause:** The source files (C4 dataset, etc.) have articles concatenated without clear delimiters. The chunker only looks for `\n\n` paragraph breaks within a 200-char window.

**Impact:**
- Keywords from unrelated content get mixed
- BM25 scores are diluted
- LLM receives confusing context

---

### 2. CRITICAL: Keyword Truncation

**Problem:** Keywords are sorted alphabetically and capped at 30, causing important keywords to be lost.

**Example:** Chunk has 35+ unique keywords but stores only:
```
['2012', '22nd', '240gb', '500gb', 'above', 'and', 'any', 'apple',
 'apron', 'are', 'axboi87', 'balay', 'bbq', 'before', 'beginner',
 'beginners', 'better', 'block', 'boot', 'bootable', 'but', 'calendar',
 'came', 'can', 'carbon', 'ccc', 'champion', 'class', 'clone', 'cloner']
```

**Missing:** 'tony', 'cost', 'missoula', 'lonestar', 'smoke', 'rangers', etc.

**Root Cause:** `extract_text_keywords()` at line 732:
```cpp
sort(keywords.begin(), keywords.end());  // Alphabetical sort
keywords.erase(unique(...));
if (keywords.size() > 30) keywords.resize(30);  // Cap at 30
```

---

### 3. MODERATE: Query Term Extraction

**Problem:** Query terms are extracted by simple whitespace split, not matched to keyword format.

**Current Code (line 343-348):**
```cpp
stringstream ss(query);
string term;
while (ss >> term) {
    transform(term.begin(), term.end(), term.begin(), ::tolower);
    query_terms.push_back(term);
}
```

**Issues:**
- "Tony" → "tony" ✓ (works)
- "is" → "is" (only 2 chars, won't match keywords)
- "Who" → "who" (3 chars, might match)
- Punctuation not stripped: "BBQ?" won't match "bbq"

---

### 4. MINOR: Short Word Exclusion

**Problem:** Words under 3 characters are excluded from keywords.

**Excluded:** "is", "a", "of", "to", "in", "on", "it", "be", etc.

**Impact:** Queries like "What is X" lose "is" as a search term.

**Note:** This is actually good - these are stopwords that would pollute results.

---

### 5. MINOR: Inconsistent Keyword Limits

**Problem:** Original Gutenberg chunks have up to 40 keywords, uploaded files capped at 30.

| Source | Max Keywords | Avg Keywords |
|--------|--------------|--------------|
| Original Gutenberg | 40 | 36.6 |
| Uploaded files | 30 | 30.0 |

---

## What's Working Correctly

### 1. ✅ Paragraph Boundary Detection
The chunker does look for `\n\n` breaks:
```cpp
// Look for paragraph break in small window
for (size_t i = end_pos; i < search_end; i++) {
    if (content[i] == '\n' && i+1 < content_len && content[i+1] == '\n') {
        end_pos = i + 2;
        break;
    }
}
```
**Limitation:** Only searches 200 chars forward, not backward.

### 2. ✅ Sentence Boundary Fallback
If no paragraph found, looks for sentence endings:
```cpp
if ((content[i-1] == '.' || content[i-1] == '!' || content[i-1] == '?') &&
    (content[i] == ' ' || content[i] == '\n')) {
    end_pos = i;
    break;
}
```

### 3. ✅ BM25 Scoring
Standard BM25 with k1=1.5, b=0.75:
```cpp
double idf = log((N - df + 0.5) / (df + 0.5) + 1.0);
int doc_len = doc.keywords.size();
double norm = k1 * (1.0 - b + b * doc_len / corpus.avgdl);
score += idf * (tf * (k1 + 1.0)) / (tf + norm);
```

### 4. ✅ Parallel Search
OpenMP parallel for loop over all documents.

### 5. ✅ UTF-8 Sanitization
`make_json_safe()` cleans invalid UTF-8.

### 6. ✅ Conversation Boosting
Conversation chunks get 1.5x score boost.

---

## Chapter Guide Status

**Current State:** Minimal
```json
{
  "chunks": {
    "by_type": {
      "CHAT": 1,
      "DOC": 5010658
    }
  },
  "conversations": {
    "count": 1,
    "summaries": []
  },
  "features": {"count": 0},
  "fixes": {"count": 0}
}
```

**Not Implemented:**
- Conversation summaries
- Feature tracking
- Fix tracking
- Code file indexing
- Cross-references between chunks

---

## Recommended Fixes (Priority Order)

### P1: Fix Keyword Extraction
**Change:** Keep most frequent keywords instead of alphabetically first.

```cpp
// Current (broken):
sort(keywords.begin(), keywords.end());
if (keywords.size() > 30) keywords.resize(30);

// Fixed: Keep most frequent words
unordered_map<string, int> freq;
for (const string& kw : keywords) freq[kw]++;
vector<pair<string, int>> sorted_by_freq(freq.begin(), freq.end());
sort(sorted_by_freq.begin(), sorted_by_freq.end(),
     [](auto& a, auto& b) { return a.second > b.second; });
keywords.clear();
for (int i = 0; i < min(30, (int)sorted_by_freq.size()); i++) {
    keywords.push_back(sorted_by_freq[i].first);
}
```

**Alternative:** Increase limit to 50 or 100 keywords.

---

### P2: Improve Chunk Boundaries
**Change:** Look for stronger article delimiters.

Detect article boundaries:
- Multiple blank lines (`\n\n\n`)
- Title patterns (ALL CAPS lines, "Chapter X", etc.)
- Date patterns at start of article
- URL patterns indicating new article

```cpp
// Better boundary detection
bool is_article_boundary(const string& content, size_t pos) {
    // Check for 3+ newlines
    if (pos + 2 < content.length() &&
        content[pos] == '\n' && content[pos+1] == '\n' && content[pos+2] == '\n')
        return true;

    // Check for title-like pattern after newlines
    if (content[pos] == '\n' && content[pos+1] == '\n') {
        size_t next_line_end = content.find('\n', pos + 2);
        string next_line = content.substr(pos + 2, next_line_end - pos - 2);
        // All caps line likely a title
        if (all_of(next_line.begin(), next_line.end(),
            [](char c) { return isupper(c) || isspace(c) || ispunct(c); }))
            return true;
    }
    return false;
}
```

---

### P3: Query Term Processing
**Change:** Apply same keyword extraction to queries.

```cpp
vector<string> query_terms = extract_text_keywords(query);
```

This ensures query terms match keyword format.

---

### P4: TF-IDF Weighting for Keywords
**Change:** Store term frequencies, not just presence.

```cpp
// In manifest, store:
"keywords": {"tony": 3, "balay": 2, "bbq": 5, ...}
// Instead of:
"keywords": ["tony", "balay", "bbq", ...]
```

This gives better BM25 scores for repeated important terms.

---

### P5: Increase Keyword Limit
**Change:** Increase from 30 to 100.

```cpp
if (keywords.size() > 100) keywords.resize(100);
```

Storage impact: ~70 bytes more per chunk × 5M chunks = 350MB more.

---

## Testing Plan

After each fix, run this test suite:

```python
questions = [
    ("Who is Tony Balay", ["bbq", "champion", "lonestar"]),
    ("BBQ class cost dollars", ["35", "dollar"]),
    ("BBQ class date September", ["22", "thursday", "september"]),
    ("Carbon Copy Cloner Mac", ["clone", "backup", "ssd"]),
    ("Denver school bond amount", ["572", "million"]),
]
```

Target: 100% accuracy on factual questions where answer is in indexed data.

---

## Statistics

| Metric | Value |
|--------|-------|
| Total Chunks | 5,058,082 |
| Chunks with <5 keywords | 876 (0.02%) |
| Chunks with 30 keywords (max) | Many thousands |
| Avg keywords per chunk | 30-36 |
| Avg chunk size | 1,870 bytes |
| Chunks with "tony" keyword | 2 (only conversations!) |
| Chunks with "balay" keyword | 18 |
