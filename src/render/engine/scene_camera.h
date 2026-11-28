#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace fuse::engine::camera
{
  using Matrix = std::array<double, 16>;  // Row-major, column vectors.

  inline Matrix Multiply(const Matrix &a, const Matrix &b)
  {
    Matrix m{};
    for (size_t y = 0; y < 4; ++y)
      for (size_t x = 0; x < 4; ++x)
        for (size_t k = 0; k < 4; ++k)
          m[y * 4 + x] += a[y * 4 + k] * b[k * 4 + x];
    return m;
  }
  inline bool Invert(const Matrix &input, Matrix &output)
  {
    double a[4][8]{};
    for (size_t y = 0; y < 4; ++y)
      for (size_t x = 0; x < 4; ++x)
      {
        if (!std::isfinite(input[y * 4 + x]))
          return false;
        a[y][x] = input[y * 4 + x];
        a[y][x + 4] = x == y ? 1. : 0.;
      }
    for (size_t i = 0; i < 4; ++i)
    {
      size_t pivot = i;
      for (size_t j = i + 1; j < 4; ++j)
        if (std::abs(a[j][i]) > std::abs(a[pivot][i]))
          pivot = j;
      if (std::abs(a[pivot][i]) < 1e-300)
        return false;
      if (pivot != i)
        for (size_t x = 0; x < 8; ++x)
          std::swap(a[pivot][x], a[i][x]);
      const double scale = a[i][i];
      for (double &value : a[i])
        value /= scale;
      for (size_t j = 0; j < 4; ++j)
        if (j != i)
        {
          const double factor = a[j][i];
          for (size_t x = 0; x < 8; ++x)
            a[j][x] -= factor * a[i][x];
        }
    }
    for (size_t y = 0; y < 4; ++y)
      for (size_t x = 0; x < 4; ++x)
      {
        output[y * 4 + x] = a[y][x + 4];
        if (!std::isfinite(output[y * 4 + x]))
          return false;
      }
    return true;
  }
  inline double MaxAbs(const Matrix &m)
  {
    double v = 0;
    for (double x : m)
      v = std::max(v, std::abs(x));
    return v;
  }

  // The draw's camera from its object matrices: object_to_clip =
  // view_projection * object_to_world (affine).
  inline bool FromObject(const Matrix &object_to_clip, const Matrix &object_to_world, Matrix &view_projection)
  {
    if (std::abs(object_to_world[12]) > 1e-6 || std::abs(object_to_world[13]) > 1e-6 ||
        std::abs(object_to_world[14]) > 1e-6 || std::abs(object_to_world[15] - 1.) > 1e-6)
      return false;
    Matrix inverse;
    if (!Invert(object_to_world, inverse))
      return false;
    view_projection = Multiply(object_to_clip, inverse);
    return true;
  }
  // The same camera within float rounding of the title's matrices: per row,
  // the axes relative to their scale, the translation (large far from the
  // origin) relative to its own size, so a camera a few millimetres off fails.
  // The depth row is not compared: an object may set its own depth (Fuse draws
  // sky-like models at the far plane, z = w), the camera is the x, y and w rows.
  inline bool Close(const Matrix &a, const Matrix &b)
  {
    for (size_t y = 0; y < 4; ++y)
    {
      if (y == 2)
        continue;
      double axes = 0;
      for (size_t x = 0; x < 3; ++x)
        axes = std::max({axes, std::abs(a[y * 4 + x]), std::abs(b[y * 4 + x])});
      for (size_t x = 0; x < 3; ++x)
        if (!(std::abs(a[y * 4 + x] - b[y * 4 + x]) <= 1e-4 * axes + 1e-9))
          return false;
      const double wa = a[y * 4 + 3], wb = b[y * 4 + 3];
      if (!(std::abs(wa - wb) <= 3e-6 * std::max({std::abs(wa), std::abs(wb), axes}) + 1e-5 * axes))
        return false;
    }
    return true;
  }

  struct Camera
  {
    std::array<float, 12> view{};  // 3x4 world -> view (right-handed, looks down -z).
    std::array<float, 16> projection{}, view_projection{};
    std::array<double, 3> eye{};
  };

  // view_projection: rows as the shader reads them. False: not a perspective
  // projection of a rigid view.
  inline bool Split(const Matrix &pv, Camera &out)
  {
    const double *r0 = &pv[0], *r1 = &pv[4], *r2 = &pv[8], *r3 = &pv[12];
    auto dot3 = [](const double *a, const double *b)
    { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const double s = std::sqrt(dot3(r3, r3));
    if (!(s > 1e-9) || !std::isfinite(s))
      return false;
    // View z: away from the forward axis (clip w = -z_view).
    double v[3][4]{};
    for (size_t i = 0; i < 3; ++i)
      v[2][i] = -r3[i] / s;
    // Depth row: along the forward axis only (no oblique clipping).
    const double depth = dot3(r2, v[2]);
    double off = 0, norm2 = std::sqrt(dot3(r2, r2));
    for (size_t i = 0; i < 3; ++i)
      off = std::max(off, std::abs(r2[i] - depth * v[2][i]));
    if (off > 1e-3 * std::max(norm2, 1e-9))
      return false;
    // x and y rows: the remaining axes, orthonormalised.
    double u0[3], u1[3];
    const double c0 = dot3(r0, v[2]);
    for (size_t i = 0; i < 3; ++i)
      u0[i] = r0[i] - c0 * v[2][i];
    const double l0 = std::sqrt(dot3(u0, u0));
    if (!(l0 > 1e-9))
      return false;
    for (size_t i = 0; i < 3; ++i)
      v[0][i] = u0[i] / l0;
    const double c1 = dot3(r1, v[2]), d1 = dot3(r1, v[0]);
    for (size_t i = 0; i < 3; ++i)
      u1[i] = r1[i] - c1 * v[2][i] - d1 * v[0][i];
    const double l1 = std::sqrt(dot3(u1, u1));
    if (!(l1 > 1e-9) || std::abs(d1) > 1e-3 * l1)
      return false;
    for (size_t i = 0; i < 3; ++i)
      v[1][i] = u1[i] / l1;
    // A proper rotation: right x up = backward.
    const double cross[3] = {v[0][1] * v[1][2] - v[0][2] * v[1][1], v[0][2] * v[1][0] - v[0][0] * v[1][2],
                             v[0][0] * v[1][1] - v[0][1] * v[1][0]};
    if (dot3(cross, v[2]) < 0)
      for (size_t i = 0; i < 3; ++i)
        v[1][i] = -v[1][i];
    // Eye: clip x, y and w are zero there (a centred perspective projection).
    Matrix system{}, inverse;
    const double *rows[3] = {r0, r1, r3};
    for (size_t y = 0; y < 3; ++y)
      for (size_t x = 0; x < 3; ++x)
        system[y * 4 + x] = rows[y][x];
    system[15] = 1;
    if (!Invert(system, inverse))
      return false;
    for (size_t y = 0; y < 3; ++y)
    {
      out.eye[y] = 0;
      for (size_t x = 0; x < 3; ++x)
        out.eye[y] -= inverse[y * 4 + x] * rows[x][3];
    }
    for (size_t y = 0; y < 3; ++y)
      v[y][3] = -dot3(v[y], out.eye.data());
    // projection = view_projection * view^-1 (view^-1 = [R^T, eye]).
    Matrix view_inverse{};
    for (size_t y = 0; y < 3; ++y)
    {
      for (size_t x = 0; x < 3; ++x)
        view_inverse[y * 4 + x] = v[x][y];
      view_inverse[y * 4 + 3] = out.eye[y];
    }
    view_inverse[15] = 1;
    const Matrix p = Multiply(pv, view_inverse);
    const double scale = MaxAbs(p);
    if (!(scale > 0) || std::abs(p[12]) > 1e-4 * scale || std::abs(p[13]) > 1e-4 * scale ||
        std::abs(p[15]) > 1e-3 * scale || std::abs(p[14]) < 1e-6 * scale)
      return false;
    // Depth must vary with distance (z row not the w row: Fuse's front-end 3D
    // draws with z = w, a singular matrix the SDK refuses).
    if (std::abs(p[11]) < 1e-6 * scale)
      return false;
    for (size_t y = 0; y < 3; ++y)
      for (size_t x = 0; x < 4; ++x)
        out.view[y * 4 + x] = float(v[y][x]);
    for (size_t i = 0; i < 16; ++i)
    {
      out.projection[i] = float(p[i]);
      out.view_projection[i] = float(pv[i]);
      if (!std::isfinite(out.projection[i]) || !std::isfinite(out.view_projection[i]))
        return false;
    }
    return true;
  }
} // namespace fuse::engine::camera
