# Tests CMakeLists.txt
# AES67 macOS Driver - Build #4
# Test suite compilation

cmake_minimum_required(VERSION 3.15)

# Test: SDP Parser
add_executable(TestSDPParser
    TestSDPParser.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestSDPParser PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestSDPParser PRIVATE cxx_std_17)

# Test: Channel Mapper
add_executable(TestChannelMapper
    TestChannelMapper.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestChannelMapper PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestChannelMapper PRIVATE cxx_std_17)

# Test: Ring Buffer
add_executable(TestRingBuffer
    TestRingBuffer.cpp
)

target_include_directories(TestRingBuffer PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRingBuffer PRIVATE cxx_std_17)

# Test: RTP Receiver
add_executable(TestRTPReceiver
    TestRTPReceiver.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPReceiver PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPReceiver PRIVATE cxx_std_17)

# Test: RTP Transmitter
add_executable(TestRTPTransmitter
    TestRTPTransmitter.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestRTPTransmitter PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestRTPTransmitter PRIVATE cxx_std_17)

# Test: PTP Clock
add_executable(TestPTPClock
    TestPTPClock.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Driver/SDPParser.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestPTPClock PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestPTPClock PRIVATE cxx_std_17)

# Test: Stream Manager
add_executable(TestStreamManager
    TestStreamManager.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestStreamManager PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestStreamManager PRIVATE cxx_std_17)

# Integration Test: Multi-Stream Scenarios
add_executable(TestMultiStream
    TestMultiStream.cpp
    ../Driver/SDPParser.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/PTP/PTPClock.cpp
    ../NetworkEngine/PTP/PTPDInterface.cpp
    ../NetworkEngine/PTP/PTPSlave.cpp
    ../NetworkEngine/PTP/PhaseLockedLoop.cpp
    ../NetworkEngine/PTP/CustomSys.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestMultiStream PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestMultiStream PRIVATE cxx_std_17)

# Benchmark: I/O Handler Performance
add_executable(BenchmarkIOHandler
    BenchmarkIOHandler.cpp
    ../Driver/AES67IOHandler.cpp
    ../Shared/Types.cpp
)

target_include_directories(BenchmarkIOHandler PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ASPL_INCLUDE_DIR}
)

target_compile_features(BenchmarkIOHandler PRIVATE cxx_std_17)

# Link libASPL for benchmark
if(ASPL_LIBRARY)
    target_link_libraries(BenchmarkIOHandler PRIVATE ${ASPL_LIBRARY})
endif()

# Integration Test: Full Audio Path
add_executable(TestIntegrationAudioPath
    TestIntegrationAudioPath.cpp
    ../NetworkEngine/RTP/RTPReceiver.cpp
    ../NetworkEngine/RTP/RTPTransmitter.cpp
    ../NetworkEngine/RTP/SimpleRTP.cpp
    ../NetworkEngine/RTP/LockFreeCircularJitterBuffer.cpp
    ../NetworkEngine/RTP/LockFreePacketPool.cpp
    ../Driver/SDPParser.cpp
    ../Driver/AudioThreadPriority.cpp
    ../NetworkEngine/StreamChannelMapper.cpp
    ../NetworkEngine/NetworkInterfaceDetection.cpp
    ../Shared/Types.cpp
)

target_include_directories(TestIntegrationAudioPath PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestIntegrationAudioPath PRIVATE cxx_std_17)

# Regression Test: Critical path defects (links the full driver)
set(DRIVER_TEST_SOURCES ${ALL_CPP_SOURCES})
list(TRANSFORM DRIVER_TEST_SOURCES PREPEND "${CMAKE_SOURCE_DIR}/")

add_executable(TestCriticalPathRegressions
    TestCriticalPathRegressions.cpp
    ${DRIVER_TEST_SOURCES}
)

target_include_directories(TestCriticalPathRegressions PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${ASPL_INCLUDE_DIR}
)

target_compile_features(TestCriticalPathRegressions PRIVATE cxx_std_17)

target_link_libraries(TestCriticalPathRegressions PRIVATE
    ${ASPL_LIBRARY}
    "-framework CoreAudio"
    "-framework CoreFoundation"
    "-framework AudioToolbox"
)

# Test: Step 2 clock foundations (HostTime, MediaClock, TimestampedAudioBuffer)
add_executable(TestClockFoundations
    TestClockFoundations.cpp
    ../NetworkEngine/Clock/TimestampedAudioBuffer.cpp
)

target_include_directories(TestClockFoundations PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_compile_features(TestClockFoundations PRIVATE cxx_std_17)

# Add tests to CTest
enable_testing()
add_test(NAME ClockFoundations COMMAND TestClockFoundations)
add_test(NAME CriticalPathRegressions COMMAND TestCriticalPathRegressions)
add_test(NAME SDPParser COMMAND TestSDPParser)
add_test(NAME ChannelMapper COMMAND TestChannelMapper)
add_test(NAME RingBuffer COMMAND TestRingBuffer)
add_test(NAME RTPReceiver COMMAND TestRTPReceiver)
add_test(NAME RTPTransmitter COMMAND TestRTPTransmitter)
add_test(NAME PTPClock COMMAND TestPTPClock)
add_test(NAME StreamManager COMMAND TestStreamManager)
add_test(NAME MultiStream COMMAND TestMultiStream)
add_test(NAME IntegrationAudioPath COMMAND TestIntegrationAudioPath)

message(STATUS "Tests configured:")
message(STATUS "  - TestSDPParser")
message(STATUS "  - TestChannelMapper")
message(STATUS "  - TestRingBuffer")
message(STATUS "  - TestRTPReceiver")
message(STATUS "  - TestRTPTransmitter")
message(STATUS "  - TestPTPClock")
message(STATUS "  - TestStreamManager")
message(STATUS "  - TestMultiStream (Integration)")
message(STATUS "  - TestIntegrationAudioPath (Integration)")
message(STATUS "")
message(STATUS "Benchmarks configured:")
message(STATUS "  - BenchmarkIOHandler")
message(STATUS "")
message(STATUS "Run tests with: ctest or make test")
message(STATUS "Run benchmarks manually: ./BenchmarkIOHandler")
