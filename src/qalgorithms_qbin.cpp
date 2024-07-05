#include "../include/qalgorithms_qbin.h"
#include <cassert>
#include <iostream>
#include <vector>
#include <numeric>
#include <math.h>
#include <fstream>
#include <sstream>
#include <algorithm>
// #include <functional>
#include <iterator>
// #include <thread> // unused as of now
// #include <ranges>
#include <deque> // main bin container is a deque
#include <string>
// #include <iomanip> // for printing with full precision
#include <chrono> // time code execution
#include <ctime>
// #include <regex>
#include <omp.h>
#include <filesystem> // printing absolute path in case read fails

namespace q
{
    namespace qBinning
    {
        // declarations of static functions
        /// @brief calculate the mean distance in mz to all other elements of a sorted vector for one element
        /// @param pointsInBin vector of data points sorted by mz
        /// @return vector of the mean inner distances for all elements in the same order as pointsInBin
        static std::vector<double> meanDistance(const std::vector<qCentroid *> pointsInBin);

        /// @brief calculate the data quality score as described by Reuschenbach et al. for one datapoint in a bin
        /// @param MID mean inner distance in mz to all other elements in the bin
        /// @param MOD minimum outer distance - the shortest distance in mz to a data point that is within maxdist and not in the bin
        /// @return the data quality score for the specified element
        static inline double calcDQS(const double MID, const double MOD); // Mean Inner Distance, Minimum Outer Distance

        static void scaleDistancesForDQS_gauss(int maxdist); // @experimental

        // binning-specific variables
        int maxdist;
        std::vector<qCentroid *> outOfBins; // this vector contains all points which are not included in bins
        int duplicatesTotal;                // total times the distance 0 (in scans) was measured in closed bins
        std::vector<double> scalarForMOD;   // This vector contains factors for an inverse gaussian scaling of distances during DQS calculation

#define IGNORE -256 // nonsense value if no real number exists. Must be
                    // negative since it is used later when searching for
                    // the smallest distance. It is not set to -Inf so that
                    // the resulting DQS is still numeric.
#define NO_MIN_FOUND -INFINITY
#define NO_MAX_FOUND -INFINITY

        bool readcsv(CentroidedData *rawData, std::string user_file, int d_mz, int d_mzError, int d_scanNo, int d_intensity, int d_DQScentroid)
        { // @todo stop segfaults when reading empty lines; use buffers for speedup
            int now = time(0);
            int lengthAllPoints = 0;
            rawData->allDatapoints.push_back(std::vector<qCentroid>(0)); // first element empty since scans are 1-indexed
            std::ifstream file(user_file);
            if (!file.is_open())
            {
                std::cout << "the file could not be opened\n";

                return false;
            }

            if (!file.good())
            {
                std::cout << "something is wrong with the input file\n";
                return false;
            }
            std::string line;
            std::string dummy;
            std::getline(file, dummy); // do not read first row @todo check if first row starts with a number; parralelise?
            while (std::getline(file, line))
            {
                std::istringstream ss(line);
                std::string cell;
                std::vector<double> row;
                while (std::getline(ss, cell, ','))
                {
                    row.push_back(std::stod(cell));
                }
                const unsigned int i_scanNo = (unsigned int)row[d_scanNo];
                if (i_scanNo > rawData->allDatapoints.size() - 1)
                {
                    for (size_t i = rawData->allDatapoints.size() - 1; i < i_scanNo; i++)
                    {
                        rawData->allDatapoints.push_back(std::vector<qCentroid>(0));
                    }
                }
                qCentroid F = qCentroid{row[d_mz], row[d_mzError], i_scanNo, row[d_intensity],
                                        row[d_DQScentroid]};

                ++lengthAllPoints;
                rawData->allDatapoints[i_scanNo].push_back(F); // every subvector in allDatapoints is one complete scan - does not require a sorted input file!
            }
            // #pragma omp parallel for
            // for (size_t i = 1; i < rawData->allDatapoints.size(); i++) // make sure data conforms to expectations
            // {
            //     std::sort(rawData->allDatapoints[i].begin(), rawData->allDatapoints[i].end(), [](const qCentroid lhs, const qCentroid rhs)
            //               { return lhs.mz < rhs.mz; });
            // }
            std::cout << "Finished reading file in " << time(0) - now << " seconds\nRead " << lengthAllPoints << " datapoints in " << rawData->allDatapoints.size() << " scans\n";
            return true;
            // CentroidedData is always a vector of vectors where the first index is the scan number (starting at 1) and every scsn is sorted by mz
        }

#pragma region "BinContainer"

        enum SubsetMethods
        {
            mz,
            scans
        };

        BinContainer::BinContainer() {}

        void BinContainer::makeFirstBin(CentroidedData *rawdata) // @todo inline
        {
            finishedBins.reserve(rawdata->lengthAllPoints / 50); // max of two reallocations
            Bin firstBin(rawdata);
            binDeque.push_back(firstBin);
        }

