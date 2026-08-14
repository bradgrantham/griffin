// sprite_header_test.cpp — proves the generated sprite header compiles alone.
//
// Build: c++ -std=c++23 -I. -c sprite_header_test.cpp
//
// Nothing here draws anything; the point is that smurf_sprites.h is a
// self-contained, constant-foldable header with no other dependency than
// <cstdint>, so firmware or an app can include it directly.

#include "smurf-assets/smurf_sprites.h"

namespace
{

// Every frame's row table must cover exactly the spans the frame owns, and no
// row may exceed the budget the generator checked against.
consteval bool frames_are_consistent()
{
    for (int i = 0; i < SPRITE_FRAME_COUNT; i++)
    {
        const SpriteFrame &f = *SPRITE_FRAMES[i];
        if (f.width == 0 || f.height == 0)
        {
            return false;
        }
        if (f.max_row_spans > SPRITE_SPAN_BUDGET)
        {
            return false;
        }
        uint16_t expect = 0;
        for (int y = 0; y < f.height; y++)
        {
            if (f.rows[y].first != expect)
            {
                return false;
            }
            if (f.rows[y].count > f.max_row_spans)
            {
                return false;
            }
            for (int s = 0; s < f.rows[y].count; s++)
            {
                const SpriteSpan &sp = f.spans[f.rows[y].first + s];
                if (sp.width == 0 || sp.x + sp.width > f.width || sp.rgb444 > 0x0FFF)
                {
                    return false;
                }
            }
            expect = static_cast<uint16_t>(expect + f.rows[y].count);
        }
    }
    return true;
}

static_assert(SPRITE_FRAME_COUNT > 0);
static_assert(frames_are_consistent());

}  // namespace

int main()
{
    return 0;
}
