// qalgorithms_qpeaks.cpp
//
// internal
#include "qalgorithms_qpeaks.h"
#include "qalgorithms_utils.h"
#include "qalgorithms_global_vars.h"
#include "qalgorithms_datatype_peak.h"

#include <algorithm>
#include <cassert>
#include <numeric>
#include <cmath>
#include <array>

// external

namespace qAlgorithms
{

#pragma region "pass to qBinning"
    CentroidedData passToBinning(std::vector<std::vector<CentroidPeak>> &allPeaks, std::vector<unsigned int> addEmpty)
    {
        // initialise empty vector with enough room for all scans - centroids[0] must remain empty
        std::vector<std::vector<qCentroid>> centroids(allPeaks.size() + 1, std::vector<qCentroid>(0));
        int totalCentroids = 0;
        int scanRelative = 0;
        // std::vector<qCentroid> scan(0);
        for (size_t i = 0; i < allPeaks.size(); ++i)
        {
            if (addEmpty[i] != 0)
            {
                for (size_t j = 0; j < addEmpty[i]; j++)
                {
                    centroids.push_back(std::vector<qCentroid>(0));
                    scanRelative++;
                }
            }

            if (!allPeaks[i].empty())
            {
                ++scanRelative; // scans start at 1
                // sort the peaks in ascending order of retention time
                std::sort(allPeaks[i].begin(), allPeaks[i].end(), [](const CentroidPeak a, const CentroidPeak b)
                          { return a.scanNumber < b.scanNumber; });
                for (size_t j = 0; j < allPeaks[i].size(); ++j)
                {
                    auto &peak = allPeaks[i][j];
                    qCentroid F = qCentroid{peak.mzUncertainty, peak.mz, scanRelative, peak.area, peak.height, peak.dqsCen};
                    centroids[scanRelative].push_back(F);
                    ++totalCentroids;
                }
            }
            else
            {
                centroids.push_back(std::vector<qCentroid>(0));
                scanRelative++;
            }
        }
        // the first scan must be empty for compatibility with qBinning
        assert(centroids[0].empty());

        for (size_t i = 1; i < centroids.size(); i++)
        {
            std::sort(centroids[i].begin(), centroids[i].end(), [](qCentroid lhs, qCentroid rhs)
                      { return lhs.mz < rhs.mz; });
        }

        return CentroidedData{centroids, totalCentroids};
    }
#pragma endregion "pass to qBinning"

#pragma region "initialize"
    //  float INV_ARRAY[64][6]; // array to store the 6 unique values of the inverse matrix for each scale

