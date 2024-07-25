// qalgorithms_measurement_data_tensor.cpp

// internal
#include "../include/qalgorithms_measurement_data_tensor.h"
#include "../external/StreamCraft/src/StreamCraft_mzml.h"
#include "../include/qalgorithms_measurement_data.h"
#include "../include/qalgorithms_qpeaks.h"

#include <algorithm>

namespace q
{
    namespace MeasurementData
    {

        // methods
        float
        TensorData::calcRTDiff(std::vector<double> &retention_times)
        {
            float sum = 0.0;
            for (size_t i = 1; i < retention_times.size(); ++i)
            {
                sum += retention_times[i] - retention_times[i - 1];
            }
            return sum / (retention_times.size() - 1);
        }

        bool TensorData::readCSV(
            std::string filename,
            int rowStart,
            int rowEnd,
            int colStart,
            int colEnd,
            char separator,
            std::vector<DataType::DataField> variableTypes)
        {
            return true;
        }

        std::vector<std::vector<std::unique_ptr<DataType::Peak>>>
        TensorData::findCentroids_MZML(
            q::Algorithms::qPeaks &qpeaks,
            sc::MZML &data,
            const bool ms1only,
            const std::string polarity,
            const int start_index)
        {
            std::vector<std::string> spectrum_mode = data.get_spectra_mode();         // get spectrum mode (centroid or profile)
            std::vector<std::string> spectrum_polarity = data.get_spectra_polarity(); // get spectrum polarity (positive or negative)
            double expectedDifference = 0.0;                                          // expected difference between two consecutive x-axis values
            bool needsZeroFilling = true;                                             // check if the instrument is Orbitrap
            std::vector<int> indices = data.get_spectra_index();                      // get all indices
            std::vector<int> ms_levels = data.get_spectra_level();                    // get all MS levels
            if (ms1only)
            {
                indices.erase(std::remove_if(indices.begin(), indices.end(), [&ms_levels](int i)
                                             { return ms_levels[i] != 1; }),
                              indices.end()); // keep only MS1 spectra
            }
            indices.erase(std::remove_if(indices.begin(), indices.end(), [&spectrum_polarity, &polarity](int i)
                                         { return spectrum_polarity[i] != polarity; }),
                          indices.end());                                       // keep only spectra with the specified polarity
            std::vector<double> retention_times = data.get_spectra_rt(indices); // get retention times
            rt_diff = calcRTDiff(retention_times);                              // retention time difference
            if (spectrum_mode[0] == "centroid")
            {
                return transfereCentroids(data, indices, retention_times, start_index);
            }
            std::vector<std::vector<std::unique_ptr<DataType::Peak>>> centroids =
                std::vector<std::vector<std::unique_ptr<DataType::Peak>>>(indices.size()); // create vector of unique pointers to peaks
            if (centroids.size() == 0)
            {
                return centroids;
            }
            {
                alignas(64) std::vector<std::vector<double>> data_vec = data.get_spectrum(indices[start_index]); // get first spectrum (x-axis)
                expectedDifference = calcExpectedDiff(data_vec[0]);                                              // calculate expected difference & check if Orbitrap
                isZeroFillingNeeded(data_vec[1], needsZeroFilling);                                              // check if zero filling is needed
            }

            if (needsZeroFilling)
            {
#pragma omp parallel for
                for (size_t i = 0; i < indices.size(); ++i) // loop over all indices
                {
                    const int index = indices[i]; // spectrum index
                    if (index < start_index)
                    {
                        continue; // skip due to index
                    }

                    alignas(64) std::vector<std::vector<double>> spectrum = data.get_spectrum(index); // get spectrum at index
                    if (spectrum[0].size() < 5)
                    {
                        continue; // skip due to lack of data, i.e., degree of freedom will be zero
                    }
                    spectrum.push_back(std::vector<double>(spectrum[0].size(), 1.0));                    // add df column for interpolation
                    int num_subsets = zeroFilling_vec(spectrum, expectedDifference);                     // zero fill the spectrum
                    std::vector<std::vector<double>::iterator> separators(num_subsets);                  // vector of iterators at separation points (x axis)
                    extrapolateData_vec(spectrum, separators);                                           // interpolate the data when zero filled
                    qpeaks.findCentroids(centroids[i], spectrum, separators, index, retention_times[i]); // find peaks
                } // for
            } // if zero filling
            else
            {
#pragma omp parallel for
                for (size_t i = 0; i < indices.size(); ++i) // loop over all indices
                {
                    const int index = indices[i]; // spectrum index
                    if (index < start_index)
                    {
                        continue; // skip due to index
                    }
                    alignas(64) std::vector<std::vector<double>> spectrum = data.get_spectrum(index); // spectrum at index
                    if (spectrum[0].size() < 5)
                    {
                        continue; // skip due to lack of data, i.e., degree of freedom will be zero
                    }

                    spectrum.push_back(std::vector<double>(spectrum[0].size(), 1.0));                    // add df column for interpolation
                    std::vector<std::vector<double>::iterator> separators;                               // vector of iterators at separation points (x axis)
                    cutData_vec_orbitrap(spectrum, expectedDifference, separators);                      // find separation points in the data
                    interpolateData_vec_orbitrap(spectrum, separators);                                  // interpolate the data
                    extrapolateData_vec_orbitrap(spectrum, separators);                                  // extrapolate the data
                    qpeaks.findCentroids(centroids[i], spectrum, separators, index, retention_times[i]); // find peaks
                } // for
            } // else zero filling (not needed)
            return centroids;
        } // readStreamCraftMZML

