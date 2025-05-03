#ifndef WRITEBACK_H
#define WRITEBACK_H

#include <string>
#include <vector>

struct WBPoint {
    int x, y;
    bool onCurve;
    bool endPt;
};

int writeback(std::string input_filename, std::string output_filename, std::vector<int> glyphIndices, std::vector<std::vector<WBPoint>> points);

#endif
