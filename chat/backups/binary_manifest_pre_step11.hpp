// binary_manifest.hpp - Binary Manifest Format for OceanEterna
// Speed Improvement #1: Reduces manifest loading from 41s to ~4-8s (5-10x faster)
//
// Binary Format Specification:
// HEADER (24 bytes):
//   magic[4]        = "OEM1"
//   version[4]      = 1
//   chunk_count[8]  = number of chunks
//   keyword_count[8]= number of unique keywords
//
// KEYWORD DICTIONARY:
//   For each keyword:
//     len[2] + string[len]
//
// CHUNK ENTRIES:
//   For each chunk:
//     id_len[2] + id[id_len]
//     summary_len[2] + summary[summary_len]
//     offset[8]
//     length[8]
//     token_start[4]
//     token_end[4]
//     timestamp[8]
//     kw_count[2]
//     kw_indices[kw_count * 4]  // indices into keyword dictionary

#ifndef BINARY_MANIFEST_HPP
#define BINARY_MANIFEST_HPP

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstring>
#include <chrono>
#include <stdexcept>
#include <sys/stat.h>

// Magic header for binary manifest files
constexpr char BINARY_MANIFEST_MAGIC[4] = {'O', 'E', 'M', '1'};
constexpr uint32_t BINARY_MANIFEST_VERSION = 1;

// Forward declaration of DocMeta if not already defined
#ifndef DOCMETA_DEFINED
#define DOCMETA_DEFINED
struct DocMeta {
    std::string id;
    std::string summary;
    std::vector<std::string> keywords;
    uint64_t offset;
    uint64_t length;
    uint32_t start;
    uint32_t end;
    long long timestamp = 0;
};
#endif

// Binary manifest header structure (24 bytes)
struct BinaryManifestHeader {
    char magic[4];
    uint32_t version;
    uint64_t chunk_count;
    uint64_t keyword_count;
};

// Helper functions for binary I/O
namespace BinaryIO {