    const std::array<float, 384> initialize()
    {
        std::array<float, 384> invArray;
        // init invArray
        float XtX_00 = 1.f;
        float XtX_02 = 0.f;
        float XtX_11 = 0.f;
        float XtX_12 = 0.f;
        float XtX_13 = 0.f;
        float XtX_22 = 0.f;
        for (int i = 1; i < 64; ++i)
        {
            XtX_00 += 2.f;
            XtX_02 += i * i;
            XtX_11 = XtX_02 * 2.f;
            XtX_13 += i * i * i;
            XtX_12 = -XtX_13;
            XtX_22 += i * i * i * i;

            float L_00 = std::sqrt(XtX_00);
            float L_11 = std::sqrt(XtX_11);
            float L_20 = XtX_02 / L_00;
            float L_21 = XtX_12 / L_11;
            float L_20sq = L_20 * L_20;
            float L_21sq = L_21 * L_21;
            float L_22 = std::sqrt(XtX_22 - L_20sq - L_21sq);
            float L_32 = 1 / L_22 * (-L_20sq + L_21sq);
            float L_33 = std::sqrt(XtX_22 - L_20sq - L_21sq - L_32 * L_32);

            float inv_00 = 1.f / L_00;
            float inv_11 = 1.f / L_11;
            float inv_22 = 1.f / L_22;
            float inv_33 = 1.f / L_33;
            float inv_20 = -L_20 * inv_00 / L_22;
            float inv_30 = -(L_20 * inv_00 + L_32 * inv_20) / L_33;
            float inv_21 = -L_21 * inv_11 / L_22;
            float inv_31 = -(-L_21 * inv_11 + L_32 * inv_21) / L_33;
            float inv_32 = -L_32 * inv_22 / L_33;

            invArray[i * 6 + 0] = inv_00 * inv_00 + inv_20 * inv_20 + inv_30 * inv_30; // cell: 0,0
            invArray[i * 6 + 1] = inv_22 * inv_20 + inv_32 * inv_30;                   // cell: 0,2
            invArray[i * 6 + 2] = inv_11 * inv_11 + inv_21 * inv_21 + inv_31 * inv_31; // cell: 1,1
            invArray[i * 6 + 3] = -inv_31 * inv_33;                                    // cell: 1,2
            invArray[i * 6 + 4] = inv_33 * inv_33;                                     // cell: 2,2
            invArray[i * 6 + 5] = inv_32 * inv_33;                                     // cell: 2,3
        }
        return invArray;
    }
#pragma endregion "initialize"

#pragma region "find centroids"
    std::vector<CentroidPeak> findCentroids(
        treatedData &treatedData,
        const int scanNumber,
        const float retentionTime)
    {
        std::vector<CentroidPeak> all_peaks;
        for (auto it_separators = treatedData.separators.begin(); it_separators != treatedData.separators.end() - 1; it_separators++)
        {
            const int n = *(it_separators + 1) - *it_separators; // calculate the number of data points in the block
            if (n <= 512)
            {
                // STATIC APPROACH
                float Y[512];                                  // measured y values
                float Ylog[512];                               // log-transformed measured y values
                float X[512];                                  // measured x values
                bool df[512];                                  // degree of freedom vector, 0: interpolated, 1: measured
                ValidRegression_static validRegressions[2048]; // array of valid regressions with default initialization, i.e., random states
                int validRegressionsIndex = 0;                 // index of the valid regressions

                // iterators to the start of the data
                const auto y_start = Y;
                const auto ylog_start = Ylog;
                const auto mz_start = X;
                const auto df_start = df;

                int i = 0;
                for (int idx = *it_separators; idx < *(it_separators + 1); idx++)
                {
                    Y[i] = treatedData.dataPoints[idx].y;
                    X[i] = treatedData.dataPoints[idx].x;
                    df[i] = treatedData.dataPoints[idx].df;
                    i++;
                }

                // perform log-transform on Y
                std::transform(y_start, y_start + n, ylog_start, [](float y)
                               { return std::log(y); });
                runningRegression_static(y_start, ylog_start, df_start, n, validRegressions, validRegressionsIndex);
                if (validRegressionsIndex == 0)
                {
                    continue; // no valid peaks
                }
                createCentroidPeaks(all_peaks, validRegressions, nullptr, validRegressionsIndex, y_start, mz_start, df_start, scanNumber);
            }
            else
            {
                // DYNAMIC APPROACH
                float *Y = new float[n];
                float *Ylog = new float[n];
                float *X = new float[n];
                bool *df = new bool[n];
                std::vector<ValidRegression_static> validRegressions;

                // iterator to the start
                const auto y_start = Y;
                const auto ylog_start = Ylog;
                const auto mz_start = X;
                const auto df_start = df;

                int i = 0;
                for (int idx = *it_separators; idx < *(it_separators + 1); idx++)
                {
                    Y[i] = treatedData.dataPoints[idx].y;
                    X[i] = treatedData.dataPoints[idx].x;
                    df[i] = treatedData.dataPoints[idx].df;
                    i++;
                }

                // perform log-transform on Y
                std::transform(y_start, y_start + n, ylog_start, [](float y)
                               { return std::log(y); });
                runningRegression(y_start, ylog_start, df_start, n, validRegressions);
                if (validRegressions.empty())
                {
                    continue; // no valid peaks
                }
                createCentroidPeaks(all_peaks, nullptr, &validRegressions, validRegressions.size(), y_start, mz_start, df_start, scanNumber);
                delete[] Y;
                delete[] Ylog;
                delete[] X;
                delete[] df;
            }
        }
        return all_peaks;
    }
#pragma endregion "find centroids"

#pragma region "find peaks"
    void findPeaks(
        std::vector<FeaturePeak> &all_peaks,
        treatedData &treatedData)
    {
        for (auto it_separators = treatedData.separators.begin(); it_separators != treatedData.separators.end() - 1; it_separators++)
        {
            const int n = *(it_separators + 1) - *it_separators; // calculate the number of data points in the block
            assert(n > 0);                                       // check if the number of data points is greater than 0
            if (n <= 512)
            {
                // STATIC APPROACH
                float Y[512];                                  // measured y values
                float Ylog[512];                               // log-transformed measured y values
                float X[512];                                  // measured x values
                bool df[512];                                  // degree of freedom vector, 0: interpolated, 1: measured
                float mz[512];                                 // measured mz values
                float dqs_cen[512];                            // measured dqs values
                float dqs_bin[512];                            // measured dqs values
                ValidRegression_static validRegressions[2048]; // array of valid regressions with default initialization, i.e., random states
                int validRegressionsIndex = 0;                 // index of the valid regressions

                // iterators to the start of the data
                const auto y_start = Y;
                const auto ylog_start = Ylog;
                const auto rt_start = X;
                const auto df_start = df;
                const auto mz_start = mz;
                const auto dqs_cen_start = dqs_cen;
                const auto dqs_bin_start = dqs_bin;
                int i = 0;
                for (int idx = *it_separators; idx < *(it_separators + 1); idx++)
                {
                    Y[i] = treatedData.dataPoints[idx].y;
                    X[i] = treatedData.dataPoints[idx].x;
                    df[i] = treatedData.dataPoints[idx].df;
                    mz[i] = treatedData.dataPoints[idx].mz;
                    dqs_cen[i] = treatedData.dataPoints[idx].dqsCentroid;
                    dqs_bin[i] = treatedData.dataPoints[idx].dqsBinning;
                    i++;
                }

                // perform log-transform on Y
                std::transform(y_start, y_start + n, ylog_start, [](float y)
                               { return std::log(y); });
                runningRegression_static(y_start, ylog_start, df_start, n, validRegressions, validRegressionsIndex);
                if (validRegressionsIndex == 0)
                {
                    continue; // no valid peaks
                }
                createFeaturePeaks(all_peaks, validRegressions, nullptr, validRegressionsIndex, y_start, mz_start,
                                   rt_start, df_start, dqs_cen_start, dqs_bin_start);
            }
            else
            {
                // DYNAMIC APPROACH
                float *Y = new float[n];
                float *Ylog = new float[n];
                float *X = new float[n];
                bool *df = new bool[n];
                float *mz = new float[n];
                float *dqs_cen = new float[n];
                float *dqs_bin = new float[n];
                std::vector<ValidRegression_static> validRegressions;

                // iterator to the start
                const auto y_start = Y;
                const auto ylog_start = Ylog;
                const auto rt_start = X;
                const auto df_start = df;
                const auto mz_start = mz;
                const auto dqs_cen_start = dqs_cen;
                const auto dqs_bin_start = dqs_bin;

                int i = 0;
                assert(n == *(it_separators + 1) - *it_separators);
                for (int idx = *it_separators; idx < *(it_separators + 1); idx++)
                {
                    Y[i] = treatedData.dataPoints[idx].y;
                    X[i] = treatedData.dataPoints[idx].x;
                    df[i] = treatedData.dataPoints[idx].df;
                    mz[i] = treatedData.dataPoints[idx].mz;
                    dqs_cen[i] = treatedData.dataPoints[idx].dqsCentroid;
                    dqs_bin[i] = treatedData.dataPoints[idx].dqsBinning;
                    i++;
                }

                // perform log-transform on Y
                std::transform(y_start, y_start + n, ylog_start, [](float y)
                               { return std::log(y); });
                runningRegression(y_start, ylog_start, df_start, n, validRegressions);
                if (validRegressions.empty())
                {
                    continue; // no valid peaks
                }
                createFeaturePeaks(all_peaks, nullptr, &validRegressions, validRegressions.size(), y_start,
                                   mz_start, rt_start, df_start, dqs_cen_start, dqs_bin_start);
                delete[] Y;
                delete[] Ylog;
                delete[] X;
                delete[] df;
                delete[] mz;
                delete[] dqs_cen;
                delete[] dqs_bin;
            }
        }
    }
#pragma endregion "find peaks"

#pragma region "running regression"
    void runningRegression(
        const float *y_start,
        const float *ylog_start,
        const bool *df_start,
        const int n, // number of data points
        std::vector<ValidRegression_static> &validRegressions)
    {
        const int maxScale = std::min(GLOBAL_MAXSCALE, (int)(n - 1) / 2);

        // @todo is this more efficient than just reserving a relatively large amount?
        int sum = 0;
        for (int i = 4; i <= GLOBAL_MAXSCALE * 2; i += 2)
        {
            sum += std::max(0, n - i);
        }

        validRegressions.reserve(sum);
        for (int scale = 2; scale <= maxScale; scale++)
        {
            const int k = 2 * scale + 1;                  // window size
            const int n_segments = n - k + 1;             // number of segments, i.e. regressions considering the array size
            __m128 *beta = new __m128[n_segments];        // coefficients matrix
            convolve_dynamic(scale, ylog_start, n, beta); // do the regression
            validateRegressions(beta, n_segments, y_start, ylog_start, df_start, scale, validRegressions);
            // validRegressions.push_back(
            //     validateRegressions(beta, n_segments, y_start, ylog_start, df_start, scale, validRegressions));
        } // end for scale loop
        mergeRegressionsOverScales(validRegressions, y_start, df_start);
    } // end runningRegression
#pragma endregion "running regression"

#pragma region "running regression static"
    void runningRegression_static(
        const float *y_start,
        const float *ylog_start,
        const bool *df_start,
        const int n,
        ValidRegression_static *validRegressions,
        int &validRegressionsIndex)
    {
        // @todo return a vector of valid regressions
        int maxScale = std::min(GLOBAL_MAXSCALE, (int)(n - 1) / 2);

        for (int scale = 2; scale <= maxScale; scale++)
        {
            validateRegressions_static(n, y_start, ylog_start, df_start, scale, validRegressionsIndex, validRegressions);
        } // end for scale loop
        mergeRegressionsOverScales_static(validRegressions, validRegressionsIndex, y_start, df_start);
    }
#pragma endregion "running regression static"

#pragma region validateRegressions
    void validateRegressions(
        const __m128 *beta,      // coefficients matrix
        const int n_segments,    // number of segments, i.e. regressions
        const float *y_start,    // pointer to the start of the Y matrix
        const float *ylog_start, // pointer to the start of the Ylog matrix
        const bool *df_start,    // degree of freedom vector, 0: interpolated, 1: measured
        const int scale,         // scale, i.e., the number of data points in a half window excluding the center point
        std::vector<ValidRegression_static> &validRegressions)
    {
        std::vector<ValidRegression_static> validRegsTmp; // temporary vector to store valid regressions <index, apex_position>

        // iterate columwise over the coefficients matrix beta
        for (int i = 0; i < n_segments; i++)
        {
            if (calcDF(df_start, i, 2 * scale + i) > 4)
            {
                const __m128 coeff = beta[i]; // coefficient register from beta @ i
                ValidRegression_static selectRegression = makeValidRegression(i, scale, df_start, y_start,
                                                                              ylog_start, coeff);
                if (selectRegression.isValid)
                {
                    validRegsTmp.push_back(selectRegression);
                }
            }
        }
        // early return if no or only one valid peak
        if (validRegsTmp.size() < 2)
        {
            if (validRegsTmp.empty())
            {
                return; // no valid peaks
            }
            validRegressions.push_back(std::move(validRegsTmp[0]));
            return; // not enough peaks to form a group
        }
        /*
          Grouping:
          This block of code implements the grouping. It groups the valid peaks based
          on the apex positions. Peaks are defined as similar, i.e., members of the
          same group, if they fullfill at least one of the following conditions:
          - The difference between two peak apexes is less than 4. (Nyquist Shannon
          Sampling Theorem, separation of two maxima)
          - At least one apex of a pair of peaks is within the window of the other peak.
          (Overlap of two maxima)
        */

        // vector with the access pattern [2*i] for start and [2*i + 1] for end point of a regression group
        // @todo first group always starts at 0, so the vector can have the access pattern vec[i - 1] + 1 : vec[i]
        std::vector<int> startEndGroups;
        startEndGroups.reserve(validRegsTmp.size());

        size_t prev_i = 0;

        for (size_t i = 0; i < validRegsTmp.size() - 1; i++)
        {
            // check if the difference between two peak apexes is less than 4 (Nyquist Shannon
            // Sampling Theorem, separation of two maxima), or if the apex of a peak is within
            // the window of the other peak (Overlap of two maxima)
            if (std::abs(validRegsTmp[i].apex_position - validRegsTmp[i + 1].apex_position) > 4 &&
                validRegsTmp[i].apex_position < validRegsTmp[i + 1].left_limit &&
                validRegsTmp[i + 1].apex_position > validRegsTmp[i].right_limit)
            {
                // the two regressions differ, i.e. create a new group
                startEndGroups.push_back(prev_i);
                startEndGroups.push_back(i);
                prev_i = i + 1;
            }
        }
        startEndGroups.push_back(prev_i);
        startEndGroups.push_back(validRegsTmp.size() - 1); // last group ends with index of the last element

        /*
          Survival of the Fittest Filter:
          This block of code implements the survival of the fittest filter. It selects the peak with
          the lowest mean squared error (MSE) as the representative of the group. If the group contains
          only one peak, the peak is directly pushed to the valid regressions. If the group contains
          multiple peaks, the peak with the lowest MSE is selected as the representative of the group
          and pushed to the valid regressions.
        */
        for (size_t groupIdx = 0; groupIdx < startEndGroups.size(); groupIdx += 2)
        {
            if (startEndGroups[groupIdx] == startEndGroups[groupIdx + 1])
            { // already isolated peak => push to valid regressions
                int regIdx = startEndGroups[groupIdx];
                validRegressions.push_back(std::move(validRegsTmp[regIdx]));
            }
            else
            { // survival of the fittest based on mse between original data and reconstructed (exp transform of regression)
                assert(startEndGroups[groupIdx] != startEndGroups[groupIdx + 1]);
                auto bestRegIdx = findBestRegression(y_start, validRegsTmp, df_start,
                                                     startEndGroups[groupIdx], startEndGroups[groupIdx + 1]);

                ValidRegression_static bestReg = validRegsTmp[bestRegIdx.first];
                bestReg.mse = bestRegIdx.second;
                validRegressions.push_back(std::move(bestReg));
            }
        } // end for loop (group in vector of groups)
    } // end validateRegressions
#pragma endregion validateRegressions

#pragma region "validate regressions static"
    void validateRegressions_static(
        const int n,
        const float *y_start,
        const float *ylog_start,
        const bool *df_start,
        const int scale,
        int &validRegressionsIndex,
        ValidRegression_static *validRegressions)
    {
        ValidRegression_static validRegressionsTmp[512];                      // temporary vector to store valid regressions initialized with random states
        int validRegressionsIndexTmp = 0;                                     // index of the valid regressions
        std::array<__m128, 512> beta = convolve_static(scale, ylog_start, n); // do the regression
        const int n_segments = n - 2 * scale;                                 // number of segments, i.e. regressions considering the number of data points

        // iterate columwise over the coefficients matrix beta
        for (int i = 0; i < n_segments; i++)
        {
            if (calcDF(df_start, i, 2 * scale + i) > 4)
            {
                const __m128 coeff = beta[i]; // coefficient register from beta @ i
                // @todo add test for coeff = 0
                ValidRegression_static selectRegression = makeValidRegression(i, scale, df_start, y_start,
                                                                              ylog_start, coeff);
                if (selectRegression.isValid)
                {
                    validRegressionsTmp[validRegressionsIndexTmp] = selectRegression;
                    validRegressionsIndexTmp++;
                }
            }
        }
        // early return if no or only one valid peak
        if (validRegressionsIndexTmp < 2)
        {
            if (validRegressionsIndexTmp == 1)
            {
                *(validRegressions + validRegressionsIndex) = validRegressionsTmp[0];
                validRegressionsIndex++;
            }
            return; // not enough peaks to form a group
        }

        // lambda function to process a group of valid regressions, i.e., find the peak with the lowest MSE and push it to the valid regressions
        auto processGroup = [&validRegressions, &validRegressionsTmp, &validRegressionsIndex, y_start, df_start](int i, int start_index_group)
        {
            int group_size = i - start_index_group + 1; // size of the group
            if (group_size == 1)
            { // single item group
                *(validRegressions + validRegressionsIndex) = validRegressionsTmp[start_index_group];
                validRegressionsIndex++;
            }
            else
            { // multiple item group
                // survival of the fittest based on mse between original data and reconstructed (exp transform of regression)
                ValidRegression_static *regression_start = &validRegressionsTmp[start_index_group]; // start of the group
                calcExtendedMse_static(y_start, regression_start, group_size, df_start);            // calculate the extended MSE
                for (int j = 0; j < group_size; j++)
                {
                    if (regression_start[j].isValid)
                    {
                        *(validRegressions + validRegressionsIndex) = regression_start[j]; // push the peak to the valid regressions
                        validRegressionsIndex++;
                    }
                }
            }
        };

        /*
          Grouping:
          This block of code implements the grouping. It groups the valid peaks based on the apex positions.
          Peaks are defined as similar, i.e., members of the same group, if they fullfill at least one of the following conditions:
          - The difference between two peak apexes is less than 4. (Nyquist Shannon Sampling Theorem, separation of two maxima)
          - At least one apex of a pair of peaks is within the window of the other peak. (Overlap of two maxima)
        */
        const int last_valid_index = validRegressionsIndexTmp - 1; // last valid index in the temporary vector of valid regressions
        int start_index_group = 0;                                 // start index of the group
        for (int i = 0; i < last_valid_index; i++)
        {
            if (
                std::abs(validRegressionsTmp[i].apex_position - validRegressionsTmp[i + 1].apex_position) > 4 && // difference between two peak apexes is greater than 4 (Nyquist Shannon Sampling Theorem, separation of two maxima)
                validRegressionsTmp[i].apex_position < validRegressionsTmp[i + 1].left_limit &&                  // left peak is not within the window of the right peak
                validRegressionsTmp[i + 1].apex_position > validRegressionsTmp[i].right_limit)                   // right peak is not within the window of the left peak
            {                                                                                                    // the two regressions differ,
                processGroup(i, start_index_group);                                                              // process the group
                start_index_group = i + 1;                                                                       // start index of the next group
                if (i == last_valid_index - 1)
                { // if last round compare, add the last regression to the valid regressions
                    *(validRegressions + validRegressionsIndex) = validRegressionsTmp[i + 1];
                    validRegressionsIndex++;
                }
            }
            else
            {
                if (i == last_valid_index - 1)
                {                                                      // if last round compare
                    processGroup(last_valid_index, start_index_group); // process the group
                }
            }
        }
    }
#pragma endregion "validate regressions static"

#pragma region "validate regression test series"

