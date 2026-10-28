#pragma once
#include <opencv2/core.hpp>
namespace imaging {
// flow is full output resolution. Only checked frames need the scaled flow written back.
void prepareCoordinates(cv::Mat &flow, cv::Mat &coords, cv::Mat &invalid, float scale,
                        bool retainScaledFlow, bool vectorize = true);
const char *cpuCoordinateKernel();
} // namespace imaging
