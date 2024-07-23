// internal
#include "../include/qalgorithms_measurement_data_lcms.h"
#include "../include/qalgorithms_measurement_data_tensor.h"
#include "../include/qalgorithms_qpeaks.h"

// external
#include "../external/StreamCraft/src/StreamCraft_mzml.h"
#include <iostream>
#include <chrono>
#include <fstream>
#include <cstdlib>
#include <iomanip>
#include <filesystem> // printing absolute path in case read fails

// console output
#define PRINT_DONE                         \
    SetConsoleTextAttribute(hConsole, 10); \
    std::cout << "done\n";                 \
    SetConsoleTextAttribute(hConsole, 15);

// console output
#define PRINT_DONE_no_n                    \
    SetConsoleTextAttribute(hConsole, 10); \
    std::cout << "done";                   \
    SetConsoleTextAttribute(hConsole, 15);

int main(int argc, char *argv[])
{
    std::string filename_input;
    std::filesystem::path pathSource;
    std::filesystem::path pathOutput;
    // ask for file if none are specified
    if (argc == 1)
    {
        std::cout << "Enter a filename (replace every backslash with a forward slash) "
                  << "to process that file. You must select an .mzML file.  ";
        std::cin >> filename_input;
        // filename_input = "C:/Users/unisys/Documents/Studium/Messdaten/LCMS_pressure_error/22090901_H2O_1_pos.mzML";
        pathSource = filename_input;
        if (!std::filesystem::exists(pathSource))
        {
            std::cout << "Error: The selected file does not exist.\nSupplied path: " << std::filesystem::absolute(pathSource)
                      << "\nCurrent directory: " << std::filesystem::current_path() << "\n\nTerminated Program.\n\n";
            exit(101); // @todo sensible exit codes
        }
        if (pathSource.extension() != ".mzML") // @todo make sure this is the end of the filename, switch to regex
        {
            std::cout << "Error: the selected file has the type " << pathSource.extension() << ", but only \".mzML\" is supported";
            exit(101); // @todo sensible exit codes
        }
        std::cout << "\nfile accepted, enter the output directory or \"#\" to use the input directory:  ";
        std::cin >> filename_input;
        if (filename_input == "#")
        {
            pathOutput = pathSource;
        }
        else
        {
            pathOutput = filename_input;
            if (pathOutput == pathSource)
            {
                std::cout << "Error: use \"#\" if you want to save your results to the location of your input file";
                exit(101);
            }
        }
    }

    // if only one argument is given, check if it is any variation of help
    // if this is not the case, try to use the string as a filepath

    bool silent = false;
    bool recursiveSearch = false;
    bool fileSpecified = false;

#pragma region cli arguments
    for (int i = 1; i < argc; i++)
    {
        std::string argument = argv[i];
        if ((argument == "-h") | (argument == "-help"))
        {
            std::cout << argv[0] << " help information:\n"
                      << "qAlgorithms is a software project for non-target screening using mass spectrometry."
                      << "For more information, visit our github page: https://github.com/odea-project/qAlgorithms\n"
                      << "As of now (23.07.2024), only mzML files are supported. This program accepts the following command-line arguments:"
                      << "-h, -help: open this help menu\n"
                      << "-s, -silent: do not print progress reports to standard out\n"
                      << "-w, -wordy: print a detailed progress report to standard out"
                      << "-f, -file: specifies the input file. Use as -f FILEPATH\n"
                      << "-tl, -tasklist: pass a list of file paths to the function"
                      << "-r, -recursive: recursive search for .mzML files in the specified directory. "
                      << "Use as -r DIRECTORY\n"
                      << "-o, -output: directory into which all output files should be printed. "
                      << " Use as -o DIRECTORY. If you want to print all results into the folder which "
                      << "contains the .mzML file, write \"#\". The default output is standard out, "
                      << "unless you did not specify an input file. in that case, "
                      << "you will be prompted to enter the output location.\n"
                      << "-pb, -printbins: If this flag is set, both bin summary information and "
                      << "all binned centroids will be printed to the output location in addition "
                      << "to the final peak table. The files end in _summary.csv and _bins.csv.\n"
                      << "-log: This option will create a detailed log file in the program directory.";

            exit(0);
        }
        if ((argument == "-s") | (argument == "-silent"))
        {
            silent = true;
        }
        if ((argument == "-f") | (argument == "-file"))
        {
            // @todo
            fileSpecified = true;
            ++i;
        }
        if ((argument == "-r") | (argument == "-recursive"))
        {
            // @todo
            recursiveSearch = true;
            ++i;
        }
        if ((argument == "-o") | (argument == "-output"))
        {
            // @todo
            recursiveSearch = true;
            ++i;
        }
    }

    if (!fileSpecified)
    {
        std::cout << "Error: no file supplied. Specify a file using the -f flag.\n";
        exit(1);
    }

    if (fileSpecified & recursiveSearch)
    {
        std::cout << "Error: Recursive search and target file are incompatible\n";
        exit(100);
    }

    // initialize qPeaks static variables and lcmsData object
    q::Algorithms::qPeaks::initialize();

    auto timeStart = std::chrono::high_resolution_clock::now();
    // check if file exists; if not, repeat the input until a valid file is entered
    if (!std::ifstream(filename_input))
    {
        std::cout << "Error: file not found\n"
                  << std::endl;
        exit(1);
    }

    std::cout << "reading file...\n";

#pragma region file processing
    sc::MZML data(filename_input);             // create mzML object @todo change to use filesystem::path
    q::Algorithms::qPeaks qpeaks;              // create qPeaks object
    q::MeasurementData::TensorData tensorData; // create tensorData object
    std::vector<std::vector<std::unique_ptr<q::DataType::Peak>>> centroids =
        tensorData.findCentroids_MZML(qpeaks, data, true, 10); // read mzML file and find centroids via qPeaks

    q::Algorithms::qBinning::CentroidedData testdata = qpeaks.passToBinning(centroids, centroids.size());
    std::string summary_output_location = "summary_output_location";
    auto timeEnd = std::chrono::high_resolution_clock::now();

    std::cout << "produced " << testdata.lengthAllPoints << " centroids from " << centroids.size()
              << " spectra in " << (timeEnd - timeStart).count() << " ns\n\n";

    bool silentBinning = true;
    timeStart = std::chrono::high_resolution_clock::now();
    std::vector<q::Algorithms::qBinning::EIC> binnedData = q::Algorithms::qBinning::performQbinning(testdata, summary_output_location, 6, silentBinning, false);
    timeEnd = std::chrono::high_resolution_clock::now();

    if (silentBinning)
    {
        std::cout << "assembled " << binnedData.size() << " bins in " << (timeEnd - timeStart).count() << " ns\n\n";
    }

    timeStart = std::chrono::high_resolution_clock::now();
    std::vector<std::vector<std::unique_ptr<q::DataType::Peak>>> peaks =
        tensorData.findPeaks_QBIN(qpeaks, binnedData);
    timeEnd = std::chrono::high_resolution_clock::now();
    std::cout << "found " << peaks.size() << " peaks in " << (timeEnd - timeStart).count() << " ns\n\n";

    // write peaks to csv file
    std::cout << "writing peaks to file... ";
    std::string output_filename = "output.csv";
    exit(10);
    std::ofstream output_file(output_filename);
    output_file << "mz,rt,int,mzUncertainty,rtUncertainty,intUncertainty,dqs_cen,dqs_bin,dqs_peak\n";
    for (size_t i = 0; i < peaks.size(); ++i)
    {
        for (size_t j = 0; j < peaks[i].size(); ++j)
        {
            output_file << peaks[i][j]->mz << "," << peaks[i][j]->retentionTime << "," << peaks[i][j]->area << ","
                        << peaks[i][j]->mzUncertainty << "," << peaks[i][j]->retentionTimeUncertainty << ","
                        << peaks[i][j]->areaUncertainty << "," << peaks[i][j]->dqsCen << "," << peaks[i][j]->dqsBin
                        << "," << peaks[i][j]->dqsPeak << "\n";
        }
    }
    output_file.close();

    return 0;
}