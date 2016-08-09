#include <thread>
#include <algorithm>

#include <fcntl.h>
#include <locale.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Cycles.h"

using namespace PerfUtils;

/**
 * Disk Benchmark that measures the latency and bandwidth of pread-ing and
 * pwrite-ing in various chunk sizes.
 */

// The Benchmark measures pread/pwrite sizes in powers of 2. The following
// two options configure the minimum and maximum powers of 2 to test.
const int minExp = 9;
const int maxExp = 28;
static_assert(minExp < maxExp, "minExp should be >= maxExp");

// Specifies the byte threshold at which the benchmark should toggle averaging
// each data point smallCount times to bigCount times.
const double smallBigThreshold = 1e6;

// For data points where the read/write size is < smallBigThreshold, average
// the data point over smallCount repetitions
const int smallCount = 100;

// For data points where the read/write size is >= smallBigThreshold, average
// the data point over largeCount repetitions
const int largeCount = 3;

// Derived Configuration 
const int maxSize = 1 << maxExp;

/**
 * Perform the write benchmark by opening filename with extraFileOps options.
 * 
 * \param filename          - File to perform benchmark on
 * \param extraComments     - Comments to add to the benchmark printout
 * \param extraFileOps      - File parameters to open filename with
 */
static void
benchmark_write(const char *filename, const char *extraComments = NULL,
                    int extraFileOps = 0)
{
    printf("# Benchmarking various write sizes to file %s\r\n"
            "# Each result < %0.2lf MB is averaged %d times and "
            "everything >= %0.2lf MB %d times\r\n",
            filename,
            smallBigThreshold/1e6, smallCount,
            smallBigThreshold/1e6, largeCount);

    if (extraComments)
        printf("# Extra Comment: %s\r\n", extraComments);

    setlocale(LC_ALL, "");  // For thousands separators in printf
    printf("# %16s %16s %16s %16s\r\n",
            "Write Size (bytes)",
            "Bandwidth (MB/s)",
            "Write Time (sec)",
            "fsync Time (sec)");

    char *buffer;
    if (posix_memalign(reinterpret_cast<void**>(&buffer), 512, maxSize)) {
        perror("Memalign failed");
        exit(-1);
    }

    if (extraFileOps & O_DIRECT) {
        static_assert(minExp >= 9, "O_DIRECT requires minExp >=9");
        static_assert(maxExp >= 9, "O_DIRECT requires maxExp >=9");
    }

    for (int e = minExp; e <= maxExp; ++e) {
        int outputfile = open(filename, O_WRONLY|O_CREAT|extraFileOps, 0666);
        if (-1 == outputfile) {
            perror("Opening file failed");
            exit(-1);
        }

        uint32_t filePos = 0;
        uint32_t writeSize = 1 << e;
        uint64_t writeCycles = 0, fsyncCycles = 0;
        int count = (writeSize > smallBigThreshold) ? largeCount : smallCount;

        for (int i = 0; i < count; ++i) {
            uint64_t startWrite = Cycles::rdtsc();
            if (writeSize != pwrite(outputfile, buffer, writeSize, filePos)) {
                perror("Write");
                exit(-1);
            } else {
                filePos += writeSize;
            }

            uint64_t fsyncStart = Cycles::rdtsc();
            fsync(outputfile);
            uint64_t stop = Cycles::rdtsc();

            writeCycles += fsyncStart - startWrite;
            fsyncCycles += stop - fsyncStart;
        }

        close(outputfile);
        std::remove(filename);

        uint64_t totalCycles = writeCycles + fsyncCycles;
        printf("%'16d %16.3lf %16.6lf %16.3lf\r\n",
                writeSize,
                (writeSize/(Cycles::toSeconds(totalCycles)/count))/1e6,
                Cycles::toSeconds(writeCycles)/count,
                Cycles::toSeconds(fsyncCycles)/count);
    }

    printf("\r\n");

    free(buffer);
    buffer = NULL;
}