        void BinContainer::subsetBins(const std::vector<int> dimensions, const unsigned int maxdist, bool rebin, const double massError = -1)
        {
            auto timeStart = std::chrono::high_resolution_clock::now();
            auto timeEnd = std::chrono::high_resolution_clock::now();
            std::string subsetType;
            std::cout << "\nOpen Bins: " << binDeque.size() << " - ";

            while (!binDeque.empty()) // while elements are in the bin deque -> if any bin is not fully subset
            {
                for (size_t i = 0; i < dimensions.size(); i++) // dimensions vector allows user to choose order of executing subsetting actions.
                {
                    const size_t startpoint = binDeque.size(); // startpoint is the first element of the deque that will not be subset, equivalent to do n times
                    subsetCount = 0;
                    switch (dimensions[i])
                    {
                    case mz:
                    {
                        subsetType = "MZ";
                        for (size_t j = 0; j < startpoint; j++) // for every element in the deque before writing new bins
                        {
                            binDeque.front().makeOS();
                            if (massError < 0) // @todo more efficient solution?
                            {
                                binDeque.front().makeCumError(); // always after order space, since makeOS sorts
                            }
                            else
                            {
                                binDeque.front().makeCumError(massError);
                            }
                            // takes element from binDeque, starts subsetting, appends bins to binDeque
                            binDeque.front().subsetMZ(&binDeque, binDeque.front().activeOS, 0, binDeque.front().activeOS.size() - 1, subsetCount);
                            binDeque.pop_front(); // remove the element that was processed from binDeque
                        }
                        break;
                    }

                    case scans:
                    {
                        subsetType = "Scans";
                        std::vector<Bin> &target = rebin ? redoneBins : finishedBins;
                        // move bins to the normal container. Choose redoneBins if finsihedBins was already filled once

                        for (size_t j = 0; j < startpoint; j++)
                        {
                            binDeque.front().subsetScan(&binDeque, &target, maxdist, subsetCount);
                            binDeque.pop_front();
                        }

                        break;
                    }

                    default:
                        std::cout << "\nSeparation method " << dimensions[i] << " is not a valid parameter, terminating program\n";
                        exit(201);
                    }
                    timeEnd = std::chrono::high_resolution_clock::now();
                    std::cout << "subset in " << subsetType << "\nTime: " << (timeEnd - timeStart).count() << " ns\nSubsets performed: "
                              << subsetCount << "\nClosed Bins: " << finishedBins.size() << "\n--\nOpen Bins: " << binDeque.size() << " - ";
                    timeStart = std::chrono::high_resolution_clock::now();
                }
            }
            std::cout << "completed subsetting\n";
        }

        void BinContainer::assignDQSB(const CentroidedData *rawdata, const unsigned int maxdist, bool rebin) // @todo assert
        {
            auto timeStart = std::chrono::high_resolution_clock::now();

            std::vector<Bin> &target = rebin ? redoneBins : finishedBins;
            assert(target[0].DQSB_base.size() == 0); // bins are sorted after DQSB calculation, meaning scanMin and ScanMax parameters are wrong

            for (size_t i = 0; i < target.size(); i++)
            {
                target[i].makeDQSB(rawdata, maxdist);
            }
            auto timeEnd = std::chrono::high_resolution_clock::now();
            std::cout << "Finished Calculating DQSBs in " << (timeEnd - timeStart).count() << " ns\n";
        }

        void BinContainer::redoBinningIfTooclose(const std::vector<int> dimensions, const CentroidedData *rawdata, std::vector<qCentroid *> notbinned, const unsigned int maxdist)
        {
            std::cout << "starting re-binning procedure...\n";
            // assemble new bin by copying bins with hot ends into outOfBins
            std::vector<size_t> binsWithHotEnds;

            binsWithHotEnds.reserve(finishedBins.size() / 10);
            for (size_t i = 0; i < finishedBins.size(); i++)
            {
                if (finishedBins[i].l_maxdist_tooclose | finishedBins[i].r_maxdist_tooclose)
                {
                    binsWithHotEnds.push_back(i);
                }
            }
            if (binsWithHotEnds.empty())
            {
                std::cout << "nothing to re-bin, continuing...\n";
                return;
            }

            // Assemble new starting bin
            Bin startingRebin;
            startingRebin.pointsInBin.reserve(outOfBins.size() + binsWithHotEnds.size() * 16);                     // @todo test the average size for non-warburg data
            startingRebin.pointsInBin.insert(startingRebin.pointsInBin.end(), outOfBins.begin(), outOfBins.end()); // copy outofbins into new bin
            outOfBins.resize(0);                                                                                   // @todo is it sensible to leave as much space free?
            // copy old pointsInBin for each selected bin
            for (size_t i : binsWithHotEnds)
            {
                startingRebin.pointsInBin.insert(startingRebin.pointsInBin.end(), finishedBins[i].pointsInBin.begin(), finishedBins[i].pointsInBin.end());
            }
            binDeque.push_back(startingRebin);
            // call binning wrapper with a vector other than finishedbins as write target
            this->subsetBins(dimensions, maxdist, true);

            // calculate DQS on new bins
            this->assignDQSB(rawdata, maxdist, true);
            // re-add the newly constructed bins to finishedBins
            if (redoneBins.size() < binsWithHotEnds.size()) // there will be gaps left in finsihedBins that must be closed
            {
                size_t i = 0;
                for (Bin newBin : redoneBins) // fill gaps with new bins
                {
                    finishedBins[binsWithHotEnds[i]] = newBin;
                    ++i;
                }
                while (i < binsWithHotEnds.size()) // transfer elements from finishedBins
                {
                    finishedBins[binsWithHotEnds[i]] = finishedBins.back();
                    finishedBins.pop_back();
                    ++i;
                }
            }
            else
            {
                size_t i = 0;
                for (size_t j : binsWithHotEnds)
                {
                    finishedBins[j] = redoneBins[i];
                    ++i;
                }
                while (i < redoneBins.size())
                {
                    finishedBins.push_back(redoneBins[i]);
                    ++i;
                }
            }

            std::cout << "re-binning completed\nremoved " << binsWithHotEnds.size() << " incomplete bins, added "
                      << redoneBins.size() << " corrected bins\n";

            redoneBins.resize(0); // allow other functions to use redoneBins
            return;
        }

