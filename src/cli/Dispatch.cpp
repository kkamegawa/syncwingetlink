// SPDX-License-Identifier: MIT

#include "Dispatch.h"

#include "ArgParser.h"
#include "Console.h"
#include "Json.h"
#include "ScanReport.h"
#include "Version.h"

#include "core/AliasResolver.h"
#include "core/IPackageSource.h"
#include "core/LinkInspector.h"
#include "core/PackageFilter.h"
#include "core/PackageSourceFactory.h"
#include "core/Paths.h"
#include "core/RepairBatch.h"
#include "rules/RuleSetSelector.h"
#include "tui/ChecklistModel.h"
#include "tui/TerminalSession.h"
#include "tui/TuiApp.h"

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
extern "C" __declspec(dllimport) HINSTANCE __stdcall ShellExecuteW(
    HWND hwnd, LPCWSTR lpOperation, LPCWSTR lpFile, LPCWSTR lpParameters, LPCWSTR lpDirectory,
    INT nShowCmd);

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
        return L"<unrepresentable>";
    }

    std::wstring result(static_cast<std::size_t>(required), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                             required);
    if (written <= 0)
    {
        return L"<unrepresentable>";
    }

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

// Display text for the shared repair-batch executor's per-candidate result
// (core/RepairBatch.h RepairDisposition), used by both the non-interactive and TUI
// `fix` paths' `[current/total] alias: result` progress lines (docs/adr-phase-6.md
// ADR-0028). Failed/NotAttempted are never actually reached here: a failed item is
// reported via its own "error: ..." line instead (see the onProgress callback in
// runFix()), and NotAttempted items are never added to RepairBatchResult::items at
// all - both cases are included only so this switch stays total.
[[nodiscard]] std::wstring_view repairDispositionDisplayName(RepairDisposition disposition) noexcept
{
    switch (disposition)
    {
    case RepairDisposition::Created:
        return L"created";
    case RepairDisposition::ReplacedBroken:
        return L"replaced";
    case RepairDisposition::PlannedCreate:
        return L"would create";
    case RepairDisposition::PlannedReplaceBroken:
        return L"would replace";
    case RepairDisposition::Declined:
        return L"declined";
    case RepairDisposition::SkippedOk:
        return L"already Ok";
    case RepairDisposition::RefusedMismatch:
        return L"refused (mismatch)";
    case RepairDisposition::Failed:
        return L"failed";
    case RepairDisposition::NotAttempted:
        return L"not attempted";
    }
    return L"unknown";
}

enum class UiLanguage
{
    English,
    Japanese,
};

enum class StartupPermissionMessage
{
    DeveloperModeDisabled,
    DeveloperModeUnknown,
    ElevationPrompt,
    ElevationLaunchFailed,
};

[[nodiscard]] UiLanguage detectUiLanguage() noexcept
{
    const LANGID language = ::GetUserDefaultUILanguage();
    return PRIMARYLANGID(language) == LANG_JAPANESE ? UiLanguage::Japanese
                                                    : UiLanguage::English;
}

[[nodiscard]] std::wstring localizedMessage(UiLanguage language,
                                            StartupPermissionMessage message)
{
    if (language == UiLanguage::Japanese)
    {
        switch (message)
        {
        case StartupPermissionMessage::DeveloperModeDisabled:
            return L"開発者モードが無効です。シンボリックリンクの作成には、開発者モードを有効にするか、管理者として実行する必要があります。";
        case StartupPermissionMessage::DeveloperModeUnknown:
            return L"開発者モードの状態を確認できませんでした。シンボリックリンクの作成には、開発者モードを確認するか、管理者として実行してください。";
        case StartupPermissionMessage::ElevationPrompt:
            return L"管理者権限で再起動しますか? [y/N] ";
        case StartupPermissionMessage::ElevationLaunchFailed:
            return L"管理者権限での再起動に失敗しました。管理者として再実行してください。";
        }
    }

    switch (message)
    {
    case StartupPermissionMessage::DeveloperModeDisabled:
        return L"Developer Mode is disabled. Creating symlinks requires Developer Mode or running elevated.";
    case StartupPermissionMessage::DeveloperModeUnknown:
        return L"Could not determine Developer Mode state. Check Developer Mode or run elevated to create symlinks.";
    case StartupPermissionMessage::ElevationPrompt:
        return L"Restart with administrator privileges? [y/N] ";
    case StartupPermissionMessage::ElevationLaunchFailed:
        return L"Could not restart with administrator privileges. Re-run this command from an elevated shell.";
    }

    return L"";
}