    ValidRegression_static makeValidRegression(
        const int i,
        const int scale,
        const bool *df_start,
        const float *y_start,
        const float *ylog_start,
        const __m128 coeff)
    { // @todo order by effort to calculate
        RegCoeffs replacer;
        replacer.b0 = ((float *)&coeff)[0];
        replacer.b1 = ((float *)&coeff)[1];
        replacer.b2 = ((float *)&coeff)[2];
        replacer.b3 = ((float *)&coeff)[3];
        /*
          Apex and Valley Position Filter:
          This block of code implements the apex and valley position filter.
          It calculates the apex and valley positions based on the coefficients
          matrix B. If the apex is outside the data range, the loop continues
          to the next iteration. If the apex and valley positions are too close
          to each other, the loop continues to the next iteration.
        */
        // check if beta 2 or beta 3 is zero
        if (replacer.b2 == 0.0f || replacer.b3 == 0.0f)
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // invalid case
        }

        float valley_position;
        float apex_position = 0.f;
        // no easy replace
        if (!calcApexAndValleyPos(coeff, scale, apex_position, valley_position))
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // invalid apex and valley positions
        }

        /*
          Degree of Freedom Filter:
          This block of code implements the degree of freedom filter. It calculates the
          degree of freedom based df vector. If the degree of freedom is less than 5,
          the loop continues to the next iteration. The value 5 is chosen as the
          minimum number of data points required to fit a quadratic regression model.
        */
        unsigned int left_limit = (valley_position < 0) ? std::max(i, static_cast<int>(valley_position) + i + scale) : i;
        unsigned int right_limit = (valley_position > 0) ? std::min(i + 2 * scale, static_cast<int>(valley_position) + i + scale) : i + 2 * scale;

        int df_sum = calcDF(df_start, left_limit, right_limit); // degrees of freedom considering the left and right limits
        if (df_sum < 5)
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // degree of freedom less than 5; i.e., less then 5 measured data points
        }

        /*
          Area Pre-Filter:
          This test is used to check if the later-used arguments for exp and erf
          functions are within the valid range, i.e., |x^2| < 25. If the test fails,
          the loop continues to the next iteration. @todo why 25?
          x is in this case -apex_position * b1 / 2 and -valley_position * b1 / 2.
        */
        if (apex_position * replacer.b1 > 50 || valley_position * replacer.b1 < -50)
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // invalid area pre-filter
        }

        /*
          Apex to Edge Filter:
          This block of code implements the apex to edge filter. It calculates
          the ratio of the apex signal to the edge signal and ensures that the
          ratio is greater than 2. This is a pre-filter for later
          signal-to-noise ratio checkups.
        */
        float apexToEdge = calcApexToEdge(apex_position, scale, i, y_start);
        if (!(apexToEdge > 2))
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // invalid apex to edge ratio
        }

        /*
          Quadratic Term Filter:
          This block of code implements the quadratic term filter. It calculates
          the mean squared error (MSE) between the predicted and actual values.
          Then it calculates the t-value for the quadratic term. If the t-value
          is less than the corresponding value in the T_VALUES, the quadratic
          term is considered statistically insignificant, and the loop continues
          to the next iteration.
        */
        float mse = calcSSE(-scale, scale, replacer, ylog_start + i) / (df_sum - 4); // mean squared error

        if (!isValidQuadraticTerm(replacer, scale, mse, df_sum))
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // statistical insignificance of the quadratic term
        }
        /*
          Height Filter:
          This block of code implements the height filter. It calculates the height
          of the peak based on the coefficients matrix B. Then it calculates the
          uncertainty of the height based on the Jacobian matrix and the variance-covariance
          matrix of the coefficients. If the height is statistically insignificant,
          the loop continues to the next iteration.
        */

        float uncertainty_height = calcPeakHeightUncert(mse, scale, apex_position);
        if (1 / uncertainty_height <= T_VALUES[df_sum - 5]) // statistical significance of the peak height
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg;
        }
        // at this point without height, i.e., to get the real uncertainty
        // multiply with height later. This is done to avoid exp function at this point
        if (!isValidPeakHeight(mse, scale, apex_position, valley_position, df_sum, apexToEdge))
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // statistical insignificance of the height
        }

        /*
          Area Filter:
          This block of code implements the area filter. It calculates the Jacobian
          matrix for the peak area based on the coefficients matrix B. Then it calculates
          the uncertainty of the peak area based on the Jacobian matrix. If the peak
          area is statistically insignificant, the loop continues to the next iteration.
          NOTE: this function does not consider b0: i.e. to get the real uncertainty and
          area multiply both with Exp(b0) later. This is done to avoid exp function at this point
        */
        float area = 0.f; // peak area
        float uncertainty_area = 0.f;
        if (!isValidPeakArea(replacer, mse, scale, df_sum, area, uncertainty_area))
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // statistical insignificance of the area
        }

        /*
          Chi-Square Filter:
          This block of code implements the chi-square filter. It calculates the chi-square
          value based on the weighted chi squared sum of expected and measured y values in
          the exponential domain. If the chi-square value is less than the corresponding
          value in the CHI_SQUARES, the loop continues to the next iteration.
        */
        float chiSquare = calcSSE(-scale, scale, replacer, y_start + i, true, true);
        if (chiSquare < CHI_SQUARES[df_sum - 5])
        {
            ValidRegression_static badReg;
            badReg.isValid = false;
            return badReg; // statistical insignificance of the chi-square value
        }
        float uncertainty_pos = calcUncertaintyPos(mse, replacer, apex_position, scale);

        return ValidRegression_static{
            replacer,
            coeff,
            i + scale,
            scale,
            df_sum - 4, // @todo add explanation for -4
            apex_position + i + scale,
            0, // mse @todo
            true,
            left_limit,
            right_limit,
            area,
            uncertainty_area,
            uncertainty_pos,
            uncertainty_height};
    }
#pragma endregion "validate regression test series"

#pragma region mergeRegressionsOverScales
    void mergeRegressionsOverScales(
        std::vector<ValidRegression_static> &validRegressions,
        const float *y_start,
        const bool *df_start)
    {
        if (validRegressions.empty())
        {
            return; // no valid peaks at all
        }

        if (validRegressions.size() == 1)
        {
            return; // only one valid regression, i.e., no further grouping required; validRegressions is already fine.
        }

        /*
          Grouping Over Scales:
          This block of code implements the grouping over scales. It groups the valid
          peaks based on the apex positions. Peaks are defined as similar, i.e.,
          members of the same group, if they fullfill at least one of the following conditions:
          - The difference between two peak apexes is less than 4. (Nyquist Shannon Sampling Theorem, separation of two maxima)
          - At least one apex of a pair of peaks is within the window of the other peak. (Overlap of two maxima)
        */
        // iterate over the validRegressions vector
        for (auto it_new_peak = validRegressions.begin(); it_new_peak != validRegressions.end(); ++it_new_peak)
        {
            const double left_limit = it_new_peak->left_limit;          // left limit of the current peak regression window in the Y matrix
            const double right_limit = it_new_peak->right_limit;        // right limit of the current peak regression window in the Y matrix
            double grpMSE = 0;                                          // group mean squared error
            int grpDF = 0;                                              // group degree of freedom
            int numPeaksInGroup = 0;                                    // number of peaks in the group
            auto it_ref_peak = validRegressions.begin();                // iterator for the reference peak, i.e., a valid peak to compare with the new peak
            std::vector<decltype(it_ref_peak)> validRegressionsInGroup; // vector of iterators

            // iterate over the validRegressions vector till the new peak
            for (; it_ref_peak < it_new_peak; ++it_ref_peak)
            {
                if (!it_ref_peak->isValid)
                {
                    continue; // skip the invalid peaks
                }
                if ( // check for the overlap of the peaks
                    (
                        it_ref_peak->apex_position > left_limit &&   // ref peak matches the left limit
                        it_ref_peak->apex_position < right_limit) || // ref peak matches the right limit
                    (
                        it_new_peak->apex_position > it_ref_peak->left_limit && // new peak matches the left limit
                        it_new_peak->apex_position < it_ref_peak->right_limit)) // new peak matches the right limit
                {
                    if (it_ref_peak->mse == 0.0)
                    { // calculate the mse of the ref peak
                        it_ref_peak->mse = calcSSE(
                                               (it_ref_peak->left_limit) - it_ref_peak->index_x0,  // left limit of the ref peak
                                               (it_ref_peak->right_limit) - it_ref_peak->index_x0, // right limit of the ref peak
                                               it_ref_peak->newCoeffs,                             // regression coefficients
                                               y_start + (int)it_ref_peak->left_limit,             // pointer to the start of the Y matrix
                                               true) /
                                           it_ref_peak->df;
                    }
                    grpDF += it_ref_peak->df;                     // add the degree of freedom
                    grpMSE += it_ref_peak->mse * it_ref_peak->df; // add the sum of squared errors
                    numPeaksInGroup++;                            // increment the number of peaks in the group
                    // add the iterator of the ref peak to a vector of iterators
                    validRegressionsInGroup.push_back(it_ref_peak);
                }
            } // end for loop, inner loop, it_ref_peak

            if (grpDF > 0)
            {
                grpMSE /= grpDF;
            }
            else
            {
                continue; // no peaks in the group, i.e., the current peak stays valid
            }

            if (it_new_peak->mse == 0.0)
            { // calculate the mse of the current peak
                it_new_peak->mse = calcSSE(
                                       (it_new_peak->left_limit) - it_new_peak->index_x0,  // left limit of the new peak
                                       (it_new_peak->right_limit) - it_new_peak->index_x0, // right limit of the new peak
                                       it_new_peak->newCoeffs,                             // regression coefficients
                                       y_start + (int)it_new_peak->left_limit,             // pointer to the start of the Y matrix
                                       true) /
                                   it_new_peak->df;
            }

            if (numPeaksInGroup == 1)
            {
                // calculate the extended MSE using the current peak and the ref peak and set the worse one to invalid
                // create a temporary std::vector<std::unique_ptr<validRegression>> with the new peak and the ref peak
                std::vector<ValidRegression_static> tmpRegressions;
                tmpRegressions.push_back(std::move(*it_new_peak));
                tmpRegressions.push_back(std::move(validRegressionsInGroup[0][0]));
                calcExtendedMse(y_start, tmpRegressions, df_start);
                // Move the unique_ptrs back to validRegressionsInGroup
                validRegressionsInGroup[0][0] = std::move(tmpRegressions[1]);
                *it_new_peak = std::move(tmpRegressions[0]);
                continue;
            }
            if (it_new_peak->mse < grpMSE)
            {
                // Set isValid to false for the candidates from the group
                for (auto it_ref_peak : validRegressionsInGroup)
                {
                    it_ref_peak->isValid = false;
                }
            }
            else
            { // Set isValid to false for the current peak
                it_new_peak->isValid = false;
            }
        } // end for loop, outer loop, it_current_peak

        // Remove the peaks with isValid == false from the validRegressions
        validRegressions.erase(std::remove_if(validRegressions.begin(), validRegressions.end(),
                                              [](const auto &peak)
                                              { return !peak.isValid; }),
                               validRegressions.end());
    } // end mergeRegressionsOverScales