        void BinContainer::printAllBins(std::string path, const CentroidedData *rawdata)
        {
            std::fstream file_out;
            std::stringstream output;
            file_out.open(path, std::ios::out);
            assert(file_out.is_open());
            // possible symbols
            // char shapes[] = "acGuxnoQzRsTvjIEf"; // random shape when displaying with makie @todo does not work

            output << "mz,scan,ID,DQSC,DQSB_base\n";

            for (size_t i = 0; i < finishedBins.size(); i++) // @todo make faster
            {
                // char x = shapes[rand() % 17];
                // int colour = i % 9; // 10 colours total @todo printf
                std::vector<qCentroid *> binnedPoints = finishedBins[i].pointsInBin;

                for (size_t j = 0; j < binnedPoints.size(); j++)
                {
                    char buffer[128];
                    sprintf(buffer, "%0.15f,%d,%zu,%0.15f,%0.15f\n", binnedPoints[j]->mz, binnedPoints[j]->scanNo, i + 1,
                            binnedPoints[i]->DQScentroid, finishedBins[i].DQSB_base[j]);
                    output << buffer;
                }
            }
            file_out << output.str();
        }

        std::vector<EIC> BinContainer::returnBins()
        {
            std::vector<EIC> finalBins;
            finalBins.reserve(finishedBins.size());
            for (size_t i = 0; i < finishedBins.size(); i++)
            {
                finalBins.push_back(finishedBins[i].createEIC());
            }
            return finalBins;
        };

        void BinContainer::printSelectBins(std::byte mask, bool fullBins, std::string location) // @todo combine with makeBinSelection
        {
            // print optional summary file
            // filename to directory
            size_t found = location.find_last_of("/\\");
            std::cout << location.substr(0, found) << "\n";
            std::filesystem::path p = location.substr(0, found);

            if (!std::filesystem::exists(p))
            {
                std::cout << "Error during summary printing: The selected directory does not exist.\nSupplied path: " << std::filesystem::absolute(p)
                          << "\nCurrent directory: " << std::filesystem::current_path() << "\ncontinuing...\n";
                return;
            }
            if (fullBins)
            {
                std::cout << "writing bin summary and complete centroids to " << std::filesystem::canonical(p) << '\n';
            }
            else
            {
                std::cout << "writing bin summary to " << std::filesystem::canonical(p) << '\n';
            }

            std::string binsSummary = location + "_summary.csv";
            std::vector<size_t> indices;
            std::fstream file_out_sum;
            std::stringstream output_sum;
            file_out_sum.open(binsSummary, std::ios::out);
            assert(file_out_sum.is_open());
            output_sum << "ID,errorcode,size,mean_mz,median_mz,stdev_mz,mean_scans,DQSB_base,DQSB_scaled,DQSC_min,mean_error\n";
            for (size_t i = 0; i < finishedBins.size(); i++)
            {
                SummaryOutput res = finishedBins[i].summariseBin();
                if (bool(mask & res.errorcode))
                {
                    indices.push_back(i); // save these bins for printing
                    char buffer[256];
                    sprintf(buffer, "%llu,%d,%llu,%0.15f,%0.15f,%0.15f,%0.2f,%0.9f,%0.9f,%0.9f,%0.15f\n",
                            i + 1, int(res.errorcode), res.binsize, res.mean_mz, res.median_mz, res.stddev_mz,
                            res.mean_scans, res.DQSB_base, res.DQSB_scaled, res.DQSC_min, res.mean_error);
                    output_sum << buffer;
                }
            }
            file_out_sum << output_sum.str();
            file_out_sum.close();
            // print all bins
            if (fullBins)
            {
                std::string binsFull = location + "_selectBins_full.csv";
                std::fstream file_out_all;
                std::stringstream output_all;
                file_out_all.open(binsFull, std::ios::out);
                assert(file_out_all.is_open());
                output_all << "ID,mz,scan,intensity,mzError,DQSC,DQSB_base,DQSB_scaled\n";
                for (size_t i = 0; i < indices.size(); i++)
                {
                    const unsigned int pos = indices[i];
                    std::vector<qCentroid *> binnedPoints = finishedBins[pos].pointsInBin;

                    for (size_t j = 0; j < binnedPoints.size(); j++)
                    {
                        char buffer[128];
                        sprintf(buffer, "%d,%0.15f,%d,%0.2f,%0.15f,%0.9f,%0.9f,%0.9f\n",
                                pos + 1, binnedPoints[j]->mz, binnedPoints[j]->scanNo,
                                binnedPoints[j]->intensity, binnedPoints[j]->mzError, binnedPoints[j]->DQScentroid,
                                finishedBins[pos].DQSB_base[j], finishedBins[pos].DQSB_scaled[j]);
                        output_all << buffer;
                    }
                }
                file_out_all << output_all.str();
                file_out_all.close();
            }
        };

#pragma endregion "BinContainer"

#pragma region "Bin"

        Bin::Bin() {};

        Bin::Bin(const std::vector<qCentroid *>::iterator &binStartInOS, const std::vector<qCentroid *>::iterator &binEndInOS) // const std::vector<qCentroid> &sourceList,
        {
            pointsInBin = std::vector<qCentroid *>(binStartInOS, binEndInOS);
        }
        Bin::Bin(CentroidedData *rawdata) // @todo relatively time intensive, better solution?; why does rawdata need to be mutable?
        {
            // collect indices for
            pointsInBin.reserve(rawdata->lengthAllPoints);
            for (size_t i = 1; i < rawdata->allDatapoints.size(); i++)
            {
                for (size_t j = 0; j < rawdata->allDatapoints[i].size(); j++)
                {
                    pointsInBin.push_back(&rawdata->allDatapoints[i][j]);
                }
            }
        }
        Bin::~Bin() {}

        void Bin::makeOS()
        {
            std::sort(pointsInBin.begin(), pointsInBin.end(), [](const qCentroid *lhs, const qCentroid *rhs)
                      { return lhs->mz < rhs->mz; });

            activeOS.reserve(pointsInBin.size());               // OS = Order Space
            for (size_t i = 0; i < pointsInBin.size() - 1; i++) // -1 to prevent accessing outside of vector
            {
                activeOS.push_back((pointsInBin[i + 1]->mz - pointsInBin[i]->mz) * 1000000);
            }
            activeOS.push_back(NAN); // last element of OS is never checked, dummy value due to recursive function
        }

