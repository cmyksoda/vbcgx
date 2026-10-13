#include "vbrender.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
static unsigned offset(unsigned x, unsigned y, unsigned w)
{
    return ((y / 4) * (w / 4) + x / 4) * 64 + ((y % 4) * 4 + x % 4) * 2;
}
static void check(bool ok, const char *reason)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", reason); std::exit(1); }
}
int main()
{
    for (unsigned sourceH : {224u, 448u}) {
        std::vector<uint8_t> input(4 * sourceH * 4), output(4 * 188 * 4);
        for (unsigned row = 0; row < sourceH; ++row) {
            std::fill(input.begin(), input.end(), 0);
            for (unsigned x = 0; x < 4; ++x) input[(row * 4 + x) * 4 + 2] = 255;
            ReduceVBFrame(input.data(), 4, sourceH, 4 * 4, output.data(), 4, 188);
            unsigned red = 0;
            for (unsigned y = 0; y < 188; ++y) red += output[offset(0, y, 4) + 1];
            check(red > 0, "every mono/top-bottom source row survives 240p reduction");
        }
    }
    std::vector<uint8_t> input(768 * 4 * 4), output(640 * 4 * 4);
    for (unsigned column = 0; column < 768; ++column) {
        std::fill(input.begin(), input.end(), 0);
        for (unsigned y = 0; y < 4; ++y) input[(y * 768 + column) * 4] = 255;
        ReduceVBFrame(input.data(), 768, 4, 768 * 4, output.data(), 640, 4);
        unsigned blue = 0;
        for (unsigned x = 0; x < 640; ++x) blue += output[offset(x, 0, 640) + 33];
        check(blue > 0, "every side-by-side source column survives reduction");
    }
    std::fill(input.begin(), input.end(), 0);
    for (size_t i = 0; i < input.size(); i += 4) {
        input[i] = 17; input[i + 1] = 53; input[i + 2] = 129;
    }
    ReduceVBFrame(input.data(), 768, 4, 768 * 4, output.data(), 640, 4);
    for (unsigned x = 0; x < 640; ++x) {
        unsigned at = offset(x, 0, 640);
        check(output[at] == 255 && output[at + 1] == 129 && output[at + 32] == 53 && output[at + 33] == 17,
            "explicit GX AR/GB byte order and constant-color preservation");
    }
    std::puts("PASS: 672 source-row phases, 768 column phases, GX color byte order");
}
