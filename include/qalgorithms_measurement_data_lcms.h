#ifndef QALGORITHMS_MEASUREMENT_DATA_LCMS_H
#define QALGORITHMS_MEASUREMENT_DATA_LCMS_H

// internal
#include "qalgorithms_measurement_data.h"

#include <unordered_map> 

namespace q {
    /**
     * @brief A class to store LC-MS measurement data
     * @details The LCMSData class is a subclass of the MeasurementData class used to store LC-MS measurement data.
     * 
     * @param data A map of LC-MS data /ref DataType::LC_MS
     */
    class LCMSData : public MeasurementData {
        public:
            // constructors
            LCMSData();
            
            // destructor
            ~LCMSData();

            // methods
            /**
             * @brief Read LC-MS data from a CSV file
             * @details The readCSV method reads LC-MS data from a CSV file and stores it in the LCMSData object. The method takes the filename of the CSV file, the start and end rows and columns, the separator, and the variable types. The variable types are used to determine the type of data in each column. The method requires the variable types: \b scanNumber, \b retentionTime, \b mz, and \b intensity. Other variable types can be added to the list of variable types.\ref DataType::DataField
             * 
             * @param filename The filename of the CSV file
             * @param rowStart The start row (0-based index)
             * @param rowEnd The end row (if -1, then set to the maximum number of rows)
             * @param colStart The start column (0-based index)
             * @param colEnd The end column (if -1, then set to the maximum number of columns)
             * @param separator The separator, e.g., ',' or '\t'
             * @param variableTypes The variable types. Required variable types: DataField::SCANNUMBER, DataField::RETENTIONTIME, DataField::MZ, and DataField::INTENSITY
             *              
             */
            void readCSV(std::string filename,int rowStart, int rowEnd, int colStart, int colEnd, char separator,std::vector<DataType::DataField> variableTypes);
            
            /**
             * @brief Identify and fill gaps in the LC-MS data
             * @details The fillGaps method identifies and fills gaps in the LC-MS data. The method checks the difference between neighboring data points and compares it to the expected difference. If the difference is larger than the expected difference, the method fills the gap with zero values. The expected difference is calculated as the average difference between neighboring data points. However, the maximum number of filling points is limited to 4 per side. I.e. large gaps are filled like this: X 0 0 0 0 large gap 0 0 0 0 X, where X is the original data point.
             */
            void zeroFilling();
            
            // debugging
            void print();
        private:
            std::unordered_map<int, DataType::LC_MS> data;
    };
}
#endif // QALGORITHMS_MEASUREMENT_DATA_LCMS_H
