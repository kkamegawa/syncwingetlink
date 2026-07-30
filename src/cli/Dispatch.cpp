// SPDX-License-Identifier: MIT

#include "Dispatch.h"

#include "ArgParser.h"
#include "Console.h"
#include "Json.h"

#include "core/AliasResolver.h"
#include "core/IPackageSource.h"
#include "core/LinkInspector.h"
#include "core/PackageFilter.h"
#include "core/PackageSourceFactory.h"
#include "core/Paths.h"
#include "rules/RuleSetSelector.h"

#include <Windows.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace syncwingetlink::cli
{
namespace
{
// Set by the console control handler below, checked between (never during) items in
// the fix batch loop - Ctrl+C stops the batch at the next opportunity rather than
// mid-item, and never bypasses SymlinkService's own re-inspection/no-rollback rules.
std::atomic<bool> g_ctrlCRequested{false};

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType)
{
    if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT)
    {
        g_ctrlCRequested.store(true);
        return TRUE; // Handled: suppresses the default immediate-termination behavior
                     // so the fix loop can stop cleanly at its next check instead.
    }
    return FALSE;
}

// Every exception type this dispatch layer catches (PackageSourceError, RuleSetError,
// SymlinkServiceError, LinkInspectionError, ArgParseError) builds its what() as UTF-8
// (the same convention rules/RuleSetSelector.cpp and cli/ArgParser.cpp already use for
// their own diagnostic text) - decoding it here as UTF-8, not the process ACP, is what
// docs/adr-phase-5.md ADR-0021 already requires of Console callers generally.
[[nodiscard]] std::wstring utf8ToWide(std::string_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0)
    {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                         required);
    return result;
}

