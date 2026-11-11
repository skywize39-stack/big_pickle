#pragma once

// Pixel-aligned, exclusive rectangle bounds shared by Surface and ImGui.
// The renderer supplies the draw callback; this file owns no graphics state.
namespace BoxGeometry
{
    template<class Emit>
    void Segments(int left, int top, int right, int bottom, bool corners, Emit emit)
    {
        const int width = right - left, height = bottom - top;
        if (width <= 0 || height <= 0) return;
        if (!corners)
        {
            emit(left, top, right, top + 1);
            emit(left, bottom - 1, right, bottom);
            emit(left, top, left + 1, bottom);
            emit(right - 1, top, right, bottom);
            return;
        }
        const int x = width / 3 > 0 ? width / 3 : 1;
        const int y = height / 3 > 0 ? height / 3 : 1;
        emit(left, top, left + x, top + 1);
        emit(right - x, top, right, top + 1);
        emit(left, bottom - 1, left + x, bottom);
        emit(right - x, bottom - 1, right, bottom);
        emit(left, top, left + 1, top + y);
        emit(right - 1, top, right, top + y);
        emit(left, bottom - y, left + 1, bottom);
        emit(right - 1, bottom - y, right, bottom);
    }
}