        void Bin::makeCumError()
        {
            cumError.reserve(pointsInBin.size());
            cumError.push_back(0); // the first element has a precursor error of 0
            std::transform(pointsInBin.begin(), pointsInBin.end(), back_inserter(cumError), [](qCentroid *F)
                           { return F->mzError * 1000000; });                     // *1000000 for compatibility with ppm
            std::partial_sum(cumError.begin(), cumError.end(), cumError.begin()); // cumulative sum
        }

        void Bin::makeCumError(const double ppm)
        {
            cumError.reserve(pointsInBin.size());
            std::transform(pointsInBin.begin(), pointsInBin.end(), back_inserter(cumError), [ppm](qCentroid *F)
                           { return F->mz * ppm; });
            std::partial_sum(cumError.begin(), cumError.end(), cumError.begin());
        }

        void Bin::subsetMZ(std::deque<Bin> *bincontainer, std::vector<double> &OS, const int binStartInOS, const int binEndInOS, unsigned int &counter) // bincontainer is binDeque of BinContainer // OS cannot be solved with pointers since index has to be transferred to frature list
        {
            // @todo bad data input still causes an early termination here
            assert(binStartInOS >= 0);
            assert(binEndInOS >= binStartInOS);
            const int binsizeInOS = binEndInOS - binStartInOS + 1; // +1 to avoid length zero

            if (binsizeInOS < 5)
            {
                for (int i = 0; i < binsizeInOS; i++)
                {
                    qCentroid *F = *(pointsInBin.begin() + binStartInOS + i);
                    outOfBins.push_back(F);
                }
                return;
            }
            ++counter;
            auto pmax = std::max_element(OS.begin() + binStartInOS, OS.begin() + binEndInOS);

            double vcrit = 3.05037165842070 * pow(log(binsizeInOS), (-0.4771864667153)) * // critical value for alpha = 0.01 @todo add functionality for custom alpha?
                           (cumError[binEndInOS + 1] - cumError[binStartInOS]);           // + 1 to binEnd since cumerror starts at 0
            double max = *pmax * binsizeInOS;                                             // moved binsize here since multiplication is faster than division

            if (max < vcrit) // all values in range are part of one mz bin
            {
                Bin output(pointsInBin.begin() + binStartInOS, pointsInBin.begin() + binEndInOS + 1); // binEndInOS+1 since the iterator has to point behind the last element to put into the vector
                bincontainer->push_back(output);
                return;
            }
            else
            {
                const int cutpos = std::distance(OS.begin() + binStartInOS, pmax);
                subsetMZ(bincontainer, OS, binStartInOS, binStartInOS + cutpos, counter);
                subsetMZ(bincontainer, OS, binStartInOS + cutpos + 1, binEndInOS, counter);
                return;
            }
        }

        void Bin::subsetScan(std::deque<Bin> *bincontainer, std::vector<Bin> *finishedBins, const int maxdist, unsigned int &counter)
        {
            // function is called on a bin sorted by mz
            int control_duplicatesIn = 0;
            const size_t binSize = pointsInBin.size();
            std::sort(pointsInBin.begin(), pointsInBin.end(), [](qCentroid *lhs, qCentroid *rhs)
                      { return lhs->scanNo < rhs->scanNo; });
            std::vector<qCentroid *>::iterator newstart = pointsInBin.begin();
            int lastpos = 0;
            for (size_t i = 0; i < binSize - 1; i++) // -1 since difference to next data point is checked @todo rework to start at i = 0
            {
                int distanceScan = pointsInBin[i + 1]->scanNo - pointsInBin[i]->scanNo;
                if (distanceScan > maxdist) // bin needs to be split
                {
                    ++counter;
                    // less than five points in bin
                    if (i - lastpos + 1 < 5) // +1 since i starts at 0
                    {
                        for (size_t j = lastpos; j <= i; j++) // @todo make macro for compile time exclusion
                        {
                            qCentroid *F = *(pointsInBin.begin() + j);
                            outOfBins.push_back(F);
                        }
                    }
                    else
                    {
                        // viable bin, stable in scan dimension @todo fixed error of last element being omitted
                        Bin output(newstart, pointsInBin.begin() + i + 1); // +1 since otherwise last element of the correct range is not included
                        bincontainer->push_back(output);
                    }
                    lastpos = i + 1;                          // sets previous i to the position one i ahead, since
                    newstart = pointsInBin.begin() + lastpos; // for the next split this is the first element
                }
                else if (distanceScan == 0)
                {
                    duplicateScan = true;
                    ++control_duplicatesIn;
                }
            }
            // check for open bin at the end
            if (lastpos == 0) // no cut has occurred
            {
                finishedBins->push_back(bincontainer->front());
                duplicatesTotal += control_duplicatesIn;
            }
            else if (binSize - lastpos >= 5) // binsize starts at 1 // @todo check for correctness
            {
                // viable bin, stable in scan dimension @todo checked for correctness
                Bin output(newstart, pointsInBin.end());
                bincontainer->push_back(output);
            }
            else
            {
                for (size_t j = lastpos; j < pointsInBin.size(); j++) // @todo checked for correctness
                {
                    qCentroid *F = *(pointsInBin.begin() + j);
                    outOfBins.push_back(F);
                }
            }
        }

