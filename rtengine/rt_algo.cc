/*
 *  This file is part of RawTherapee.
 *
 *  RawTherapee is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  RawTherapee is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with RawTherapee.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

#include "gauss.h"
#include "opthelper.h"
#include "rt_algo.h"
#include "rt_math.h"
#include "sleef.c"

namespace {
float calcBlendFactor(float val, float threshold) {
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    return 1.f / (1.f + xexpf(16.f - 16.f * val / threshold));
}

#ifdef __SSE2__
vfloat calcBlendFactor(vfloat valv, vfloat thresholdv) {
    // sigmoid function
    // result is in ]0;1] range
    // inflexion point is at (x, y) (threshold, 0.5)
    const vfloat onev = F2V(1.f);
    const vfloat c16v = F2V(16.f);
    return onev / (onev + xexpf(c16v - c16v * valv / thresholdv));
}
#endif
}

namespace rtengine
{

void findMinMaxPercentile(const float* data, size_t size, float minPrct, float& minOut, float maxPrct, float& maxOut, bool multithread)
{
    // Copyright (c) 2017 Ingo Weyrich <heckflosse67@gmx.de>
    // We need to find the (minPrct*size) smallest value and the (maxPrct*size) smallest value in data.
    // We use a histogram based search for speed and to reduce memory usage.
    // Memory usage of this method is histoSize * sizeof(uint32_t) * (t + 1) byte,
    // where t is the number of threads and histoSize is in [1;65536].
    // Processing time is O(n) where n is size of the input array.
    // It scales well with multiple threads if the size of the input array is large.
    // The current implementation is not guaranteed to work correctly if size > 2^32 (4294967296).

    assert(minPrct <= maxPrct);

    if (size == 0) {
        return;
    }

    size_t numThreads = 1;
#ifdef _OPENMP
    // Because we have an overhead in the critical region of the main loop for each thread
    // we make a rough calculation to reduce the number of threads for small data size.
    // This also works fine for the minmax loop.
    if (multithread) {
        const size_t maxThreads = omp_get_max_threads();
        while (size > numThreads * numThreads * 16384 && numThreads < maxThreads) {
            ++numThreads;
        }
    }
#endif

    // We need min and max value of data to calculate the scale factor for the histogram
    float minVal = data[0];
    float maxVal = data[0];
#ifdef _OPENMP
    #pragma omp parallel for reduction(min:minVal) reduction(max:maxVal) num_threads(numThreads)
#endif
    for (size_t i = 1; i < size; ++i) {
        minVal = std::min(minVal, data[i]);
        maxVal = std::max(maxVal, data[i]);
    }

    if (std::fabs(maxVal - minVal) == 0.f) { // fast exit, also avoids division by zero in calculation of scale factor
        minOut = maxOut = minVal;
        return;
    }

    // Caution: Currently this works correctly only for histoSize in range[1;65536].
    // For small data size (i.e. thumbnails) we reduce the size of the histogram to the size of data.
    const unsigned int histoSize = std::min<size_t>(65536, size);

    // calculate scale factor to use full range of histogram
    const float scale = (histoSize - 1) / (maxVal - minVal);

    // We need one main histogram
    std::vector<uint32_t> histo(histoSize, 0);

    if (numThreads == 1) {
        // just one thread => use main histogram
        for (size_t i = 0; i < size; ++i) {
            // we have to subtract minVal and multiply with scale to get the data in [0;histosize] range
            histo[static_cast<uint16_t>(scale * (data[i] - minVal))]++;
        }
    } else {
#ifdef _OPENMP
    #pragma omp parallel num_threads(numThreads)
#endif
        {
            // We need one histogram per thread
            std::vector<uint32_t> histothr(histoSize, 0);

#ifdef _OPENMP
            #pragma omp for nowait
#endif
            for (size_t i = 0; i < size; ++i) {
                // we have to subtract minVal and multiply with scale to get the data in [0;histosize] range
                histothr[static_cast<uint16_t>(scale * (data[i] - minVal))]++;
            }

#ifdef _OPENMP
            #pragma omp critical
#endif
            {
                // add per thread histogram to main histogram
#ifdef _OPENMP
                #pragma omp simd
#endif

                for (size_t i = 0; i < histoSize; ++i) {
                    histo[i] += histothr[i];
                }
            }
        }
    }

    size_t k = 0;
    size_t count = 0;

    // find (minPrct*size) smallest value
    const float threshmin = minPrct * size;
    while (count < threshmin) {
        count += histo[k++];
    }

    if (k > 0) { // interpolate
        const size_t count_ = count - histo[k - 1];
        const float c0 = count - threshmin;
        const float c1 = threshmin - count_;
        minOut = (c1 * k + c0 * (k - 1)) / (c0 + c1);
    } else {
        minOut = k;
    }
    // go back to original range
    minOut /= scale;
    minOut += minVal;

    // find (maxPrct*size) smallest value
    const float threshmax = maxPrct * size;
    while (count < threshmax) {
        count += histo[k++];
    }

    if (k > 0) { // interpolate
        const size_t count_ = count - histo[k - 1];
        const float c0 = count - threshmax;
        const float c1 = threshmax - count_;
        maxOut = (c1 * k + c0 * (k - 1)) / (c0 + c1);
    } else {
        maxOut = k;
    }
    // go back to original range
    maxOut /= scale;
    maxOut += minVal;
}

void buildBlendMask(float** luminance, rtengine::JaggedArray<float> &blend, int W, int H, float contrastThreshold, float amount) {

    if(contrastThreshold == 0.f) {
        for(int j = 0; j < H; ++j) {
            for(int i = 0; i < W; ++i) {
                blend[j][i] = amount;
            }
        }
    } else {
        constexpr float scale = 0.0625f / 327.68f;
#ifdef _OPENMP
        #pragma omp parallel
#endif
    {
#ifdef __SSE2__
        const vfloat contrastThresholdv = F2V(contrastThreshold);
        const vfloat scalev = F2V(scale);
        const vfloat amountv = F2V(amount);
#endif
#ifdef _OPENMP
        #pragma omp for schedule(dynamic,16)
#endif

        for(int j = 2; j < H - 2; ++j) {
            int i = 2;
#ifdef __SSE2__
            for(; i < W - 5; i += 4) {
                vfloat contrastv = vsqrtf(SQRV(LVFU(luminance[j][i+1]) - LVFU(luminance[j][i-1])) + SQRV(LVFU(luminance[j+1][i]) - LVFU(luminance[j-1][i])) +
                                          SQRV(LVFU(luminance[j][i+2]) - LVFU(luminance[j][i-2])) + SQRV(LVFU(luminance[j+2][i]) - LVFU(luminance[j-2][i]))) * scalev;

                STVFU(blend[j][i], amountv * calcBlendFactor(contrastv, contrastThresholdv));
            }
#endif
            for(; i < W - 2; ++i) {

                float contrast = sqrtf(rtengine::SQR(luminance[j][i+1] - luminance[j][i-1]) + rtengine::SQR(luminance[j+1][i] - luminance[j-1][i]) + 
                                       rtengine::SQR(luminance[j][i+2] - luminance[j][i-2]) + rtengine::SQR(luminance[j+2][i] - luminance[j-2][i])) * scale;

                blend[j][i] = amount * calcBlendFactor(contrast, contrastThreshold);
            }
        }
#ifdef _OPENMP
        #pragma omp single
#endif
        {
            // upper border
            for(int j = 0; j < 2; ++j) {
                for(int i = 2; i < W - 2; ++i) {
                    blend[j][i] = blend[2][i];
                }
            }
            // lower border
            for(int j = H - 2; j < H; ++j) {
                for(int i = 2; i < W - 2; ++i) {
                    blend[j][i] = blend[H-3][i];
                }
            }
            for(int j = 0; j < H; ++j) {
                // left border
                blend[j][0] = blend[j][1] = blend[j][2];
                // right border
                blend[j][W - 2] = blend[j][W - 1] = blend[j][W - 3];
            }
        }
        // blur blend mask to smooth transitions
        gaussianBlur(blend, blend, W, H, 2.0);
    }
    }
}
}
