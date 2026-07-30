// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Dispatch.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

// cli::run() itself is not exercised here: it enumerates real packages, resolves real
// aliases, inspects real filesystem entries, and reads a real console, none of which
// this test project mocks out for cli::Dispatch (unlike core/SymlinkService's
// operations seam). It was instead verified by hand against a real build of
// syncwingetlink.exe, driven with --source fs and --packages-dir/--links-dir pointed
// at a scratch directory tree, covering: --help/--version, an unknown option, scan
// (console and --json, including --fail-on-missing), fix --dry-run (proven
// side-effect-free), fix --yes hitting InsufficientPermission on this host (no
// Developer Mode/elevation) with the correct guidance text and exit code 2, the
// confirmation prompt's decline/EOF/accept paths over a redirected stdin, and alias
// collision exclusion from fix. See docs/task.md's issue #56 entry for the full
// transcript. What *is* exhaustively tested here is exitCodeFor()'s totality over every
// error kind it maps - the one piece of this module's logic that is both pure and
// completely enumerable.
namespace syncwingetlink::tests
{
TEST_CLASS(ExitCodeForPackageSourceErrorKindTests)
{
public:
    TEST_METHOD(everyKindMapsToPackageEnumerationFailed)
    {
        constexpr PackageSourceErrorKind kAllKinds[] = {
            PackageSourceErrorKind::AppInstallerMissing,
            PackageSourceErrorKind::PolicyBlocked,
            PackageSourceErrorKind::AccessDenied,
            PackageSourceErrorKind::ServerUnavailable,
            PackageSourceErrorKind::CatalogError,
            PackageSourceErrorKind::ScanFailed,
            PackageSourceErrorKind::Unknown,
        };

        for (const PackageSourceErrorKind kind : kAllKinds)
        {
            Assert::IsTrue(ExitCode::PackageEnumerationFailed == exitCodeFor(kind));
        }
    }
};

TEST_CLASS(ExitCodeForRuleSetErrorKindTests)
{
public:
    TEST_METHOD(everyKindMapsToArgumentError)
    {
        constexpr RuleSetErrorKind kAllKinds[] = {
            RuleSetErrorKind::ParseError,
            RuleSetErrorKind::UnsupportedVersion,
            RuleSetErrorKind::MissingField,
            RuleSetErrorKind::InvalidFieldType,
            RuleSetErrorKind::InvalidRuleName,
            RuleSetErrorKind::InvalidFlag,
            RuleSetErrorKind::InvalidRegex,
            RuleSetErrorKind::FileReadError,
            RuleSetErrorKind::LimitExceeded,
            RuleSetErrorKind::RegexEvaluationFailed,
        };

        for (const RuleSetErrorKind kind : kAllKinds)
        {
            Assert::IsTrue(ExitCode::ArgumentError == exitCodeFor(kind));
        }
    }
};

TEST_CLASS(ExitCodeForSymlinkServiceErrorKindTests)
{
public:
    TEST_METHOD(insufficientPermissionMapsToInsufficientPermission)
    {
        Assert::IsTrue(ExitCode::InsufficientPermission ==
                       exitCodeFor(SymlinkServiceErrorKind::InsufficientPermission));
    }

    TEST_METHOD(everyOtherKindMapsToPartialFailure)
    {
        constexpr SymlinkServiceErrorKind kOtherKinds[] = {
            SymlinkServiceErrorKind::DeleteFailed,
            SymlinkServiceErrorKind::CreateFailed,
            SymlinkServiceErrorKind::VerificationFailed,
        };

        for (const SymlinkServiceErrorKind kind : kOtherKinds)
        {
            Assert::IsTrue(ExitCode::PartialFailure == exitCodeFor(kind));
        }
    }
};
} // namespace syncwingetlink::tests