        void Bin::makeDQSB(const CentroidedData *rawdata, const unsigned int maxdist) // @todo split this function
        {
            // assumes bin is saved sorted by scans, since the result from scan gap checks is the final control
            const size_t binsize = pointsInBin.size();
            DQSB_base.reserve(binsize);
            DQSB_scaled.reserve(binsize);
            // determine start and end of relevant scan section, used as repeats for the for loop; -1 since accessed vector is zero-indexed
            // @todo make it so this is not dependent on getting a scan-sorted bin
            const unsigned int minScanInner = pointsInBin.front()->scanNo;
            int scanRangeStart = pointsInBin.front()->scanNo - maxdist;
            int scanRangeEnd = pointsInBin.back()->scanNo + maxdist;

            // determine min and max in mz - sort, since then calculation of inner distances is significantly faster
            std::sort(pointsInBin.begin(), pointsInBin.end(), [](qCentroid *lhs, qCentroid *rhs)
                      { return lhs->mz < rhs->mz; });
            const double minInnerMZ = pointsInBin.front()->mz;
            const double maxInnerMZ = pointsInBin.back()->mz;

            auto mzMax = *std::max_element(pointsInBin.begin(), pointsInBin.end(), [](qCentroid *lhs, qCentroid *rhs)
                                           { return lhs->mz < rhs->mz; });

            auto mzMin = *std::min_element(pointsInBin.begin(), pointsInBin.end(), [](qCentroid *lhs, qCentroid *rhs)
                                           { return lhs->mz < rhs->mz; });

            assert(mzMax->mz == maxInnerMZ);
            assert(mzMin->mz == minInnerMZ);

            std::vector<double> meanInnerDistances = meanDistance(pointsInBin);
            // find median in mz
            const int posMedian = binsize / 2;
            if (binsize & 1)
            {
                medianMZ = pointsInBin[posMedian]->mz; // for lenght 1: index 0
            }
            else
            {
                medianMZ = (pointsInBin[posMedian]->mz + pointsInBin[posMedian + 1]->mz) / 2; // +1 to round to nearest, since integers are truncated
            }

            int maxScansReduced = 0;              // add this many dummy values to prevent segfault when bin is in one of the last scans
            std::vector<double> minMaxOutPerScan; // contains both mz values (per scan) next or closest to all m/z in the bin

            int edgecase = 0;

            if (scanRangeStart < 1)
            {
                for (int i = 0; i < (1 - scanRangeStart) * 2; i++) // fill with dummy values to prevent segfault when distance checker expects negative scan number
                {
                    minMaxOutPerScan.push_back(IGNORE);
                }
                scanRangeStart = 1;
                edgecase++;
            }
            if (scanRangeEnd > (rawdata->allDatapoints.size() - 1))
            {
                maxScansReduced = scanRangeEnd - rawdata->allDatapoints.size() - 1; // dummy values have to be added later
                scanRangeEnd = rawdata->allDatapoints.size() - 1;
                edgecase += 2;
            }
            assert(scanRangeEnd < rawdata->allDatapoints.size());

            minMaxOutPerScan.reserve((scanRangeEnd - scanRangeStart + 1) * 2);

            // for all scans relevant to the bin
            int needle = 0; // position in scan where a value was found - starts at 0 for first scan

            for (int i = scanRangeStart; i <= scanRangeEnd; i++)
            {
                bool minFound = false; // only execute search if min or max is present in the scan
                bool maxFound = false;
                const int scansize = rawdata->allDatapoints[i].size() - 1;
                if (scansize == -1)
                {
                    minMaxOutPerScan.push_back(NO_MIN_FOUND);
                    minMaxOutPerScan.push_back(NO_MAX_FOUND);
                    continue;
                }
                else if (needle > scansize)
                {
                    needle = scansize; // prevent searching outside of the valid scan region
                }
                // check if the bin is outside the region this scan covers
                if (rawdata->allDatapoints[i][0].mz >= minInnerMZ)
                {
                    minMaxOutPerScan.push_back(NO_MIN_FOUND);
                    minFound = true;
                    if (rawdata->allDatapoints[i][0].mz > maxInnerMZ)
                    {
                        minMaxOutPerScan.push_back(rawdata->allDatapoints[i][0].mz);
                        assert(minMaxOutPerScan.back() > maxInnerMZ);
                        maxFound = true;
                        continue;
                    }
                    else
                    {
                        needle = 0;
                    }
                }
                // check end of bin
                // double lastmz = rawdata->allDatapoints[i][scansize].mz;
                if (rawdata->allDatapoints[i][scansize].mz <= maxInnerMZ)
                {
                    minMaxOutPerScan.push_back(NO_MAX_FOUND);
                    maxFound = true;
                    if (rawdata->allDatapoints[i][scansize].mz < minInnerMZ)
                    {
                        minMaxOutPerScan.push_back(rawdata->allDatapoints[i][scansize].mz);
                        assert(minMaxOutPerScan.back() < minInnerMZ);
                        minFound = true;
                        continue; // @todo work with goto here
                    }
                    else
                    {
                        needle = scansize;
                    }
                }
                // rawdata is always sorted by mz within scans @todo add goto to skip needless loop steps
                // use two while loops to find desired value from any place in the scan, accounts for large shifts between scans
                if (!minFound)
                {
                    while (rawdata->allDatapoints[i][needle].mz < minInnerMZ)
                    {
                        ++needle; // steps through the dataset and increments until needle is the first value >= minInnerMZ
                    }
                    while (rawdata->allDatapoints[i][needle].mz >= minInnerMZ)
                    {
                        --needle; // steps through the dataset and decrements until needle is the desired mz value
                    }
                    minMaxOutPerScan.push_back(rawdata->allDatapoints[i][needle].mz);
                    assert(minMaxOutPerScan.back() < minInnerMZ);
                }

                if (!maxFound)
                {
                    while (rawdata->allDatapoints[i][needle].mz > maxInnerMZ)
                    {
                        --needle;
                    }
                    while (rawdata->allDatapoints[i][needle].mz <= maxInnerMZ)
                    {
                        ++needle;
                    }
                    minMaxOutPerScan.push_back(rawdata->allDatapoints[i][needle].mz);
                    assert(minMaxOutPerScan.back() > maxInnerMZ);
                }
            }
            // minMaxOutPerScan contains the relevant distances in order of scans, with both min and max per scan being stored for comparison
            if (maxScansReduced != 0) // add dummy values after last scan
            {
                for (int i = 0; i < 2 * maxScansReduced; i++)
                {
                    minMaxOutPerScan.push_back(IGNORE);
                }
            }

            // calculate critical DQS for finding points that are within the critical distance in mz and maxdist scans
            double meanerror = std::accumulate(pointsInBin.begin(), pointsInBin.end(), 0.0, [](double error, const qCentroid *point)
                                               { return error + point->mzError; }) /
                               binsize;
            double vcrit = 3.05037165842070 * pow(log(binsize + 1), (-0.4771864667153)) * meanerror;
            // binsize + 1 to not include points which would be removed after adding them
            // vcrit has the same scaling as mz of bin centroids

            // find min distance in minMaxOutPerScan, then calculate DQS for that point
            for (size_t i = 0; i < binsize; i++)
            {
                double minDist_base = INFINITY;
                double minDist_scaled = INFINITY;
                const double currentMZ = pointsInBin[i]->mz;

                // currentRangeStart gives position of the first value in minMaxOutPerScan that must be considered,
                // assuming the first value in minMaxOutPerScan (index 0) is only relevant to the
                // qCentroid in the lowest scan. For every increase in scans, that range starts two elements later
                const unsigned int currentRangeStart = (pointsInBin[i]->scanNo - minScanInner) * 2;
                const unsigned int currentRangeEnd = currentRangeStart + maxdist * 4 + 1;
                for (unsigned int j = currentRangeStart; j < currentRangeEnd; j++) // from lowest scan to highest scan relevant to this
                // point, +1 since scan no of point has to be included.
                {
                    if (currentMZ == minMaxOutPerScan[j])
                    {
                        std::cout << std::setprecision(15) << "length: " << binsize << " position: " << i << " equal mz: "
                                  << currentMZ << " rangeStart: " << currentRangeStart << " current: " << j
                                  << " rangeEnd: " << currentRangeEnd << " mzMax: " << maxInnerMZ << " mzMin: "
                                  << minInnerMZ << " edge: " << edgecase << "\n";
                    }
                    double dist = std::abs(currentMZ - minMaxOutPerScan[j]);
                    if (dist < minDist_base)
                    {
                        minDist_base = dist;
                        assert(minDist_base > 0); // error if one mass appears twice in the data
                    }
                    if (dist * scalarForMOD[j - currentRangeStart] < minDist_scaled) // scaling is calculated by a separate function
                    {
                        minDist_scaled = dist * scalarForMOD[j - currentRangeStart];
                    }
                }

                DQSB_base.push_back(calcDQS(meanInnerDistances[i], minDist_base));
                DQSB_scaled.push_back(calcDQS(meanInnerDistances[i], minDist_scaled));

                /// section to check hot ends ///
                // if the DQS at either end is less than critDQS, a point exists that could be included in the bin
                if (i == 0) // first element of the bin when sorted by mz
                {
                    double critDQS = calcDQS(meanInnerDistances[i], vcrit);
                    if (critDQS > DQSB_base[i])
                    {
                        l_maxdist_tooclose = true;
                    }
                }
                if (i == binsize - 1) // last element of the bin when sorted by mz
                {
                    double critDQS = calcDQS(meanInnerDistances[i], vcrit);
                    if (critDQS > DQSB_base[i])
                    {
                        r_maxdist_tooclose = true;
                    }
                }
            }
            // since rebinning does not fully eliminate bins with too strict separation,
            // it is important to keep track of bins with one-sided overlap. The maxdist_abs
            // parameters are set here since vcrit is already calculated and the bin sorted.
            l_maxdist_abs = pointsInBin.front()->mz - vcrit;
            r_maxdist_abs = pointsInBin.back()->mz + vcrit;
            return;
        }