/**
 * Perform the read benchmark by opening filename with extraFileOps options.
 *
 * \param filename          - File to perform benchmark on
 * \param extraComments     - Comments to add to the benchmark printout
 * \param extraFileOps      - File parameters to open filename with
 */
static void
benchmark_read(const char *filename, const char *extraComments = NULL,
                int extraFileOps = 0)
{
    setlocale(LC_ALL, "");  // For separators in printf
    printf("# Benchmarking various read sizes to file %s.\r\n"
            "# Each result < %0.2lf MB is averaged %d times and "
            "everything >= %0.2lf MB %d times\r\n",
            filename,
            smallBigThreshold/1e6, smallCount,
            smallBigThreshold/1e6, largeCount);

    if (extraComments)
        printf("# Extra Comment: %s\r\n", extraComments);

    printf("# %16s %16s %16s\r\n",
            "Read Size (bytes)",
            "Bandwidth (MB/s)",
            "Read Time (sec)");

    char *buffer;
    const int maxSize = std::max((1 << maxExp)*largeCount, (1<<20)*smallCount);
    if (posix_memalign(reinterpret_cast<void**>(&buffer), 512, maxSize)) {
        perror("Memalign failed");
        exit(-1);
    }

    if (extraFileOps & O_DIRECT) {
        static_assert(minExp >= 9, "O_DIRECT requires minExp >=9");
        static_assert(maxExp >= 9, "O_DIRECT requires maxExp >=9");
    }
    
    int file = open(filename, O_RDWR|O_CREAT|extraFileOps, 0666);
    if (-1 == file) {
        perror("Opening file failed");
       exit(-1);
    }

    if (maxSize != write(file, buffer, maxSize)) {
        perror("Initial Write");
       exit(-1);
    }

    fsync(file);

    for (int i = minExp; i <= maxExp;  ++i) {
        uint32_t readSize = 1 << i;
        int count = (readSize > smallBigThreshold) ? largeCount : smallCount;
        uint32_t filePosIncrement = maxSize/count;
        uint64_t readCycles = 0;
        
        for (int i = 0; i < count; ++i) {
            // Seek to a different position (512 aligned) before a read
            int filePos = (i*filePosIncrement) & ~(512 - 1);

            uint64_t startRead = Cycles::rdtsc();
            uint32_t bytesRead = pread(file, buffer, readSize, filePos);
            if (readSize != bytesRead) {
                printf("Expected %u bytes, but read only %d\r\n",
                        readSize, bytesRead);
                perror("Read");
                exit(-1);
            }

            uint64_t stop = Cycles::rdtsc();

            readCycles += (stop - startRead);
        }

        printf("%'16d %16.3lf %16.6lf\r\n",
                readSize,
                (readSize/(Cycles::toSeconds(readCycles)/count))/1e6,
                Cycles::toSeconds(readCycles)/count);
    }
     
    close(file);
    std::remove(filename);

    printf("\r\n");

    free(buffer);
    buffer = NULL;
}

int main(int argc, char** argv) {
    static const char *filename = "/tmp/benchmark.tmp";

    if (argc >= 3) {
        printf("Disk Benchmark that measures the latency and bandwidth of "
                "pread-ing and pwrite-ing in various chunk sizes.\r\n\r\n");
        printf("Usage: ./Benchmark [filePath]\r\n");
        exit(-1);
    }

    if (argc == 2) {
        filename = argv[1];
    }

//    benchmark_write(filename, "Without any special args");
//    benchmark_write(filename, "With O_SYNC", O_SYNC);
//    benchmark_write(filename, "With O_DIRECT", O_DIRECT);
    benchmark_write(filename, "With O_DIRECT|O_SYNC", O_DIRECT|O_SYNC);
    benchmark_read(filename, "With O_DIRECT|O_SYNC", O_DIRECT|O_SYNC);
}