#pragma endregion mergeRegressionsOverScales

#pragma region "merge regressions over scales static"
    void mergeRegressionsOverScales_static(
        ValidRegression_static *validRegressions,
        const int n_regressions,
        const float *y_start,
        // const float *ylog_start,
        const bool *df_start)
    {
        if (n_regressions == 0)
        {
            return; // no valid peaks at all
        }

        if (n_regressions == 1)
        {
            return; // only one valid regression, i.e., no further grouping required; validRegressions is already fine.
        }

        /*
          Grouping Over Scales:
          This block of code implements the grouping over scales. It groups the valid peaks based
          on the apex positions. Peaks are defined as similar, i.e., members of the same group,
          if they fullfill at least one of the following conditions:
          - The difference between two peak apexes is less than 4. (Nyquist Shannon Sampling Theorem, separation of two maxima)
          - At least one apex of a pair of peaks is within the window of the other peak. (Overlap of two maxima)
        */
        // iterate over the validRegressions vector
        for (int i_new_peak = 1; i_new_peak < n_regressions; i_new_peak++)
        {
            if (!validRegressions[i_new_peak].isValid)
            {
                continue; // skip the invalid peaks
            }
            const int current_scale = validRegressions[i_new_peak].scale;                   // scale of the current peak
            const int current_left_limit = validRegressions[i_new_peak].left_limit;         // left limit of the current peak regression window in the Y matrix
            const int current_right_limit = validRegressions[i_new_peak].right_limit;       // right limit of the current peak regression window in the Y matrix
            const float current_apex_position = validRegressions[i_new_peak].apex_position; // apex position of the current peak
            std::vector<int> validRegressionsInGroup;                                       // vector of indices of valid regressions in the group
            // iterate over the validRegressions vector till the new peak
            for (int i_ref_peak = 0; i_ref_peak < i_new_peak; i_ref_peak++)
            {
                if (!validRegressions[i_ref_peak].isValid)
                {
                    continue; // skip the invalid peaks
                }
                if (validRegressions[i_ref_peak].scale >= current_scale)
                {
                    break; // skip the peaks with a scale greater or equal to the current scale
                }
                // check for overlaps
                if (
                    (
                        validRegressions[i_ref_peak].apex_position > current_left_limit &&   // ref peak matches the left limit
                        validRegressions[i_ref_peak].apex_position < current_right_limit) || // ref peak matches the right limit
                    (
                        current_apex_position > validRegressions[i_ref_peak].left_limit && // new peak matches the left limit
                        current_apex_position < validRegressions[i_ref_peak].right_limit)) // new peak matches the right limit
                {                                                                          // overlap detected
                    validRegressionsInGroup.push_back(i_ref_peak);                         // add the index of the ref peak to the vector of indices
                    continue;                                                              // continue with the next ref peak
                }
            } // end for loop, inner loop, i_ref_peak

            if (validRegressionsInGroup.size() == 0)
            {
                continue; // no peaks in the group, i.e., the current peak stays valid
            }

            if (validRegressionsInGroup.size() == 1)
            { // comparison of two regressions just with different scale.
                calcExtendedMsePair(y_start, &validRegressions[validRegressionsInGroup[0]], &validRegressions[i_new_peak], df_start);
                continue; // continue with the next new peak
            }

            // comparison of the new regression (high) with multiple ref regressions (low)
            calcExtendedMseOverScales(y_start, validRegressions, validRegressionsInGroup, i_new_peak);
        }
    } // end mergeRegressionsOverScales_static
#pragma endregion "merge regressions over scales static"

#pragma region "create peaks"

    std::pair<float, float> weightedMeanAndVariance(const float *x, const float *w, const bool *df,
                                                    int left_limit, int right_limit)
    {
        // weighted mean using y_start as weighting factor and left_limit right_limit as range
        int n = right_limit - left_limit + 1;
        float mean_wt = 0.0; // mean of w
        for (int j = left_limit; j <= right_limit; j++)
        {
            if (!*(df + j))
            {
                n--;
                continue;
            }
            mean_wt += *(w + j);
        }
        mean_wt /= n;
        float sum_xw = 0.0;     // sum of x*w
        float sum_weight = 0.0; // sum of w
        for (int j = left_limit; j <= right_limit; j++)
        {
            if (!*(df + j))
            {
                continue;
            }
            sum_xw += *(x + j) * *(w + j) / mean_wt;
            sum_weight += *(w + j) / mean_wt;
        }
        float weighted_mean = sum_xw / sum_weight;
        float sum_Qxxw = 0.0; // sum of (x - mean)^2 * w
        for (int j = left_limit; j <= right_limit; j++)
        {
            if (*(x + j) <= 0.f)
            {
                continue;
            }
            sum_Qxxw += (*(x + j) - weighted_mean) * (*(x + j) - weighted_mean) * *(w + j);
        }
        float uncertaintiy = std::sqrt(sum_Qxxw / sum_weight / n);
        return std::make_pair(weighted_mean, uncertaintiy);
    };

    void createCentroidPeaks(
        std::vector<CentroidPeak> &peaks,
        ValidRegression_static *validRegressions,
        std::vector<ValidRegression_static> *validRegressionsVec,
        const int validRegressionsIndex,
        const float *y_start,
        const float *mz_start,
        const bool *df_start,
        const int scanNumber)
    {
        // iterate over the validRegressions vector
        for (int i = 0; i < validRegressionsIndex; i++)
        {
            ValidRegression_static regression;

            if (validRegressionsVec == nullptr)
            {
                regression = validRegressions[i];
            }
            else if (validRegressions == nullptr)
            {
                regression = (*validRegressionsVec)[i];
            }
            else
            {
                std::cerr << "Error: no regressions supplied";
                assert(false);
            }

            if (regression.isValid)
            {
                CentroidPeak peak;

                peak.scanNumber = scanNumber;

                // add height
                RegCoeffs coeff = regression.newCoeffs;
                peak.height = exp_approx_d(coeff.b0 + (regression.apex_position - regression.index_x0) * coeff.b1 * 0.5); // peak height (exp(b0 - b1^2/4/b2)) with position being -b1/2/b2
                peak.heightUncertainty = regression.uncertainty_height * peak.height;

                // add area
                float exp_b0 = exp_approx_d(coeff.b0); // exp(b0)
                peak.area = regression.area * exp_b0;
                peak.areaUncertainty = regression.uncertainty_area * exp_b0;

                // re-scale the apex position to x-axis
                float mz0 = *(mz_start + (int)std::floor(regression.apex_position));
                float dmz = *(mz_start + (int)std::floor(regression.apex_position) + 1) - mz0;
                peak.mz = mz0 + dmz * (regression.apex_position - std::floor(regression.apex_position));
                peak.mzUncertainty = regression.uncertainty_pos * dmz * T_VALUES[regression.df + 1] * sqrt(1 + 1 / (regression.df + 4));

                peak.dqsCen = experfc(regression.uncertainty_area / regression.area, -1.0);

                /// @todo consider adding these properties so we can trace back everything completely
                // peak.idxPeakStart = regression.left_limit;
                // peak.idxPeakEnd = regression.right_limit;

                peaks.push_back(std::move(peak));
            }
        }
    }

    void createFeaturePeaks(
        std::vector<FeaturePeak> &peaks,
        ValidRegression_static *validRegressions,
        std::vector<ValidRegression_static> *validRegressionsVec,
        const int validRegressionsIndex,
        const float *y_start,
        const float *mz_start,
        const float *rt_start,
        const bool *df_start,
        const float *dqs_cen,
        const float *dqs_bin)
    {
        // iterate over the validRegressions vector
        for (int i = 0; i < validRegressionsIndex; i++)
        {
            ValidRegression_static regression;
            if (validRegressionsVec == nullptr)
            {
                regression = validRegressions[i];
            }
            else if (validRegressions == nullptr)
            {
                regression = (*validRegressionsVec)[i];
            }
            else
            {
                std::cerr << "Error: no regressions supplied";
                assert(false);
            }

            if (regression.isValid)
            {
                FeaturePeak peak;

                // add height
                RegCoeffs coeff = regression.newCoeffs;
                peak.height = exp_approx_d(coeff.b0 + (regression.apex_position - regression.index_x0) * coeff.b1 * 0.5); // peak height (exp(b0 - b1^2/4/b2)) with position being -b1/2/b2
                peak.heightUncertainty = regression.uncertainty_height * peak.height;

                // add area
                float exp_b0 = exp_approx_d(coeff.b0); // exp(b0)
                peak.area = regression.area * exp_b0;
                peak.areaUncertainty = regression.uncertainty_area * exp_b0;

                const double rt0 = *(rt_start + (int)std::floor(regression.apex_position));
                const double drt = *(rt_start + (int)std::floor(regression.apex_position) + 1) - rt0;
                const double apex_position = rt0 + drt * (regression.apex_position - std::floor(regression.apex_position));
                peak.retentionTime = apex_position;
                peak.retentionTimeUncertainty = regression.uncertainty_pos * drt;

                std::pair<float, float> mz = weightedMeanAndVariance(mz_start, y_start, df_start, regression.left_limit, regression.right_limit);
                peak.mz = mz.first;
                peak.mzUncertainty = mz.second;

                peak.dqsCen = weightedMeanAndVariance(dqs_cen, y_start, df_start, regression.left_limit, regression.right_limit).first;
                peak.dqsBin = weightedMeanAndVariance(dqs_bin, y_start, df_start, regression.left_limit, regression.right_limit).first;
                peak.dqsPeak = experfc(regression.uncertainty_area / regression.area, -1.0);

                peak.idxPeakStart = regression.left_limit;
                peak.idxPeakEnd = regression.right_limit;

                peak.coefficients = coeff;

                peaks.push_back(std::move(peak));
            }
        }
    }
