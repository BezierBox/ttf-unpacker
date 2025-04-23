#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

struct TableRecord {
  uint32_t offset;
  uint32_t length;
};

struct Point {
  int x, y;
  bool onCurve;
  bool endPt;
};

// Reading Binary Data

uint16_t read_u16(ifstream &f) {
  uint8_t bytes[2];
  f.read((char *)bytes, 2);
  return (bytes[0] << 8) | bytes[1];
}

uint32_t read_u32(ifstream &f) {
  uint8_t bytes[4];
  f.read((char *)bytes, 4);
  return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

int16_t read_i16(ifstream &f) { return (int16_t)read_u16(f); }

// Load Table Directory

map<string, TableRecord> read_table_directory(ifstream &f) {
  f.seekg(0);
  f.ignore(4); // skip scaler type
  uint16_t numTables = read_u16(f);
  f.ignore(6); // skip searchRange, entrySelector, rangeShift

  map<string, TableRecord> tables;
  for (int i = 0; i < numTables; ++i) {
    char tag[5] = {0};
    f.read(tag, 4);
    f.ignore(4); // checksum
    uint32_t offset = read_u32(f);
    uint32_t length = read_u32(f);
    tables[string(tag)] = {offset, length};
  }
  return tables;
}

// Load Key Font Info

uint16_t get_num_glyphs(ifstream &f, const TableRecord &maxp) {
  f.seekg(maxp.offset + 4);
  return read_u16(f);
}

uint16_t get_index_to_loc_format(ifstream &f, const TableRecord &head) {
  f.seekg(head.offset + 50);
  return read_u16(f);
}

// Load Loca Table

vector<uint32_t> read_loca(ifstream &f, const TableRecord &loca, int numGlyphs,
                           bool shortFormat) {
  vector<uint32_t> offsets(numGlyphs + 1);
  f.seekg(loca.offset);
  if (shortFormat) {
    for (int i = 0; i <= numGlyphs; ++i) {
      offsets[i] = read_u16(f) * 2;
    }
  } else {
    for (int i = 0; i <= numGlyphs; ++i) {
      offsets[i] = read_u32(f);
    }
  }
  return offsets;
}

// Extract One Glyph

vector<Point> read_simple_glyph(ifstream &f, const TableRecord &glyf,
                                uint32_t glyphOffset) {
  vector<Point> points;

  f.seekg(glyf.offset + glyphOffset);
  int16_t numContours = read_i16(f);
  f.ignore(8); // skip bbox

  if (numContours < 0)
    return {}; // skip composite

  vector<uint16_t> endPts(numContours);
  for (int i = 0; i < numContours; ++i) {
    endPts[i] = read_u16(f);
  }

  uint16_t instructionLength = read_u16(f);
  f.ignore(instructionLength);

  int numPoints = endPts.back() + 1;

  // Read flags
  vector<uint8_t> flags;
  while ((int)flags.size() < numPoints) {
    uint8_t flag = f.get();
    flags.push_back(flag);
    if (flag & 0x08) { // repeat
      uint8_t repeatCount = f.get();
      for (int i = 0; i < repeatCount; ++i)
        flags.push_back(flag);
    }
  }

  // Read X coords
  vector<int> xCoords(numPoints);
  int x = 0;
  for (int i = 0; i < numPoints; ++i) {
    uint8_t flag = flags[i];
    int dx = 0;
    if (flag & 0x02) {
      uint8_t val = f.get();
      dx = (flag & 0x10) ? val : -val;
    } else if (!(flag & 0x10)) {
      dx = (int16_t)read_u16(f);
    }
    x += dx;
    xCoords[i] = x;
  }

  // Read Y coords
  vector<int> yCoords(numPoints);
  int y = 0;
  for (int i = 0; i < numPoints; ++i) {
    uint8_t flag = flags[i];
    int dy = 0;
    if (flag & 0x04) {
      uint8_t val = f.get();
      dy = (flag & 0x20) ? val : -val;
    } else if (!(flag & 0x20)) {
      dy = (int16_t)read_u16(f);
    }
    y += dy;
    yCoords[i] = y;
  }

  for (int i = 0; i < numPoints; ++i) {
    points.push_back(
        {xCoords[i], yCoords[i], static_cast<bool>((flags[i] & 0x01)),
         std::find(endPts.begin(), endPts.end(), i) != endPts.end()});
  }

  return points;
}

TableRecord find_cmap_subtable(ifstream &f, const TableRecord &cmap) {
  f.seekg(cmap.offset); // Ignore Version Number
  read_u16(f);
  uint16_t num_tables = read_u16(f); // Read Number of Subtables
  // f.ignore(4);

  uint32_t subtable_offset = 0;
  printf("num tables: %d\n", num_tables);
  for (int i = 0; i < num_tables; ++i) {
    uint16_t platformID = read_u16(f);
    uint16_t encodingID = read_u16(f);
    uint32_t offset = read_u32(f);
    f.seekg(cmap.offset + offset);
    uint16_t format = read_u16(f); // check the table

    cout << "platform ID: " << platformID << " encoding ID: " << encodingID
         << " format: " << format << "\n";

    // Prefer Unicode platform
    if ((platformID == 0 && (encodingID == 3 || encodingID == 4)) &&
        format == 4) {
      subtable_offset = cmap.offset + offset;
      break;
    }

    // Fallback to Microsoft BMP
    if (subtable_offset == 0 && platformID == 3 && encodingID == 1 &&
        format == 4) {
      subtable_offset = cmap.offset + offset;
    }

    f.ignore(8);
  }

  f.seekg(subtable_offset + 2);
  uint16_t length = read_u16(f);

  return TableRecord{subtable_offset, length};
}

int find_glyph_index(ifstream &f, const TableRecord &cmap_sub,
                     uint16_t unicodeChar) {
  f.seekg(cmap_sub.offset + 6); // Ignore Version Number

  uint16_t segCount = read_u16(f);
  // const uint8_t endCodes = cmap_sub.offset + 14;
  // const uint8_t startCodes = endCodes + 2 + segCount * 2;
  // const uint8_t idDeltas = startCodes + segCount * 2;
  // const uint8_t idRangeOffsets = idDeltas + segCount * 2;
  // if (segCount == 0)
  //   printf("da bug\n");

  // for (int i = 0; i < segCount; ++i) {
  //   f.seekg(startCodes + i * 2);
  //   uint16_t start = read_u16(f);
  //   f.seekg(endCodes + i * 2);
  //   uint16_t end = read_u16(f);
  //   if (unicodeChar >= start && unicodeChar <= end) {
  //     f.seekg(idDeltas + i * 2);
  //     uint16_t delta = read_u16(f);
  //     f.seekg(idRangeOffsets + i * 2);
  //     uint16_t rangeOffset = read_u16(f);
  //     if (rangeOffset == 0) {
  //       return (unicodeChar + delta) % 65536;
  //     } else {
  //       const uint8_t glyphIndexPtr =
  //           idRangeOffsets + i * 2 + rangeOffset + 2 * (unicodeChar - start);
  //       f.seekg(glyphIndexPtr);
  //       uint16_t glyphIndex = read_u16(f);
  //       if (glyphIndex != 0) {
  //         glyphIndex = (glyphIndex + delta) % 65536;
  //       }
  //       return glyphIndex;
  //     }
  //   }
  // }
  std::vector<uint16_t> endCode(segCount);
  for (int i = 0; i < segCount; ++i) endCode[i] = read_u16(f);
  read_u16(f);  // reservedPad
  std::vector<uint16_t> startCode(segCount);
  for (int i = 0; i < segCount; ++i) startCode[i] = read_u16(f);
  std::vector<int16_t> idDelta(segCount);
  for (int i = 0; i < segCount; ++i) idDelta[i] = read_u16(f);
  std::vector<uint16_t> idRangeOffset(segCount);
  for (int i = 0; i < segCount; ++i) idRangeOffset[i] = read_u16(f);

  uint32_t glyphIdArrayStart = f.tellg();

  uint16_t charCode = unicodeChar;
  for (int i = 0; i < segCount; ++i) {
      if (startCode[i] <= charCode && charCode <= endCode[i]) {
          if (idRangeOffset[i] == 0) {
              return (charCode + idDelta[i]) % 65536;
          } else {
              std::streampos pos = glyphIdArrayStart + 2 * i + idRangeOffset[i] + 2 * (charCode - startCode[i]);
              f.seekg(pos);
              uint16_t glyphId = read_u16(f);
              if (glyphId == 0) return 0;
              return (glyphId + idDelta[i]) % 65536;
          }
      }
  }

  return 0;
}

uint16_t getGlyphIndex(const std::string &filename, int16_t character) {
  std::ifstream file(filename, std::ios::binary);
  if (!file) {
      std::cerr << "Failed to open font file." << std::endl;
      return 0;
  }

  // Skip sfnt version
  file.seekg(4);
  uint16_t numTables = read_u16(file);
  file.seekg(6, std::ios::cur);  // skip searchRange, entrySelector, rangeShift

  // === Locate cmap table ===
  uint32_t cmapOffset = 0;
  for (int i = 0; i < numTables; ++i) {
      char tag[5] = {0};
      file.read(tag, 4);
      read_u32(file);  // checksum
      uint32_t offset = read_u32(file);
      read_u32(file);  // length

      if (std::string(tag) == "cmap") {
          cmapOffset = offset;
      }
  }

  if (cmapOffset == 0) {
      std::cerr << "No cmap table found." << std::endl;
      return 0;
  }

  file.seekg(cmapOffset);
  read_u16(file);  // version
  uint16_t numSubtables = read_u16(file);

  uint32_t bestSubtableOffset = 0;
  for (int i = 0; i < numSubtables; ++i) {
      uint16_t platformID = read_u16(file);
      uint16_t encodingID = read_u16(file);
      uint32_t offset = read_u32(file);

      if (platformID == 3 && (encodingID == 1 || encodingID == 10)) {
          bestSubtableOffset = cmapOffset + offset;
          break;
      }
  }

  if (bestSubtableOffset == 0) {
      std::cerr << "No usable cmap subtable found." << std::endl;
      return 0;
  }

  file.seekg(bestSubtableOffset);
  uint16_t format = read_u16(file);
  if (format != 4) {
      std::cerr << "Only Format 4 cmap is supported." << std::endl;
      return 0;
  }

  uint16_t length = read_u16(file);
  read_u16(file);  // language
  uint16_t segCountX2 = read_u16(file);
  uint16_t segCount = segCountX2 / 2;

  file.seekg(6, std::ios::cur);  // skip searchRange, entrySelector, rangeShift

  std::vector<uint16_t> endCode(segCount);
  for (int i = 0; i < segCount; ++i) endCode[i] = read_u16(file);
  read_u16(file);  // reservedPad
  std::vector<uint16_t> startCode(segCount);
  for (int i = 0; i < segCount; ++i) startCode[i] = read_u16(file);
  std::vector<int16_t> idDelta(segCount);
  for (int i = 0; i < segCount; ++i) idDelta[i] = read_i16(file);
  std::vector<uint16_t> idRangeOffset(segCount);
  for (int i = 0; i < segCount; ++i) idRangeOffset[i] = read_u16(file);

  uint32_t glyphIdArrayStart = file.tellg();

  uint16_t charCode = character;
  for (int i = 0; i < segCount; ++i) {
      if (startCode[i] <= charCode && charCode <= endCode[i]) {
          if (idRangeOffset[i] == 0) {
              return (charCode + idDelta[i]) % 65536;
          } else {
              std::streampos pos = glyphIdArrayStart + 2 * i + idRangeOffset[i] + 2 * (charCode - startCode[i]);
              file.seekg(pos);
              uint16_t glyphId = read_u16(file);
              if (glyphId == 0) return 0;
              return (glyphId + idDelta[i]) % 65536;
          }
      }
  }

  return 0;  // not found
}

// Main Program

int main(int argc, char **args) {
  // char *font_name = args[1];
  string font_name = "Georgia.ttf";

  ifstream font(font_name, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  auto tables = read_table_directory(font);

  uint16_t numGlyphs = get_num_glyphs(font, tables["maxp"]);
  uint16_t format = get_index_to_loc_format(font, tables["head"]);

  auto loca = read_loca(font, tables["loca"], numGlyphs, format == 0);

  uint16_t unicode;
  cout << "Unicode Input: ";
  cin >> unicode;
  cout << "cmap " << unicode << "\n";
  auto cmapTable = find_cmap_subtable(font, tables["cmap"]);
  cout << "cmap table: offset: " << cmapTable.offset
       << " length: " << cmapTable.length << "\n";
  cout << "index\n";
  int glyphIndex = getGlyphIndex(font_name, unicode);//find_glyph_index(font, cmapTable, unicode);
  cout << "read glyph: " << glyphIndex << "\n";
  auto points = read_simple_glyph(font, tables["glyf"], loca[glyphIndex]);

  for (auto &pt : points) {
    cout << "(" << pt.x << ", " << pt.y << ") " << (pt.onCurve ? "on" : "off")
         << (pt.endPt ? " end" : " middle") << endl;
  }

  return 0;
}
