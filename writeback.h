#ifndef WRITEBACK_H
#define WRITEBACK_H

#include <string>
#include <vector>

struct WBPoint {
    int x, y;
    bool onCurve;
    bool endPt;
};

int writeback(std::string input_filename, std::string output_filename, int glyphIndex, std::vector<WBPoint> points);

#endif
