#ifndef XBRZSCALER_HPP
#define XBRZSCALER_HPP

#include <cstdint>

// Edge-aware pixel-art upscaler following the xBRZ algorithm (Zenju).
//
// This is an independent implementation written for WarpNES (MIT licensed);
// it does not contain code from the GPL-licensed xBRZ reference library, so
// output can differ slightly from it.
//
// src: width x height pixels, 0xAARRGGBB. dst: (width*factor) x (height*factor).
// factor: 2, 3 or 4.
void xbrzStyleScale(int factor, const uint32_t* src, uint32_t* dst, int width, int height);

#endif // XBRZSCALER_HPP