        SummaryOutput Bin::summariseBin()
        {
            size_t binsize = pointsInBin.size();
            double meanMZ = 0;
            double meanScan = 0;
            double meanDQS_base = 0;
            double meanDQS_scaled = 0;
            double DQS_control = 0;
            double meanCenError = 0;
            double worstCentroid = INFINITY;
            for (size_t i = 0; i < binsize; i++)
            {
                meanMZ += pointsInBin[i]->mz;
                meanScan += pointsInBin[i]->scanNo;
                // DQS_control += pointsInBin[i]->control_DQSbin;
                meanCenError += pointsInBin[i]->mzError;
                if (pointsInBin[i]->DQScentroid < worstCentroid)
                {
                    worstCentroid = pointsInBin[i]->DQScentroid;
                }

                meanDQS_base += DQSB_base[i];
                meanDQS_scaled += DQSB_scaled[i];
            }

            meanMZ = meanMZ / binsize;
            meanScan = meanScan / binsize;
            meanDQS_base = meanDQS_base / binsize;
            meanDQS_scaled = meanDQS_scaled / binsize;
            meanCenError = meanCenError / binsize;
            DQS_control = DQS_control / binsize;

            // calc DQS when assuming the MOD to be critval
            double sumOfDist = 0;
            std::for_each(pointsInBin.begin(), pointsInBin.end(), [&](const qCentroid *point)
                          { sumOfDist += (point->mz - meanMZ) * (point->mz - meanMZ); }); // squared distance to mean

            double stdev = sqrt(sumOfDist / (binsize - 1));

            std::sort(pointsInBin.begin(), pointsInBin.end(), [](qCentroid *lhs, qCentroid *rhs)
                      { return lhs->scanNo < rhs->scanNo; });

            // test for one-sided intensity profile
            bool halfPeakL = true;
            double prevInt = 0;
            int wrongCount = 0;
            for (qCentroid *cen : pointsInBin)
            {
                if (cen->intensity > prevInt)
                {
                    prevInt = cen->intensity;
                    wrongCount = 0;
                }
                else
                {
                    ++wrongCount;
                }
                if (wrongCount > 2)
                {
                    halfPeakL = false;
                    break;
                }
            }

            bool halfPeakR = true;
            prevInt = INFINITY;
            wrongCount = 0;
            for (qCentroid *cen : pointsInBin)
            {
                if (cen->intensity < prevInt)
                {
                    prevInt = cen->intensity;
                    wrongCount = 0;
                }
                else
                {
                    ++wrongCount;
                }
                if (wrongCount > 2)
                {
                    halfPeakR = false;
                    break;
                }
            }

            // @todo use enums here
            std::byte selector{0b00000000}; // used as bitmask during printSelectBins()
            if (duplicateScan)
            {
                selector |= std::byte{0b00000001};
            }
            if (l_maxdist_tooclose)
            {
                selector |= std::byte{0b00000010};
            }
            if (r_maxdist_tooclose)
            {
                selector |= std::byte{0b00000100};
            }
            if (abs(meanMZ - medianMZ) > 2 * meanCenError)
            {
                selector |= std::byte{0b00001000};
            }
            if ((meanMZ + 3 * stdev < r_maxdist_abs) | (meanMZ - 3 * stdev > l_maxdist_abs)) // if a value in the bin is outside of 3 sigma
            {
                selector |= std::byte{0b00010000};
            }
            if (meanDQS_base < 0.5) // @todo these should just be removed by default
            {
                selector |= std::byte{0b00100000};
            }
            if (halfPeakL | halfPeakR) // mostly uniform increase in intensity over bin
            {
                selector |= std::byte{0b01000000};
            }
            if (true)
            {
                selector |= std::byte{0b10000000};
            }

            return SummaryOutput{selector, binsize, meanMZ, medianMZ, stdev, meanScan, medianScan, meanDQS_base,
                                 meanDQS_scaled, worstCentroid, meanCenError}; // @todo remove worst dqs from output
        }

