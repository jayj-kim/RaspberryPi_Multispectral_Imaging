#include "cpu_coords.hpp"
#include <opencv2/core/hal/intrin.hpp>
namespace imaging {
using namespace cv;
const char *cpuCoordinateKernel() {
#if CV_SIMD128 && CV_NEON
  return "NEON 128-bit";
#elif CV_SIMD128 && CV_SSE2
  return "SSE2 128-bit";
#elif CV_SIMD128
  return "OpenCV SIMD 128-bit";
#else
  return "scalar";
#endif
}
void prepareCoordinates(Mat &flow, Mat &coords, Mat &invalid, float scale, bool retainScaledFlow,
                        bool vectorize) {
  CV_Assert(flow.type() == CV_32FC2 && flow.cols <= 640 && flow.rows <= 480);
  coords.create(flow.size(), CV_32FC2);
  invalid.create(flow.size(), CV_8U);
  const bool scaled = scale != 1.f;
  parallel_for_(Range(0, flow.rows), [&](const Range &range) {
    for (int y = range.start; y < range.end; ++y) {
      auto *f = flow.ptr<Vec2f>(y), *c = coords.ptr<Vec2f>(y);
      auto *bad = invalid.ptr<uchar>(y);
      int x = 0;
#if CV_SIMD128
      if (vectorize) {
        const auto factor = v_setall_f32(scale), row = v_setall_f32(float(y));
        const auto limit = v_setall_f32(3600.f);
        for (; x + 16 <= flow.cols; x += 16) {
          auto four = [&](int col) {
            v_float32x4 fx, fy;
            v_load_deinterleave(reinterpret_cast<const float *>(f + col), fx, fy);
            if (scaled) {
              fx = fx * factor;
              fy = fy * factor;
              if (retainScaledFlow)
                v_store_interleave(reinterpret_cast<float *>(f + col), fx, fy);
            }
            const v_float32x4 column(float(col), float(col + 1), float(col + 2), float(col + 3));
            v_store_interleave(reinterpret_cast<float *>(c + col), fx + column, fy + row);
            // Squared magnitude < 3600 excludes NaN/Inf too. Valid flow components
            // are <60, so adding the bounded output coordinates remains finite.
            return ~v_reinterpret_as_u32((fx * fx + fy * fy) < limit);
          };
          auto a = four(x), b = four(x + 4), d = four(x + 8), e = four(x + 12);
          v_store(bad + x, v_pack(v_pack(a, b), v_pack(d, e)));
        }
      }
#else
      (void)vectorize;
#endif
      for (; x < flow.cols; ++x) {
        const Vec2f value = scaled ? f[x] * scale : f[x];
        if (scaled && retainScaledFlow)
          f[x] = value;
        c[x] = value + Vec2f(float(x), float(y));
        bad[x] = value.dot(value) < 3600.f ? 0 : 255;
      }
    }
  });
}
} // namespace imaging
