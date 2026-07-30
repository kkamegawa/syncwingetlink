// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace syncwingetlink
{
enum class LinkStatus
{
    Ok,
    Missing,
    Broken,
    Mismatch,
};

// What LinkInspector found at Links\<alias>.exe before classification. Distinct from
// LinkStatus: this is the raw observed filesystem entry type, not the verdict derived
// from it (docs/adr-phase-3.md).
enum class LinkEntryKind
{
    None,
    RegularFile,
    SymbolicLink,
    OtherReparsePoint,
};

enum class AppCommand
{
    Scan,
    Fix,
    TestRule,
    Help,
    Version,
};

enum class PackageSource
{
    Auto,
    Com,
    FileSystem,
};

enum class LogLevel
{
    Quiet,
    Normal,
    Verbose,
};

struct PackageExe
{
    std::filesystem::path path;
};

struct InstalledPackage
{
    std::wstring id;
    std::wstring name;
    std::wstring version;
    std::filesystem::path installLocation;
    std::vector<PackageExe> executables;
};

struct RepairItem
{
    PackageExe executable;
    std::wstring alias;
    std::filesystem::path linkPath;
    LinkStatus status{LinkStatus::Missing};
    LinkEntryKind entryKind{LinkEntryKind::None};
    std::optional<std::filesystem::path> existingTarget;
};

// Reports that two or more distinct package executables resolved to the same alias.
// Returned separately from LinkStatus - a colliding alias is never itself one of the
// four link states - so downstream code (M6/M7) can warn about it and require an
// explicit user choice, rather than sending it into an automatic repair path
// (docs/adr-phase-3.md).
struct AliasCollision
{
    std::wstring alias;
    // Distinct executables that resolved to alias, sorted deterministically. Never
    // fewer than two entries - see detectAliasCollisions() in core/LinkInspector.h.
    std::vector<PackageExe> executables;
};

struct AppOptions
{
    AppCommand command{AppCommand::Scan};
    PackageSource source{PackageSource::Auto};
    LogLevel logLevel{LogLevel::Normal};
    std::optional<std::filesystem::path> linksDirectory;
    std::optional<std::filesystem::path> packagesDirectory;
    std::optional<std::filesystem::path> rulesPath;
    std::vector<std::wstring> includePatterns;
    std::vector<std::wstring> excludePatterns;
    std::optional<std::wstring> testRuleName;
    bool useTui{false};
    bool dryRun{false};
    bool assumeYes{false};
    bool jsonOutput{false};
    bool failOnMissing{false};
    bool noColor{false};
};
} // namespace syncwingetlink
