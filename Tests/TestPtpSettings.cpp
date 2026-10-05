//
// TestPtpSettings.cpp
// AES67 macOS Driver
// PTP settings (ptp.json): off unless a valid file turns PTP on.
//

#include "../NetworkEngine/PTP/PtpSettings.h"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace AES67::Ptp;

namespace {

int checksPassed = 0;
int checksFailed = 0;

#define CHECK(condition, message)                                              \
    do {                                                                       \
        if (condition) {                                                       \
            ++checksPassed;                                                    \
        } else {                                                               \
            ++checksFailed;                                                    \
            std::cerr << "  FAIL: " << message << " (" << __FILE__ << ":"      \
                      << __LINE__ << ")" << std::endl;                         \
        }                                                                      \
    } while (0)

void testParsesAllFields() {
    std::cout << "A full settings file" << std::endl;
    const Settings s = Settings::parse(R"({ "enabled": true, "interface": "en7", "domain": 5, "hybrid": true })");
    CHECK(s.enabled && s.networkInterface == "en7" && s.domain == 5 && s.unicastDelayRequests, "every field is read");
}

void testDefaults() {
    std::cout << "Missing fields take defaults; PTP stays off unless enabled" << std::endl;
    const Settings none = Settings::parse("{}");
    CHECK(!none.enabled && none.networkInterface.empty() && none.domain == 0 && !none.unicastDelayRequests,
          "an empty object is all defaults, PTP off");
    const Settings on = Settings::parse(R"({"enabled":true})");
    CHECK(on.enabled && on.networkInterface.empty() && on.domain == 0 && !on.unicastDelayRequests,
          "enabled alone: the primary interface, domain 0, multicast Delay_Req");
    const Settings off = Settings::parse(R"({"enabled": false, "interface": "en0"})");
    CHECK(!off.enabled, "enabled false is off");
}

void testRejectsBadFiles() {
    std::cout << "Anything malformed leaves PTP off, with a reason" << std::endl;
    const char* bad[] = {
        "",                                          // empty
        "not json",                                  // not an object
        R"({"enabled": true, "domain": 128})",       // domain out of range
        R"({"enabled": true, "domain": -1})",
        R"({"enabled": true, "domain": "zero"})",    // wrong type
        R"({"enabled": "yes"})",
        R"({"enabled": true, "interface": 7})",
        R"({"enabled": true, "hybrid": 1})",
        R"({"enabled": true, "domain": 0)",          // unterminated
    };
    for (const char* json : bad) {
        std::string error;
        const Settings s = Settings::parse(json, &error);
        CHECK(!s.enabled && !error.empty(), "off for: " << json << " (error: '" << error << "')");
    }
    std::string error;
    Settings::parse(R"({"enabled": true, "domain": 127, "interface": "192.168.1.20"})", &error);
    CHECK(error.empty(), "domain 127 and an address are fine (" << error << ")");
}

void testLoadsFromTheOverridePath() {
    std::cout << "Loading: the AES67_PTP_CONFIG_PATH file first; none found means off" << std::endl;
    char path[] = "/tmp/aes67-ptp-settings-XXXXXX";
    const int fd = mkstemp(path);
    close(fd);
    {
        std::ofstream file(path);
        file << R"({"enabled": true, "interface": "lo0", "domain": 3})";
    }
    setenv("AES67_PTP_CONFIG_PATH", path, 1);
    std::string foundAt;
    const Settings s = Settings::load(&foundAt);
    CHECK(s.enabled && s.networkInterface == "lo0" && s.domain == 3 && foundAt == path, "read from " << foundAt);
    CHECK(!Settings::searchPaths().empty() && Settings::searchPaths().front() == path, "searched first");
    unlink(path);
    // Set, the override is the only file read, so a real ptp.json never leaks into tests
    setenv("AES67_PTP_CONFIG_PATH", "/nonexistent/ptp.json", 1);
    const Settings missing = Settings::load(&foundAt);
    CHECK(!missing.enabled && foundAt.empty() && Settings::searchPaths().size() == 1,
          "a missing override file means off, and nothing else is searched");
    unsetenv("AES67_PTP_CONFIG_PATH");
    CHECK(Settings::searchPaths().back() == "/Library/Application Support/AES67Driver/ptp.json",
          "otherwise the system file is searched last");
}

} // namespace

int main() {
    testParsesAllFields();
    testDefaults();
    testRejectsBadFiles();
    testLoadsFromTheOverridePath();

    std::cout << "\nPTP settings: " << checksPassed << " passed, " << checksFailed << " failed" << std::endl;
    return checksFailed == 0 ? 0 : 1;
}