        EIC Bin::createEIC() // @todo add option for return in vectors
        {
            const size_t binsize = pointsInBin.size();
            std::vector<DatapointEIC> tmp_pointsInEIC;
            double tmp_meanDQS = 0;
            double tmp_meanMZ = 0;
            double tmp_maxInt = 0;
            for (size_t i = 0; i < binsize; i++)
            {
                tmp_meanDQS += DQSB_base[i];
                tmp_meanMZ += pointsInBin[i]->mz;
                tmp_maxInt = std::max(tmp_maxInt, pointsInBin[i]->intensity);
                DatapointEIC F = {
                    pointsInBin[i]->mz,
                    // pointsInBin[i]->RT,
                    pointsInBin[i]->scanNo,
                    pointsInBin[i]->intensity,
                    DQSB_base[i]};
                tmp_pointsInEIC.push_back(F);
            }
            // medianScan is determined with the DQSB_base
            EIC returnEIC = {
                tmp_pointsInEIC,
                tmp_meanDQS / binsize,
                tmp_meanMZ / binsize,
                medianScan,
                tmp_maxInt};

            return returnEIC;
        }

#pragma endregion "Bin"

#pragma region "Functions"

        static std::vector<double> meanDistance(const std::vector<qCentroid *> pointsInBin)
        {
            // assumes bin is sorted by mz
            const size_t binsize = pointsInBin.size();
            const double ld_binsize(binsize);
            std::vector<double> output(binsize);
            double totalSum = std::accumulate(pointsInBin.begin(), pointsInBin.end(), 0.0, [](double acc, const qCentroid *point)
                                              { return acc + point->mz; }); // totalSum is the sum of all mz ahead of the current element
            double beforeSum = 0;                                           // beforeSum is the sum of all mz which had their distance calculated already
            for (size_t i = 0; i < binsize; i++)
            {
                const double ld_i(i);
                double v1 = fmal(pointsInBin[i]->mz, -(ld_binsize - ld_i), totalSum); // sum of all distances to mz ahead of the current element
                // (totalSum - pointsInBin[i]->mz * (binsize - i));
                double v2 = fmal(pointsInBin[i]->mz, ld_i, -beforeSum); // sum of all distances to mz past the current element (starts at 0)
                // (pointsInBin[i]->mz * i - beforeSum);
                beforeSum += pointsInBin[i]->mz;
                totalSum -= pointsInBin[i]->mz;
                output[i] = (v1 + v2) / (binsize - 1); // -1 since the distance of an element to itself is not factored in
            }
            return output;
        }

        static inline double calcDQS(const double MID, const double MOD) // mean inner distance, minimum outer distance
        {
            double dqs(MOD); // MOD should always be less than MID, less cache misses
            if (dqs < MID)
            {
                dqs = MID;
            }
            // dqs = (MOD - MID) * (1 / (1 + MID)) / dqs; // sm(i) term
            // interval transform, equivalent to (dqs + 1) / 2;
            // if dqs = nan, set it to 1 during EIC construction @todo
            return fmal((MOD - MID) / fmal(MID, dqs, dqs), 0.5, 0.5);
        }

        static inline double calcVcrit(const int binSize, const double errorStart, const double errorEnd)
        {
            return 3.05037165842070 * pow(log(binSize), (-0.4771864667153)) * (errorEnd - errorStart) / binSize;
        }

