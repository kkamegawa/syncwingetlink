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
    std::optional<std::wstring> metadataAlias;
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
    std::optional<std::filesystem::path> existingTarget;
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
};
} // namespace syncwingetlink
