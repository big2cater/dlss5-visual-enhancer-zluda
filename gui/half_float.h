// Conversion between the half-floats the network works in and the bytes an
// image file and a screen work in.
//
// Written out rather than pulled from a library because it is twenty lines and
// a dependency for twenty lines is a dependency to install, version and explain.

#pragma once

#include <cstdint>
#include <cstring>

namespace enhancer {

inline uint16_t float_to_half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    const uint32_t f_exp = (bits >> 23) & 0xFFu;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (f_exp == 0xFFu) {
        // NaN or Infinity
        if (mantissa == 0) return (uint16_t)(sign | 0x7C00u); // Infinity
        uint16_t nan_mant = (uint16_t)((mantissa >> 13) & 0x3FFu);
        if (nan_mant == 0) nan_mant = 1;
        return (uint16_t)(sign | 0x7C00u | nan_mant);
    }

    int exponent = (int)f_exp - 127 + 15;

    if (exponent >= 0x1F) {
        // Overflow to infinity
        return (uint16_t)(sign | 0x7C00u);
    }

    if (exponent <= 0) {
        // Too small for a normal half: either zero or subnormal with round-to-nearest-even.
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exponent);
        const uint32_t round_bit = 1u << (shift - 1);
        const uint32_t sticky_mask = round_bit - 1;
        uint32_t half_mant = mantissa >> shift;
        if ((mantissa & round_bit) && ((mantissa & sticky_mask) || (half_mant & 1))) {
            half_mant++;
        }
        return (uint16_t)(sign | half_mant);
    }

    // Normal half with round-to-nearest-even
    const uint32_t round_bit = 0x1000u;
    const uint32_t sticky_mask = 0x0FFFu;
    uint32_t half_mant = mantissa >> 13;
    if ((mantissa & round_bit) && ((mantissa & sticky_mask) || (half_mant & 1))) {
        half_mant++;
        if (half_mant > 0x3FFu) {
            half_mant = 0;
            exponent++;
            if (exponent >= 0x1F) return (uint16_t)(sign | 0x7C00u);
        }
    }
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | half_mant);
}

inline float half_to_float(uint16_t half) {
    const uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    const uint32_t mantissa = half & 0x3FFu;

    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalise it by hand.
            uint32_t e = 0, m = mantissa;
            while (!(m & 0x400u)) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

} // namespace enhancer
