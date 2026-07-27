// SPDX-License-Identifier: MIT

#include "RuleSet.h"

#include "../core/ComApartment.h"

// WIN32_LEAN_AND_MEAN (set project-wide in props/syncwingetlink.common.props) excludes
// <ole2.h> from <Windows.h>, which is where CompareStringOrdinal lives via <winnls.h>
// (pulled in transitively) - see PackageSourceError.cpp's isPortableInstallerType for the
// same pattern.
#include <Windows.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>

// winrt::Windows::Data::Json activates WinRT runtime classes, which need
// runtimeobject.lib (see WingetComSource.cpp for the full rationale). A linker directive
// embedded in an .obj propagates through the static library archive, but only for a link
// that actually pulls that .obj in - a test binary exercising RuleSet without touching
// WingetComSource needs its own copy of this pragma, not just the one already in
// WingetComSource.cpp.
#pragma comment(lib, "runtimeobject.lib")

#include <regex>
#include <unordered_set>
#include <utility>

namespace syncwingetlink
{
namespace
{
using namespace winrt::Windows::Data::Json;

// Converts a rule name (or other diagnostic text) to UTF-8 for embedding in a
// std::runtime_error message, which is narrow. Rule names are user-authored JSON strings
// and are not guaranteed ASCII.
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

[[nodiscard]] std::wstring requireStringField(const JsonObject& object, const wchar_t* field)
{
    if (!object.HasKey(field))
    {
        throw RuleSetError(RuleSetErrorKind::MissingField,
                           "a rule is missing a required field: " + toUtf8(field));
    }

    const IJsonValue value = object.GetNamedValue(field);
    if (value.ValueType() != JsonValueType::String)
    {
        throw RuleSetError(RuleSetErrorKind::InvalidFieldType,
                           "a rule field must be a JSON string: " + toUtf8(field));
    }

    return std::wstring(value.GetString());
}
} // namespace

bool isValidAliasFileName(std::wstring_view candidate) noexcept
{
    constexpr std::wstring_view kExeExtension = L".exe";

    if (candidate.size() <= kExeExtension.size())
    {
        return false;
    }
    if (candidate.find(L'\\') != std::wstring_view::npos ||
        candidate.find(L'/') != std::wstring_view::npos)
    {
        return false;
    }

    const std::wstring_view suffix = candidate.substr(candidate.size() - kExeExtension.size());
    // Ordinal, not locale-sensitive - same rationale as isPortableInstallerType() in
    // PackageSourceError.cpp: a locale-dependent case fold must not change whether a
    // rule-produced alias is accepted.
    const int comparison = ::CompareStringOrdinal(
        suffix.data(), static_cast<int>(suffix.size()), kExeExtension.data(),
        static_cast<int>(kExeExtension.size()), TRUE);
    if (comparison != CSTR_EQUAL)
    {
        return false;
    }

    const std::wstring_view stem = candidate.substr(0, candidate.size() - kExeExtension.size());
    // Rejects a stem made up entirely of dots (e.g. "..exe" -> stem ".."), which would
    // otherwise smuggle a "." or ".." path segment through as an alias file name.
    return stem.find_first_not_of(L'.') != std::wstring_view::npos;
}

RuleSet::RuleSet(std::vector<AliasRule> rules)
{
    std::unordered_set<std::wstring> seenNames;
    m_rules.reserve(rules.size());

    for (AliasRule& rule : rules)
    {
        if (rule.name.empty())
        {
            throw RuleSetError(RuleSetErrorKind::InvalidRuleName,
                               "a rule's \"name\" must not be empty");
        }
        if (!seenNames.insert(rule.name).second)
        {
            throw RuleSetError(RuleSetErrorKind::InvalidRuleName,
                               "duplicate rule name: " + toUtf8(rule.name));
        }

        auto flags = std::regex::ECMAScript;
        if (rule.ignoreCase)
        {
            flags |= std::regex::icase;
        }

        std::wregex compiled;
        try
        {
            compiled = std::wregex(rule.pattern, flags);
        }
        catch (const std::regex_error&)
        {
            throw RuleSetError(RuleSetErrorKind::InvalidRegex,
                               "rule pattern does not compile as an ECMAScript regex: " +
                                   toUtf8(rule.name));
        }

        m_rules.push_back(
            CompiledRule{std::move(rule.name), std::move(compiled), std::move(rule.replacement)});
    }
}

RuleSet RuleSet::parse(std::wstring_view jsonText)
{
    // winrt::Windows::Data::Json requires an initialized apartment - see the class
    // comment in RuleSet.h and docs/adr-phase-2.md ADR-0011. Tolerates a thread that
    // already has one (e.g. WingetComSource's own ComApartment further up the call
    // stack), same as every other ComApartment use in this codebase.
    ComApartment apartment;

    JsonObject root{nullptr};
    try
    {
        root = JsonObject::Parse(winrt::hstring(jsonText));
    }
    catch (const winrt::hresult_error&)
    {
        throw RuleSetError(RuleSetErrorKind::ParseError,
                           "rules.json is not well-formed JSON, or its root is not an object");
    }

    if (!root.HasKey(L"version"))
    {
        throw RuleSetError(RuleSetErrorKind::UnsupportedVersion,
                           "rules.json is missing the required \"version\" field");
    }
    const IJsonValue versionValue = root.GetNamedValue(L"version");
    if (versionValue.ValueType() != JsonValueType::Number || versionValue.GetNumber() != 1.0)
    {
        throw RuleSetError(RuleSetErrorKind::UnsupportedVersion,
                           "rules.json \"version\" must be the number 1");
    }

    if (!root.HasKey(L"rules"))
    {
        throw RuleSetError(RuleSetErrorKind::MissingField,
                           "rules.json is missing the required \"rules\" array");
    }
    const IJsonValue rulesValue = root.GetNamedValue(L"rules");
    if (rulesValue.ValueType() != JsonValueType::Array)
    {
        throw RuleSetError(RuleSetErrorKind::InvalidFieldType,
                           "rules.json \"rules\" must be an array");
    }
    const JsonArray rulesArray = rulesValue.GetArray();

    std::vector<AliasRule> parsedRules;
    parsedRules.reserve(rulesArray.Size());

    for (const IJsonValue& element : rulesArray)
    {
        if (element.ValueType() != JsonValueType::Object)
        {
            throw RuleSetError(RuleSetErrorKind::InvalidFieldType,
                               "each entry in \"rules\" must be a JSON object");
        }
        const JsonObject ruleObject = element.GetObject();

        AliasRule rule;
        rule.name = requireStringField(ruleObject, L"name");
        if (rule.name.empty())
        {
            throw RuleSetError(RuleSetErrorKind::InvalidRuleName,
                               "a rule's \"name\" must not be empty");
        }
        rule.pattern = requireStringField(ruleObject, L"pattern");
        rule.replacement = requireStringField(ruleObject, L"replacement");

        if (ruleObject.HasKey(L"flags"))
        {
            const IJsonValue flagsValue = ruleObject.GetNamedValue(L"flags");
            if (flagsValue.ValueType() != JsonValueType::Array)
            {
                throw RuleSetError(RuleSetErrorKind::InvalidFieldType,
                                   "a rule's \"flags\" must be an array of strings");
            }
            for (const IJsonValue& flag : flagsValue.GetArray())
            {
                if (flag.ValueType() != JsonValueType::String)
                {
                    throw RuleSetError(RuleSetErrorKind::InvalidFieldType,
                                       "a rule's \"flags\" entries must be strings");
                }
                const std::wstring flagText(flag.GetString());
                if (flagText != L"ignorecase")
                {
                    throw RuleSetError(
                        RuleSetErrorKind::InvalidFlag,
                        "unsupported rule flag (only \"ignorecase\" is recognized): " +
                            toUtf8(flagText));
                }
                rule.ignoreCase = true;
            }
        }

        parsedRules.push_back(std::move(rule));
    }

    // Re-validates names/patterns through the same path RuleSet(vector<AliasRule>) uses
    // for #38's embedded rules, so parsed and embedded rules can never drift in what
    // counts as valid.
    return RuleSet(std::move(parsedRules));
}

std::optional<AliasRuleMatch> RuleSet::resolve(std::wstring_view fileName) const
{
    const std::wstring subject(fileName);

    for (const CompiledRule& rule : m_rules)
    {
        std::wsmatch match;
        if (!std::regex_match(subject, match, rule.regex))
        {
            continue;
        }

        std::wstring alias = match.format(rule.replacement);
        if (!isValidAliasFileName(alias))
        {
            // The first matching rule produced something that cannot be a real alias
            // (e.g. an empty capture, or a replacement missing ".exe"). Do not keep
            // trying later rules - AliasResolver's raw-filename fallback exists exactly
            // for this case, and which rule "won" a given file name must stay
            // predictable rather than depending on what rules follow it.
            return std::nullopt;
        }

        return AliasRuleMatch{rule.name, std::move(alias)};
    }

    return std::nullopt;
}
} // namespace syncwingetlink
