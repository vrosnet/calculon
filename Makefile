CXX = clang++
LLVM = -I$(shell llvm-config-3.2 --includedir) -lLLVM-3.2
#LLVM = $(shell /tmp/llvm/bin/llvm-config --cflags --ldflags --libs)

all: ctest fractal

ctest: ctest.cc Makefile $(wildcard calculon*.h)
	$(CXX) -g -o $@ $< $(LLVM)
	
fractal: fractal.cc Makefile $(wildcard calculon*.h)
	$(CXX) -g -o $@ $< $(LLVM) -lboost_program_options
	
	