#pragma endregion "create peaks"

#pragma region calcSSE

    // Lambda function to calculate the full segments
    float calcFullSegments(RegCoeffs coeff, int limit_L, int limit_R, const float *y_start, bool calc_EXP, bool calc_CHISQ)
    {
        assert(limit_L < 0);
        assert(limit_R > 0);
        double result = 0;
        __m256 b0 = _mm256_set1_ps(coeff.b0); // b0 is eight times coeff 0
        __m256 b1 = _mm256_set1_ps(coeff.b1); // b1 is eight times coeff 1
        __m256 b2 = _mm256_set1_ps(coeff.b2);
        __m256 b3 = _mm256_set1_ps(coeff.b3);

        // left side
        int j = 0;
        long int nFullSegments = -limit_L;

        double result2 = 0.0f;
        for (long int iSegment = 0; iSegment < nFullSegments; iSegment++)
        {
            double new_x = limit_L + iSegment;
            double y_base = coeff.b0 + coeff.b1 * new_x + coeff.b2 * new_x * new_x;
            double y_new = y_base;
            if (calc_EXP)
            {
                y_new = exp_approx_d(y_base); // calculate the exp of the yhat values (if needed)
            }
            double y_current = y_start[iSegment];
            double newdiff = (y_current - y_new) * (y_current - y_new);
            if (newdiff == INFINITY)
            {
                std::cout << y_base << ", " << y_new;
                exit(1);
            }
            if (calc_CHISQ)
            {
                assert(y_new != 0);
                newdiff = newdiff / y_new; // Calculate the weighted square of the difference
            }
            result2 += newdiff;
        }

        __m256 LINSPACE = _mm256_set_ps(7.f, 6.f, 5.f, 4.f, 3.f, 2.f, 1.f, 0.f);
        __m256 x = _mm256_add_ps(_mm256_set1_ps(limit_L - 8.0), LINSPACE); // formerly i
        nFullSegments = -limit_L / 8;
        j = 0;
        for (int iSegment = 0; iSegment < nFullSegments; ++iSegment, j += 8)
        {
            // Load 8 values of i directly as float
            x = _mm256_add_ps(_mm256_set1_ps(8), x); // x vector : -k to -k+7

            // Calculate the yhat values
            __m256 yhat = _mm256_fmadd_ps(_mm256_fmadd_ps(b2, x, b1), x, b0); // b0 + b1 * x + b2 * x^2
            if (calc_EXP)
            {
                yhat = exp_approx_vf(yhat); // calculate the exp of the yhat values (if needed)
            }
            const __m256 y_vec = _mm256_loadu_ps(y_start + j); // Load 8 values from y considering the offset j
            const __m256 diff = _mm256_sub_ps(y_vec, yhat);    // Calculate the difference between y and yhat
            __m256 diff_sq = _mm256_mul_ps(diff, diff);        // Calculate the square of the difference
            if (calc_CHISQ)
            {
                diff_sq = _mm256_div_ps(diff_sq, yhat); // Calculate the weighted square of the difference
            }
            result += sum8(diff_sq); // Calculate the sum of the squares and add it to the result
        }

        // std::cout << abs(result2 - result) << " " << nFullSegments << ", ";
        // std::cout.flush();
        // assert(abs(abs(result2) - abs(result)) < 0.00001);

        // result = result2;

        // right side
        j = (limit_R - limit_L + 1 - 8);
        nFullSegments = limit_R / 8;
        LINSPACE = _mm256_set_ps(0.f, -1.f, -2.f, -3.f, -4.f, -5.f, -6.f, -7.f);
        x = _mm256_add_ps(_mm256_set1_ps(limit_R + 8.0), LINSPACE); // x vector : -k to -k+7

        for (int iSegment = 0; iSegment < nFullSegments; ++iSegment, j -= 8)
        {
            // Load 8 values of i directly as float
            x = _mm256_add_ps(_mm256_set1_ps(-8.0), x); // x vector : -k to -k+7
            // Calculate the yhat values
            __m256 yhat = _mm256_fmadd_ps(_mm256_fmadd_ps(b3, x, b1), x, b0); // b0 + b1 * x + b2 * x^2
            if (calc_EXP)
            {
                yhat = exp_approx_vf(yhat); // calculate the exp of the yhat values (if needed)
            }
            const __m256 y_vec = _mm256_loadu_ps(y_start + j); // Load 8 values from y considering the offset j
            const __m256 diff = _mm256_sub_ps(y_vec, yhat);    // Calculate the difference between y and yhat
            __m256 diff_sq = _mm256_mul_ps(diff, diff);        // Calculate the square of the difference
            if (calc_CHISQ)
            {
                diff_sq = _mm256_div_ps(diff_sq, yhat); // Calculate the weighted square of the difference
            }
            result += sum8(diff_sq); // Calculate the sum of the squares and add it to the result
        }

        return result;
    };

    float calcRemaining(RegCoeffs coeff, const int nRemaining, const __m256 x, const int y_start_offset,
                        const int y_end_offset, const int mask_offset, const __m256 b_quadratic,
                        bool calc_EXP, bool calc_CHISQ, const float *y_start, int limit_L, int limit_R)
    {
        const __m256 b0 = _mm256_set1_ps(coeff.b0);
        const __m256 b1 = _mm256_set1_ps(coeff.b1);

        // Calculate the yhat values for the remaining elements
        __m256 yhat = _mm256_fmadd_ps(_mm256_fmadd_ps(b_quadratic, x, b1), x, b0); // b0 + b1 * x + b2 * x^2
        if (calc_EXP)
        {
            yhat = exp_approx_vf(yhat); // calculate the exp of the yhat values (if needed)
        }
        // Load the remaining values from y
        float y_remaining[8] = {0.0f};
        std::copy(y_start - limit_L + y_start_offset, y_start - limit_L + y_end_offset, y_remaining);
        const __m256 y_vec = _mm256_loadu_ps(y_remaining);

        __m256i LINSPACE_UP_INT_256 = _mm256_set_epi32(7, 6, 5, 4, 3, 2, 1, 0);
        const __m256i mask = _mm256_cmpgt_epi32(_mm256_set1_epi32(nRemaining + mask_offset), LINSPACE_UP_INT_256); // mask for the remaining elements
        yhat = _mm256_blendv_ps(y_vec, yhat, _mm256_castsi256_ps(mask));                                           // set the remaining elements to zero

        const __m256 diff = _mm256_sub_ps(y_vec, yhat); // calculate the difference between y and yhat
        __m256 diff_sq = _mm256_mul_ps(diff, diff);     // calculate the square of the difference
        if (calc_CHISQ)
        {
            diff_sq = _mm256_div_ps(diff_sq, yhat);                                              // calculate the weighted square of the difference
            diff_sq = _mm256_blendv_ps(_mm256_setzero_ps(), diff_sq, _mm256_castsi256_ps(mask)); // set the nan values to zero
        }
        return sum8(diff_sq); // calculate the sum of the squares and add it to the result
    };

    float calcSSE(
        const int left_limit,
        const int right_limit,
        RegCoeffs coeff,
        const float *y_start,
        const bool calc_EXP,
        const bool calc_CHISQ)
    {
        // exception handling if the right limits is negative or the left limit is positive
        if (right_limit < 0 || left_limit > 0)
        {
            throw std::invalid_argument("right_limit must be positive and left_limit must be negative");
        }

        // exception if nullptr is passed
        if (y_start == nullptr)
        {
            throw std::invalid_argument("y_start must not be nullptr");
        }

        float result = 0.0f; // result variable

        // Calculate the full segments
        // only applies if there are at least eight numbers on one side of the peak
        if (left_limit < -7 || right_limit > 7)
        {
            result += calcFullSegments(coeff, left_limit, right_limit, y_start, calc_EXP, calc_CHISQ);
        }

        const int nRemaining_left = -left_limit % 8;  // calculate the number of remaining elements
        const int nRemaining_right = right_limit % 8; // calculate the number of remaining elements

        __m256 LINSPACE_UP_POS_256 = _mm256_set_ps(7.f, 6.f, 5.f, 4.f, 3.f, 2.f, 1.f, 0.f);

        if (nRemaining_left > 0)
        {
            const __m256 b2 = _mm256_set1_ps(coeff.b2);
            __m256 x_left = _mm256_add_ps(_mm256_set1_ps(-static_cast<float>(nRemaining_left)), LINSPACE_UP_POS_256); // x vector : -nRemaining_left to -nRemaining_left+7
            result += calcRemaining(coeff, nRemaining_left, x_left, -nRemaining_left, 0, 0, b2,
                                    calc_EXP, calc_CHISQ, y_start, left_limit, right_limit);
        }

        if (nRemaining_right > 0)
        {
            const __m256 b3 = _mm256_set1_ps(coeff.b3);
            result += calcRemaining(coeff, nRemaining_right, LINSPACE_UP_POS_256, 0, nRemaining_right + 1, 1, b3,
                                    calc_EXP, calc_CHISQ, y_start, left_limit, right_limit);
        }

        return result;
    } // end calcSSE
#pragma endregion calcSSE

