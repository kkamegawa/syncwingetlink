// SPDX-License-Identifier: MIT

#include "ArgParser.h"

#include "rules/RuleSet.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>; only WideCharToMultiByte is needed here, which lives in
// <stringapiset.h> (pulled in transitively via <Windows.h>) - the same boundary
// rules/RuleSetSelector.cpp crosses for the same reason.
#include <Windows.h>

#include <cstddef>
#include <filesystem>

namespace syncwingetlink::cli
{
namespace
{
[[nodiscard]] std::string toUtf8(std::wstring_view text)
{
    if (text.empty())
    {
        return {};
    }

    const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                                static_cast<int>(text.size()), nullptr, 0,
                                                nullptr, nullptr);
    std::string result(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                         required, nullptr, nullptr);
    return result;
}

[[noreturn]] void throwError(ArgParseErrorKind kind, const std::string& message)
{
    throw ArgParseError(kind, message);
}

// Consumes and returns the value token following an option at args[index], advancing
// index to point at the value. Throws MissingOptionValue if the option is the last
// token.
[[nodiscard]] const std::wstring& takeOptionValue(const std::vector<std::wstring>& args,
                                                  std::size_t& index,
                                                  std::wstring_view optionName)
{
    if (index + 1 >= args.size())
    {
        throwError(ArgParseErrorKind::MissingOptionValue,
                   "missing value for " + toUtf8(optionName));
    }

    return args[++index];
}

[[nodiscard]] PackageSource parseSource(std::wstring_view value)
{
    if (value == L"com")
    {
        return PackageSource::Com;
    }
    if (value == L"fs")
    {
        return PackageSource::FileSystem;
    }
    if (value == L"auto")
    {
        return PackageSource::Auto;
    }

    throwError(ArgParseErrorKind::InvalidOptionValue,
              "--source must be one of com, fs, auto (got \"" + toUtf8(value) + "\")");
}

// Validates and normalizes a --links-dir/--packages-dir/--rules value. Rejected only for
// being empty or a "\\.\" device path - deliberately NOT required to already exist (see
// ArgParser.h's documentation on parseArguments() for why: an absent Packages or Links
// directory is a normal, tolerated state elsewhere in the codebase). Made absolute and
// lexically normalized so AppOptions always holds a stable, displayable path rather than
// whatever relative form the user happened to type, relative to the process's current
// directory at parse time.
[[nodiscard]] std::filesystem::path validatePathOverride(const std::wstring& value,
                                                          std::wstring_view optionName)
{
    if (value.empty())
    {
        throwError(ArgParseErrorKind::InvalidPathOverride,
                  toUtf8(optionName) + " must not be empty");
    }

    if (value.starts_with(LR"(\\.\)"))
    {
        throwError(ArgParseErrorKind::InvalidPathOverride,
                  toUtf8(optionName) + " must not be a device path: " + toUtf8(value));
    }

    try
    {
        std::filesystem::path path(value);
        path = path.is_absolute() ? path : std::filesystem::absolute(path);
        path = path.lexically_normal();
        path.make_preferred();
        return path;
    }
    catch (const std::filesystem::filesystem_error& error)
    {
        throwError(ArgParseErrorKind::InvalidPathOverride,
                  toUtf8(optionName) + " could not be resolved: " + error.what());
    }
}

// Returns true when the token was a global short-circuit flag (--help/-h/--version) and
// options.command was set accordingly - the caller should stop parsing immediately in
// that case, per the documented "help/version wins over every other validation" rule.
[[nodiscard]] bool tryHandleGlobalFlag(const std::wstring& arg, AppOptions& options)
{
    if (arg == L"--help" || arg == L"-h")
    {
        options.command = AppCommand::Help;
        return true;
    }

    if (arg == L"--version")
    {
        options.command = AppCommand::Version;
        return true;
    }

    return false;
}

void handleOption(const std::wstring& arg, const std::vector<std::wstring>& args,
                  std::size_t& index, AppOptions& options)
{
    if (arg == L"--source")
    {
        options.source = parseSource(takeOptionValue(args, index, L"--source"));
    }
    else if (arg == L"--tui")
    {
        options.useTui = true;
    }
    else if (arg == L"--dry-run")
    {
        options.dryRun = true;
    }
    else if (arg == L"--yes" || arg == L"-y")
    {
        options.assumeYes = true;
    }
    else if (arg == L"--rules")
    {
        options.rulesPath = validatePathOverride(takeOptionValue(args, index, L"--rules"),
                                                  L"--rules");
    }
    else if (arg == L"--packages-dir")
    {
        options.packagesDirectory =
            validatePathOverride(takeOptionValue(args, index, L"--packages-dir"),
                                 L"--packages-dir");
    }
    else if (arg == L"--links-dir")
    {
        options.linksDirectory =
            validatePathOverride(takeOptionValue(args, index, L"--links-dir"), L"--links-dir");
    }
    else if (arg == L"--include")
    {
        options.includePatterns.push_back(takeOptionValue(args, index, L"--include"));
    }
    else if (arg == L"--exclude")
    {
        options.excludePatterns.push_back(takeOptionValue(args, index, L"--exclude"));
    }
    else if (arg == L"--json")
    {
        options.jsonOutput = true;
    }
    else if (arg == L"--verbose")
    {
        options.logLevel = LogLevel::Verbose;
    }
    else if (arg == L"--quiet")
    {
        options.logLevel = LogLevel::Quiet;
    }
    else if (arg == L"--fail-on-missing")
    {
        options.failOnMissing = true;
    }
    else if (arg == L"--no-color")
    {
        options.noColor = true;
    }
    else
    {
        throwError(ArgParseErrorKind::UnknownOption, "unknown option: " + toUtf8(arg));
    }
}

void applyCommandToken(const std::wstring& token, AppOptions& options)
{
    if (token == L"scan")
    {
        options.command = AppCommand::Scan;
    }
    else if (token == L"fix")
    {
        options.command = AppCommand::Fix;
    }
    else if (token == L"test-rule")
    {
        options.command = AppCommand::TestRule;
    }
    else
    {
        throwError(ArgParseErrorKind::UnknownCommand, "unknown command: " + toUtf8(token));
    }
}

void applyPositionals(std::vector<std::wstring> positionals, AppOptions& options)
{
    std::size_t index = 0;

    if (index < positionals.size())
    {
        applyCommandToken(positionals[index], options);
        ++index;
    }

    if (options.command == AppCommand::TestRule)
    {
        if (index >= positionals.size())
        {
            throwError(ArgParseErrorKind::MissingArgument, "test-rule requires NAME");
        }

        const std::wstring& name = positionals[index];
        if (!isValidAliasFileName(name))
        {
            throwError(ArgParseErrorKind::InvalidTestRuleName,
                      "test-rule NAME must be a bare file name, not a path: " + toUtf8(name));
        }

        options.testRuleName = name;
        ++index;
    }

    if (index < positionals.size())
    {
        throwError(ArgParseErrorKind::UnexpectedArgument,
                  "unexpected argument: " + toUtf8(positionals[index]));
    }
}
} // namespace

AppOptions parseArguments(const std::vector<std::wstring>& args)
{
    AppOptions options;
    std::vector<std::wstring> positionals;
    bool sawTerminator = false;

    for (std::size_t index = 0; index < args.size(); ++index)
    {
        const std::wstring& arg = args[index];

        if (!sawTerminator && arg == L"--")
        {
            sawTerminator = true;
            continue;
        }

        if (!sawTerminator && tryHandleGlobalFlag(arg, options))
        {
            // --help/--version short-circuits every other validation: a malformed
            // command line elsewhere must not prevent the user from getting help.
            return options;
        }

        if (!sawTerminator && !arg.empty() && arg[0] == L'-')
        {
            handleOption(arg, args, index, options);
            continue;
        }

        positionals.push_back(arg);
    }

    applyPositionals(std::move(positionals), options);

    if (options.jsonOutput && options.command == AppCommand::Fix && !options.assumeYes)
    {
        throwError(ArgParseErrorKind::ConflictingOptions,
                  "--json with fix requires --yes: a script cannot answer an interactive "
                  "confirmation prompt");
    }

    return options;
}
} // namespace syncwingetlink::cli
