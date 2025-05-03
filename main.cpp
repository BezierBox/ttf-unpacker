#include <cstddef>
#include <emscripten.h>
#include <emscripten/bind.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "writeback.h"
#include "reorganize.h"

using namespace std;
using namespace emscripten;

struct TableRecord {
  uint32_t offset;
  uint32_t length;
};

struct Point {
  int x, y;
  bool onCurve;
};

std::string filename;

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

vector<vector<Point>> read_simple_glyph(ifstream &f, const TableRecord &glyf,
                                uint32_t glyphOffset) {
  vector<vector<Point>> points;

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

  vector<Point> temp;
  for (int i = 0; i < numPoints; ++i) {
    temp.push_back(
        {xCoords[i], yCoords[i], static_cast<bool>((flags[i] & 0x01))});
    if (std::find(endPts.begin(), endPts.end(), i) != endPts.end()) {
        points.push_back(temp);
        temp.clear();
    }
  }

  if (!temp.empty()) {
    points.push_back(temp);
  }

  return points;
}

uint16_t get_glyph_index(ifstream &file, const TableRecord &cmap,
                       int16_t character) {

  file.seekg(cmap.offset);
  read_u16(file); // version
  uint16_t numSubtables = read_u16(file);

  uint32_t bestSubtableOffset = 0;
  for (int i = 0; i < numSubtables; ++i) {
    uint16_t platformID = read_u16(file);
    uint16_t encodingID = read_u16(file);
    uint32_t offset = read_u32(file);

    if (platformID == 3 && (encodingID == 1 || encodingID == 10)) {
      bestSubtableOffset = cmap.offset + offset;
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
  read_u16(file); // language
  uint16_t segCountX2 = read_u16(file);
  uint16_t segCount = segCountX2 / 2;

  file.seekg(6, std::ios::cur); // skip searchRange, entrySelector, rangeShift

  std::vector<uint16_t> endCode(segCount);
  for (int i = 0; i < segCount; ++i)
    endCode[i] = read_u16(file);
  read_u16(file); // reservedPad
  std::vector<uint16_t> startCode(segCount);
  for (int i = 0; i < segCount; ++i)
    startCode[i] = read_u16(file);
  std::vector<int16_t> idDelta(segCount);
  for (int i = 0; i < segCount; ++i)
    idDelta[i] = read_i16(file);
  std::vector<uint16_t> idRangeOffset(segCount);
  for (int i = 0; i < segCount; ++i)
    idRangeOffset[i] = read_u16(file);

  uint32_t glyphIdArrayStart = file.tellg();

  uint16_t charCode = character;
  for (int i = 0; i < segCount; ++i) {
    if (startCode[i] <= charCode && charCode <= endCode[i]) {
      if (idRangeOffset[i] == 0) {
        return (charCode + idDelta[i]) % 65536;
      } else {
        std::streampos pos = glyphIdArrayStart + 2 * i + idRangeOffset[i] +
                             2 * (charCode - startCode[i]);
        file.seekg(pos);
        uint16_t glyphId = read_u16(file);
        if (glyphId == 0)
          return 0;
        return (glyphId + idDelta[i]) % 65536;
      }
    }
  }

  return 0; // not found
}

vector<vector<Point>> read_glyph(ifstream &file, map<string, TableRecord> tables, vector<uint32_t> loca, int unicode) {
  int glyph_index = get_glyph_index(file, tables["cmap"], unicode);
  auto glyph = read_simple_glyph(file, tables["glyf"], loca[glyph_index]);

  return glyph;
}

std::map<uint16_t, std::vector<uint16_t>> gntu_map(ifstream &file, const TableRecord &cmap) {

  std::map<uint16_t, std::vector<uint16_t>> glyphToUnicode;

  file.seekg(cmap.offset);
  read_u16(file); // version
  uint16_t numSubtables = read_u16(file);

  uint32_t bestSubtableOffset = 0;
  for (int i = 0; i < numSubtables; ++i) {
    uint16_t platformID = read_u16(file);
    uint16_t encodingID = read_u16(file);
    uint32_t offset = read_u32(file);

    if (platformID == 3 && (encodingID == 1 || encodingID == 10)) {
      bestSubtableOffset = cmap.offset + offset;
      break;
    }
  }

  if (bestSubtableOffset == 0) {
    std::cerr << "No usable cmap subtable found." << std::endl;
    return glyphToUnicode;
  }

  file.seekg(bestSubtableOffset);
  uint16_t format = read_u16(file);
  if (format != 4) {
    std::cerr << "Only Format 4 cmap is supported." << std::endl;
    return glyphToUnicode;
  }

  uint16_t length = read_u16(file);
  read_u16(file); // language
  uint16_t segCountX2 = read_u16(file);
  uint16_t segCount = segCountX2 / 2;

  file.seekg(6, std::ios::cur); // skip searchRange, entrySelector, rangeShift

  std::vector<uint16_t> endCode(segCount);
  for (int i = 0; i < segCount; ++i)
    endCode[i] = read_u16(file);
  read_u16(file); // reservedPad
  std::vector<uint16_t> startCode(segCount);
  for (int i = 0; i < segCount; ++i)
    startCode[i] = read_u16(file);
  std::vector<int16_t> idDelta(segCount);
  for (int i = 0; i < segCount; ++i)
    idDelta[i] = read_i16(file);
  std::vector<uint16_t> idRangeOffset(segCount);
  for (int i = 0; i < segCount; ++i)
    idRangeOffset[i] = read_u16(file);

  uint32_t glyphIdArrayStart = file.tellg();
  for (uint16_t charCode = 0; charCode < 65535; charCode++) {
    for (int i = 0; i < segCount; ++i) {
      if (startCode[i] <= charCode && charCode <= endCode[i]) {
        if (idRangeOffset[i] == 0) {
          glyphToUnicode[(charCode + idDelta[i]) % 65536].push_back(charCode);
        } else {
          std::streampos pos = glyphIdArrayStart + 2 * i + idRangeOffset[i] +
                              2 * (charCode - startCode[i]);
          file.seekg(pos);
          uint16_t glyphId = read_u16(file);
          if (glyphId == 0)
            glyphToUnicode[0].push_back(charCode);
          glyphToUnicode[(charCode + idDelta[i]) % 65536].push_back(charCode);
        }
      }
    }
    //cout << charCode;
  }

  return glyphToUnicode;
}

vector<vector<vector<Point>>> read_glyphs(ifstream &file, map<string, TableRecord> tables, vector<uint32_t> loca, int num_glyphs) {
  vector<vector<vector<Point>>> glyphs;
  for (int i = 0; i < num_glyphs; i++) {
    auto glyph = read_simple_glyph(file, tables["glyf"], loca[i]);
    glyphs.push_back(glyph);
  }

  return glyphs;
}

// Main Program
EMSCRIPTEN_KEEPALIVE
void open_font(const std::string font_name) {
  ifstream font(font_name, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  font.close();

  reorganize(font_name);

  filename = "output.ttf";
}

EMSCRIPTEN_KEEPALIVE
int find_glyph_index(uint16_t unicode) {
  ifstream font(filename, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  auto tables = read_table_directory(font);
  int glyph_index = get_glyph_index(font, tables["cmap"], unicode);

  return glyph_index;
}

EMSCRIPTEN_KEEPALIVE
vector<vector<vector<Point>>> extract_glyphs() {
  ifstream font(filename, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  auto tables = read_table_directory(font);

  uint16_t numGlyphs = get_num_glyphs(font, tables["maxp"]);
  uint16_t format = get_index_to_loc_format(font, tables["head"]);

  auto loca = read_loca(font, tables["loca"], numGlyphs, format == 0);

  auto glyphs = read_glyphs(font, tables, loca, numGlyphs);
  font.close();

  return glyphs;
}

EMSCRIPTEN_KEEPALIVE
vector<vector<Point>> extract_glyph(int unicode) {
  ifstream font(filename, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  auto tables = read_table_directory(font);

  uint16_t numGlyphs = get_num_glyphs(font, tables["maxp"]);
  uint16_t format = get_index_to_loc_format(font, tables["head"]);

  auto loca = read_loca(font, tables["loca"], numGlyphs, format == 0);

  auto glyphs = read_glyph(font, tables, loca, unicode);
  font.close();

  return glyphs;
}

EMSCRIPTEN_KEEPALIVE
std::map<uint16_t, std::vector<uint16_t>> glyph_index_to_unicode_map() {
  ifstream font(filename, ios::binary);
  if (!font)
    throw runtime_error("Font not found");

  auto tables = read_table_directory(font);
  std::map<uint16_t, std::vector<uint16_t>> glyphNumToUnicode = gntu_map(font, tables["cmap"]);
  font.close();

  return glyphNumToUnicode;
}

EMSCRIPTEN_KEEPALIVE
void write_entries(map<uint16_t, vector<WBPoint>> points) {
    std::vector<int> glyphIndices;
    std::vector<std::vector<WBPoint>> pointsVectors;
    for (const auto& pair : points) {
        glyphIndices.push_back(pair.first);
        pointsVectors.push_back(pair.second);
    }
    writeback(filename, filename, glyphIndices, pointsVectors);
}

EMSCRIPTEN_BINDINGS(my_module) {
  emscripten::value_object<Point>("Point")
    .field("x", &Point::x)
    .field("y", &Point::y)
    .field("onCurve", &Point::onCurve);

  emscripten::value_object<WBPoint>("WBPoint")
    .field("x", &WBPoint::x)
    .field("y", &WBPoint::y)
    .field("onCurve", &WBPoint::onCurve)
    .field("endPt", &WBPoint::endPt);

  emscripten::register_vector<Point>("VectorPoint");
  emscripten::register_vector<std::vector<Point>>("VectorVectorPoint");
  emscripten::register_map<uint16_t, std::vector<std::vector<Point>>>("MapUint16ToVectorVectorPoint");
  emscripten::register_vector<uint16_t>("vector<uint16_t>");
  emscripten::register_map<uint16_t, std::vector<uint16_t>>("map<uint16_t, vector<uint16_t>>");
  emscripten::register_vector<std::vector<std::vector<Point>>>("VectorVectorVectorPoint");

  emscripten::register_vector<WBPoint>("VectorWBPoint");
  emscripten::register_vector<std::vector<WBPoint>>("VectorVectorWBPoint");
  emscripten::register_map<uint16_t, std::vector<WBPoint>>("MapUint16VectorWBPoint");



  // Bind functions
  emscripten::function("open_font", &open_font);
  emscripten::function("find_glyph_index", &find_glyph_index);
  emscripten::function("extract_glyph", &extract_glyph);
  emscripten::function("glyph_index_to_unicode_map", &glyph_index_to_unicode_map);
  emscripten::function("extract_glyphs", &extract_glyphs);
  emscripten::function("write_entries", &write_entries);
}