        std::vector<std::vector<std::unique_ptr<DataType::Peak>>>
        TensorData::findPeaks_QBIN(
            q::Algorithms::qPeaks &qpeaks,
            std::vector<q::Algorithms::qBinning::EIC> &data)
        {
            std::vector<std::vector<std::unique_ptr<DataType::Peak>>> peaks =
                std::vector<std::vector<std::unique_ptr<DataType::Peak>>>(data.size()); // create vector of unique pointers to peaks
#pragma omp parallel for
            for (size_t i = 0; i < data.size(); ++i) // loop over all data
            {
                const int num_data_points = data[i].scanNumbers.size(); // number of data points
                if (num_data_points < 5)
                {
                    continue; // skip due to lack of data, i.e., degree of freedom will be zero
                }
                std::vector<size_t> indices(data[i].scanNumbers.size());
                std::iota(indices.begin(), indices.end(), 0);
                auto compare = [&data, &i](size_t a, size_t b)
                {
                    return data[i].rententionTimes[a] < data[i].rententionTimes[b];
                };

                std::vector<std::vector<double>> eic;
                if (!std::is_sorted(data[i].rententionTimes.begin(), data[i].rententionTimes.end())) // WILL BE DELETED IN THE FUTURE
                {
                    std::sort(indices.begin(), indices.end(), compare);
                    std::vector<double> rt_sorted(num_data_points);
                    std::vector<double> int_sorted(num_data_points);
                    std::vector<double> mz_sorted(num_data_points);
                    std::vector<double> DQSC_sorted(num_data_points);
                    std::vector<double> DQSB_sorted(num_data_points);
                    for (int j = 0; j < num_data_points; ++j)
                    {
                        rt_sorted[j] = data[i].rententionTimes[indices[j]];
                        int_sorted[j] = data[i].intensities[indices[j]];
                        mz_sorted[j] = data[i].mz[indices[j]];
                        DQSC_sorted[j] = data[i].DQSC[indices[j]];
                        DQSB_sorted[j] = data[i].DQSB[indices[j]];
                    }
                    eic = {rt_sorted, int_sorted, std::vector<double>(num_data_points, 1.0), mz_sorted, DQSC_sorted, DQSB_sorted}; // create vector of retention times and intensities
                }
                else
                {
                    eic = {data[i].rententionTimes,
                           data[i].intensities,
                           std::vector<double>(num_data_points, 1.0),
                           data[i].mz,
                           data[i].DQSC,
                           data[i].DQSB}; // create vector of retention times and intensities
                }

                int num_subsets = zeroFilling_vec(eic, rt_diff, false);             // zero fill the spectrum
                std::vector<std::vector<double>::iterator> separators(num_subsets); // vector of iterators at separation points (x axis)
                extrapolateData_vec(eic, separators);
                qpeaks.findPeaks(peaks[i], eic, separators); // find peaks
            } // parallel for
            return peaks;
        } // readQBinning
    } // namespace MeasurementData
} // namespace q