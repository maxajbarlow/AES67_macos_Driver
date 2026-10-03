//
// BenchmarkIOHandler.cpp
// AES67 macOS Driver - Build #6
// Performance benchmark for I/O handler batch processing
//

#include "../Driver/AES67IOHandler.h"
#include "../NetworkEngine/RTSafeStreamInterface.h"
#include "../NetworkEngine/Clock/HostTime.h"
#include "../NetworkEngine/Clock/MediaClock.h"
#include "../NetworkEngine/Clock/TimestampedAudioBuffer.h"
#include "../Shared/RingBuffer.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>
#include <numeric>
#include <cmath>
#include <utility>

using namespace AES67;
using namespace std::chrono;

struct BenchmarkResult {
    std::string testName;
    double avgTimeUs;
    double minTimeUs;
    double maxTimeUs;
    double stdDevUs;
    double throughputMBps;
    size_t iterations;
};

class IOHandlerBenchmark {
public:
    IOHandlerBenchmark()
        : inputUnderruns_(0)
        , outputUnderruns_(0)
        , ioRunning_(false)
        , rtInterface_(rxRouting_, mediaClock_, linkOffsetFrames_, txRouting_,
                       inputUnderruns_, outputUnderruns_, ioRunning_)
    {
        // Worst case for input: 16 streams of 8 channels routed onto all 128
        // device channels, each holding audio at the positions being read
        mediaClock_.reset(0, 0, MediaClock::samplesPerTick(48000.0, 1.0, HostTimebase::current()));
        std::vector<float> data(kMaxBenchmarkFrames * kStreamChannels);
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<float>(i % 1000);
        for (size_t s = 0; s < kNumChannels / kStreamChannels; ++s) {
            auto stream = std::make_unique<TimestampedAudioBuffer>(kStreamChannels, 8192);
            stream->write(-linkOffsetFrames_.load(), kMaxBenchmarkFrames, data.data(), kStreamChannels, 0);
            rxRouting_.publish(stream.get(), static_cast<uint32_t>(s * kStreamChannels));
            streams_.push_back(std::move(stream));
        }

        // Worst case for output: 16 TX streams of 8 channels taking all 128
        for (size_t s = 0; s < kNumChannels / kStreamChannels; ++s) {
            auto stream = std::make_unique<TimestampedAudioBuffer>(kStreamChannels, 8192);
            txRouting_.publish(stream.get(), static_cast<uint32_t>(s * kStreamChannels));
            txStreams_.push_back(std::move(stream));
        }

        // Create I/O handler using RT-safe interface
        ioHandler_ = std::make_unique<AES67IOHandler>(rtInterface_);
    }

    ~IOHandlerBenchmark() {
        for (auto& stream : streams_) rxRouting_.unpublish(stream.get());
        for (auto& stream : txStreams_) txRouting_.unpublish(stream.get());
    }

    BenchmarkResult benchmarkInputProcessing(UInt32 frameCount, size_t iterations) {
        std::vector<double> timings;
        timings.reserve(iterations);

        // Prepare test data
        std::vector<float> outputBuffer(frameCount * kNumChannels);

        // Warmup (reads are non-destructive, so the same audio is read every time)
        for (int i = 0; i < 10; ++i) {
            ioHandler_->processInput(outputBuffer.data(), frameCount, kNumChannels, 0.0);
        }

        // Benchmark
        for (size_t i = 0; i < iterations; ++i) {
            auto start = high_resolution_clock::now();

            ioHandler_->processInput(outputBuffer.data(), frameCount, kNumChannels, 0.0);

            auto end = high_resolution_clock::now();
            auto duration = duration_cast<nanoseconds>(end - start);
            timings.push_back(duration.count() / 1000.0); // Convert to microseconds
        }

        return calculateStatistics("Input Processing", timings, frameCount);
    }

    BenchmarkResult benchmarkOutputProcessing(UInt32 frameCount, size_t iterations) {
        std::vector<double> timings;
        timings.reserve(iterations);

        // Prepare test data
        std::vector<float> inputBuffer(frameCount * kNumChannels);
        for (size_t i = 0; i < inputBuffer.size(); ++i) {
            inputBuffer[i] = static_cast<float>(i);
        }

        // Warmup
        for (int i = 0; i < 10; ++i) {
            ioHandler_->processOutput(inputBuffer.data(), frameCount, kNumChannels, 0.0);
        }

        // Benchmark
        for (size_t i = 0; i < iterations; ++i) {
            auto start = high_resolution_clock::now();

            ioHandler_->processOutput(inputBuffer.data(), frameCount, kNumChannels, static_cast<Float64>(i) * frameCount);

            auto end = high_resolution_clock::now();
            auto duration = duration_cast<nanoseconds>(end - start);
            timings.push_back(duration.count() / 1000.0);
        }

        return calculateStatistics("Output Processing", timings, frameCount);
    }

