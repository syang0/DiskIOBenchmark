all: Benchmark

Benchmark: Benchmark.cpp
	g++ -o Benchmark -O3 -std=c++0x Benchmark.cpp Cycles.cpp

clean:
	rm -f Benchmark *.o *.gch *.log