#pragma region calcExtendedMse
    std::pair<size_t, float> findBestRegression( // index, mse
        const float *y_start,                    // start of the measured data
        std::vector<ValidRegression_static> regressions,
        const bool *df_start,
        size_t startIdx,
        size_t endIdx) // degrees of freedom
    {
        /*
          The function consists of the following steps:
          1. Identify left and right limit of the grouped regression windows.
          2. Calculate the mean squared error (MSE) between the predicted and actual values.
          3. Identify the best regression based on the MSE and return the MSE and the index of the best regression.
        */
        // declare variables
        float best_mse = INFINITY;
        auto best_regression = regressions.begin();
        size_t bestRegIdx = 0;

        // step 1: identify left (smallest) and right (largest) limit of the grouped regression windows
        unsigned int left_limit = std::numeric_limits<int>::max();
        unsigned int right_limit = 0;
        for (size_t i = startIdx; i < endIdx + 1; i++)
        {
            left_limit = std::min(left_limit, regressions[i].left_limit);
            right_limit = std::max(right_limit, regressions[i].right_limit);
        }

        const int df_sum = calcDF(df_start, left_limit, right_limit);

        for (size_t i = startIdx; i < endIdx + 1; i++)
        {
            // step 2: calculate the mean squared error (MSE) between the predicted and actual values
            const float mse = calcSSE(
                                  left_limit - regressions[i].index_x0,  // left limit of the regression window (normalized scale)
                                  right_limit - regressions[i].index_x0, // right limit of the regression window (normalized scale)
                                  regressions[i].newCoeffs,              // regression coefficients
                                  y_start + left_limit,                  // start of the measured data
                                  true) /                                // calculate the exp of the yhat values
                              (df_sum - 4);

            // step 3: identify the best regression based on the MSE and return the MSE and the index of the best regression
            if (mse < best_mse)
            {
                best_mse = mse;
                bestRegIdx = i;
            }
        } // end for loop (index in groupIndices)
        return std::pair(bestRegIdx, best_mse);
    }

    void calcExtendedMse(
        const float *y_start,                             // start of the measured data
        std::vector<ValidRegression_static> &regressions, // regressions to compare
        const bool *df_start)                             // degrees of freedom
    {
        /*
          The function consists of the following steps:
          1. Identify left and right limit of the grouped regression windows.
          2. Calculate the mean squared error (MSE) between the predicted and actual values.
          3. Identify the best regression based on the MSE and return the MSE and the index of the best regression.
        */
        // declare variables
        float best_mse = INFINITY;
        auto best_regression = regressions.begin();

        // step 1: identify left (smallest) and right (largest) limit of the grouped regression windows
        int left_limit = std::numeric_limits<int>::max();
        int right_limit = 0;
        for (auto regression = regressions.begin(); regression != regressions.end(); ++regression)
        {
            left_limit = std::min(left_limit, static_cast<int>(regression->left_limit));
            right_limit = std::max(right_limit, static_cast<int>(regression->right_limit));
        }

        const int df_sum = calcDF(df_start, left_limit, right_limit);
        if (df_sum <= 4) // @todo this condition is never fulfilled
        {
            std::cerr << "BANG!";
            exit(1);
            // set isValid to false for all regressions
            for (auto regression = regressions.begin(); regression != regressions.end(); ++regression)
            {
                regression->isValid = false;
            }
            return; // not enough degrees of freedom
        }

        for (auto regression = regressions.begin(); regression != regressions.end(); ++regression)
        {
            // step 2: calculate the mean squared error (MSE) between the predicted and actual values
            const float mse = calcSSE(
                                  left_limit - regression->index_x0,  // left limit of the regression window (normalized scale)
                                  right_limit - regression->index_x0, // right limit of the regression window (normalized scale)
                                  regression->newCoeffs,              // regression coefficients
                                  y_start + left_limit,               // start of the measured data
                                  true) /                             // calculate the exp of the yhat values
                              (df_sum - 4);

            // step 3: identify the best regression based on the MSE and return the MSE and the index of the best regression
            if (mse < best_mse)
            {
                best_mse = mse;
                regression->mse = mse;
                best_regression = regression;
            }
            else
            {
                regression->isValid = false;
            }
        } // end for loop (index in groupIndices)
        // set isValid to false for all regressions except the best one
        for (auto regression = regressions.begin(); regression != regressions.end(); ++regression)
        {
            if (regression != best_regression)
            {
                regression->isValid = false;
            }
        }
    } // end calcExtendedMse
#pragma endregion calcExtendedMse

#pragma region calcExtendedMse_static
    void calcExtendedMse_static(
        const float *y_start,
        ValidRegression_static *regressions_start,
        const int n_regressions,
        const bool *df_start)
    {
        /*
          The function consists of the following steps:
          1. Identify left and right limit of the grouped regression windows.
          2. Calculate the mean squared error (MSE) between the predicted and actual values.
          3. Identify the best regression based on the MSE and return the MSE and the index of the best regression.
        */
        // declare variables
        float best_mse = std::numeric_limits<float>::infinity();

        // step 1: identify left (smallest) and right (largest) limit of the grouped regression windows
        unsigned int left_limit = regressions_start->left_limit;
        unsigned int right_limit = regressions_start->right_limit;
        for (int i = 1; i < n_regressions; i++)
        {
            left_limit = std::min(left_limit, (regressions_start + i)->left_limit);
            right_limit = std::max(right_limit, (regressions_start + i)->right_limit);
        }

        const int df_sum = calcDF(df_start, left_limit, right_limit);
        if (df_sum <= 4)
        {
            // set isValid to false for all regressions
            for (int i = 0; i < n_regressions; ++i)
            {
                (regressions_start + i)->isValid = false;
            }
            return; // not enough degrees of freedom
        }

        for (int i = 0; i < n_regressions; ++i)
        {
            // step 2: calculate the mean squared error (MSE) between the predicted and actual values
            const float mse = calcSSE(
                                  left_limit - (regressions_start + i)->index_x0,  // left limit of the regression window (normalized scale)
                                  right_limit - (regressions_start + i)->index_x0, // right limit of the regression window (normalized scale)
                                  (regressions_start + i)->newCoeffs,              // regression coefficients
                                  y_start + left_limit,                            // start of the measured data
                                  true) /                                          // calculate the exp of the yhat values
                              (df_sum - 4);

            // step 3: identify the best regression based on the MSE and return the MSE and the index of the best regression
            if (mse < best_mse)
            {
                best_mse = mse;
                (regressions_start + i)->mse = mse;
            }
            else
            {
                (regressions_start + i)->isValid = false;
            }
        } // end for loop (index in groupIndices)
        // set isValid to false for all regressions except the best one
        for (int i = 0; i < n_regressions; ++i)
        {
            if ((regressions_start + i)->mse > best_mse)
            {
                (regressions_start + i)->isValid = false;
            }
        }
    }
#pragma endregion calcExtendedMse_static

#pragma region calcExtendedMsePair
    void calcExtendedMsePair(
        const float *y_start,
        ValidRegression_static *low_scale_regression,
        ValidRegression_static *hi_scale_regression,
        const bool *df_start)
    {
        /*
          The function consists of the following steps:
          1. Identify left and right limit of the grouped regression windows.
          2. Calculate the mean squared error (MSE) between the predicted and actual values.
          3. Identify the best regression based on the MSE and return the MSE and the index of the best regression.
        */

        // step 1: identify left (smallest) and right (largest) limit of the grouped regression windows
        int left_limit = std::min(low_scale_regression->left_limit, hi_scale_regression->left_limit);
        int right_limit = std::max(low_scale_regression->right_limit, hi_scale_regression->right_limit);

        const int df_sum = calcDF(df_start, left_limit, right_limit);
        if (df_sum <= 4)
        {
            // set isValid to false for all regressions
            low_scale_regression->isValid = false;
            hi_scale_regression->isValid = false;
            return; // not enough degrees of freedom
        }

        // step 2: calculate the mean squared error (MSE) between the predicted and actual values
        const float mse_low_scale = calcSSE(
                                        left_limit - low_scale_regression->index_x0,  // left limit of the regression window (normalized scale)
                                        right_limit - low_scale_regression->index_x0, // right limit of the regression window (normalized scale)
                                        low_scale_regression->newCoeffs,              // regression coefficients
                                        y_start + left_limit,                         // start of the measured data
                                        true) /                                       // calculate the exp of the yhat values
                                    (df_sum - 4);

        const float mse_hi_scale = calcSSE(
                                       left_limit - hi_scale_regression->index_x0,  // left limit of the regression window (normalized scale)
                                       right_limit - hi_scale_regression->index_x0, // right limit of the regression window (normalized scale)
                                       hi_scale_regression->newCoeffs,              // regression coefficients
                                       y_start + left_limit,                        // start of the measured data
                                       true) /                                      // calculate the exp of the yhat values
                                   (df_sum - 4);

        // step 3: identify the best regression based on the MSE and return the MSE and the index of the best regression
        if (mse_low_scale < mse_hi_scale)
        {
            low_scale_regression->mse = mse_low_scale;
            hi_scale_regression->isValid = false;
        }
        else
        {
            hi_scale_regression->mse = mse_hi_scale;
            low_scale_regression->isValid = false;
        }
    }
#pragma endregion calcExtendedMsePair

#pragma region calcExtendedMseOverScales
    void calcExtendedMseOverScales(
        const float *y_start,
        ValidRegression_static *validRegressions,
        const std::vector<int> &validRegressionsInGroup,
        const int i_new_peak)
    {
        // comparison of the new regression (high) with multiple ref regressions (low)
        // check new_peak for mse
        if (validRegressions[i_new_peak].mse == 0.0)
        { // calculate the mse of the current peak
            validRegressions[i_new_peak].mse = calcSSE(
                                                   validRegressions[i_new_peak].left_limit - validRegressions[i_new_peak].index_x0,  // left limit of the new peak (normalized scale)
                                                   validRegressions[i_new_peak].right_limit - validRegressions[i_new_peak].index_x0, // right limit of the new peak (normalized scale)
                                                   validRegressions[i_new_peak].newCoeffs,                                           // regression coefficients
                                                   y_start + validRegressions[i_new_peak].left_limit,
                                                   true) /
                                               validRegressions[i_new_peak].df;
        }
        // calculate the group sse
        float groupSSE = 0.0f;
        int groupDF = 0;
        // iterate through the group of reference peaks
        for (int i_ref_peak : validRegressionsInGroup)
        {
            if (validRegressions[i_ref_peak].mse == 0.0)
            { // calculate the mse of the ref peak
                validRegressions[i_ref_peak].mse = calcSSE(
                                                       validRegressions[i_ref_peak].left_limit - validRegressions[i_ref_peak].index_x0,  // left limit of the ref peak (normalized scale)
                                                       validRegressions[i_ref_peak].right_limit - validRegressions[i_ref_peak].index_x0, // right limit of the ref peak (normalized scale)
                                                       validRegressions[i_ref_peak].newCoeffs,                                           // regression coefficients
                                                       y_start + validRegressions[i_ref_peak].left_limit,
                                                       true) /
                                                   validRegressions[i_ref_peak].df;
            }
            groupSSE += validRegressions[i_ref_peak].mse * validRegressions[i_ref_peak].df;
            groupDF += validRegressions[i_ref_peak].df;
        }
        // compare mse of the new peak with the group mse
        if (validRegressions[i_new_peak].mse < groupSSE / groupDF)
        {
            // Set isValid to false for the candidates from the group
            for (int i_ref_peak : validRegressionsInGroup)
            {
                validRegressions[i_ref_peak].isValid = false;
            }
        }
        else
        { // Set isValid to false for the current peak
            validRegressions[i_new_peak].isValid = false;
        }
    }
