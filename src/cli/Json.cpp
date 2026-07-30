// SPDX-License-Identifier: MIT

#include "Json.h"

#include "Console.h"

#include <format>

namespace syncwingetlink::cli
{
namespace
{
constexpr char32_t kReplacementCharacter = 0xFFFD;

void appendUtf8(std::string& out, char32_t codepoint)
{
    if (codepoint <= 0x7F)
    {
        out.push_back(static_cast<char>(codepoint));
    }
    else if (codepoint <= 0x7FF)
    {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else if (codepoint <= 0xFFFF)
    {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
}

[[nodiscard]] bool isHighSurrogate(char16_t unit) noexcept
{
    return unit >= 0xD800 && unit <= 0xDBFF;
}

[[nodiscard]] bool isLowSurrogate(char16_t unit) noexcept
{
    return unit >= 0xDC00 && unit <= 0xDFFF;
}

[[nodiscard]] std::wstring_view linkStatusName(LinkStatus status) noexcept
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

[[nodiscard]] std::wstring_view linkEntryKindName(LinkEntryKind kind) noexcept
{
    switch (kind)
    {
    case LinkEntryKind::None:
        return L"None";
    case LinkEntryKind::RegularFile:
        return L"RegularFile";
    case LinkEntryKind::SymbolicLink:
        return L"SymbolicLink";
    case LinkEntryKind::OtherReparsePoint:
        return L"OtherReparsePoint";
    }
    return L"Unknown";
}

[[nodiscard]] std::wstring_view symlinkRepairOutcomeName(SymlinkRepairOutcome outcome) noexcept
{
    switch (outcome)
    {
    case SymlinkRepairOutcome::WouldCreate:
        return L"WouldCreate";
    case SymlinkRepairOutcome::WouldReplaceBroken:
        return L"WouldReplaceBroken";
    case SymlinkRepairOutcome::Created:
        return L"Created";
    case SymlinkRepairOutcome::ReplacedBroken:
        return L"ReplacedBroken";
    case SymlinkRepairOutcome::SkippedOk:
        return L"SkippedOk";
    case SymlinkRepairOutcome::RefusedMismatch:
        return L"RefusedMismatch";
    }
    return L"Unknown";
}

[[nodiscard]] std::string toJsonOptionalPath(const std::optional<std::filesystem::path>& path)
{
    if (!path.has_value())
    {
        return "null";
    }
    return toJsonString(sanitizeForDisplay(path->native()));
}

[[nodiscard]] std::string joinJsonArray(const std::vector<std::string>& elements)
{
    std::string result = "[";
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        if (i > 0)
        {
            result += ",";
        }
        result += elements[i];
    }
    result += "]";
    return result;
}
} // namespace

std::string escapeJsonString(std::wstring_view text)
{
    std::string out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const char16_t unit = static_cast<char16_t>(text[i]);

        switch (unit)
        {
        case L'"':
            out += "\\\"";
            continue;
        case L'\\':
            out += "\\\\";
            continue;
        case L'\b':
            out += "\\b";
            continue;
        case L'\f':
            out += "\\f";
            continue;
        case L'\n':
            out += "\\n";
            continue;
        case L'\r':
            out += "\\r";
            continue;
        case L'\t':
            out += "\\t";
            continue;
        default:
            break;
        }

        if (unit <= 0x1F)
        {
            out += std::format("\\u{:04x}", static_cast<unsigned>(unit));
            continue;
        }

        if (isHighSurrogate(unit))
        {
            if (i + 1 < text.size())
            {
                const char16_t next = static_cast<char16_t>(text[i + 1]);
                if (isLowSurrogate(next))
                {
                    const char32_t codepoint = 0x10000 +
                        ((static_cast<char32_t>(unit) - 0xD800) << 10) +
                        (static_cast<char32_t>(next) - 0xDC00);
                    appendUtf8(out, codepoint);
                    ++i; // consume the low surrogate too
                    continue;
                }
            }
            appendUtf8(out, kReplacementCharacter);
            continue;
        }

        if (isLowSurrogate(unit))
        {
            appendUtf8(out, kReplacementCharacter);
            continue;
        }

        appendUtf8(out, static_cast<char32_t>(unit));
    }

    return out;
}

std::string toJsonString(std::wstring_view text)
{
    return "\"" + escapeJsonString(text) + "\"";
}

std::string toJsonPathString(const std::filesystem::path& path)
{
    return toJsonString(std::wstring_view(path.native()));
}

std::string toJsonBool(bool value)
{
    return value ? "true" : "false";
}

std::string toJson(const RepairItem& item)
{
    std::string json = "{";
    json += "\"executable\":" + toJsonString(sanitizeForDisplay(item.executable.path.native()));
    json += ",\"alias\":" + toJsonString(sanitizeForDisplay(item.alias));
    json += ",\"linkPath\":" + toJsonString(sanitizeForDisplay(item.linkPath.native()));
    json += ",\"status\":" + toJsonString(linkStatusName(item.status));
    json += ",\"entryKind\":" + toJsonString(linkEntryKindName(item.entryKind));
    json += ",\"existingTarget\":" + toJsonOptionalPath(item.existingTarget);
    json += "}";
    return json;
}

std::string toJson(const AliasCollision& collision)
{
    std::vector<std::string> executables;
    executables.reserve(collision.executables.size());
    for (const PackageExe& executable : collision.executables)
    {
        executables.push_back(
            toJsonString(sanitizeForDisplay(executable.path.native())));
    }

    std::string json = "{\"alias\":" + toJsonString(sanitizeForDisplay(collision.alias));
    json += ",\"executables\":" + joinJsonArray(executables);
    json += "}";
    return json;
}

std::string toJson(const SymlinkRepairResult& result)
{
    std::string json = "{\"item\":" + toJson(result.preActionItem);
    json += ",\"outcome\":" + toJsonString(symlinkRepairOutcomeName(result.outcome));
    json += ",\"verifiedItem\":";
    json += result.postActionItem.has_value() ? toJson(*result.postActionItem) : "null";
    json += "}";
    return json;
}

std::string toJsonScanResult(const std::vector<RepairItem>& repairItems,
                             const std::vector<AliasCollision>& collisions)
{
    std::vector<std::string> items;
    items.reserve(repairItems.size());
    for (const RepairItem& item : repairItems)
    {
        items.push_back(toJson(item));
    }

    std::vector<std::string> collisionEntries;
    collisionEntries.reserve(collisions.size());
    for (const AliasCollision& collision : collisions)
    {
        collisionEntries.push_back(toJson(collision));
    }

    std::string json = R"({"schemaVersion":1,"command":"scan","repairItems":)";
    json += joinJsonArray(items);
    json += ",\"collisions\":" + joinJsonArray(collisionEntries);
    json += "}";
    return json;
}

std::string toJsonFixResult(const std::vector<SymlinkRepairResult>& results,
                           const std::vector<AliasCollision>& collisions)
{
    std::vector<std::string> resultEntries;
    resultEntries.reserve(results.size());
    for (const SymlinkRepairResult& result : results)
    {
        resultEntries.push_back(toJson(result));
    }

    std::vector<std::string> collisionEntries;
    collisionEntries.reserve(collisions.size());
    for (const AliasCollision& collision : collisions)
    {
        collisionEntries.push_back(toJson(collision));
    }

    std::string json = R"({"schemaVersion":1,"command":"fix","results":)";
    json += joinJsonArray(resultEntries);
    json += ",\"collisions\":" + joinJsonArray(collisionEntries);
    json += "}";
    return json;
}
} // namespace syncwingetlink::cli