        static void scaleDistancesForDQS_gauss(int maxdist) // @experimental
        {
            // calculate the gauss function transformed so that f(x = 0) == 1
            static const double scaleHeight = 2.506628274631;    // scale the curve height
            static const double xAtMaxdist = 2.575829;           // x for the normal dist at maxdist. x for maxdist*2+1 == 0
            const double xPerStep = xAtMaxdist / maxdist;        // point at which the gauss function is calculated at x=1
            static const double invSqrt2Pi = 0.3989422804014327; // 1/(sqrt(2*pi*sigma^2)) for sigma == 1
            scalarForMOD.resize(maxdist * 4 + 2);                // every value is doubled, same length as selected mz vector during DQS calculation
            for (int i = 0; i < maxdist; i++)                    // add the x values where the gauss curve must be calculated
            {
                scalarForMOD[i * 2] = xPerStep * (maxdist - i);
            }
            for (int i = 0; i < 2 * maxdist + 1; i++) // scalarForMod contains the correct scaling to be used on the MOD for any maxdist
            {
                // 1 (base scaling) + 1 - gauss mod (scalar for distance) ; equivalent of an upside-down gauss curve with f(x = 0) == 1 used for scaling
                const double scalarSingle = 1 + 1 - scaleHeight * invSqrt2Pi * std::exp(-scalarForMOD[i] * scalarForMOD[i] / 2);
                scalarForMOD[i] = scalarSingle;
                scalarForMOD[i + 1] = scalarSingle;
                scalarForMOD[scalarForMOD.size() - 1 - i] = scalarSingle;
                scalarForMOD[scalarForMOD.size() - 2 - i] = scalarSingle; // values at maxdist and maxdist + 1 are always == 1 + (floating point error)
                i++;
            }
        }

#pragma endregion "Functions"

        std::vector<EIC> performQbinning(CentroidedData centroidedData, std::string outpath, int inputMaxdist = 6, bool silent = false)
        {
            std::streambuf *old = std::cout.rdbuf(); // save standard out config
            std::stringstream ss;

            if (silent) // @todo change this to a global variable
            {
                std::cout.rdbuf(ss.rdbuf());
            }

            std::cout << "starting binning process...\n";

            maxdist = inputMaxdist;
            duplicatesTotal = 0;
            BinContainer activeBins;
            activeBins.makeFirstBin(&centroidedData);
            std::vector<int> measurementDimensions = {SubsetMethods::mz, SubsetMethods::scans}; // at least one element must be terminator
            activeBins.subsetBins(measurementDimensions, maxdist, false);

            std::cout << "Total duplicates: " << duplicatesTotal << "\n--\ncalculating DQSBs...\n";

            // calculate data quality scores
            scaleDistancesForDQS_gauss(inputMaxdist); // this sets q::scalarForMOD to the correct scaling so that at the distance in scans == maxdist the curve encloses 99% of its area
            activeBins.assignDQSB(&centroidedData, maxdist, false);

            // @todo add a way for selection / bin summary to be given upstream

            // @todo add bin reassembly code here
            activeBins.redoBinningIfTooclose(measurementDimensions, &centroidedData, q::qBinning::outOfBins, q::qBinning::maxdist);

            activeBins.printSelectBins(std::byte{0b11111111}, true, outpath); //@todo make this a function parameter

            std::cout << "\n\nDone!\n\n";

            std::cout.rdbuf(old); // restore previous standard out

            // @todo bins must be sorted by scans before being passed to centroiding

            return activeBins.returnBins();
        }

    }
}
int main()
{
    std::cout << "starting...\n";

    q::qBinning::duplicatesTotal = 0;

    int inputMaxdist = 6;
    q::qBinning::maxdist = inputMaxdist;

    std::string filename_input = "../../rawdata/210229_C1_S1_W_MI_1_pos_centroids_a.csv"; // no variability after qCentroiding

    const std::filesystem::path p = filename_input;
    if (!std::filesystem::exists(p))
    {
        std::cout << "Error: The selected file does not exist.\nSupplied path: " << std::filesystem::absolute(p)
                  << "\nCurrent directory: " << std::filesystem::current_path() << "\n\nTerminated Program.\n\n";
        exit(101);
    }

    q::qBinning::CentroidedData testdata;
    // path to data, mz, centroid error, scan number, intensity, DQS centroid
    //  path to data, scan position, mz, centroid error, scan number, intensity, DQS centroid
    // if (!q::qBinning::readcsv(&testdata, filename_input, 0, 1, 3, 4, 6)) // ../../rawdata/control_bins.csv reduced_DQSdiff
    if (!q::qBinning::readcsv(&testdata, filename_input, 1, 2, 3, 4, 5))
    {
        exit(101); // error codes: 1.. = reading / writing failed, 2.. = improper input,
    }

    q::qBinning::BinContainer testcontainer;
    testcontainer.makeFirstBin(&testdata);
    std::vector<int> measurementDimensions = {q::qBinning::SubsetMethods::mz, q::qBinning::SubsetMethods::scans}; // at least one element must be terminator
    testcontainer.subsetBins(measurementDimensions, inputMaxdist, false);                                         // add value after maxdist for error in ppm instead of centroid error
    std::cout << "Total duplicates: " << q::qBinning::duplicatesTotal << "\n--\ncalculating DQSBs...\n";

    // calculate data quality scores
    q::qBinning::scaleDistancesForDQS_gauss(inputMaxdist);    // this sets q::scalarForMOD to the correct scaling so that at the distance in scans == maxdist the curve encloses 99% of its area
    testcontainer.assignDQSB(&testdata, inputMaxdist, false); // int = max dist in scans

    testcontainer.redoBinningIfTooclose(measurementDimensions, &testdata, q::qBinning::outOfBins, q::qBinning::maxdist);

    // print bin selection
    testcontainer.printSelectBins(std::byte{0b11111111}, true, "../.."); // one bit per test

    // testcontainer.printBinSummary("../../summary_bins.csv");
    // testcontainer.printworstDQS();
    // testcontainer.printTstats();
    // testcontainer.printAllBins("../../qbinning_binlist.csv", &testdata);
    std::cout << "printed all bins\n";

    std::cout << "\n\nDone!\n\n";
    return 0;
}
