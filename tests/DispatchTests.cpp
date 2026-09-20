// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Dispatch.h>
#include <cli/Version.h>

#include <string>

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
//
// exitCodeAfterElevationDeclined() and checklistRowKindFor() are covered here for the
// same reason: both are pure `--tui` policy decisions that cli::Dispatch exports
// precisely so they can be asserted without a console, a filesystem, or a package
// source. Everything they feed - runTuiChecklistIfRequested(), runFix() - stays in an
// anonymous namespace and is verified by the manual checks recorded in docs/task.md.
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
            PackageSourceErrorKind::PackageIdentityRequired,
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

TEST_CLASS(StartupPermissionGateTests)
{
public:
    TEST_METHOD(declinedElevationStopsTui)
    {
        const std::optional<ExitCode> result = exitCodeAfterElevationDeclined(true);

        Assert::IsTrue(result.has_value());
        Assert::AreEqual(static_cast<int>(ExitCode::InsufficientPermission),
                         static_cast<int>(*result));
    }

    TEST_METHOD(declinedElevationKeepsLineOrientedFixFlow)
    {
        Assert::IsFalse(exitCodeAfterElevationDeclined(false).has_value());
    }
};

// printVersion() itself is not exported (it is file-local to Dispatch.cpp) and was
// verified by hand to print "syncwingetlink " followed by this constant - see
// docs/task.md's issue #57 entry. What is tested here is that cli::kVersion, the one
// place that output reads from, is a real, non-empty version string - regression
// coverage for the "single source of truth" property #57 introduced (no second
// hardcoded literal anywhere in cli/).
TEST_CLASS(VersionTests)
{
public:
    TEST_METHOD(kVersionIsNonEmpty)
    {
        Assert::IsTrue(std::wstring(kVersion).size() > 0);
    }

    TEST_METHOD(kVersionMatchesTheDocumentedFirstRelease)
    {
        // docs/PLAN.md and src/app.manifest's assemblyIdentity both name this the
        // first release's version. The two are not build-time unified (cli/Version.h's
        // own doc comment explains why), so this pins the value this codebase commits
        // to keeping in sync by hand.
        Assert::AreEqual(std::wstring(L"0.1.0"), std::wstring(kVersion));
    }
};

// docs/adr-phase-10.md ADR-0047. This mapping is the load-bearing half of that
// decision: tui::ChecklistModel never interprets a LinkStatus, so what appears in the
// checklist - and what can be selected in it - is decided entirely here.
TEST_CLASS(ChecklistRowKindForTests)
{
public:
    TEST_METHOD(missingAndBrokenAreSelectable)
    {
        Assert::IsTrue(checklistRowKindFor(LinkStatus::Missing) ==
                       ChecklistRowKind::Selectable);
        Assert::IsTrue(checklistRowKindFor(LinkStatus::Broken) ==
                       ChecklistRowKind::Selectable);
    }

    // The regression this guards: a Mismatch must be *listed* (issue #179 was it being
    // invisible) but must never become selectable, since repairLink() always refuses
    // one and ADR-0014 is unchanged.
    TEST_METHOD(mismatchIsListedButNotSelectable)
    {
        Assert::IsTrue(checklistRowKindFor(LinkStatus::Mismatch) ==
                       ChecklistRowKind::Informational);
    }

    TEST_METHOD(okIsNotListed)
    {
        Assert::IsTrue(checklistRowKindFor(LinkStatus::Ok) == ChecklistRowKind::NotListed);
    }

    // Totality: every LinkStatus maps to exactly one kind, and only Missing/Broken are
    // ever selectable. A new status added without a decision here would show up as a
    // compile error in the switch, but this keeps the counts honest too.
    TEST_METHOD(everyStatusMapsAndOnlyTwoAreSelectable)
    {
        constexpr LinkStatus kAllStatuses[] = {
            LinkStatus::Ok,
            LinkStatus::Missing,
            LinkStatus::Broken,
            LinkStatus::Mismatch,
        };

        int selectable = 0;
        int informational = 0;
        int notListed = 0;
        for (const LinkStatus status : kAllStatuses)
        {
            switch (checklistRowKindFor(status))
            {
            case ChecklistRowKind::Selectable:
                ++selectable;
                break;
            case ChecklistRowKind::Informational:
                ++informational;
                break;
            case ChecklistRowKind::NotListed:
                ++notListed;
                break;
            }
        }

        Assert::AreEqual(2, selectable);
        Assert::AreEqual(1, informational);
        Assert::AreEqual(1, notListed);
    }
};
} // namespace syncwingetlink::tests