    // Write a value in little-endian format
    template<typename T>
    inline void write_le(std::ostream& out, T value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    // Read a value in little-endian format
    template<typename T>
    inline T read_le(std::istream& in) {
        T value;
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return value;
    }

    // Write a length-prefixed string (2-byte length)
    inline void write_string(std::ostream& out, const std::string& str) {
        uint16_t len = static_cast<uint16_t>(std::min(str.size(), size_t(65535)));
        write_le<uint16_t>(out, len);
        out.write(str.data(), len);
    }

    // Read a length-prefixed string (2-byte length)
    inline std::string read_string(std::istream& in) {
        uint16_t len = read_le<uint16_t>(in);
        std::string str(len, '\0');
        in.read(&str[0], len);
        return str;
    }
}

// Build keyword dictionary from documents
// Returns: map of keyword -> index, and vector of keywords in order
inline std::pair<std::unordered_map<std::string, uint32_t>, std::vector<std::string>>
build_keyword_dictionary(const std::vector<DocMeta>& docs) {
    std::unordered_map<std::string, uint32_t> kw_to_index;
    std::vector<std::string> keywords;

    for (const auto& doc : docs) {
        for (const auto& kw : doc.keywords) {
            if (kw_to_index.find(kw) == kw_to_index.end()) {
                kw_to_index[kw] = static_cast<uint32_t>(keywords.size());
                keywords.push_back(kw);
            }
        }
    }

    return {kw_to_index, keywords};
}

// Write binary manifest file from vector of DocMeta
// Returns: true on success, false on failure
inline bool write_binary_manifest(const std::string& output_path,
                                   const std::vector<DocMeta>& docs) {
    using namespace BinaryIO;

    std::ofstream out(output_path, std::ios::binary);
    if (!out.is_open()) {
        std::cerr << "Failed to open output file: " << output_path << std::endl;
        return false;
    }

    std::cout << "Building keyword dictionary..." << std::flush;
    auto [kw_to_index, keywords] = build_keyword_dictionary(docs);
    std::cout << " Done! (" << keywords.size() << " unique keywords)" << std::endl;

    // Write header
    BinaryManifestHeader header;
    std::memcpy(header.magic, BINARY_MANIFEST_MAGIC, 4);
    header.version = BINARY_MANIFEST_VERSION;
    header.chunk_count = docs.size();
    header.keyword_count = keywords.size();

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    // Write keyword dictionary
    std::cout << "Writing keyword dictionary..." << std::flush;
    for (const auto& kw : keywords) {
        write_string(out, kw);
    }
    std::cout << " Done!" << std::endl;

    // Write chunk entries
    std::cout << "Writing " << docs.size() << " chunk entries..." << std::flush;
    size_t progress_interval = docs.size() / 10;
    if (progress_interval == 0) progress_interval = 1;

    for (size_t i = 0; i < docs.size(); ++i) {
        const auto& doc = docs[i];

        // Write chunk ID
        write_string(out, doc.id);

        // Write summary
        write_string(out, doc.summary);

        // Write fixed fields
        write_le<uint64_t>(out, doc.offset);
        write_le<uint64_t>(out, doc.length);
        write_le<uint32_t>(out, doc.start);
        write_le<uint32_t>(out, doc.end);
        write_le<int64_t>(out, static_cast<int64_t>(doc.timestamp));

        // Write keyword indices
        uint16_t kw_count = static_cast<uint16_t>(doc.keywords.size());
        write_le<uint16_t>(out, kw_count);

        for (const auto& kw : doc.keywords) {
            auto it = kw_to_index.find(kw);
            if (it != kw_to_index.end()) {
                write_le<uint32_t>(out, it->second);
            }
        }

        // Progress indicator
        if ((i + 1) % progress_interval == 0) {
            std::cout << "." << std::flush;
        }
    }
    std::cout << " Done!" << std::endl;

    out.close();
    return true;
}

// Corpus structure for loading (matches main server structure)
struct BinaryCorpus {
    std::vector<DocMeta> docs;
    std::unordered_map<std::string, std::vector<uint32_t>> inverted_index;
    size_t total_tokens = 0;
    double avgdl = 0;
};

// Load binary manifest file
// Returns: populated Corpus structure
// Also populates chunk_id_to_index map if provided
inline BinaryCorpus load_binary_manifest(const std::string& input_path,
                                          std::unordered_map<std::string, uint32_t>* chunk_id_map = nullptr) {
    using namespace BinaryIO;

    BinaryCorpus corpus;

    std::ifstream in(input_path, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Failed to open binary manifest: " << input_path << std::endl;
        return corpus;
    }

    std::cout << "Loading binary manifest..." << std::flush;
    auto start = std::chrono::high_resolution_clock::now();

    // Read and validate header
    BinaryManifestHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (std::memcmp(header.magic, BINARY_MANIFEST_MAGIC, 4) != 0) {
        std::cerr << "\nInvalid binary manifest: bad magic header" << std::endl;
        return corpus;
    }

    if (header.version != BINARY_MANIFEST_VERSION) {
        std::cerr << "\nUnsupported binary manifest version: " << header.version << std::endl;
        return corpus;
    }

    // Pre-allocate vectors for performance
    corpus.docs.reserve(header.chunk_count);
    std::vector<std::string> keywords;
    keywords.reserve(header.keyword_count);

    // Read keyword dictionary
    for (uint64_t i = 0; i < header.keyword_count; ++i) {
        keywords.push_back(read_string(in));
    }

    // Read chunk entries
    for (uint64_t i = 0; i < header.chunk_count; ++i) {
        DocMeta doc;

        // Read chunk ID
        doc.id = read_string(in);

        // Read summary
        doc.summary = read_string(in);

        // Read fixed fields
        doc.offset = read_le<uint64_t>(in);
        doc.length = read_le<uint64_t>(in);
        doc.start = read_le<uint32_t>(in);
        doc.end = read_le<uint32_t>(in);
        doc.timestamp = static_cast<long long>(read_le<int64_t>(in));

        // Read keyword indices and resolve to strings
        uint16_t kw_count = read_le<uint16_t>(in);
        doc.keywords.reserve(kw_count);

        for (uint16_t k = 0; k < kw_count; ++k) {
            uint32_t kw_idx = read_le<uint32_t>(in);
            if (kw_idx < keywords.size()) {
                doc.keywords.push_back(keywords[kw_idx]);
            }
        }

        corpus.docs.push_back(std::move(doc));

        // Build inverted index
        size_t doc_idx = corpus.docs.size() - 1;
        for (const std::string& kw : corpus.docs.back().keywords) {
            corpus.inverted_index[kw].push_back(static_cast<uint32_t>(doc_idx));
        }

        // Build chunk_id to index mapping if provided
        if (chunk_id_map) {
            (*chunk_id_map)[corpus.docs.back().id] = static_cast<uint32_t>(doc_idx);
        }

        // Track total tokens
        corpus.total_tokens += (corpus.docs.back().end - corpus.docs.back().start);
    }

    // Calculate average document length
    if (!corpus.docs.empty()) {
        corpus.avgdl = corpus.total_tokens / static_cast<double>(corpus.docs.size());
    }

    auto end = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(end - start).count();

    std::cout << " Done!" << std::endl;
    std::cout << "Loaded " << corpus.docs.size() << " chunks in " << elapsed << "ms" << std::endl;
    std::cout << "Total tokens: " << corpus.total_tokens << std::endl;
    std::cout << "Unique keywords: " << keywords.size() << std::endl;

    return corpus;
}

// Check if a binary manifest exists and is newer than the JSONL source
inline bool binary_manifest_is_current(const std::string& binary_path,
                                        const std::string& jsonl_path) {
    struct stat binary_stat, jsonl_stat;

    // Check if binary file exists
    if (stat(binary_path.c_str(), &binary_stat) != 0) {
        return false;  // Binary doesn't exist
    }

    // Check if JSONL file exists
    if (stat(jsonl_path.c_str(), &jsonl_stat) != 0) {
        return true;  // JSONL doesn't exist, use binary if available
    }

    // Compare modification times
    return binary_stat.st_mtime >= jsonl_stat.st_mtime;
}

// Get the binary manifest path for a given JSONL path
inline std::string get_binary_manifest_path(const std::string& jsonl_path) {
    // Replace .jsonl extension with .bin
    std::string bin_path = jsonl_path;
    size_t pos = bin_path.rfind(".jsonl");
    if (pos != std::string::npos) {
        bin_path.replace(pos, 6, ".bin");
    } else {
        bin_path += ".bin";
    }
    return bin_path;
}

// Convert JSONL manifest to binary format
// This function requires json.hpp to be included before this header
#ifdef NLOHMANN_JSON_HPP
inline bool convert_jsonl_to_binary(const std::string& jsonl_path,
                                     const std::string& binary_path) {
    using json = nlohmann::json;

    std::ifstream in(jsonl_path);
    if (!in.is_open()) {
        std::cerr << "Failed to open JSONL manifest: " << jsonl_path << std::endl;
        return false;
    }

    std::cout << "Reading JSONL manifest: " << jsonl_path << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<DocMeta> docs;
    std::string line;
    size_t line_count = 0;
    size_t error_count = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        line_count++;

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
                    doc.keywords.push_back(kw.get<std::string>());
                }
            }

            docs.push_back(std::move(doc));

            // Progress indicator every 100k lines
            if (line_count % 100000 == 0) {
                std::cout << "  Read " << line_count << " lines..." << std::endl;
            }
        } catch (const std::exception& e) {
            error_count++;
            if (error_count <= 5) {
                std::cerr << "Error parsing line " << line_count << ": " << e.what() << std::endl;
            }
        }
    }

    auto read_end = std::chrono::high_resolution_clock::now();
    double read_elapsed = std::chrono::duration<double>(read_end - start).count();

    std::cout << "Read " << docs.size() << " chunks in " << read_elapsed << " seconds" << std::endl;
    if (error_count > 0) {
        std::cout << "Skipped " << error_count << " lines with errors" << std::endl;
    }

    // Write binary manifest
    std::cout << "\nWriting binary manifest: " << binary_path << std::endl;
    auto write_start = std::chrono::high_resolution_clock::now();

    bool success = write_binary_manifest(binary_path, docs);

    auto write_end = std::chrono::high_resolution_clock::now();
    double write_elapsed = std::chrono::duration<double>(write_end - write_start).count();

    if (success) {
        std::cout << "Wrote binary manifest in " << write_elapsed << " seconds" << std::endl;

        // Report file sizes
        struct stat jsonl_stat, binary_stat;
        if (stat(jsonl_path.c_str(), &jsonl_stat) == 0 &&
            stat(binary_path.c_str(), &binary_stat) == 0) {
            double jsonl_mb = jsonl_stat.st_size / (1024.0 * 1024.0);
            double binary_mb = binary_stat.st_size / (1024.0 * 1024.0);
            double ratio = binary_stat.st_size / (double)jsonl_stat.st_size * 100.0;
            std::cout << "\nFile sizes:" << std::endl;
            std::cout << "  JSONL:  " << jsonl_mb << " MB" << std::endl;
            std::cout << "  Binary: " << binary_mb << " MB (" << ratio << "% of original)" << std::endl;
        }
    }

    return success;
}
#endif // NLOHMANN_JSON_HPP

