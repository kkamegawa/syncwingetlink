// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <rules/RuleSetSelector.h>

#include "TempDirectory.h"

#include <fstream>
#include <optional>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
void writeUtf8File(const std::filesystem::path& path, std::string_view content,
                   bool withBom = false)
{
    std::ofstream stream(path, std::ios::binary);
    if (withBom)
    {
        stream << "\xEF\xBB\xBF";
    }
    stream << content;
}

[[nodiscard]] std::filesystem::path missingPath(const TempDirectory& temp)
{
    return temp.path() / L"does-not-exist.json";
}
} // namespace

TEST_CLASS(RuleSetSelectorTests)
{
public:
    TEST_METHOD(anExplicitPathTakesPriorityAndIsUsedEvenIfAUserFileExists)
    {
        const TempDirectory temp(L"selector-explicit-priority");
        const std::filesystem::path explicitFile = temp.path() / L"explicit.json";
        const std::filesystem::path userFile = temp.path() / L"user.json";

        writeUtf8File(explicitFile,
                     R"({"version": 1, "rules": [{"name": "from-explicit", )"
                     R"("pattern": "^codex\\.exe$", "replacement": "explicit-codex.exe"}]})");
        writeUtf8File(userFile,
                     R"({"version": 1, "rules": [{"name": "from-user", )"
                     R"("pattern": "^codex\\.exe$", "replacement": "user-codex.exe"}]})");

        const RuleSet rules =
            selectRuleSet(explicitFile, [&userFile] { return userFile; });

        const auto match = rules.resolve(L"codex.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"explicit-codex.exe"), match->alias);
    }

    TEST_METHOD(theUserFileIsUsedWhenPresentAndNoExplicitPathIsGiven)
    {
        const TempDirectory temp(L"selector-user-file");
        const std::filesystem::path userFile = temp.path() / L"user.json";
        writeUtf8File(userFile,
                     R"({"version": 1, "rules": [{"name": "from-user", )"
                     R"("pattern": "^codex\\.exe$", "replacement": "user-codex.exe"}]})");

        const RuleSet rules = selectRuleSet(std::nullopt, [&userFile] { return userFile; });

        const auto match = rules.resolve(L"codex.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"user-codex.exe"), match->alias);
    }

    TEST_METHOD(anAbsentUserFileFallsBackToEmbeddedDefaults)
    {
        const TempDirectory temp(L"selector-absent-user-file");
        const std::filesystem::path absent = missingPath(temp);

        const RuleSet rules = selectRuleSet(std::nullopt, [&absent] { return absent; });

        // A default-rules behavior: the documented Rust target-triple example.
        const auto match = rules.resolve(L"codex-x86_64-pc-windows-msvc.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"codex.exe"), match->alias);
    }

    TEST_METHOD(aMissingExplicitPathThrowsRatherThanFallingBackToAnything)
    {
        const TempDirectory temp(L"selector-missing-explicit");
        const std::filesystem::path absent = missingPath(temp);

        try
        {
            static_cast<void>(selectRuleSet(absent, [] { return std::filesystem::path(); }));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::FileReadError == error.kind());
        }
    }

    TEST_METHOD(aMalformedExplicitFileThrowsRatherThanFallingBackToAnything)
    {
        const TempDirectory temp(L"selector-malformed-explicit");
        const std::filesystem::path malformed = temp.path() / L"malformed.json";
        writeUtf8File(malformed, "{ not json");

        Assert::ExpectException<RuleSetError>(
            [&malformed] { static_cast<void>(selectRuleSet(malformed, [] { return std::filesystem::path(); })); });
    }

    TEST_METHOD(aMalformedUserFileThrowsAndDoesNotSilentlyFallBackToEmbeddedDefaults)
    {
        const TempDirectory temp(L"selector-malformed-user");
        const std::filesystem::path malformed = temp.path() / L"user.json";
        writeUtf8File(malformed, "{ not json");

        try
        {
            static_cast<void>(selectRuleSet(std::nullopt, [&malformed] { return malformed; }));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::ParseError == error.kind());
        }
    }

    TEST_METHOD(loadRuleSetFromFileTreatsAMissingFileAsAFileReadError)
    {
        const TempDirectory temp(L"selector-load-missing");

        try
        {
            static_cast<void>(loadRuleSetFromFile(missingPath(temp)));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::FileReadError == error.kind());
        }
    }

    TEST_METHOD(loadRuleSetFromFileStripsALeadingUtf8Bom)
    {
        const TempDirectory temp(L"selector-bom");
        const std::filesystem::path withBom = temp.path() / L"with-bom.json";
        writeUtf8File(withBom,
                     R"({"version": 1, "rules": [{"name": "n", "pattern": "^codex\\.exe$", )"
                     R"("replacement": "codex-alias.exe"}]})",
                     /* withBom */ true);

        const RuleSet rules = loadRuleSetFromFile(withBom);

        const auto match = rules.resolve(L"codex.exe");
        Assert::IsTrue(match.has_value());
        Assert::AreEqual(std::wstring(L"codex-alias.exe"), match->alias);
    }

    TEST_METHOD(loadRuleSetFromFileRejectsInvalidUtf8WithAClearError)
    {
        // A lone continuation byte (0x80) is never valid on its own in UTF-8. Before the
        // MB_ERR_INVALID_CHARS fix, MultiByteToWideChar silently produced an empty wide
        // string here, and RuleSet::parse("") failed with a confusing ParseError instead
        // of reporting the actual encoding problem.
        const TempDirectory temp(L"selector-invalid-utf8");
        const std::filesystem::path invalid = temp.path() / L"invalid.json";
        writeUtf8File(invalid, "\x80\x80\x80");

        try
        {
            static_cast<void>(loadRuleSetFromFile(invalid));
            Assert::Fail(L"expected RuleSetError");
        }
        catch (const RuleSetError& error)
        {
            Assert::IsTrue(RuleSetErrorKind::FileReadError == error.kind());
        }
    }
};
} // namespace syncwingetlink::tests