[[nodiscard]] bool equalsOrdinalIgnoreCase(std::wstring_view left,
                                          std::wstring_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    return ::CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                  static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool isCollisionAlias(const std::vector<AliasCollision>& collisions,
                                    std::wstring_view alias) noexcept
{
    for (const AliasCollision& collision : collisions)
    {
        if (equalsOrdinalIgnoreCase(collision.alias, alias))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::wstring_view linkStatusDisplayName(LinkStatus status) noexcept
{
    switch (status)
    {
    case LinkStatus::Ok:
        return L"Ok";
    case LinkStatus::Missing:
        return L"Missing";
    case LinkStatus::Broken:
        return L"Broken";
    case LinkStatus::Mismatch:
        return L"Mismatch";
    }
    return L"Unknown";
}

[[nodiscard]] std::wstring_view outcomeDisplayName(SymlinkRepairOutcome outcome) noexcept
{
    switch (outcome)
    {
    case SymlinkRepairOutcome::WouldCreate:
        return L"would create";
    case SymlinkRepairOutcome::WouldReplaceBroken:
        return L"would replace";
    case SymlinkRepairOutcome::Created:
        return L"created";
    case SymlinkRepairOutcome::ReplacedBroken:
        return L"replaced";
    case SymlinkRepairOutcome::SkippedOk:
        return L"already Ok";
    case SymlinkRepairOutcome::RefusedMismatch:
        return L"refused (mismatch)";
    }
    return L"unknown";
}

// The four guidance strings ADR-0019/the Wiki page document for an InsufficientPermission
// failure, chosen from the (elevation, developerMode) pair it carries. Never suggests
// self-elevation (no ShellExecuteW/runas) - only what the user should do themselves.
[[nodiscard]] std::wstring permissionGuidance(const SymlinkServiceError& error)
{
    const ElevationState elevation = error.elevationState();
    const DeveloperModeState developerMode = error.developerModeState();

    if (elevation == ElevationState::Elevated)
    {
        return L"The elevated token or local policy still lacks symlink privilege.";
    }
    if (developerMode == DeveloperModeState::Disabled)
    {
        return L"Enable Developer Mode, or run elevated.";
    }
    if (developerMode == DeveloperModeState::Unknown)
    {
        return L"Check whether Developer Mode is enabled, or run elevated.";
    }
    return L"Check write/delete access to the Links directory.";
}

struct RepairCandidateSet
{
    std::vector<RepairItem> allItems;
    std::vector<AliasCollision> collisions;
    std::vector<RepairItem> nonCollisionItems;
};

// Enumerates, filters, resolves aliases, and inspects every executable's link state.
// Shared by scan and fix - both need the same inventory, differing only in what they do
// with it afterward.
[[nodiscard]] RepairCandidateSet buildRepairCandidates(const AppOptions& options,
                                                        Console& console)
{
    const auto onDegrade = [&console](const PackageSourceError& error) {
        console.writeLine(std::format(L"warning: --source auto fell back to a filesystem "
                                      L"scan: {}",
                                      utf8ToWide(error.what())),
                          ConsoleStream::Error);
    };

    const std::unique_ptr<IPackageSource> source = createPackageSource(options, onDegrade);
    std::vector<InstalledPackage> packages = source->enumeratePackages();

    const PackageFilter filter(options.includePatterns, options.excludePatterns);
    packages = filter.apply(std::move(packages));

    const RuleSet rules = selectRuleSet(options.rulesPath, defaultUserRulesPathProvider);
    const std::filesystem::path linksDirectory = paths::getLinksDirectory(options.linksDirectory);

    RepairCandidateSet candidates;
    for (const InstalledPackage& package : packages)
    {
        for (const PackageExe& executable : package.executables)
        {
            const std::optional<AliasResolution> resolution =
                resolveAlias(executable.path, rules);
            if (!resolution.has_value())
            {
                console.writeLine(std::format(L"warning: could not derive a valid alias "
                                              L"for {}",
                                              sanitizeForDisplay(executable.path.native())),
                                  ConsoleStream::Error);
                continue;
            }

            const std::filesystem::path linkPath = linksDirectory / resolution->alias;
            candidates.allItems.push_back(
                inspectLink(executable, resolution->alias, linkPath));
        }
    }

    candidates.collisions = detectAliasCollisions(candidates.allItems);
    candidates.nonCollisionItems.reserve(candidates.allItems.size());
    for (const RepairItem& item : candidates.allItems)
    {
        if (!isCollisionAlias(candidates.collisions, item.alias))
        {
            candidates.nonCollisionItems.push_back(item);
        }
    }

    return candidates;
}

void printScanItem(Console& console, const RepairItem& item)
{
    console.writeLine(std::format(L"{}: {} -> {}", linkStatusDisplayName(item.status),
                                  sanitizeForDisplay(item.alias),
                                  sanitizeForDisplay(item.executable.path.native())));
}

void printCollisions(Console& console, const std::vector<AliasCollision>& collisions)
{
    for (const AliasCollision& collision : collisions)
    {
        console.writeLine(std::format(L"warning: alias collision for {} - {} executables "
                                      L"resolve to it; excluded from automatic repair",
                                      sanitizeForDisplay(collision.alias),
                                      collision.executables.size()),
                          ConsoleStream::Error);
    }
}

// Writes an already-produced JSON document (UTF-8 bytes) as the sole content of
// stdout, per the security contract's "--json stream purity" rule. Decoded via
// utf8ToWide() (correct UTF-8 decoding), not a byte-for-byte widening, since the
// document can contain multi-byte UTF-8 sequences for any non-ASCII character that
// escapeJsonString() left unescaped (docs/PLAN.md's documented behavior for ordinary
// Unicode text).
void writeJsonDocument(Console& console, const std::string& json)
{
    console.writeLine(utf8ToWide(json));
}

[[nodiscard]] ExitCode runScan(const AppOptions& options, Console& console)
{
    const RepairCandidateSet candidates = buildRepairCandidates(options, console);

    bool anyNotOk = false;
    for (const RepairItem& item : candidates.allItems)
    {
        if (item.status != LinkStatus::Ok)
        {
            anyNotOk = true;
        }
    }

    if (options.jsonOutput)
    {
        writeJsonDocument(console, toJsonScanResult(candidates.allItems, candidates.collisions));
    }
    else
    {
        for (const RepairItem& item : candidates.allItems)
        {
            printScanItem(console, item);
        }
        printCollisions(console, candidates.collisions);
    }

    if (options.failOnMissing && anyNotOk)
    {
        return ExitCode::FixNeeded;
    }
    return ExitCode::Success;
}

[[nodiscard]] ExitCode runFix(const AppOptions& options, Console& console)
{
    const RepairCandidateSet candidates = buildRepairCandidates(options, console);
    printCollisions(console, candidates.collisions);

    ::SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE);
    g_ctrlCRequested.store(false);

    std::vector<SymlinkRepairResult> results;
    bool anyInsufficientPermission = false;
    bool anyOtherFailure = false;
    bool interrupted = false;

    for (const RepairItem& candidate : candidates.nonCollisionItems)
    {
        if (g_ctrlCRequested.load())
        {
            interrupted = true;
            break;
        }

        RepairMode mode = RepairMode::Execute;
        if (options.dryRun)
        {
            mode = RepairMode::DryRun;
        }
        else if (!options.assumeYes &&
                (candidate.status == LinkStatus::Missing ||
                 candidate.status == LinkStatus::Broken))
        {
            const bool confirmed = console.confirm(
                std::format(L"Repair {} (currently {})? [y/N] ",
                           sanitizeForDisplay(candidate.alias),
                           linkStatusDisplayName(candidate.status)),
                /* assumeYes */ false);
            mode = confirmed ? RepairMode::Execute : RepairMode::DryRun;
        }

        try
        {
            SymlinkRepairResult result = repairLink(candidate, mode);
            if (!options.jsonOutput)
            {
                console.writeLine(std::format(L"{}: {}", sanitizeForDisplay(candidate.alias),
                                              outcomeDisplayName(result.outcome)));
            }
            results.push_back(std::move(result));
        }
        catch (const SymlinkServiceError& error)
        {
            if (error.kind() == SymlinkServiceErrorKind::InsufficientPermission)
            {
                anyInsufficientPermission = true;
                console.writeLine(std::format(L"error: {} - {}",
                                              sanitizeForDisplay(candidate.alias),
                                              permissionGuidance(error)),
                                  ConsoleStream::Error);
            }
            else
            {
                anyOtherFailure = true;
                console.writeLine(std::format(L"error: {} - repair failed",
                                              sanitizeForDisplay(candidate.alias)),
                                  ConsoleStream::Error);
            }
        }
    }

    ::SetConsoleCtrlHandler(&consoleCtrlHandler, FALSE);

    if (options.jsonOutput)
    {
        writeJsonDocument(console, toJsonFixResult(results, candidates.collisions));
    }

    if (interrupted)
    {
        return ExitCode::PartialFailure;
    }
    if (anyInsufficientPermission)
    {
        return ExitCode::InsufficientPermission;
    }
    if (anyOtherFailure)
    {
        return ExitCode::PartialFailure;
    }
    return ExitCode::Success;
}

[[nodiscard]] ExitCode runTestRule(const AppOptions& options, Console& console)
{
    const RuleSet rules = selectRuleSet(options.rulesPath, defaultUserRulesPathProvider);

    const std::wstring& name = *options.testRuleName;
    const std::optional<AliasRuleMatch> match = rules.resolve(name);

    if (match.has_value())
    {
        console.writeLine(std::format(L"{} -> rule \"{}\" -> {}", sanitizeForDisplay(name),
                                      sanitizeForDisplay(match->ruleName),
                                      sanitizeForDisplay(match->alias)));
    }
    else if (isValidAliasFileName(name))
    {
        console.writeLine(std::format(L"{} -> no rule matched -> {} (raw file name)",
                                      sanitizeForDisplay(name), sanitizeForDisplay(name)));
    }
    else
    {
        console.writeLine(std::format(L"{} -> no rule matched, and the raw file name is "
                                      L"not a valid alias",
                                      sanitizeForDisplay(name)));
    }

    return ExitCode::Success;
}

// Console::writeLine() sanitizes its argument via sanitizeForDisplay(), which strips
// every C0 control character - including "\n" - since a *untrusted* string (a package
// id, alias, or path) must never be able to forge an extra output line
// (docs/adr-phase-5.md ADR-0021). This static, trusted help text is legitimately
// multi-line, so it is written one writeLine() call per line instead of one call with
// embedded "\n" characters, which sanitizeForDisplay() would otherwise silently
// collapse into a single unreadable line.
void printHelp(Console& console)
{
    static constexpr std::wstring_view kHelpLines[] = {
        L"syncwingetlink [command] [options]",
        L"",
        L"Commands:",
        L"  scan            detect only (read-only, default command)",
        L"  fix             create/repair missing/broken links",
        L"  test-rule NAME  show the replacement result for a real file name",
        L"",
        L"Options:",
        L"  --source com|fs|auto   package enumeration source (default auto)",
        L"  --tui                  run in interactive TUI mode",
        L"  --dry-run              show the plan without executing (for fix)",
        L"  --yes, -y              skip all confirmations and execute",
        L"  --rules <path>         path to a replacement-rules JSON",
        L"  --packages-dir <path>  override the Packages directory",
        L"  --links-dir <path>     override the Links directory",
        L"  --include <glob>       narrow target packages/exes",
        L"  --exclude <glob>       exclude",
        L"  --json                 emit results as JSON (scripting)",
        L"  --verbose / --quiet    log level",
        L"  --fail-on-missing      scan exits 1 if a problem is found",
        L"  --no-color             disable colored/VT output",
        L"  --version / --help",
        L"",
        L"Exit codes: 0 success, 1 fix needed, 2 insufficient permission, "
        L"3 argument/config error, 4 package enumeration failed, 10 some repairs failed.",
    };
    for (const std::wstring_view line : kHelpLines)
    {
        console.writeLine(line);
    }
}

void printVersion(Console& console)
{
    // A single source of truth: docs/PLAN.md and src/app.manifest both currently name
    // this the first release, tracked as version 0.1.0 (app.manifest's
    // assemblyIdentity version). #57 (help/version) is expected to derive this from
    // one place rather than a second hardcoded literal, once that issue's scope is
    // implemented; this is a functional placeholder covering #56's requirement to
    // handle AppCommand::Version at all.
    console.writeLine(L"syncwingetlink 0.1.0");
}
} // namespace

ExitCode exitCodeFor(PackageSourceErrorKind kind) noexcept
{
    switch (kind)
    {
    case PackageSourceErrorKind::AppInstallerMissing:
    case PackageSourceErrorKind::PolicyBlocked:
    case PackageSourceErrorKind::AccessDenied:
    case PackageSourceErrorKind::ServerUnavailable:
    case PackageSourceErrorKind::CatalogError:
    case PackageSourceErrorKind::ScanFailed:
    case PackageSourceErrorKind::Unknown:
        return ExitCode::PackageEnumerationFailed;
    }
    return ExitCode::PackageEnumerationFailed;
}

ExitCode exitCodeFor(RuleSetErrorKind kind) noexcept
{
    switch (kind)
    {
    case RuleSetErrorKind::ParseError:
    case RuleSetErrorKind::UnsupportedVersion:
    case RuleSetErrorKind::MissingField:
    case RuleSetErrorKind::InvalidFieldType:
    case RuleSetErrorKind::InvalidRuleName:
    case RuleSetErrorKind::InvalidFlag:
    case RuleSetErrorKind::InvalidRegex:
    case RuleSetErrorKind::FileReadError:
    case RuleSetErrorKind::LimitExceeded:
    case RuleSetErrorKind::RegexEvaluationFailed:
        return ExitCode::ArgumentError;
    }
    return ExitCode::ArgumentError;
}

ExitCode exitCodeFor(SymlinkServiceErrorKind kind) noexcept
{
    switch (kind)
    {
    case SymlinkServiceErrorKind::InsufficientPermission:
        return ExitCode::InsufficientPermission;
    case SymlinkServiceErrorKind::DeleteFailed:
    case SymlinkServiceErrorKind::CreateFailed:
    case SymlinkServiceErrorKind::VerificationFailed:
        return ExitCode::PartialFailure;
    }
    return ExitCode::PartialFailure;
}

int run(const std::vector<std::wstring>& args)
{
    AppOptions options;
    try
    {
        options = parseArguments(args);
    }
    catch (const ArgParseError& error)
    {
        Console console(false);
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        console.writeLine(L"Run with --help for usage.", ConsoleStream::Error);
        return static_cast<int>(ExitCode::ArgumentError);
    }

    Console console(options.noColor);

    if (options.command == AppCommand::Help)
    {
        printHelp(console);
        return static_cast<int>(ExitCode::Success);
    }
    if (options.command == AppCommand::Version)
    {
        printVersion(console);
        return static_cast<int>(ExitCode::Success);
    }

    try
    {
        if (options.command == AppCommand::TestRule)
        {
            return static_cast<int>(runTestRule(options, console));
        }
        if (options.command == AppCommand::Fix)
        {
            return static_cast<int>(runFix(options, console));
        }
        return static_cast<int>(runScan(options, console));
    }
    catch (const PackageSourceError& error)
    {
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(exitCodeFor(error.kind()));
    }
    catch (const RuleSetError& error)
    {
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(exitCodeFor(error.kind()));
    }
    catch (const SymlinkServiceError& error)
    {
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(exitCodeFor(error.kind()));
    }
    catch (const LinkInspectionError& error)
    {
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(ExitCode::PackageEnumerationFailed);
    }
    catch (const std::exception& error)
    {
        // A last-resort bucket for anything not explicitly named above (e.g. a
        // std::filesystem::filesystem_error querying the Packages directory). Exit
        // code 3 is the closest documented fit ("argument/config error") for a
        // condition this dispatch layer did not anticipate closely enough to name -
        // see docs/adr-phase-5.md ADR-0024.
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(ExitCode::ArgumentError);
    }
}
} // namespace syncwingetlink::cli
