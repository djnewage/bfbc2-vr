// Row-vector 4x4 matrix helpers, shared by the hook code and the pure policy
// modules (and their tests). Convention throughout the project:
//
//   v_out = v_in * M        (row vectors, D3D9 fixed-function style)
//   at(m, r, c)             row r, column c, stored row-major in float[16]
//
// A matrix that the GAME stores in a shader constant span is the TRANSPOSE of
// this (HLSL column-major packing): register j holds column j of M. Callers
// transpose on the way in and out; nothing here knows about registers.
//
// Header-only and free of Windows/D3D headers on purpose, so a plain console
// test can include it.
#pragma once

#include <cmath>
#include <cstring>

namespace m4 {

using Mat4 = float[16];

inline float& at(Mat4 m, int r, int c)       { return m[r * 4 + c]; }
inline float  at(const Mat4 m, int r, int c) { return m[r * 4 + c]; }

inline void identity(Mat4 m)
{
    std::memset(m, 0, sizeof(float) * 16);
    at(m, 0, 0) = at(m, 1, 1) = at(m, 2, 2) = at(m, 3, 3) = 1.0f;
}

inline void copy(const Mat4 src, Mat4 dst) { std::memcpy(dst, src, sizeof(float) * 16); }

// out = a * b. `out` must not alias a or b.
inline void multiply(const Mat4 a, const Mat4 b, Mat4 out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += at(a, r, k) * at(b, k, c);
            at(out, r, c) = sum;
        }
}

inline void transpose(const Mat4 m, Mat4 out)
{
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            at(out, r, c) = at(m, c, r);
}

inline void rotation_y(float rad, Mat4 m)
{
    identity(m);
    const float s = std::sin(rad), c = std::cos(rad);
    at(m, 0, 0) =  c; at(m, 0, 2) = -s;
    at(m, 2, 0) =  s; at(m, 2, 2) =  c;
}

// Rodrigues rotation about an arbitrary (normalized) axis, row-vector form.
inline void rotation_axis(const float axis[3], float rad, Mat4 m)
{
    const float c = std::cos(rad), s = std::sin(rad), t = 1.0f - c;
    const float x = axis[0], y = axis[1], z = axis[2];
    identity(m);
    at(m,0,0) = t*x*x + c;   at(m,0,1) = t*x*y + s*z; at(m,0,2) = t*x*z - s*y;
    at(m,1,0) = t*x*y - s*z; at(m,1,1) = t*y*y + c;   at(m,1,2) = t*y*z + s*x;
    at(m,2,0) = t*x*z + s*y; at(m,2,1) = t*y*z - s*x; at(m,2,2) = t*z*z + c;
}

inline void translation(float x, float y, float z, Mat4 m)
{
    identity(m);
    at(m, 3, 0) = x; at(m, 3, 1) = y; at(m, 3, 2) = z;
}

inline void scale(float x, float y, float z, Mat4 m)
{
    identity(m);
    at(m, 0, 0) = x; at(m, 1, 1) = y; at(m, 2, 2) = z;
}

// General 4x4 inverse (cofactor expansion). A projection matrix is not affine,
// so the cheap affine-inverse shortcut does not apply here.
inline bool invert(const Mat4 m, Mat4 out)
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (std::fabs(det) < 1e-12f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

} // namespace m4
