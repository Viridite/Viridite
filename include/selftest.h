#pragma once
#include <string>
#include <vector>
#include "apk.h"

// ── Self-test ────────────────────────────────────────────────────────────────
// Runs every check that would otherwise mean launching a game, pulling the SD
// card and reading logs on a PC. Read-only apart from one scratch file it
// removes again, so it is always safe to run.
//
// Results are also written to sdmc:/switch/Viridite/selftest.txt.

enum class TestStatus { Pass, Warn, Fail };

struct TestResult {
    TestStatus  status;
    std::string name;
    std::string detail;
};

// `progress` (optional) is called with a short stage label before each group,
// so the caller can keep drawing while this runs — some checks touch the SD
// card and the whole pass takes a second or two.
std::vector<TestResult> selfTestRun(const std::vector<ApkInfo>& apks,
                                    void (*progress)(const char*) = nullptr);
