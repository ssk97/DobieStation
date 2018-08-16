#ifndef GSSHARED_HPP
#define GSSHARED_HPP

#include "gscontext.hpp"
struct PRMODE
{
    bool gourand_shading;
    bool texture_mapping;
    bool fog;
    bool alpha_blend;
    bool antialiasing;
    bool use_UV;
    bool use_context2;
    bool fix_fragment_value;
};

struct PRIM_REG
{
    uint8_t prim_type;
    bool gourand_shading;
    bool texture_mapping;
    bool fog;
    bool alpha_blend;
    bool antialiasing;
    bool use_UV;
    bool use_context2;
    bool fix_fragment_value;
};

struct RGBAQ_REG
{
    uint8_t r, g, b, a;
    float q;
};

struct BITBLTBUF_REG
{
    uint32_t source_base;
    uint32_t source_width;
    uint8_t source_format;
    uint32_t dest_base;
    uint32_t dest_width;
    uint8_t dest_format;
};

struct TRXREG_REG
{
    uint16_t width;
    uint16_t height;
};

struct TRXPOS_REG
{
    uint16_t source_x, source_y;
    uint16_t dest_x, dest_y;
    uint16_t int_source_x, int_dest_x;
    uint8_t trans_order;
};

struct TEXA_REG
{
    uint8_t alpha0, alpha1;
    bool trans_black;
};

struct TEXCLUT_REG
{
    uint16_t width, x, y;
};

struct Vertex
{
    int32_t x, y, z;

    RGBAQ_REG rgbaq;
    UV_REG uv;
    float s, t;
    void to_relative(XYOFFSET xyoffset)
    {
        x -= xyoffset.x;
        y -= xyoffset.y;
    }
};

#endif