CXX := g++

CXXFLAGS := -std=c++20 -Wall -Wno-unknown-pragmas -fdiagnostics-color=always -g -fopenmp -O3 -mavx2 -mfma
CXXFLAGS += -I$(CURDIR)/external/StreamCraft/src
CXXFLAGS += -I$(CURDIR)/include
CXXFLAGS += -I$(CURDIR)/external/zlib131
CXXFLAGS += -I$(CURDIR)/external/zlib131/build

LDFLAGS := -L$(CURDIR)/external/zlib131/build

#LDLIBS := -lzlibstatic

LDLIBS = $(CURDIR)/external/zlib131/build/libzlibstatic.a

# Source files
SRCS :=	$(CURDIR)/external/StreamCraft/src/StreamCraft_utils.cpp
SRCS +=	$(CURDIR)/external/StreamCraft/src/StreamCraft_mzml.cpp
SRCS +=	$(CURDIR)/src/qalgorithms_utils.cpp
SRCS +=	$(CURDIR)/src/qalgorithms_measurement_data.cpp
SRCS += $(CURDIR)/src/qalgorithms_measurement_data_tensor.cpp
SRCS +=	$(CURDIR)/src/qalgorithms_qpeaks.cpp
SRCS +=	$(CURDIR)/src/qalgorithms_qbin.cpp
SRCS +=	$(CURDIR)/src/qalgorithms_main.cpp

OBJECTS := $(SRCS:.cpp=.o)

EXECUTABLE := qalgorithms_main.exe

all: $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $(OBJECTS) -o $(EXECUTABLE) $(LDLIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
		del /Q src\*.o
		del /Q external\StreamCraft\src\*.o
		del /Q qalgorithms_main.exe

run: clean all
		./$(EXECUTABLE)

runonly:
		./$(EXECUTABLE)