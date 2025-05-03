#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <cstring>
#include <unordered_map>

// using namespace std;

struct WBPoint {
    int x, y;
    bool onCurve;
    bool endPt;
};

// Big-endian helpers
uint16_t read_u16(const std::vector<uint8_t>& data, size_t offset) {
    return (data[offset] << 8) | data[offset + 1];
}

uint32_t read_u32(const std::vector<uint8_t>& data, size_t offset) {
    return (data[offset] << 24) | (data[offset + 1] << 16) |
           (data[offset + 2] << 8) | data[offset + 3];
}

void write_u16(std::vector<uint8_t>& data, size_t offset, uint16_t value) {
    data[offset] = value >> 8;
    data[offset + 1] = value & 0xFF;
}

void write_u32(std::vector<uint8_t>& data, size_t offset, uint32_t value) {
    data[offset] = value >> 24;
    data[offset + 1] = (value >> 16) & 0xFF;
    data[offset + 2] = (value >> 8) & 0xFF;
    data[offset + 3] = value & 0xFF;
}

struct TableDirectoryEntry {
    char tag[5];
    uint32_t checksum;
    uint32_t offset;
    uint32_t length;
};

uint16_t get_num_glyphs(std::vector<uint8_t>& font, const TableDirectoryEntry& maxp) {
    return read_u16(font, maxp.offset + 4);
}

std::unordered_map<std::string, TableDirectoryEntry> parse_table_directory(const std::vector<uint8_t>& font, int& numTables) {
    numTables = read_u16(font, 4);
    std::unordered_map<std::string, TableDirectoryEntry> tableMap;
    for (int i = 0; i < numTables; ++i) {
        size_t offset = 12 + i * 16;
        TableDirectoryEntry entry;
        memcpy(entry.tag, &font[offset], 4);
        entry.tag[4] = '\0';
        entry.checksum = read_u32(font, offset + 4);
        entry.offset = read_u32(font, offset + 8);
        entry.length = read_u32(font, offset + 12);
        tableMap[std::string(entry.tag)] = entry;
    }
    return tableMap;
}

uint32_t modify_glyph(std::vector<uint8_t>& font, const TableDirectoryEntry& glyf, const TableDirectoryEntry& loca, uint16_t index, bool longLocaFormat, std::vector<WBPoint> points) {
    size_t locaOffset = loca.offset;
    uint32_t glyphOffset, nextGlyphOffset;
    if (longLocaFormat) {
        glyphOffset = read_u32(font, locaOffset + index * 4);
        nextGlyphOffset = read_u32(font, locaOffset + (index + 1) * 4);
    } else {
        glyphOffset = read_u16(font, locaOffset + index * 2) * 2;
        nextGlyphOffset = read_u16(font, locaOffset + (index + 1) * 2) * 2;
    }
    size_t glyphStart = glyf.offset + glyphOffset;

    int ind = 0;
    std::vector<uint16_t> contourEndIndex;
    for (WBPoint p : points) {
        if (p.endPt) {
            contourEndIndex.push_back(ind);
        }
        ind++;
    }

    int newGlyphLength = points.size() * 5 + contourEndIndex.size() * 2 + 12;
    // account for longer glyphs than what was originally there (adding points)
    if (nextGlyphOffset - glyphOffset < newGlyphLength) {
        // if new length is longer shift everything after farther down to make space
        int toAdd = newGlyphLength - (nextGlyphOffset - glyphOffset);
        for (int i = 0; i < toAdd; i++) {
            font.insert(font.begin() + glyphStart, 0);
        }
    }

    // Build new glyph
    std::vector<uint8_t> newGlyph;
    int currOffset = 10;
    newGlyph.resize(10); // header
    write_u16(newGlyph, 0, (uint16_t) contourEndIndex.size());     // numberOfContours
    write_u16(newGlyph, 2, read_u16(font, glyphStart + 2));     // xMin
    write_u16(newGlyph, 4, read_u16(font, glyphStart + 4));     // yMin
    write_u16(newGlyph, 6, read_u16(font, glyphStart + 6));   // xMax
    write_u16(newGlyph, 8, read_u16(font, glyphStart + 8));   // yMax

    currOffset += 2 * contourEndIndex.size(); // set space for counter end indexes
    newGlyph.resize(currOffset); // make room for endPtsOfContours
    for (int startOff = 10; startOff < currOffset; startOff += 2) {
        write_u16(newGlyph, startOff, contourEndIndex[(startOff - 10) / 2]); // place contours in
    }
    // write_u16(newGlyph, 10, points.size() - 1); // endPtsOfContours[0] = 1

    currOffset += 2; // 14
    newGlyph.resize(currOffset); // instructionLength = 0
    write_u16(newGlyph, currOffset - 2, 0);

    // place flags for on curve
    for (WBPoint p : points) {
        newGlyph.push_back((uint8_t) p.onCurve);
        currOffset++;
    }

    // Coordinates (as signed 8-bit deltas for simplicity)

    // x coordinates
    int past_x = 0;
    for (WBPoint p : points) {
        currOffset += 2;
        newGlyph.resize(currOffset);
        write_u16(newGlyph, currOffset - 2, p.x - past_x);
        past_x = p.x;
    }

    // y coordinates
    int past_y = 0;
    for (WBPoint p : points) {
        currOffset += 2;
        newGlyph.resize(currOffset);
        write_u16(newGlyph, currOffset - 2, p.y - past_y);
        past_y = p.y;
    }

    // Overwrite glyph in font buffer
    size_t glyphLength = (newGlyphLength <= nextGlyphOffset - glyphOffset) ? nextGlyphOffset - glyphOffset : newGlyphLength;
    for (size_t i = 0; i < newGlyph.size(); ++i) {
        font[glyphStart + i] = newGlyph[i];
    }
    for (size_t i = newGlyph.size(); i < glyphLength; ++i) {
        font[glyphStart + i] = 0;
    }

    return glyphLength + glyphOffset; // switch to newGlyph.size() + glyphOffset; once shifting glyph table is done
}

