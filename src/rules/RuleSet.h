// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace syncwingetlink
{
// Why parsing or validating rules.json failed. AliasResolver / the M6 CLI map every kind
// onto exit code 3 ("argument/config error") - see docs/PLAN.md S8's exit code table.
enum class RuleSetErrorKind
{
    ParseError,         // the text is not well-formed JSON, or its root is not an object
    UnsupportedVersion, // "version" is missing, not a number, or not the supported value 1
    MissingField,       // a required field ("rules", or a rule's name/pattern/replacement) is absent
    InvalidFieldType,   // a field is present but is not the JSON type this schema requires
    InvalidRuleName,    // a rule name is empty, or duplicates an earlier rule's name
    InvalidFlag,        // rules[].flags contains a value other than "ignorecase"
    InvalidRegex,       // rules[].pattern does not compile as an ECMAScript regex
    FileReadError,      // a selected rules file (--rules, or the user rules file once it
                        // is known to exist) could not be opened or read - see
                        // rules/RuleSetSelector.h
};

class RuleSetError : public std::runtime_error
{
public:
    RuleSetError(RuleSetErrorKind kind, const std::string& message)
        : std::runtime_error(message), m_kind(kind)
    {
    }

    [[nodiscard]] RuleSetErrorKind kind() const noexcept
    {
        return m_kind;
    }

private:
    RuleSetErrorKind m_kind;
};

// One replacement rule as parsed from rules.json, before its pattern is compiled. Exposed
// (rather than kept private to RuleSet) so #38's embedded default rules can be constructed
// without round-tripping through JSON, and so a future test-rule command can inspect a
// rule's declared name/pattern/replacement.
struct AliasRule
{
    std::wstring name;
    std::wstring pattern;
    std::wstring replacement;
    bool ignoreCase{false};
};

// The result of a successful rule match: which rule matched, and the alias it produced.
struct AliasRuleMatch
{
    std::wstring ruleName;
    std::wstring alias;
};

// True when candidate is a well-formed alias file name: non-empty, ending in ".exe"
// (ordinal, case-insensitive - see the rationale on isPortableInstallerType() in
// PackageSourceError.cpp), with a non-empty stem that is not made up entirely of dots
// (which would otherwise let a "." or ".." path segment through a rule's replacement
// text), and containing no path separator. AliasResolver (a later M3 issue) applies this
// to its raw-filename fallback tier too, not just to regex-produced aliases.
[[nodiscard]] bool isValidAliasFileName(std::wstring_view candidate) noexcept;

// An ordered list of regex replacement rules for deriving <alias>.exe from a real
// executable file name (docs/rules.md). Pure logic once constructed - matching a file name
// against already-compiled rules needs no filesystem or WinRT dependency, so it stays
// unit-testable without an initialized COM apartment. parse() is the one entry point that
// needs one (winrt::Windows::Data::Json requires it at runtime), confined to
// RuleSet.cpp - see docs/adr-phase-2.md ADR-0011.
class RuleSet
{
public:
    RuleSet() = default;

    // Validates and compiles rules directly, without going through JSON. Used by #38's
    // embedded default rules and by parse() below. Throws RuleSetError for an empty or
    // duplicate rule name, or a pattern that fails to compile; never leaves *this
    // partially constructed on failure (the exception is thrown from the constructor).
    explicit RuleSet(std::vector<AliasRule> rules);

    // Parses and validates a version-1 rules.json document. Throws RuleSetError on any
    // malformed or invalid input.
    [[nodiscard]] static RuleSet parse(std::wstring_view jsonText);

    // Applies the first rule (in declared order) whose pattern matches fileName in full
    // (std::regex_match against the whole string, not a partial std::regex_search).
    // Returns nullopt if no rule matches, or if the matching rule's replacement produces
    // an alias that fails isValidAliasFileName - in that case the caller (AliasResolver)
    // is expected to fall back to the raw file name rather than trying a later rule, so
    // which rule "won" a given file name stays predictable and does not depend on what
    // other rules happen to be configured.
    [[nodiscard]] std::optional<AliasRuleMatch> resolve(std::wstring_view fileName) const;

    [[nodiscard]] bool isEmpty() const noexcept
    {
        return m_rules.empty();
    }

    [[nodiscard]] std::size_t size() const noexcept
    {
        return m_rules.size();
    }

private:
    struct CompiledRule
    {
        std::wstring name;
        std::wregex regex;
        std::wstring replacement;
    };

    std::vector<CompiledRule> m_rules;
};
} // namespace syncwingetlink