    void printResult(const BenchmarkResult& result) {
        std::cout << std::fixed << std::setprecision(2);
        std::cout << result.testName << " (" << result.iterations << " iterations):" << std::endl;
        std::cout << "  Average: " << std::setw(8) << result.avgTimeUs << " μs" << std::endl;
        std::cout << "  Min:     " << std::setw(8) << result.minTimeUs << " μs" << std::endl;
        std::cout << "  Max:     " << std::setw(8) << result.maxTimeUs << " μs" << std::endl;
        std::cout << "  StdDev:  " << std::setw(8) << result.stdDevUs << " μs" << std::endl;
        std::cout << "  Throughput: " << std::setw(6) << result.throughputMBps << " MB/s" << std::endl;
        std::cout << std::endl;
    }

    void printEstimatedCPU(const BenchmarkResult& result, double sampleRate, UInt32 bufferSize) {
        // Calculate callback frequency
        double callbackFreqHz = sampleRate / bufferSize;

        // CPU time per callback
        double cpuTimePerCallbackMs = result.avgTimeUs / 1000.0;

        // Total CPU time per second
        double cpuTimePerSecondMs = cpuTimePerCallbackMs * callbackFreqHz;

        // CPU usage percentage (assumes single core)
        double cpuPercentage = (cpuTimePerSecondMs / 1000.0) * 100.0;

        std::cout << std::fixed << std::setprecision(2);
        std::cout << "Estimated CPU Usage (@" << sampleRate/1000 << "kHz, " << bufferSize << " frames):" << std::endl;
        std::cout << "  Callback frequency: " << callbackFreqHz << " Hz" << std::endl;
        std::cout << "  CPU time/callback:  " << cpuTimePerCallbackMs << " ms" << std::endl;
        std::cout << "  CPU usage:          " << cpuPercentage << "%" << std::endl;
        std::cout << std::endl;
    }

private:
    static constexpr size_t kNumChannels = 128;

    BenchmarkResult calculateStatistics(const std::string& name,
                                       const std::vector<double>& timings,
                                       UInt32 frameCount) {
        BenchmarkResult result;
        result.testName = name;
        result.iterations = timings.size();

        // Calculate average
        double sum = std::accumulate(timings.begin(), timings.end(), 0.0);
        result.avgTimeUs = sum / timings.size();

        // Find min/max
        result.minTimeUs = *std::min_element(timings.begin(), timings.end());
        result.maxTimeUs = *std::max_element(timings.begin(), timings.end());

        // Calculate standard deviation
        double sqSum = 0.0;
        for (double time : timings) {
            sqSum += (time - result.avgTimeUs) * (time - result.avgTimeUs);
        }
        result.stdDevUs = std::sqrt(sqSum / timings.size());

        // Calculate throughput (MB/s)
        // Data size = frameCount × kNumChannels × sizeof(float)
        double bytesPerIteration = frameCount * kNumChannels * sizeof(float);
        double bytesPerSecond = bytesPerIteration / (result.avgTimeUs / 1000000.0);
        result.throughputMBps = bytesPerSecond / (1024 * 1024);

        return result;
    }


    static constexpr size_t kStreamChannels = 8;
    static constexpr UInt32 kMaxBenchmarkFrames = 4096;

    MediaClock mediaClock_;
    RxRouting rxRouting_;
    std::atomic<int64_t> linkOffsetFrames_{384};
    std::vector<std::unique_ptr<TimestampedAudioBuffer>> streams_;
    TxRouting txRouting_;
    std::vector<std::unique_ptr<TimestampedAudioBuffer>> txStreams_;
    std::atomic<uint64_t> inputUnderruns_;
    std::atomic<uint64_t> outputUnderruns_;
    std::atomic<bool> ioRunning_;
    RTSafeStreamInterface rtInterface_;
    std::unique_ptr<AES67IOHandler> ioHandler_;
};

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "AES67 I/O Handler Performance Benchmark" << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;

    IOHandlerBenchmark benchmark;

    // Test different buffer sizes (common in audio)
    std::vector<UInt32> bufferSizes = {16, 32, 48, 64, 128, 256, 512};
    constexpr size_t kIterations = 10000;

    std::cout << "Testing Input Processing (Network → Core Audio)" << std::endl;
    std::cout << "================================================" << std::endl;
    std::cout << std::endl;

    for (UInt32 bufferSize : bufferSizes) {
        auto result = benchmark.benchmarkInputProcessing(bufferSize, kIterations);
        std::cout << "Buffer Size: " << bufferSize << " frames" << std::endl;
        benchmark.printResult(result);
        benchmark.printEstimatedCPU(result, 48000.0, bufferSize);
    }

    std::cout << std::endl;
    std::cout << "Testing Output Processing (Core Audio → Network)" << std::endl;
    std::cout << "=================================================" << std::endl;
    std::cout << std::endl;

    for (UInt32 bufferSize : bufferSizes) {
        auto result = benchmark.benchmarkOutputProcessing(bufferSize, kIterations);
        std::cout << "Buffer Size: " << bufferSize << " frames" << std::endl;
        benchmark.printResult(result);
        benchmark.printEstimatedCPU(result, 48000.0, bufferSize);
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Benchmark Complete" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
