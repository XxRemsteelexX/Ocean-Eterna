// mmap_format_test.cpp — tests the v9.1 mmap index split-layout format
// verifies [doc_ids...][tfs...] posting layout round-trips correctly

#include <iostream>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <cstdio>
#include <algorithm>
#include <sys/stat.h>

// we reuse fnv1a_64 from mmap_index.hpp but test the format at the byte level
static inline uint64_t fnv1a_64(const char* data, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// minimal stems.idx writer for testing
// format:
//   header (32 bytes): magic[4] + pad[4] + stem_count[8] + postings_start[8] + avg_dl[8]
//   directory: stem_count * 28 bytes per entry
//     [0:8] hash, [8:12] str_offset, [12:14] str_len, [14:18] posting_offset, [18:22] posting_count, [22:26] df, [26:28] pad
//   stem strings (concatenated)
//   postings: per stem: [doc_ids (count * 4 bytes)][tfs (count * 2 bytes)]

struct TestStem {
    std::string text;
    std::vector<uint32_t> doc_ids;
    std::vector<uint16_t> tfs;
    uint32_t df;
};

static bool write_test_stems_idx(const std::string& path, const std::vector<TestStem>& stems, double avg_dl) {
    // sort stems by hash (binary search requirement)
    struct StemWithHash {
        size_t orig_idx;
        uint64_t hash;
    };
    std::vector<StemWithHash> sorted;
    for (size_t i = 0; i < stems.size(); i++) {
        sorted.push_back({i, fnv1a_64(stems[i].text.data(), stems[i].text.size())});
    }
    std::sort(sorted.begin(), sorted.end(), [](const StemWithHash& a, const StemWithHash& b) {
        return a.hash < b.hash;
    });

    // build stem strings blob
    std::string strings_blob;
    std::vector<uint32_t> str_offsets(stems.size());
    for (size_t si = 0; si < sorted.size(); si++) {
        const auto& s = stems[sorted[si].orig_idx];
        str_offsets[si] = (uint32_t)strings_blob.size();
        strings_blob += s.text;
    }

    // build postings blob
    std::vector<uint8_t> postings_blob;
    std::vector<uint32_t> posting_offsets(sorted.size());
    for (size_t si = 0; si < sorted.size(); si++) {
        const auto& s = stems[sorted[si].orig_idx];
        posting_offsets[si] = (uint32_t)postings_blob.size();
        // write doc_ids first
        for (auto did : s.doc_ids) {
            uint8_t buf[4]; memcpy(buf, &did, 4);
            postings_blob.insert(postings_blob.end(), buf, buf + 4);
        }
        // then tfs
        for (auto tf : s.tfs) {
            uint8_t buf[2]; memcpy(buf, &tf, 2);
            postings_blob.insert(postings_blob.end(), buf, buf + 2);
        }
    }

    uint64_t stem_count = sorted.size();
    uint32_t dir_size = (uint32_t)(stem_count * 28);
    uint32_t strings_size = (uint32_t)strings_blob.size();
    uint64_t postings_start = 32 + dir_size + strings_size;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    // header (32 bytes)
    out.write("OES1", 4);
    uint32_t pad4 = 0; out.write((char*)&pad4, 4);
    out.write((char*)&stem_count, 8);
    out.write((char*)&postings_start, 8);
    uint64_t avg_dl_bits; memcpy(&avg_dl_bits, &avg_dl, 8);
    out.write((char*)&avg_dl_bits, 8);

    // directory entries (28 bytes each)
    for (size_t si = 0; si < sorted.size(); si++) {
        const auto& s = stems[sorted[si].orig_idx];
        uint64_t hash = sorted[si].hash;
        uint32_t str_off = str_offsets[si];
        uint16_t str_len = (uint16_t)s.text.size();
        uint32_t post_off = posting_offsets[si];
        uint32_t post_cnt = (uint32_t)s.doc_ids.size();
        uint32_t df = s.df;
        uint16_t pad2 = 0;

        out.write((char*)&hash, 8);
        out.write((char*)&str_off, 4);
        out.write((char*)&str_len, 2);
        out.write((char*)&post_off, 4);
        out.write((char*)&post_cnt, 4);
        out.write((char*)&df, 4);
        out.write((char*)&pad2, 2);
    }

    // stem strings
    out.write(strings_blob.data(), strings_blob.size());

    // postings
    out.write((char*)postings_blob.data(), postings_blob.size());

    return out.good();
}

// minimal reader that mirrors MmapIndex logic without mmap (reads file into memory)
struct TestReader {
    std::vector<uint8_t> buf;
    uint64_t stem_count = 0;
    uint64_t postings_start = 0;
    double avg_dl = 0;
    const uint8_t* stem_dir = nullptr;
    const uint8_t* stem_strings = nullptr;
    const uint8_t* postings = nullptr;

    bool load(const std::string& path) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        f.seekg(0, std::ios::end);
        size_t sz = f.tellg();
        f.seekg(0);
        buf.resize(sz);
        f.read((char*)buf.data(), sz);
        if (!f) return false;

        const uint8_t* p = buf.data();
        if (memcmp(p, "OES1", 4) != 0) return false;
        memcpy(&stem_count, p + 8, 8);
        memcpy(&postings_start, p + 16, 8);
        uint64_t avg_dl_bits; memcpy(&avg_dl_bits, p + 24, 8);
        memcpy(&avg_dl, &avg_dl_bits, 8);

        stem_dir = p + 32;
        stem_strings = stem_dir + stem_count * 28;
        postings = p + postings_start;
        return true;
    }

    struct StemEntry {
        uint64_t hash;
        uint32_t str_offset;
        uint16_t str_len;
        uint32_t posting_offset;
        uint32_t posting_count;
        uint32_t df;
    };

    StemEntry read_entry(uint64_t idx) const {
        const uint8_t* p = stem_dir + idx * 28;
        StemEntry e;
        memcpy(&e.hash, p, 8);
        memcpy(&e.str_offset, p + 8, 4);
        memcpy(&e.str_len, p + 12, 2);
        memcpy(&e.posting_offset, p + 14, 4);
        memcpy(&e.posting_count, p + 18, 4);
        memcpy(&e.df, p + 22, 4);
        return e;
    }

    // binary search by hash, then verify string
    bool lookup(const std::string& stem, std::vector<uint32_t>& out_doc_ids, std::vector<uint16_t>& out_tfs, uint32_t& out_df) const {
        uint64_t target = fnv1a_64(stem.data(), stem.size());
        uint64_t lo = 0, hi = stem_count;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            auto e = read_entry(mid);
            if (e.hash < target) lo = mid + 1;
            else if (e.hash > target) hi = mid;
            else {
                // verify string
                const char* s = (const char*)(stem_strings + e.str_offset);
                if (e.str_len == stem.size() && memcmp(s, stem.data(), stem.size()) == 0) {
                    out_df = e.df;
                    const uint32_t* doc_ptr = (const uint32_t*)(postings + e.posting_offset);
                    const uint16_t* tf_ptr = (const uint16_t*)(postings + e.posting_offset + e.posting_count * sizeof(uint32_t));
                    out_doc_ids.assign(doc_ptr, doc_ptr + e.posting_count);
                    out_tfs.assign(tf_ptr, tf_ptr + e.posting_count);
                    return true;
                }
                // scan both directions for hash collisions
                for (uint64_t i = mid; i > 0; ) {
                    i--;
                    auto e2 = read_entry(i);
                    if (e2.hash != target) break;
                    const char* s2 = (const char*)(stem_strings + e2.str_offset);
                    if (e2.str_len == stem.size() && memcmp(s2, stem.data(), stem.size()) == 0) {
                        out_df = e2.df;
                        const uint32_t* dp = (const uint32_t*)(postings + e2.posting_offset);
                        const uint16_t* tp = (const uint16_t*)(postings + e2.posting_offset + e2.posting_count * sizeof(uint32_t));
                        out_doc_ids.assign(dp, dp + e2.posting_count);
                        out_tfs.assign(tp, tp + e2.posting_count);
                        return true;
                    }
                }
                for (uint64_t i = mid + 1; i < stem_count; i++) {
                    auto e2 = read_entry(i);
                    if (e2.hash != target) break;
                    const char* s2 = (const char*)(stem_strings + e2.str_offset);
                    if (e2.str_len == stem.size() && memcmp(s2, stem.data(), stem.size()) == 0) {
                        out_df = e2.df;
                        const uint32_t* dp = (const uint32_t*)(postings + e2.posting_offset);
                        const uint16_t* tp = (const uint16_t*)(postings + e2.posting_offset + e2.posting_count * sizeof(uint32_t));
                        out_doc_ids.assign(dp, dp + e2.posting_count);
                        out_tfs.assign(tp, tp + e2.posting_count);
                        return true;
                    }
                }
                return false;
            }
        }
        return false;
    }
};