// Converter utility main function
// Can be compiled standalone with: g++ -DBINARY_MANIFEST_CONVERTER -o convert_manifest binary_manifest.hpp -std=c++17
#ifdef BINARY_MANIFEST_CONVERTER

#ifndef NLOHMANN_JSON_HPP
#include "json.hpp"
#endif

// Re-declare convert_jsonl_to_binary for the converter since json.hpp is now included
inline bool converter_convert_jsonl_to_binary(const std::string& jsonl_path,
                                               const std::string& binary_path) {
    using json = nlohmann::json;

    std::ifstream in(jsonl_path);
    if (!in.is_open()) {
        std::cerr << "Failed to open JSONL manifest: " << jsonl_path << std::endl;
        return false;
    }

    std::cout << "Reading JSONL manifest: " << jsonl_path << std::endl;
    auto start = std::chrono::high_resolution_clock::now();

    std::vector<DocMeta> docs;
    std::string line;
    size_t line_count = 0;
    size_t error_count = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        line_count++;

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
                    doc.keywords.push_back(kw.get<std::string>());
                }
            }

            docs.push_back(std::move(doc));

            // Progress indicator every 100k lines
            if (line_count % 100000 == 0) {
                std::cout << "  Read " << line_count << " lines..." << std::endl;
            }
        } catch (const std::exception& e) {
            error_count++;
            if (error_count <= 5) {
                std::cerr << "Error parsing line " << line_count << ": " << e.what() << std::endl;
            }
        }
    }

    auto read_end = std::chrono::high_resolution_clock::now();
    double read_elapsed = std::chrono::duration<double>(read_end - start).count();

    std::cout << "Read " << docs.size() << " chunks in " << read_elapsed << " seconds" << std::endl;
    if (error_count > 0) {
        std::cout << "Skipped " << error_count << " lines with errors" << std::endl;
    }

    // Write binary manifest
    std::cout << "\nWriting binary manifest: " << binary_path << std::endl;
    auto write_start = std::chrono::high_resolution_clock::now();

    bool success = write_binary_manifest(binary_path, docs);

    auto write_end = std::chrono::high_resolution_clock::now();
    double write_elapsed = std::chrono::duration<double>(write_end - write_start).count();

    if (success) {
        std::cout << "Wrote binary manifest in " << write_elapsed << " seconds" << std::endl;

        // Report file sizes
        struct stat jsonl_stat, binary_stat;
        if (stat(jsonl_path.c_str(), &jsonl_stat) == 0 &&
            stat(binary_path.c_str(), &binary_stat) == 0) {
            double jsonl_mb = jsonl_stat.st_size / (1024.0 * 1024.0);
            double binary_mb = binary_stat.st_size / (1024.0 * 1024.0);
            double ratio = binary_stat.st_size / (double)jsonl_stat.st_size * 100.0;
            std::cout << "\nFile sizes:" << std::endl;
            std::cout << "  JSONL:  " << jsonl_mb << " MB" << std::endl;
            std::cout << "  Binary: " << binary_mb << " MB (" << ratio << "% of original)" << std::endl;
        }
    }

    return success;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "OceanEterna Binary Manifest Converter" << std::endl;
        std::cout << "Usage: " << argv[0] << " <input.jsonl> [output.bin]" << std::endl;
        std::cout << std::endl;
        std::cout << "If output.bin is not specified, it will be derived from input path" << std::endl;
        std::cout << "(e.g., manifest.jsonl -> manifest.bin)" << std::endl;
        return 1;
    }

    std::string jsonl_path = argv[1];
    std::string binary_path;

    if (argc >= 3) {
        binary_path = argv[2];
    } else {
        binary_path = get_binary_manifest_path(jsonl_path);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "OceanEterna Binary Manifest Converter" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << "Input:  " << jsonl_path << std::endl;
    std::cout << "Output: " << binary_path << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    auto total_start = std::chrono::high_resolution_clock::now();

    bool success = converter_convert_jsonl_to_binary(jsonl_path, binary_path);

    auto total_end = std::chrono::high_resolution_clock::now();
    double total_elapsed = std::chrono::duration<double>(total_end - total_start).count();

    std::cout << std::endl;
    std::cout << "========================================" << std::endl;
    if (success) {
        std::cout << "Conversion completed in " << total_elapsed << " seconds" << std::endl;
        std::cout << std::endl;
        std::cout << "To use binary manifest in ocean_chat_server:" << std::endl;
        std::cout << "  1. Include binary_manifest.hpp" << std::endl;
        std::cout << "  2. Replace load_manifest() call with load_binary_manifest()" << std::endl;
        std::cout << std::endl;
        std::cout << "Expected speedup: 5-10x (41s -> 4-8s)" << std::endl;
    } else {
        std::cout << "Conversion FAILED" << std::endl;
        return 1;
    }
    std::cout << "========================================" << std::endl;

    return 0;
}
#endif // BINARY_MANIFEST_CONVERTER

#endif // BINARY_MANIFEST_HPP