uint32_t calculate_checksum(const std::vector<uint8_t>& data, size_t offset, size_t length) {
    uint32_t sum = 0;
    for (size_t i = 0; i < length; i += 4) {
        uint32_t word = 0;
        for (int j = 0; j < 4 && (i + j) < length; ++j) {
            word |= data[offset + i + j] << (24 - j * 8);
        }
        sum += word;
    }
    return sum;
}

void update_checksums(std::vector<uint8_t>& font, std::unordered_map<std::string, TableDirectoryEntry>& tables) {
    uint32_t total = 0;
    for (auto& [tag, entry] : tables) {
        uint32_t checksum = calculate_checksum(font, entry.offset, entry.length);
        entry.checksum = checksum;
        size_t dir_offset = 12 + (16 * distance(tables.begin(), tables.find(tag)));
        write_u32(font, dir_offset + 4, checksum);
        total += checksum;
    }

    // Set checkSumAdjustment in head to 0 temporarily
    auto& head = tables["head"];
    write_u32(font, head.offset + 8, 0);
    uint32_t font_checksum = calculate_checksum(font, 0, font.size());
    uint32_t checksum_adjustment = 0xB1B0AFBA - font_checksum;
    write_u32(font, head.offset + 8, checksum_adjustment);
}

// Update loca table
uint32_t update_loca(std::vector<uint8_t>& font, TableDirectoryEntry& loca, uint16_t index, uint32_t newEnd, bool longLocaFormat, uint16_t numGlyphs) {
    size_t locaOffset = loca.offset;
    uint32_t oldEnd = 0;
    uint32_t offsetDiff = 0;
    if (longLocaFormat) {
        oldEnd = read_u32(font, locaOffset + (index + 1) * 4);
        offsetDiff = newEnd - oldEnd;
        for (int i = index + 1; i < numGlyphs; i++) {
            uint32_t oldVal = read_u32(font, locaOffset + i * 4);
            write_u32(font, locaOffset + i * 4, oldVal + offsetDiff);
        }
    } else {
        oldEnd = (uint32_t) read_u16(font, locaOffset + (index + 1) * 2);
        offsetDiff = newEnd - oldEnd;
        for (int i = index + 1; i < numGlyphs; i++) {
            uint32_t oldVal = read_u16(font, locaOffset + i * 4);
            write_u16(font, locaOffset + i * 4, oldVal + offsetDiff);
        }
    }
    return offsetDiff;
}

void padTo4(std::vector<uint8_t>& font) {
    size_t pad = (4 - (font.size() % 4)) % 4;
    font.insert(font.end(), pad, 0);
}

void updateLengthRecord(std::vector<uint8_t>& font, uint32_t length) {
    int numTables = read_u16(font, 4);
    for (int i = 0; i < numTables; ++i) {
        size_t offset = 12 + i * 16;
        TableDirectoryEntry entry;
        memcpy(entry.tag, &font[offset], 4);
        entry.tag[4] = '\0';
        if (strcmp(entry.tag, "glyf") == 0)
            write_u32(font, offset + 12, length);
    }
}

int writeback_one(std::vector<uint8_t>& font, int glyphIndex, std::vector<WBPoint> points, std::unordered_map<std::string, TableDirectoryEntry> tableMap, int numTables, bool longLocaFormat) {
    if (!tableMap.count("glyf") || !tableMap.count("loca") || !tableMap.count("head")) {
        return 1;
    }

    uint32_t newOffset = modify_glyph(font, tableMap["glyf"], tableMap["loca"], glyphIndex, longLocaFormat, points); // modify glyph index 1

    // update loca table
    uint16_t numGlyphs = get_num_glyphs(font, tableMap["maxp"]);
    uint32_t newDiff = update_loca(font, tableMap["loca"], glyphIndex, newOffset, longLocaFormat, numGlyphs);

    padTo4(font);
    updateLengthRecord(font, newDiff + tableMap["glyf"].length);
    tableMap["glyf"].length += newDiff;

    update_checksums(font, tableMap);

    return 0;
}

int writeback(std::string input_filename, std::string output_filename, std::vector<int> glyphIndices, std::vector<std::vector<WBPoint>> pointsVector) {
    std::ifstream in(input_filename, std::ios::binary);
    if (!in) {
        return 1;
    }

    std::vector<uint8_t> font((std::istreambuf_iterator<char>(in)), {});
    in.close();

    int numTables = 0;
    auto tableMap = parse_table_directory(font, numTables);
    if (!tableMap.count("glyf") || !tableMap.count("loca") || !tableMap.count("head")) {
        return 1;
    }

    bool longLocaFormat = read_u16(font, tableMap["head"].offset + 50) != 0;

    std::cerr << "starting loop:\n";
    for (int i = 0; i < pointsVector.size(); i++) {
        int w_one = writeback_one(font, glyphIndices[i], pointsVector[i], tableMap, numTables, longLocaFormat);
        if (w_one) {
            return 1;
        }
    }

    std::ofstream out(output_filename, std::ios::binary);
    out.write(reinterpret_cast<const char*>(font.data()), font.size());
    out.close();

    return 0;
}