int main() {
    int pass = 0, fail = 0;
    const std::string test_path = "/tmp/oe_test_stems.idx";

    // --- test data: 3 stems with known doc_ids and tfs ---
    std::vector<TestStem> stems = {
        {"hello",  {10, 42, 99},     {3, 1, 7},     3},
        {"world",  {10, 55},         {2, 5},         2},
        {"ocean",  {1, 2, 3, 4, 5},  {1, 1, 2, 3, 1}, 5},
    };
    double avg_dl = 150.5;

    // write the test index
    if (!write_test_stems_idx(test_path, stems, avg_dl)) {
        std::cerr << "FAIL: could not write test stems.idx" << std::endl;
        return 1;
    }
    std::cout << "wrote test stems.idx to " << test_path << std::endl;

    // load it back
    TestReader reader;
    if (!reader.load(test_path)) {
        std::cerr << "FAIL: could not load test stems.idx" << std::endl;
        return 1;
    }

    // test 1: header values
    auto check = [&](const std::string& name, bool cond) {
        if (cond) { std::cout << "PASS: " << name << std::endl; pass++; }
        else      { std::cout << "FAIL: " << name << std::endl; fail++; }
    };

    check("stem_count == 3", reader.stem_count == 3);
    check("avg_dl == 150.5", reader.avg_dl == 150.5);

    // test 2: lookup each stem and verify doc_ids + tfs
    for (const auto& s : stems) {
        std::vector<uint32_t> doc_ids;
        std::vector<uint16_t> tfs;
        uint32_t df = 0;
        bool found = reader.lookup(s.text, doc_ids, tfs, df);

        check("found stem \"" + s.text + "\"", found);
        if (found) {
            check("\"" + s.text + "\" df == " + std::to_string(s.df), df == s.df);
            check("\"" + s.text + "\" posting count == " + std::to_string(s.doc_ids.size()),
                  doc_ids.size() == s.doc_ids.size());

            bool ids_match = (doc_ids == s.doc_ids);
            check("\"" + s.text + "\" doc_ids match", ids_match);

            bool tfs_match = (tfs == s.tfs);
            check("\"" + s.text + "\" tfs match (split layout)", tfs_match);

            if (!ids_match) {
                std::cout << "  expected doc_ids:";
                for (auto d : s.doc_ids) std::cout << " " << d;
                std::cout << "\n  got:";
                for (auto d : doc_ids) std::cout << " " << d;
                std::cout << std::endl;
            }
            if (!tfs_match) {
                std::cout << "  expected tfs:";
                for (auto t : s.tfs) std::cout << " " << t;
                std::cout << "\n  got:";
                for (auto t : tfs) std::cout << " " << t;
                std::cout << std::endl;
            }
        }
    }

    // test 3: lookup a stem that doesn't exist
    {
        std::vector<uint32_t> doc_ids;
        std::vector<uint16_t> tfs;
        uint32_t df = 0;
        bool found = reader.lookup("nonexistent", doc_ids, tfs, df);
        check("\"nonexistent\" not found", !found);
    }

    // cleanup
    std::remove(test_path.c_str());

    std::cout << "\n--- Results: " << pass << " passed, " << fail << " failed ---" << std::endl;
    return fail > 0 ? 1 : 0;
}