#pragma endregion calcExtendedMseOverScales

#pragma region calcDF
    int calcDF(
        const bool *df_start,     // start of the degrees of freedom
        unsigned int left_limit,  // left limit
        unsigned int right_limit) // right limit
    {
        unsigned int degreesOfFreedom = 0;
        for (size_t i = left_limit; i < right_limit + 1; i++)
        {
            if (df_start[i])
            {
                ++degreesOfFreedom;
            }
        }
        return degreesOfFreedom;
    }
#pragma endregion calcDF

#pragma region calcApexAndValleyPos
    bool calcApexAndValleyPos(
        const __m128 coeff,
        const int scale,
        float &apex_position,
        float &valley_position)
    {
        assert(((float *)&coeff)[2] != 0.0f || ((float *)&coeff)[3] != 0.0f);
        // calculate key by checking the signs of coeff
        __m128 res = _mm_set1_ps(-0.5f); // res = -0.5
        __m128 ZERO_128 = _mm_setzero_ps();
        __m128 KEY_128 = _mm_set_ps(1.f, 2.f, 4.f, 0.f);
        __m128 signs = _mm_cmplt_ps(coeff, ZERO_128); // compare the coefficients with zero, results will have the following values: 0xFFFFFFFF if the value is negative, 0x00000000 if the value is positive
        signs = _mm_and_ps(signs, KEY_128);           // multiply a key value if the value of the coefficient is negative, i.e., b0 * 0, b1 * 4, b2 * 2, b3 * 1
        signs = _mm_hadd_ps(signs, signs);            // horizontal add of the signs
        signs = _mm_hadd_ps(signs, signs);            // horizontal add of the signs, now all values are the same, i.e. the sum
        int key = _mm_cvtss_si32(signs);

        res = _mm_mul_ps(res, _mm_shuffle_ps(coeff, coeff, 0b01010101)); // res = -0.5 * b1
        res = _mm_div_ps(res, coeff);                                    // res = -0.5 * b1 / b2

        switch (key)
        {
        case 7:                                 // Case 1a: apex left
            apex_position = ((float *)&res)[2]; //-B1 / 2 / B2;  // is negative
            valley_position = 0;                // no valley point
            return apex_position > -scale + 1;  // scale +1: prevent apex position to be at the edge of the data

        case 3:                                 // Case 1b: apex right
            apex_position = ((float *)&res)[3]; //-B1 / 2 / B3;     // is positive
            valley_position = 0;                // no valley point
            return apex_position < scale - 1;   // scale -1: prevent apex position to be at the edge of the data

        case 6:                                                                       // Case 2a: apex left | valley right
            apex_position = ((float *)&res)[2];                                       //-B1 / 2 / B2;                                             // is negative
            valley_position = ((float *)&res)[3];                                     //-B1 / 2 / B3;                                           // is positive
            return apex_position > -scale + 1 && valley_position - apex_position > 2; // scale +1: prevent apex position to be at the edge of the data

        case 1:                                                                      // Case 2b: apex right | valley left
            apex_position = ((float *)&res)[3];                                      //-B1 / 2 / B3;                                            // is positive
            valley_position = ((float *)&res)[2];                                    //-B1 / 2 / B2;                                          // is negative
            return apex_position < scale - 1 && apex_position - valley_position > 2; // scale -1: prevent apex position to be at the edge of the data

        default:
            return false; // invalid case
        } // end switch
    }
#pragma endregion calcApexAndValleyPos

#pragma region "isValidApexToEdge"

    float calcApexToEdge(
        const double apex_position,
        const int scale,
        const int index_loop,
        const float *y_start)
    {
        int idx_apex = (int)std::round(apex_position) + scale + index_loop; // index of the apex
        int idx_left = index_loop;                                          // index of the left edge
        int idx_right = 2 * scale + index_loop;                             // index of the right edge
        float apex = *(y_start + idx_apex);                                 // apex value
        float left = *(y_start + idx_left);                                 // left edge value
        float right = *(y_start + idx_right);                               // right edge value
        return (left < right) ? (apex / left) : (apex / right);             // difference between the apex and the edge
    }

#pragma endregion "isValidApexToEdge"

#pragma region isValidQuadraticTerm
    bool isValidQuadraticTerm(
        RegCoeffs coeff,
        const int scale,
        const float mse,
        const int df_sum)
    {
        float divisor = std::sqrt(INV_ARRAY[scale * 6 + 4] * mse); // inverseMatrix_2_2 is at position 4 of initialize()
        double tValue = std::max(                                  // t-value for the quadratic term
            std::abs(coeff.b2) / divisor,                          // t-value for the quadratic term left side of the peak
            std::abs(coeff.b3) / divisor);                         // t-value for the quadratic term right side of the peak
        return tValue > T_VALUES[df_sum - 5];                      // statistical significance of the quadratic term
    }
#pragma endregion isValidQuadraticTerm

#pragma region isValidPeakHeight

    float calcPeakHeightUncert(
        const float mse,
        const int scale,
        const float apex_position)
    {
        float Jacobian_height[4]{1, 0, 0, 0}; // Jacobian matrix for the height
        Jacobian_height[1] = apex_position;   // apex_position * height;
        if (apex_position < 0)
        {
            Jacobian_height[2] = apex_position * apex_position; // apex_position * Jacobian_height[1];
            // Jacobian_height[3] = 0;
        }
        else
        {
            // Jacobian_height[2] = 0;
            Jacobian_height[3] = apex_position * apex_position; // apex_position * Jacobian_height[1];
        }
        // at this point without height, i.e., to get the real uncertainty
        // multiply with height later. This is done to avoid exp function at this point
        return std::sqrt(mse * multiplyVecMatrixVecTranspose(Jacobian_height, scale));
    }

    bool isValidPeakHeight(
        const float mse,
        const int scale,
        const float apex_position,
        float valley_position,
        const int df_sum,
        const float apexToEdge)
    {
        // check if the peak height is significantly greater than edge signal

        float Jacobian_height2[4]{0, 0, 0, 0};
        // Jacobian_height2[0] = 0.f; // adjust for uncertainty calculation of apex to edge ratio

        if (apex_position < 0)
        {
            // float edge_position = (valley_position != 0) ? valley_position : static_cast<float>(-scale);
            if (valley_position == 0)
            {
                valley_position = static_cast<float>(-scale);
            }

            Jacobian_height2[1] = apex_position - valley_position;
            Jacobian_height2[2] = apex_position * apex_position - valley_position * valley_position; // adjust for uncertainty calculation of apex to edge ratio
            // Jacobian_height2[3] = 0;
        }
        else
        {
            if (valley_position == 0)
            {
                valley_position = static_cast<float>(scale);
            }
            Jacobian_height2[1] = apex_position - valley_position;
            // Jacobian_height2[2] = 0;
            Jacobian_height2[3] = apex_position * apex_position - valley_position * valley_position; // adjust for uncertainty calculation of apex to edge ratio
        }
        float uncertainty_apexToEdge = std::sqrt(mse * multiplyVecMatrixVecTranspose(Jacobian_height2, scale));
        return (apexToEdge - 2) / (apexToEdge * uncertainty_apexToEdge) > T_VALUES[df_sum - 5];
    }
#pragma endregion isValidPeakHeight

