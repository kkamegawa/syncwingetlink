// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Dispatch.h>

#include <core/ExecutableScanner.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "TempDirectory.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

// End-to-end coverage driving the real cli::run() dispatch path (issue #61), rather than
// re-implementing scan/fix's logic in the test. cli::run() constructs its own production
// Console with no injection seam, so this test asserts on exit codes and filesystem
// state only - never on output text. That is a deliberate scope limit, not an oversight:
// output-level assertions belong at the Console level, where tests/ConsoleTests.cpp
// already has the seam (ConsoleOperations) to exercise them.
//
// Two further consequences of cli::run() owning its own process-wide state, found while
// designing this test:
// - The production Console probes/toggles stdout's ENABLE_VIRTUAL_TERMINAL_PROCESSING
//   mode for whatever real console this test process is attached to, restoring it on
//   destruction. Harmless under vstest.console.exe's non-interactive invocation; this
//   test asserts nothing about VT/color state.
// - runFix() registers a process-wide Ctrl+C handler and mutates a process-wide
//   std::atomic<bool> (src/cli/Dispatch.cpp) for the duration of each `fix` call. This
//   test is therefore not safe to run in parallel, within the same test process, against
//   another test that also invokes `fix` - MSTest's native C++ framework does not
//   parallelize test classes within one process, so this is a latent risk, not one
//   observed in practice here.
namespace syncwingetlink::tests
{
namespace
{
// A --rules fixture with no rules at all, so alias resolution always falls through to
// tier 3 (the raw file name) - independent of both the embedded defaults and whatever
// real user rules.json this test happens to run next to (docs/rules.md's user-rules
// tier is keyed off the real %LOCALAPPDATA%, which this test must never touch).
void writeEmptyRulesFile(const std::filesystem::path& path)
{
    std::ofstream stream(path, std::ios::binary);
    stream << R"({"version": 1, "rules": []})";
}

[[nodiscard]] std::vector<std::wstring> buildArgs(std::wstring_view command,
                                                   const std::filesystem::path& packagesDir,
                                                   const std::filesystem::path& linksDir,
                                                   const std::filesystem::path& rulesPath,
                                                   bool failOnMissing = false,
                                                   bool assumeYes = false)
{
    std::vector<std::wstring> args{std::wstring(command),
                                   L"--source",
                                   L"fs",
                                   L"--packages-dir",
                                   packagesDir.native(),
                                   L"--links-dir",
                                   linksDir.native(),
                                   L"--rules",
                                   rulesPath.native()};
    if (failOnMissing)
    {
        args.emplace_back(L"--fail-on-missing");
    }
    if (assumeYes)
    {
        args.emplace_back(L"--yes");
    }
    return args;
}
} // namespace

TEST_CLASS(ScanFixRescanIntegrationTests)
{
public:
    TEST_METHOD(dummyTreeReachesOkThroughScanFixRescan)
    {
        const TempDirectory temp(L"integration-scan-fix-rescan");
        const std::filesystem::path packagesDir = temp.createDirectory(L"Packages");
        const std::filesystem::path linksDir = temp.createDirectory(L"Links");
        const std::filesystem::path rulesPath = temp.path() / L"rules.json";
        writeEmptyRulesFile(rulesPath);

        // A dummy portable package: Packages\SyncTestTool_test\synctesttool.exe. The
        // directory name's "_test" suffix mirrors winget's own
        // "<PackageIdentifier>_<SourceIdentifier>" convention (FsScanSource.h); the
        // executable name matches neither default rule (rules/DefaultRules.cpp), so
        // (combined with the empty --rules fixture above) its alias is unambiguously
        // "synctesttool.exe", the raw file name, regardless of which rule tier applies.
        const std::filesystem::path executablePath =
            temp.createFile(LR"(Packages\SyncTestTool_test\synctesttool.exe)");
        const std::filesystem::path expectedLinkPath = linksDir / L"synctesttool.exe";

        // 1. scan --fail-on-missing: the link does not exist yet.
        const int firstScanExitCode = run(buildArgs(L"scan", packagesDir, linksDir, rulesPath,
                                                     /* failOnMissing */ true));
        Assert::AreEqual(static_cast<int>(ExitCode::FixNeeded), firstScanExitCode);
        Assert::IsFalse(std::filesystem::exists(expectedLinkPath));

        // 2. fix --yes: create the missing link.
        const int fixExitCode =
            run(buildArgs(L"fix", packagesDir, linksDir, rulesPath, /* failOnMissing */ false,
                         /* assumeYes */ true));

        if (fixExitCode == static_cast<int>(ExitCode::InsufficientPermission))
        {
            // CppUnitTestFramework has no Assert::Inconclusive - logging and returning is
            // the closest equivalent, matching the established log-and-skip pattern
            // (docs/adr-phase-3.md ADR-0016; see e.g.
            // SymlinkServiceTests.cpp's productionRepairLinkCreatesARealMissingLink).
            Logger::WriteMessage(
                L"Skipped: fix needs Developer Mode or elevation, neither of which is "
                L"available here.\n");
            return;
        }
        Assert::AreEqual(static_cast<int>(ExitCode::Success), fixExitCode);
        Assert::IsTrue(std::filesystem::exists(expectedLinkPath));
        Assert::IsTrue(isReparsePoint(expectedLinkPath));

        // 3. scan --fail-on-missing again: now Ok.
        const int secondScanExitCode = run(buildArgs(L"scan", packagesDir, linksDir, rulesPath,
                                                      /* failOnMissing */ true));
        Assert::AreEqual(static_cast<int>(ExitCode::Success), secondScanExitCode);
    }
};
} // namespace syncwingetlink::tests