[[nodiscard]] bool relaunchElevated(const std::vector<std::wstring>& args) noexcept
{
    std::wstring parameters;
    for (std::size_t index = 0; index < args.size(); ++index)
    {
        if (index != 0)
        {
            parameters += L' ';
        }

        parameters += L'"';
        for (const wchar_t ch : args[index])
        {
            if (ch == L'"')
            {
                parameters += L'\\';
            }
            parameters += ch;
        }
        parameters += L'"';
    }

    const HINSTANCE result =
        ShellExecuteW(nullptr, L"runas", nullptr, parameters.empty() ? nullptr : parameters.c_str(),
                      nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

[[nodiscard]] std::optional<int> handleFixStartupPermissionGate(const AppOptions& options,
                                                                Console& console,
                                                                const std::vector<std::wstring>& args)
{
    if (options.command != AppCommand::Fix || options.dryRun)
    {
        return std::nullopt;
    }

    const DeveloperModeState developerMode = queryDeveloperMode();
    if (developerMode == DeveloperModeState::Enabled)
    {
        return std::nullopt;
    }

    const UiLanguage language = detectUiLanguage();
    const StartupPermissionMessage warningMessage =
        developerMode == DeveloperModeState::Disabled
            ? StartupPermissionMessage::DeveloperModeDisabled
            : StartupPermissionMessage::DeveloperModeUnknown;
    console.writeLine(localizedMessage(language, warningMessage), ConsoleStream::Error);

    if (queryElevation() == ElevationState::Elevated)
    {
        return std::nullopt;
    }

    if (options.silent)
    {
        if (const std::optional<ExitCode> tuiExitCode =
                exitCodeAfterElevationDeclined(options.useTui);
            tuiExitCode.has_value())
        {
            return static_cast<int>(*tuiExitCode);
        }
        return std::nullopt;
    }

    if (!console.confirm(localizedMessage(language, StartupPermissionMessage::ElevationPrompt),
                         options.assumeYes))
    {
        if (const std::optional<ExitCode> tuiExitCode =
                exitCodeAfterElevationDeclined(options.useTui);
            tuiExitCode.has_value())
        {
            return static_cast<int>(*tuiExitCode);
        }
        return std::nullopt;
    }

    if (relaunchElevated(args))
    {
        return static_cast<int>(ExitCode::Success);
    }

    console.writeLine(localizedMessage(language, StartupPermissionMessage::ElevationLaunchFailed),
                      ConsoleStream::Error);
    return static_cast<int>(ExitCode::InsufficientPermission);
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

// #113 (ADR-0030): --verbose's additional reporting - the resolved effective
// Links/Packages paths, the package source actually used (built from options.source
// plus whether onDegrade fired below, never a downcast on IPackageSource), and which
// rule tier was selected. Always stderr, never stdout, regardless of --json (ADR-0022's
// stdout-purity rule). The logLevel check here is only an optimization - Diagnostic
// lines are dropped by Console::writeLine()/shouldEmit() at any other level regardless -
// so a future logLevel check removed here would still behave correctly, just do
// slightly more work for a line nobody sees.
void reportVerboseDiagnostics(const AppOptions& options, Console& console,
                              const std::filesystem::path& linksDirectory,
                              bool sourceDegradedToFileSystem, const std::wstring& degradeReason)
{
    if (options.logLevel != LogLevel::Verbose)
    {
        return;
    }

    console.writeLine(std::format(L"verbose: effective Links directory: {}",
                                  sanitizeForDisplay(linksDirectory.native())),
                      ConsoleStream::Error, MessageImportance::Diagnostic);

    // Unlike linksDirectory above (already resolved unconditionally by the caller for
    // every command, verbose or not), resolving the Packages directory here is new work
    // this function alone introduces: --source com never otherwise calls
    // paths::getPackagesDirectory(), so a bare, unguarded call would add a failure mode
    // (SHGetKnownFolderPath, inside getLocalAppDataDirectory()) that only exists when
    // --verbose happens to be on. Diagnostics must be best-effort and must never turn an
    // otherwise-successful scan/fix into a failure, so this is caught and reported
    // rather than left to propagate.
    try
    {
        const std::filesystem::path packagesDirectory =
            paths::getPackagesDirectory(options.packagesDirectory);
        console.writeLine(std::format(L"verbose: effective Packages directory: {}",
                                      sanitizeForDisplay(packagesDirectory.native())),
                          ConsoleStream::Error, MessageImportance::Diagnostic);
    }
    catch (const std::exception&)
    {
        console.writeLine(L"verbose: effective Packages directory: could not be determined",
                          ConsoleStream::Error, MessageImportance::Diagnostic);
    }

    std::wstring sourceReport;
    switch (options.source)
    {
    case PackageSource::Com:
        sourceReport = L"requested: com, used: com";
        break;
    case PackageSource::FileSystem:
        sourceReport = L"requested: fs, used: fs";
        break;
    case PackageSource::Auto:
        sourceReport = sourceDegradedToFileSystem
                           ? std::format(L"requested: auto, used: filesystem (degraded: {})",
                                        sanitizeForDisplay(degradeReason))
                           : L"requested: auto, used: com";
        break;
    }
    console.writeLine(std::format(L"verbose: package source - {}", sourceReport),
                      ConsoleStream::Error, MessageImportance::Diagnostic);

    // Mirrors RuleSetSelector.cpp's own explicit/user-file/defaults priority (docs/rules.md)
    // without modifying selectRuleSet()'s interface to expose which tier it picked - the
    // same "build the report from information already available, not a new API surface"
    // approach design decision 2 uses for the package source above.
    //
    // paths::getUserRulesFilePath() (via getLocalAppDataDirectory()/SHGetKnownFolderPath)
    // can throw; the caller (buildRepairCandidates()) currently only reaches this "no
    // --rules" branch after selectRuleSet() has already called the same function without
    // throwing, but this function must not rely on that ordering to stay safe - a
    // best-effort diagnostic must never turn an otherwise-successful scan/fix into a
    // failure, regardless of what else does or doesn't call the same path first.
    std::wstring ruleSourceReport;
    if (options.rulesPath.has_value())
    {
        ruleSourceReport = L"explicit (--rules)";
    }
    else
    {
        try
        {
            std::error_code existsError;
            const bool userFileExists =
                std::filesystem::exists(paths::getUserRulesFilePath(), existsError);
            ruleSourceReport =
                (!existsError && userFileExists) ? L"user rules file" : L"embedded defaults";
        }
        catch (const std::exception&)
        {
            ruleSourceReport = L"could not be determined";
        }
    }
    console.writeLine(std::format(L"verbose: rule source - {}", ruleSourceReport),
                      ConsoleStream::Error, MessageImportance::Diagnostic);
}

// Enumerates, filters, resolves aliases, and inspects every executable's link state.
// Shared by scan and fix - both need the same inventory, differing only in what they do
// with it afterward.
[[nodiscard]] RepairCandidateSet buildRepairCandidates(const AppOptions& options,
                                                        Console& console)
{
    bool sourceDegradedToFileSystem = false;
    std::wstring degradeReason;
    const auto onDegrade = [&console, &sourceDegradedToFileSystem,
                            &degradeReason](const PackageSourceError& error) {
        sourceDegradedToFileSystem = true;
        degradeReason = utf8ToWide(error.what());
        console.writeLine(std::format(L"warning: --source auto fell back to a filesystem "
                                      L"scan: {}",
                                      degradeReason),
                          ConsoleStream::Error);
    };

    const std::unique_ptr<IPackageSource> source = createPackageSource(options, onDegrade);
    std::vector<InstalledPackage> packages = source->enumeratePackages();

    const PackageFilter filter(options.includePatterns, options.excludePatterns);
    packages = filter.apply(std::move(packages));

    const RuleSet rules = selectRuleSet(options.rulesPath, defaultUserRulesPathProvider);
    const std::filesystem::path linksDirectory = paths::getLinksDirectory(options.linksDirectory);

    reportVerboseDiagnostics(options, console, linksDirectory, sourceDegradedToFileSystem,
                             degradeReason);

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
            RepairItem item = inspectLink(executable, resolution->alias, linkPath);
            item.packageId = package.id;
            candidates.allItems.push_back(std::move(item));
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
        for (const ReportLine& line : formatGroupedReport(candidates.allItems, ReportMode::Scan))
        {
            console.writeLine(line.text, ConsoleStream::Output, line.importance);
        }
        printCollisions(console, candidates.collisions);
    }

    if (options.failOnMissing && anyNotOk)
    {
        return ExitCode::FixNeeded;
    }
    return ExitCode::Success;
}

// The result of attempting the M7 interactive checklist for `fix --tui`. Distinguishes
// three outcomes runFix() must all handle differently: the checklist ran and the user
// cancelled it (Escape/Q/Ctrl+C - success, no repairs, no filesystem mutation); the
// checklist ran and the user confirmed a (possibly empty) selection; or the checklist
// did not run at all, either because there was nothing selectable to show or because
// the terminal capability required for it was unavailable - both fall back to the
// existing line-oriented CLI confirmation flow with zero TUI escape sequences emitted.
enum class TuiRunOutcome
{
    NotRun,
    Cancelled,
    Confirmed,
};

struct TuiRunResult
{
    TuiRunOutcome outcome{TuiRunOutcome::NotRun};
    // Only meaningful when outcome is Confirmed. The *aliases* the user selected -
    // not the RepairItems themselves - because runFix()'s own loop below always
    // re-derives its candidate list from candidates.nonCollisionItems (the same fresh
    // inventory both the TUI and non-TUI paths share), rather than repairing whatever
    // (possibly stale) items the checklist happened to capture at render time.
    std::vector<std::wstring> selectedAliases;
};

// Runs the M7 repair checklist when --tui was requested, or reports why it did not
// run. Every branch that does not end in TuiRunOutcome::Confirmed/Cancelled leaves the
// terminal untouched - no TUI escape sequence is ever emitted unless a TerminalSession
// was actually acquired.
[[nodiscard]] TuiRunResult runTuiChecklistIfRequested(const AppOptions& options,
                                                       Console& console,
                                                       const RepairCandidateSet& candidates)
{
    if (!options.useTui)
    {
        return {};
    }

    std::vector<tui::ChecklistCandidate> selectable;
    for (const RepairItem& item : candidates.nonCollisionItems)
    {
        if (item.status == LinkStatus::Missing || item.status == LinkStatus::Broken)
        {
            selectable.push_back(tui::ChecklistCandidate{item});
        }
    }

    if (selectable.empty())
    {
        // Nothing to select - every remaining candidate is Ok/Mismatch/excluded, none
        // of which ever needed confirmation. Showing an empty checklist would be
        // meaningless, so runFix()'s ordinary path below handles these exactly as it
        // would without --tui at all.
        return {};
    }

    if (!console.stdinInteractive() || !console.stdoutInteractive() || !console.vtEnabled())
    {
        console.writeLine(L"warning: --tui requires an interactive terminal with "
                          L"virtual-terminal support; falling back to the "
                          L"line-oriented confirmation flow",
                          ConsoleStream::Error);
        return {};
    }

    std::optional<tui::TerminalSession> session = tui::TerminalSession::tryCreate(
        console.stdinInteractive(), console.stdoutInteractive(), console.vtEnabled());
    if (!session.has_value())
    {
        console.writeLine(L"warning: the interactive TUI could not be started; falling "
                          L"back to the line-oriented confirmation flow",
                          ConsoleStream::Error);
        return {};
    }

    tui::ChecklistModel model(std::move(selectable));
    const tui::ChecklistRunResult checklistResult = tui::runChecklist(*session, model);
    // Fold the terminal back before this function's caller writes anything else -
    // progress lines and the final summary must land on the restored, normal screen,
    // not the checklist's alternate one.
    session->restore();

    if (checklistResult.outcome == tui::ChecklistOutcome::Cancelled)
    {
        TuiRunResult result;
        result.outcome = TuiRunOutcome::Cancelled;
        return result;
    }

    TuiRunResult result;
    result.outcome = TuiRunOutcome::Confirmed;
    result.selectedAliases.reserve(checklistResult.selectedCandidates.size());
    for (const tui::ChecklistCandidate& candidate : checklistResult.selectedCandidates)
    {
        result.selectedAliases.push_back(candidate.item.alias);
    }
    return result;
}

// The final result summary the M7 Wiki plan documents: selected/processed/remaining,
// every RepairDisposition category's count, and whether the batch was interrupted.
// Never emitted when --json is set - the JSON document is the sole content of stdout
// in that mode (docs/adr-phase-5.md ADR-0022's stdout-purity rule), the same gate the
// per-item progress lines already use.
void printBatchSummary(Console& console, const RepairBatchSummary& summary)
{
    // A summary heading is skippable noise under --quiet - the per-item error/created
    // lines and the exit code already carry the load-bearing information.
    console.writeLine(std::format(L"Summary: {} selected, {} processed, {} remaining",
                                  summary.selected, summary.processed, summary.remaining),
                      ConsoleStream::Output, MessageImportance::Supplementary);
    console.writeLine(std::format(
                          L"  created: {}  replaced: {}  planned: {}  declined: {}  skipped: {}  "
                          L"refused (mismatch): {}  failed: {}",
                          summary.created, summary.replaced, summary.planned, summary.declined,
                          summary.skippedOk, summary.refusedMismatch, summary.failed),
                      ConsoleStream::Output, MessageImportance::Supplementary);
    console.writeLine(std::format(L"  interrupted: {}", summary.interrupted ? L"yes" : L"no"),
                      ConsoleStream::Output, MessageImportance::Supplementary);
}

// The one place a core::RepairBatchExitCode becomes a cli::ExitCode - total, so a
// future RepairBatchExitCode value fails to compile here rather than silently mapping
// to the wrong exit code.
[[nodiscard]] ExitCode toExitCode(RepairBatchExitCode code) noexcept
{
    switch (code)
    {
    case RepairBatchExitCode::Success:
        return ExitCode::Success;
    case RepairBatchExitCode::InsufficientPermission:
        return ExitCode::InsufficientPermission;
    case RepairBatchExitCode::PartialFailure:
        return ExitCode::PartialFailure;
    }
    return ExitCode::PartialFailure;
}

[[nodiscard]] ExitCode runFix(const AppOptions& options, Console& console)
{
    const RepairCandidateSet candidates = buildRepairCandidates(options, console);
    printCollisions(console, candidates.collisions);

    // The batch's own [current/total] progress lines (ADR-0028) stay exactly as they
    // are; this is only an up-front picture of what fix is about to consider. Skipped
    // for --tui, whose checklist supersedes it (and which must not print into the
    // alternate screen), and for --json, per ADR-0022's stdout-purity rule.
    if (!options.jsonOutput && !options.useTui)
    {
        for (const ReportLine& line :
             formatGroupedReport(candidates.allItems, ReportMode::FixPreview))
        {
            console.writeLine(line.text, ConsoleStream::Output, line.importance);
        }
    }

    const TuiRunResult tuiResult = runTuiChecklistIfRequested(options, console, candidates);
    if (tuiResult.outcome == TuiRunOutcome::Cancelled)
    {
        // Escape, Q, or Ctrl+C: success, with no repairs and no filesystem mutation -
        // per the documented checklist cancellation contract. Nothing has been
        // mutated, so there is nothing for a --json document to report either; --json
        // and --tui already conflict at parse time (cli::ArgParser), so this path
        // never needs to produce one.
        return ExitCode::Success;
    }
    const bool tuiSelectionActive = tuiResult.outcome == TuiRunOutcome::Confirmed;

    // Not fatal if this fails: Ctrl+C would then terminate the process immediately
    // instead of stopping the batch cleanly between items, a degraded (not unsafe)
    // outcome - repairLink() itself never leaves a candidate half-mutated regardless of
    // how the process ends. Still worth telling the user about, since it silently
    // changes what Ctrl+C does.
    const bool ctrlHandlerRegistered =
        ::SetConsoleCtrlHandler(&consoleCtrlHandler, TRUE) != FALSE;
    if (!ctrlHandlerRegistered)
    {
        console.writeLine(L"warning: could not register a Ctrl+C handler - Ctrl+C will "
                          L"terminate immediately instead of stopping after the current "
                          L"item",
                          ConsoleStream::Error);
    }
    g_ctrlCRequested.store(false);

    // One shared executor for both the non-interactive and TUI `fix` paths
    // (docs/adr-phase-6.md ADR-0028) - result accounting and the exit-code decision
    // (toExitCode(exitCodeFor(...)) below) are both made in exactly one place, so
    // neither path can drift from the other's contract.
    RepairBatchOptions batchOptions;
    batchOptions.mode = options.dryRun ? RepairMode::DryRun : RepairMode::Execute;
    batchOptions.assumeYes = options.assumeYes;
    if (tuiSelectionActive)
    {
        // The checklist's confirmed selection already decided consent for every
        // offered candidate; a Missing/Broken candidate not in this list is a
        // *declined* repair (see RepairDisposition::Declined), never an interactive
        // prompt.
        batchOptions.preApprovedAliases = tuiResult.selectedAliases;
    }
    else
    {
        batchOptions.consent = [&console](const RepairItem& candidate) {
            return console.confirm(
                std::format(L"Repair {} (currently {})? [y/N] ",
                           sanitizeForDisplay(candidate.alias),
                           linkStatusDisplayName(candidate.status)),
                /* assumeYes */ false);
        };
    }
    batchOptions.pollInterrupted = []() { return g_ctrlCRequested.load(); };
    batchOptions.onProgress = [&console, &options](std::size_t current, std::size_t total,
                                                    const RepairItemResult& item) {
        if (item.disposition == RepairDisposition::Failed)
        {
            // Errors are always reported, regardless of --json - matching the
            // pre-#60 behavior this replaces.
            if (item.error.has_value() &&
                item.error->kind() == SymlinkServiceErrorKind::InsufficientPermission)
            {
                console.writeLine(std::format(L"[{}/{}] error: {} - {}", current, total,
                                              sanitizeForDisplay(item.candidate.alias),
                                              permissionGuidance(*item.error)),
                                  ConsoleStream::Error);
            }
            else
            {
                console.writeLine(std::format(L"[{}/{}] error: {} - repair failed", current,
                                              total, sanitizeForDisplay(item.candidate.alias)),
                                  ConsoleStream::Error);
            }
            return;
        }
        if (!options.jsonOutput)
        {
            console.writeLine(std::format(L"[{}/{}] {}: {}", current, total,
                                          sanitizeForDisplay(item.candidate.alias),
                                          repairDispositionDisplayName(item.disposition)),
                              ConsoleStream::Output, MessageImportance::Supplementary);
        }
    };

    const RepairBatchResult batchResult =
        runRepairBatch(candidates.nonCollisionItems, batchOptions);

    if (ctrlHandlerRegistered)
    {
        ::SetConsoleCtrlHandler(&consoleCtrlHandler, FALSE);
    }

    if (!options.jsonOutput)
    {
        printBatchSummary(console, batchResult.summary);
    }

    if (options.jsonOutput)
    {
        // toJsonFixResult (docs/adr-phase-5.md ADR-0022) still consumes exactly the
        // SymlinkRepairResult vector it always has - Declined/Failed/NotAttempted
        // items simply have no repairResult to contribute, since none of them ever
        // reached repairLink().
        std::vector<SymlinkRepairResult> jsonResults;
        jsonResults.reserve(batchResult.items.size());
        for (const RepairItemResult& item : batchResult.items)
        {
            if (item.repairResult.has_value())
            {
                jsonResults.push_back(*item.repairResult);
            }
        }
        writeJsonDocument(console, toJsonFixResult(jsonResults, candidates.collisions));
    }

    return toExitCode(exitCodeFor(batchResult.summary));
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
        L"Detects and repairs missing/broken command-alias symlinks under",
        L"%LOCALAPPDATA%\\Microsoft\\WinGet\\Links for winget-installed portable",
        L"packages.",
        L"",
        L"Commands:",
        L"  scan            detect only, read-only (default command)",
        L"  fix             create/repair missing/broken links",
        L"  test-rule NAME  show the replacement result for a real file name",
        L"",
        L"Options:",
        L"  --source com|fs|auto   package enumeration source (default auto:",
        L"                         COM first, filesystem scan fallback)",
        L"  --tui                  run in interactive TUI mode",
        L"  --dry-run              show the plan without executing (for fix)",
        L"  --yes, -y              skip all confirmations and execute",
        L"  --rules <path>         path to a replacement-rules JSON",
        L"  --packages-dir <path>  override the Packages directory",
        L"  --links-dir <path>     override the Links directory",
        L"  --include <glob>       narrow target packages/exes (*, ? wildcards)",
        L"  --exclude <glob>       exclude (always wins over --include)",
        L"  --json                 emit results as JSON (scripting); stdout carries",
        L"                         only the JSON document",
        L"  --verbose / --quiet    log level",
        L"  --fail-on-missing      scan exits 1 if a Missing/Broken/Mismatch link",
        L"                         is found",
        L"  --no-color             disable colored/VT output regardless of TTY",
        L"                         state (also honors the NO_COLOR environment",
        L"                         variable)",
        L"  --silent               do not ask whether to restart elevated; print only",
        L"                         the startup permission message",
        L"  --version              print the version number and exit",
        L"  --help, -h             print this help text and exit",
        L"",
        L"Exit codes:",
        L"  0   success (nothing to fix, or fix succeeded)",
        L"  1   fix needed but not performed (scan --fail-on-missing)",
        L"  2   insufficient permission (Developer Mode off and not elevated)",
        L"  3   argument/config error (invalid option, invalid rules.json, ...)",
        L"  4   package enumeration failed (--source com/fs could not enumerate)",
        L"  10  some repairs failed",
    };
    for (const std::wstring_view line : kHelpLines)
    {
        console.writeLine(line);
    }
}

void printVersion(Console& console)
{
    console.writeLine(std::wstring(L"syncwingetlink ") + kVersion);
}
} // namespace

std::optional<ExitCode> exitCodeAfterElevationDeclined(bool useTui) noexcept
{
    if (useTui)
    {
        return ExitCode::InsufficientPermission;
    }
    return std::nullopt;
}

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
    case PackageSourceErrorKind::PackageIdentityRequired:
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

    Console console(options.noColor, options.logLevel);

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
        if (const std::optional<int> startupResult =
                handleFixStartupPermissionGate(options, console, args);
            startupResult.has_value())
        {
            return *startupResult;
        }

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
        console.writeLine(std::wstring(L"hint: ") + utf8ToWide(remediationFor(error.kind())),
                          ConsoleStream::Error);
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
        // Not ExitCode::PackageEnumerationFailed: a link-inspection failure (denied
        // access under Links, malformed reparse data, ...) is not a package
        // enumeration failure - it has no PackageSourceErrorKind of its own, so it
        // falls into the same generic-failure bucket (exit code 3) the std::exception
        // catch-all below uses for every other condition this dispatch layer did not
        // anticipate closely enough to give its own exit code.
        console.writeLine(utf8ToWide(error.what()), ConsoleStream::Error);
        return static_cast<int>(ExitCode::ArgumentError);
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