#pragma region isValidPeakArea
    bool isValidPeakArea(
        RegCoeffs coeff,
        const float mse,
        const int scale,
        const int df_sum,
        float &area,
        float &uncertainty_area)
    {
        // predefine expressions
        float b1 = coeff.b1;
        float b2 = coeff.b2;
        float b3 = coeff.b3;
        float _SQRTB2 = 1 / std::sqrt(std::abs(b2));
        float _SQRTB3 = 1 / std::sqrt(std::abs(b3));
        float B1_2_SQRTB2 = b1 / 2 * _SQRTB2;
        float B1_2_SQRTB3 = b1 / 2 * _SQRTB3;
        float B1_2_B2 = b1 / 2 / b2;
        float EXP_B12 = exp_approx_d(-b1 * B1_2_B2 / 2);
        float B1_2_B3 = b1 / 2 / b3;
        float EXP_B13 = exp_approx_d(-b1 * B1_2_B3 / 2);

        // initialize variables
        float J[4]; // Jacobian matrix

        // here we have to check if there is a valley point or not
        const float err_L =
            (b2 < 0)
                ? experfc(B1_2_SQRTB2, -1.0) // 1 - std::erf(b1 / 2 / SQRTB2) // ordinary peak
                : dawson5(B1_2_SQRTB2);      // erfi(b1 / 2 / SQRTB2);        // peak with valley point;

        float err_R =
            (b3 < 0)
                ? experfc(B1_2_SQRTB3, 1.0) // 1 + std::erf(b1 / 2 / SQRTB3) // ordinary peak
                : dawson5(-B1_2_SQRTB3);    // -erfi(b1 / 2 / SQRTB3);       // peak with valley point ;

        // calculate the Jacobian matrix terms
        float J_1_common_L = _SQRTB2; // SQRTPI_2 * EXP_B12 / SQRTB2;
        float J_1_common_R = _SQRTB3; // SQRTPI_2 * EXP_B13 / SQRTB3;
        float J_2_common_L = B1_2_B2 / b1;
        float J_2_common_R = B1_2_B3 / b1;
        float J_1_L = J_1_common_L * err_L;
        float J_1_R = J_1_common_R * err_R;
        float J_2_L = J_2_common_L - J_1_L * B1_2_B2;
        float J_2_R = -J_2_common_R - J_1_R * B1_2_B3;

        J[0] = J_1_R + J_1_L;
        J[1] = J_2_R + J_2_L;
        J[2] = -B1_2_B2 * (J_2_L + J_1_L / b1);
        J[3] = -B1_2_B3 * (J_2_R + J_1_R / b1);

        area = J[0]; // at this point the area is without exp(b0), i.e., to get the real area multiply with exp(b0) later. This is done to avoid exp function at this point
        uncertainty_area = std::sqrt(mse * multiplyVecMatrixVecTranspose(J, scale));

        if (area / uncertainty_area <= T_VALUES[df_sum - 5])
        {
            return false;
        }

        float J_covered[4];    // Jacobian matrix for the covered peak area
        float x_left = -scale; // left limit due to the window
        float x_right = scale; // right limit due to the window
        float y_left = 0;      // y value at the left limit
        float y_right = 0;     // y value at the right limit

        float err_L_covered = /// @todo : needs to be revised
            (b2 < 0)
                ?                                                                                                   // ordinary peak half, take always scale as integration limit; we use erf instead of erfi due to the sqrt of absoulte value
                erf_approx_f((b1 - 2 * b2 * scale) / 2 * _SQRTB2) * EXP_B12 * SQRTPI_2 + err_L - SQRTPI_2 * EXP_B12 // std::erf((b1 - 2 * b2 * scale) / 2 / SQRTB2) + err_L - 1
                :                                                                                                   // valley point, i.e., check position
                (-B1_2_B2 < -scale)
                    ? // valley point is outside the window, use scale as limit
                    err_L - erfi((b1 - 2 * b2 * scale) / 2 * _SQRTB2) * EXP_B12
                    : // valley point is inside the window, use valley point as limit
                    err_L;

        const float err_R_covered = ///@todo : needs to be revised
            (b3 < 0)
                ?                                                                                                   // ordinary peak half, take always scale as integration limit; we use erf instead of erfi due to the sqrt of absoulte value
                err_R - SQRTPI_2 * EXP_B13 - erf_approx_f((b1 + 2 * b3 * scale) / 2 * _SQRTB3) * SQRTPI_2 * EXP_B13 // err_R - 1 - std::erf((b1 + 2 * b3 * scale) / 2 / SQRTB3)
                :                                                                                                   // valley point, i.e., check position
                (-B1_2_B3 > scale)
                    ? // valley point is outside the window, use scale as limit
                    erfi((b1 + 2 * b3 * scale) / 2 * _SQRTB3) * EXP_B13 + err_R
                    : // valley point is inside the window, use valley point as limit
                    err_R;

        // adjust x limits
        if (b2 > 0 && -B1_2_B2 > -scale)
        { // valley point is inside the window, use valley point as limit
            x_left = -B1_2_B2;
        }

        if (b3 > 0 && -B1_2_B3 < scale)
        { // valley point is inside the window, use valley point as limit
            x_right = -B1_2_B3;
        }

        // calculate the y values at the left and right limits
        y_left = exp_approx_d(b1 * x_left + b2 * x_left * x_left);
        y_right = exp_approx_d(b1 * x_right + b3 * x_right * x_right);
        const float dX = x_right - x_left;

        // calculate the trapzoid correction terms for the jacobian matrix
        const float trpzd_b0 = (y_right + y_left) * dX / 2;
        const float trpzd_b1 = (x_right * y_right + x_left * y_left) * dX / 2;
        const float trpzd_b2 = (x_left * x_left * y_left) * dX / 2;
        const float trpzd_b3 = (x_right * x_right * y_right) * dX / 2;

        const float J_1_L_covered = J_1_common_L * err_L_covered;
        const float J_1_R_covered = J_1_common_R * err_R_covered;
        const float J_2_L_covered = J_2_common_L - J_1_L_covered * B1_2_B2;
        const float J_2_R_covered = -J_2_common_R - J_1_R_covered * B1_2_B3;

        J_covered[0] = J_1_R_covered + J_1_L_covered - trpzd_b0;
        J_covered[1] = J_2_R_covered + J_2_L_covered - trpzd_b1;
        J_covered[2] = -B1_2_B2 * (J_2_L_covered + J_1_L_covered / b1) - trpzd_b2;
        J_covered[3] = -B1_2_B3 * (J_2_R_covered + J_1_R_covered / b1) - trpzd_b3;

        float area_uncertainty_covered = std::sqrt(mse * multiplyVecMatrixVecTranspose(J_covered, scale));

        return J_covered[0] / area_uncertainty_covered > T_VALUES[df_sum - 5]; // statistical significance of the peak area
    }
#pragma endregion isValidPeakArea

#pragma region "calcUncertaintyPosition"
    float calcUncertaintyPos(
        const float mse,
        RegCoeffs coeff,
        const float apex_position,
        const int scale)
    {
        float _b1 = 1 / coeff.b1;
        float _b2 = 1 / coeff.b2;
        float _b3 = 1 / coeff.b3;
        float J[4]; // Jacobian matrix
        J[0] = 0.f;
        J[1] = apex_position * _b1;
        if (apex_position < 0)
        {
            J[2] = -apex_position * _b2;
            J[3] = 0;
        }
        else
        {
            J[2] = 0;
            J[3] = -apex_position * _b3;
        }
        return std::sqrt(mse * multiplyVecMatrixVecTranspose(J, scale));
    }
#pragma endregion "calcUncertaintyPosition"

    // #pragma region calculateNumberOfRegressions
    //     int calcNumberOfRegressions(const int n) // previously in validRegressions.reserve()
    //     {
    //         int sum = 0;
    //         for (int i = 4; i <= GLOBAL_MAXSCALE * 2; i += 2)
    //         {
    //             sum += std::max(0, n - i);
    //         }
    //         return sum;
    //     }
    // #pragma endregion calculateNumberOfRegressions

#pragma region "convolve regression"
    // these chain to return beta for a regression

    std::array<__m128, 512> convolve_static(
        const size_t scale,
        const float *vec,
        const size_t n)
    {
        if (n < 2 * scale + 1)
        {
            throw std::invalid_argument("n must be greater or equal to 2 * scale + 1");
        }
        std::array<__m128, 512> beta;
        __m128 result[512];
        __m128 products[512];
        const __m128 flipSign = _mm_set_ps(1.0f, 1.0f, -1.0f, 1.0f);
        convolve_SIMD(scale, vec, n, result, products, 512);

        for (size_t i = 0; i < n - 2 * scale; i++)
        { // swap beta2 and beta3 and flip the sign of beta1 // @todo: this is a temporary solution
            beta[i] = _mm_mul_ps(_mm_shuffle_ps(result[i], result[i], 0b10110100), flipSign);
        }
        return beta;
    }

    void convolve_dynamic(
        const size_t scale,
        const float *vec,
        const size_t n,
        __m128 *beta)
    {
        if (n < 2 * scale + 1)
        {
            throw std::invalid_argument("n must be greater or equal to 2 * scale + 1");
        }

        __m128 *result = new __m128[n - 2 * scale];
        __m128 *products = new __m128[n];
        const __m128 flipSign = _mm_set_ps(1.0f, 1.0f, -1.0f, 1.0f);
        convolve_SIMD(scale, vec, n, result, products, n);

        for (size_t i = 0; i < n - 2 * scale; i++)
        { // swap beta2 and beta3 and flip the sign of beta1 // @todo: this is a temporary solution
            beta[i] = _mm_mul_ps(_mm_shuffle_ps(result[i], result[i], 0b10110100), flipSign);
        }
        delete[] result;
        delete[] products;
    }

    void convolve_SIMD(
        const size_t scale,
        const float *vec,
        const size_t n,
        __m128 *result,
        __m128 *products,
        const size_t buffer_size)
    {
        size_t k = 2 * scale + 1;
        size_t n_segments = n - k + 1;
        size_t centerpoint = k / 2;

        for (size_t i = 0; i < n; ++i)
        {
            products[i] = _mm_setzero_ps();
        }
        __m128 kernel[3];
        kernel[0] = _mm_set_ps(INV_ARRAY[scale * 6 + 1], INV_ARRAY[scale * 6 + 1], 0.0f, INV_ARRAY[scale * 6 + 0]);
        kernel[1] = _mm_set_ps(INV_ARRAY[scale * 6 + 3] - INV_ARRAY[scale * 6 + 5], -INV_ARRAY[scale * 6 + 3] - INV_ARRAY[scale * 6 + 4], -INV_ARRAY[scale * 6 + 2] - INV_ARRAY[scale * 6 + 3], -INV_ARRAY[scale * 6 + 1]);
        kernel[2] = _mm_set_ps(2.f * INV_ARRAY[scale * 6 + 5], 2.f * INV_ARRAY[scale * 6 + 4], 2.f * INV_ARRAY[scale * 6 + 3], 2.f * INV_ARRAY[scale * 6 + 1]);

#pragma GCC ivdep
#pragma GCC unroll 8
        for (size_t i = 0; i < n_segments; i++)
        {
            __m128 vec_values = _mm_set1_ps(vec[i + centerpoint]);
            // result[i] = _mm_fmadd_ps(vec_values, kernel[0], result[i]);
            result[i] = _mm_mul_ps(vec_values, kernel[0]);
        }

        for (size_t i = 1; i < scale + 1; i++)
        {
            int u = 0;
            kernel[1] = _mm_add_ps(kernel[1], kernel[2]);
            // kernel[1] = kernel[1] original + i * kernel[2]
            kernel[0] = _mm_add_ps(kernel[0], kernel[1]);

#pragma GCC ivdep
#pragma GCC unroll 8
            for (size_t j = scale - i; j < (n - scale + i); j++)
            {
                __m128 vec_values = _mm_set1_ps(vec[j]);
                products[u] = _mm_mul_ps(vec_values, kernel[0]);
                u++;
            }

#pragma GCC ivdep
#pragma GCC unroll 8
            for (size_t j = 0; j < n_segments; j++)
            {
                if (2 * i + j >= buffer_size)
                {
                    throw std::out_of_range("Index out of range for products array: n=" + std::to_string(n) +
                                            " i=" + std::to_string(i) + " j=" + std::to_string(j));
                }
                __m128 products_temp = _mm_permute_ps(products[j], 0b10110100);
                __m128 sign_flip = _mm_set_ps(1.0f, 1.0f, -1.0f, 1.0f);
                products_temp = _mm_fmadd_ps(products_temp, sign_flip, products[2 * i + j]);
                result[j] = _mm_add_ps(result[j], products_temp);
            }
        }
    }

    float multiplyVecMatrixVecTranspose(const float vec[4], int scale)
    {
        scale *= 6;
        const float result = vec[0] * vec[0] * INV_ARRAY[scale + 0] +
                             vec[1] * vec[1] * INV_ARRAY[scale + 2] +
                             (vec[2] * vec[2] + vec[3] * vec[3]) * INV_ARRAY[scale + 4] +
                             2 * (vec[2] * vec[3] * INV_ARRAY[scale + 5] +
                                  vec[0] * (vec[1] + vec[3]) * INV_ARRAY[scale + 1] +
                                  vec[1] * (vec[2] - vec[3]) * INV_ARRAY[scale + 3]);

        return result;
    }
#pragma endregion "convolve regression"
}