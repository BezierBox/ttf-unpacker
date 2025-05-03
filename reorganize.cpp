#include <fstream>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <string>

// Align x up to multiple of a
static uint32_t align4(uint32_t x) {
    return (x + 3) & ~3u;
}

// Read big-endian unsigned 16‑bit
static uint16_t re_read_u16(std::ifstream& in) {
    uint8_t b[2];
    in.read(reinterpret_cast<char*>(b), 2);
    return (uint16_t(b[0]) << 8) | b[1];
}

// Read big-endian unsigned 32‑bit
static uint32_t re_read_u32(std::ifstream& in) {
    uint8_t b[4];
    in.read(reinterpret_cast<char*>(b), 4);
    return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16)
         | (uint32_t(b[2]) << 8)  |  uint32_t(b[3]);
}

// Write big-endian unsigned 16‑bit
static void write_u16(std::ofstream& out, uint16_t v) {
    uint8_t b[2] = { uint8_t(v >> 8), uint8_t(v & 0xFF) };
    out.write(reinterpret_cast<char*>(b), 2);
}

// Write big-endian unsigned 32‑bit
static void write_u32(std::ofstream& out, uint32_t v) {
    uint8_t b[4] = {
        uint8_t(v >> 24), uint8_t((v >> 16) & 0xFF),
        uint8_t((v >> 8) & 0xFF),  uint8_t(v & 0xFF)
    };
    out.write(reinterpret_cast<char*>(b), 4);
}

// Compute the OpenType/TrueType table checksum:
// sum of big-endian uint32 words (pad trailing bytes with 0)
static uint32_t compute_checksum(const std::vector<uint8_t>& data) {
    uint32_t sum = 0;
    size_t n = align4(data.size());
    for (size_t i = 0; i < n; i += 4) {
        uint32_t w = 0;
        for (size_t j = 0; j < 4; ++j) {
            w <<= 8;
            if (i + j < data.size()) w |= data[i + j];
        }
        sum = uint32_t(sum + w);
    }
    return sum;
}

struct TableRecord {
    char     tag[4];
    uint32_t checksum;
    uint32_t offset;
    uint32_t length;
};

struct Table {
    TableRecord rec;
    std::vector<uint8_t> data;
};

