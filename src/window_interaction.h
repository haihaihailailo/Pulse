#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdlib>

namespace pulse::ui {
constexpr int MinimumZoom = 30, MaximumZoom = 200;
enum DockEdge { DockLeft = 1, DockRight = 2, DockTop = 4, DockBottom = 8 };
inline unsigned DockedEdges(const RECT& r, const RECT& work) {
    return (r.left == work.left ? DockLeft : r.right == work.right ? DockRight : 0) |
           (r.top == work.top ? DockTop : r.bottom == work.bottom ? DockBottom : 0);
}
inline RECT SnapToWorkArea(RECT r, const RECT& work, int distance) {
    const int left = std::abs(r.left - work.left), right = std::abs(r.right - work.right);
    const int top = std::abs(r.top - work.top), bottom = std::abs(r.bottom - work.bottom);
    const int dx = left <= distance && left <= right ? work.left - r.left :
                   right <= distance ? work.right - r.right : 0;
    const int dy = top <= distance && top <= bottom ? work.top - r.top :
                   bottom <= distance ? work.bottom - r.bottom : 0;
    OffsetRect(&r, dx, dy);
    return r;
}
}