int reorganize(std::string filename) {
    // if (argc != 3) {
    //     std::cerr << "Usage: " << argv[0]
    //               << " <input.ttf> <output.ttf>\n";
    //     return 1;
    // }
    const char* outPath = "output.ttf";

    // 1) Open input and read OffsetTable
    std::ifstream in(filename, std::ios::binary);
    if (!in) { perror("Opening input"); return 1; }

    // sfnt version
    uint32_t sfntVersion = re_read_u32(in);
    uint16_t numTables   = re_read_u16(in);
    uint16_t searchRange = re_read_u16(in);
    uint16_t entrySelector = re_read_u16(in);
    uint16_t rangeShift  = re_read_u16(in);

    // 2) Read all table records
    std::vector<Table> tables;
    tables.reserve(numTables);
    std::streampos dirStart = in.tellg();  // after header
    for (int i = 0; i < numTables; ++i) {
        in.seekg(dirStart + std::streamoff(16 * i), std::ios::beg);
        Table t;
        in.read(reinterpret_cast<char*>(t.rec.tag), 4);
        t.rec.checksum = re_read_u32(in);
        t.rec.offset   = re_read_u32(in);
        t.rec.length   = re_read_u32(in);

        // Read the table data
        in.seekg(t.rec.offset, std::ios::beg);
        t.data.resize(t.rec.length);
        in.read(reinterpret_cast<char*>(t.data.data()), t.rec.length);
        tables.push_back(std::move(t));
    }

    std::cerr << "Tables in input font:\n";
    for (auto &tbl : tables) {
        std::string tag(tbl.rec.tag, 4);
        std::cerr << "  “" << tag << "” (offset=" << tbl.rec.offset
                  << ", length=" << tbl.rec.length << ")\n";
    }

    // 3) Extract glyf table
    auto it = std::find_if(tables.begin(), tables.end(),
        [](auto &t){ return std::memcmp(t.rec.tag, "glyf", 4) == 0; });
    if (it == tables.end()) {
        std::cerr << "Error: glyf table not found in input font\n";
        return 1;
    }
    Table glyfTable = *it;
    tables.erase(it);

    // 4) (Optional) keep the other tables in original order, then append glyf last
    tables.push_back(std::move(glyfTable));
    uint16_t newNumTables = uint16_t(tables.size());

    // 5) Recompute offsets, lengths, checksums
    //    Directory size = 12 + 16 * numTables
    uint32_t dirSize = 12 + 16u * newNumTables;
    uint32_t writePtr = dirSize;

    // Sort so head is first, glyf is last (head must exist!)
    std::sort(tables.begin(), tables.end(), [](auto &a, auto &b){
        bool aHead = std::memcmp(a.rec.tag,"head",4)==0;
        bool bHead = std::memcmp(b.rec.tag,"head",4)==0;
        if (aHead != bHead) return aHead;       // head first
        bool aGlyf = std::memcmp(a.rec.tag,"glyf",4)==0;
        bool bGlyf = std::memcmp(b.rec.tag,"glyf",4)==0;
        if (aGlyf != bGlyf) return !aGlyf;     // glyf last
        return std::memcmp(a.rec.tag, b.rec.tag, 4) < 0;
    });

    // Assign new offsets & recalc checksums
    for (auto &tbl : tables) {
        writePtr = align4(writePtr);
        tbl.rec.offset   = writePtr;
        tbl.rec.length   = uint32_t(tbl.data.size());
        tbl.rec.checksum = compute_checksum(tbl.data);
        writePtr += tbl.rec.length;
    }

    // 6) Write to a temporary buffer in memory to compute head.checkSumAdjustment
    std::vector<uint8_t> tmp;
    tmp.reserve(writePtr);
    // helper to append BE ints
    auto push32 = [&](uint32_t v){
        tmp.push_back(v>>24); tmp.push_back((v>>16)&0xFF);
        tmp.push_back((v>>8)&0xFF); tmp.push_back(v&0xFF);
    };
    auto push16 = [&](uint16_t v){
        tmp.push_back(v>>8); tmp.push_back(v&0xFF);
    };

    // OffsetTable
    push32(sfntVersion);
    push16(newNumTables);
    push16(searchRange);
    push16(entrySelector);
    push16(rangeShift);

    // TableRecords
    for (auto &tbl : tables) {
        tmp.insert(tmp.end(), tbl.rec.tag, tbl.rec.tag+4);
        push32(tbl.rec.checksum);
        push32(tbl.rec.offset);
        push32(tbl.rec.length);
    }
    // Table data
    for (auto &tbl : tables) {
        size_t cur = tmp.size();
        size_t want = tbl.rec.offset;
        tmp.insert(tmp.end(), want - cur, 0);
        tmp.insert(tmp.end(), tbl.data.begin(), tbl.data.end());
    }

    // Find head record in our tmp, zero its checkSumAdjustment (bytes 8–11 of head table)
    // First find head offset in tmp
        // 6a) Zero out head.checkSumAdjustment in the tmp blob.
    // Find the head table's data offset (already in tables[].rec.offset)
    uint32_t headDataOffset = 0;
    for (auto &tbl : tables) {
        if (std::memcmp(tbl.rec.tag, "head", 4) == 0) {
            headDataOffset = tbl.rec.offset;
            break;
        }
    }
    if (headDataOffset == 0) {
        std::cerr << "Error: head table not found\n";
        return 1;
    }
    // checkSumAdjustment lives at bytes 8..11 of the head table data
    for (int i = 0; i < 4; ++i) {
        tmp[headDataOffset + 8 + i] = 0;
    }


    // Compute overall checksum of tmp
    uint32_t fontSum = 0;
    for (size_t i = 0; i < tmp.size(); i += 4) {
        uint32_t w = 0;
        for (int j = 0; j < 4; ++j) {
            w <<= 8;
            if (i + j < tmp.size()) w |= tmp[i+j];
        }
        fontSum = uint32_t(fontSum + w);
    }

    // Compute checkSumAdjustment
    static constexpr uint32_t magic = 0xB1B0AFBA;
    uint32_t adjust = magic - fontSum;

    // Patch it back into tmp
    for (int i = 0; i < 4; ++i)
        tmp[headDataOffset + 8 + i] = uint8_t((adjust >> (24 - 8*i)) & 0xFF);

    // 7) Write tmp buffer out
    std::ofstream out(outPath, std::ios::binary);
    if (!out) { perror("Opening output"); return 1; }
    out.write(reinterpret_cast<char*>(tmp.data()), tmp.size());
    out.close();

    std::cout << "Reordered glyf to end and wrote: " << outPath << "\n";


    std::ifstream nin(outPath, std::ios::binary);
    if (!in) { perror("Opening input"); return 1; }

    // sfnt version
    sfntVersion = re_read_u32(nin);
    numTables   = re_read_u16(nin);
    searchRange = re_read_u16(nin);
    entrySelector = re_read_u16(nin);
    rangeShift  = re_read_u16(nin);

    // 2) Read all table records
    std::vector<Table> tables2;
    tables2.reserve(numTables);
    dirStart = nin.tellg();  // after header
    for (int i = 0; i < numTables; ++i) {
        nin.seekg(dirStart + std::streamoff(16 * i), std::ios::beg);
        Table t;
        nin.read(reinterpret_cast<char*>(t.rec.tag), 4);
        t.rec.checksum = re_read_u32(nin);
        t.rec.offset   = re_read_u32(nin);
        t.rec.length   = re_read_u32(nin);

        // Read the table data
        nin.seekg(t.rec.offset, std::ios::beg);
        t.data.resize(t.rec.length);
        nin.read(reinterpret_cast<char*>(t.data.data()), t.rec.length);
        tables2.push_back(std::move(t));
    }

    std::cerr << "Tables in input font:\n";
    for (auto &tbl : tables2) {
        std::string tag(tbl.rec.tag, 4);
        std::cerr << "  “" << tag << "” (offset=" << tbl.rec.offset
                  << ", length=" << tbl.rec.length << ")\n";
    }

    return 0;
}
