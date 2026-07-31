// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <cli/Json.h>

#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace syncwingetlink;
using namespace syncwingetlink::cli;

namespace syncwingetlink::tests
{
TEST_CLASS(EscapeJsonStringTests)
{
public:
    TEST_METHOD(ordinaryTextIsUnchanged)
    {
        Assert::AreEqual(std::string("codex.exe"), escapeJsonString(L"codex.exe"));
    }

    TEST_METHOD(quoteAndBackslashAreEscaped)
    {
        Assert::AreEqual(std::string("a\\\"b\\\\c"), escapeJsonString(LR"(a"b\c)"));
    }

    TEST_METHOD(namedControlEscapesAreUsed)
    {
        const std::wstring input =
            std::wstring(L"a") + L'\b' + L'\f' + L'\n' + L'\r' + L'\t' + L"b";
        Assert::AreEqual(std::string("a\\b\\f\\n\\r\\tb"), escapeJsonString(input));
    }

    TEST_METHOD(otherC0ControlsUseUnicodeEscape)
    {
        Assert::AreEqual(std::string("a\\u0001b"),
                         escapeJsonString(std::wstring(L"a") + wchar_t(0x01) + L"b"));
        Assert::AreEqual(std::string("a\\u001fb"),
                         escapeJsonString(std::wstring(L"a") + wchar_t(0x1F) + L"b"));
    }

    TEST_METHOD(escapeCharacterUsesUnicodeEscape)
    {
        // Adjacent string-literal concatenation - a hex escape is greedy and would
        // otherwise swallow following hex-digit-shaped characters.
        const std::wstring input = L"before" L"\x1B" L"after";
        Assert::AreEqual(std::string("before\\u001bafter"), escapeJsonString(input));
    }

    TEST_METHOD(nonAsciiBmpCharacterEncodesAsUtf8)
    {
        // U+00E9 (e with acute) - UTF-8: 0xC3 0xA9.
        const std::string result = escapeJsonString(L"caf\u00E9");
        Assert::AreEqual(std::string("caf"), result.substr(0, 3));
        Assert::AreEqual(std::size_t{5}, result.size());
        Assert::AreEqual(static_cast<unsigned char>(0xC3),
                         static_cast<unsigned char>(result[3]));
        Assert::AreEqual(static_cast<unsigned char>(0xA9),
                         static_cast<unsigned char>(result[4]));
    }

    TEST_METHOD(validSurrogatePairEncodesAsOneCodepoint)
    {
        // U+1F600 (grinning face) as a surrogate pair: D83D DE00.
        const std::wstring input(1, static_cast<wchar_t>(0xD83D));
        std::wstring withLowSurrogate = input + static_cast<wchar_t>(0xDE00);

        const std::string result = escapeJsonString(withLowSurrogate);

        // U+1F600 UTF-8: F0 9F 98 80.
        Assert::AreEqual(std::size_t{4}, result.size());
        Assert::AreEqual(static_cast<unsigned char>(0xF0),
                         static_cast<unsigned char>(result[0]));
        Assert::AreEqual(static_cast<unsigned char>(0x9F),
                         static_cast<unsigned char>(result[1]));
        Assert::AreEqual(static_cast<unsigned char>(0x98),
                         static_cast<unsigned char>(result[2]));
        Assert::AreEqual(static_cast<unsigned char>(0x80),
                         static_cast<unsigned char>(result[3]));
    }

    // #62: the same U+1F600 policy as validSurrogatePairEncodesAsOneCodepoint above, but
    // spelled with the compiler-generated \U (8-digit universal-character-name) escape
    // this codebase's test literals use for non-BMP characters, combined with Japanese
    // katakana - the exact fixture text used across SmokeTests.cpp/ConsoleTests.cpp/
    // ExecutableScannerTests.cpp/LinkInspectorTests.cpp/IntegrationTests.cpp for this
    // issue. Written with \uXXXX/\UXXXXXXXX escapes only; no raw non-ASCII bytes in this
    // file. Catches a different bug class than the manually-built-surrogate test above:
    // a compiler that mis-encodes \U0001F600 into a wide string literal would fail this
    // test even if escapeJsonString() itself were correct.
    TEST_METHOD(nonBmpFixtureNameEncodesPerAdr0022SurrogatePolicy)
    {
        const std::wstring fixtureName = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600.exe";

        const std::string result = escapeJsonString(fixtureName);

        // 6 katakana characters x 3 UTF-8 bytes each, plus U+1F600's 4-byte encoding,
        // plus 4 ASCII bytes for ".exe".
        Assert::AreEqual(std::size_t{6 * 3 + 4 + 4}, result.size());
        // U+1F600 GRINNING FACE, UTF-8: F0 9F 98 80 - immediately before ".exe".
        const std::size_t tail = result.size() - 4 - 4;
        Assert::AreEqual(static_cast<unsigned char>(0xF0),
                         static_cast<unsigned char>(result[tail + 0]));
        Assert::AreEqual(static_cast<unsigned char>(0x9F),
                         static_cast<unsigned char>(result[tail + 1]));
        Assert::AreEqual(static_cast<unsigned char>(0x98),
                         static_cast<unsigned char>(result[tail + 2]));
        Assert::AreEqual(static_cast<unsigned char>(0x80),
                         static_cast<unsigned char>(result[tail + 3]));
        Assert::AreEqual(std::string(".exe"), result.substr(result.size() - 4));
    }

    TEST_METHOD(unpairedHighSurrogateBecomesReplacementCharacter)
    {
        const std::wstring input = std::wstring(L"a") + static_cast<wchar_t>(0xD83D) + L"b";
        // U+FFFD UTF-8: EF BF BD.
        Assert::AreEqual(std::string("a\xEF\xBF\xBD" "b"), escapeJsonString(input));
    }

    TEST_METHOD(unpairedLowSurrogateBecomesReplacementCharacter)
    {
        const std::wstring input = std::wstring(L"a") + static_cast<wchar_t>(0xDE00) + L"b";
        Assert::AreEqual(std::string("a\xEF\xBF\xBD" "b"), escapeJsonString(input));
    }

    TEST_METHOD(highSurrogateAtEndOfStringIsUnpaired)
    {
        const std::wstring input = std::wstring(L"a") + static_cast<wchar_t>(0xD83D);
        Assert::AreEqual(std::string("a\xEF\xBF\xBD"), escapeJsonString(input));
    }

    TEST_METHOD(emptyInputProducesEmptyOutput)
    {
        Assert::AreEqual(std::string(""), escapeJsonString(L""));
    }
};

TEST_CLASS(JsonWrapperTests)
{
public:
    TEST_METHOD(toJsonStringWrapsInQuotes)
    {
        Assert::AreEqual(std::string("\"codex.exe\""), toJsonString(L"codex.exe"));
    }

    TEST_METHOD(toJsonBoolProducesLiteralTrueOrFalse)
    {
        Assert::AreEqual(std::string("true"), toJsonBool(true));
        Assert::AreEqual(std::string("false"), toJsonBool(false));
    }

    TEST_METHOD(toJsonPathStringWrapsInQuotesAndEscapesBackslashes)
    {
        // Windows path separators are backslashes, which JSON requires escaped.
        Assert::AreEqual(std::string(R"("C:\\Links\\codex.exe")"),
                         toJsonPathString(LR"(C:\Links\codex.exe)"));
    }

    TEST_METHOD(toJsonPathStringSanitizesControlCharactersBeforeEscaping)
    {
        // A path containing a raw ESC (0x1B) must never reach the JSON output - this is
        // the one sanitization boundary every serialized path goes through, matching
        // toJsonString(sanitizeForDisplay(...)) elsewhere in this module (found missing
        // here during Copilot review of PR #109).
        const std::filesystem::path withEscape =
            std::wstring(LR"(C:\Links\)") + wchar_t(0x1B) + L"evil.exe";

        const std::string json = toJsonPathString(withEscape);

        Assert::IsTrue(json.find('\x1B') == std::string::npos);
    }
};

TEST_CLASS(JsonDomainSerializationTests)
{
public:
    TEST_METHOD(repairItemSerializesAllFields)
    {
        RepairItem item;
        item.executable.path = LR"(C:\Packages\Codex\codex-x64.exe)";
        item.alias = L"codex.exe";
        item.linkPath = LR"(C:\Links\codex.exe)";
        item.status = LinkStatus::Broken;
        item.entryKind = LinkEntryKind::SymbolicLink;
        item.existingTarget = std::filesystem::path(LR"(C:\Packages\Old\old.exe)");

        const std::string json = toJson(item);

        Assert::IsTrue(json.find(R"("status":"Broken")") != std::string::npos);
        Assert::IsTrue(json.find(R"("entryKind":"SymbolicLink")") != std::string::npos);
        Assert::IsTrue(json.find(R"("alias":"codex.exe")") != std::string::npos);
        Assert::IsTrue(json.find("existingTarget") != std::string::npos);
        Assert::IsTrue(json.find("null") == std::string::npos);
    }

    TEST_METHOD(repairItemWithNoExistingTargetSerializesNull)
    {
        RepairItem item;
        item.executable.path = LR"(C:\Packages\Codex\codex-x64.exe)";
        item.alias = L"codex.exe";
        item.linkPath = LR"(C:\Links\codex.exe)";
        item.status = LinkStatus::Missing;
        item.entryKind = LinkEntryKind::None;

        const std::string json = toJson(item);

        Assert::IsTrue(json.find(R"("existingTarget":null)") != std::string::npos);
    }

    TEST_METHOD(repairItemFieldsAreSanitizedBeforeSerialization)
    {
        RepairItem item;
        item.executable.path = LR"(C:\Packages\evil.exe)";
        item.alias = std::wstring(L"tool") + wchar_t(0x1B) + L".exe";
        item.linkPath = LR"(C:\Links\tool.exe)";

        const std::string json = toJson(item);

        // The raw ESC byte must never appear; sanitizeForDisplay() strips it before
        // escapeJsonString() ever sees it.
        Assert::IsTrue(json.find('\x1B') == std::string::npos);
    }

    TEST_METHOD(aliasCollisionSerializesAliasAndExecutableList)
    {
        AliasCollision collision;
        collision.alias = L"codex.exe";
        collision.executables.push_back(PackageExe{LR"(C:\Packages\A\codex.exe)"});
        collision.executables.push_back(PackageExe{LR"(C:\Packages\B\codex.exe)"});

        const std::string json = toJson(collision);

        Assert::IsTrue(json.find(R"("alias":"codex.exe")") != std::string::npos);
        Assert::IsTrue(json.find("executables") != std::string::npos);
        Assert::IsTrue(json.find("A") != std::string::npos);
        Assert::IsTrue(json.find("B") != std::string::npos);
    }

    TEST_METHOD(symlinkRepairResultWithoutVerifiedItemSerializesNull)
    {
        SymlinkRepairResult result;
        result.preActionItem.status = LinkStatus::Mismatch;
        result.outcome = SymlinkRepairOutcome::RefusedMismatch;

        const std::string json = toJson(result);

        Assert::IsTrue(json.find(R"("outcome":"RefusedMismatch")") != std::string::npos);
        Assert::IsTrue(json.find(R"("verifiedItem":null)") != std::string::npos);
    }

    TEST_METHOD(symlinkRepairResultWithVerifiedItemSerializesIt)
    {
        SymlinkRepairResult result;
        result.preActionItem.status = LinkStatus::Missing;
        result.outcome = SymlinkRepairOutcome::Created;
        result.postActionItem = RepairItem{};
        result.postActionItem->status = LinkStatus::Ok;

        const std::string json = toJson(result);

        Assert::IsTrue(json.find(R"("outcome":"Created")") != std::string::npos);
        Assert::IsTrue(json.find(R"("verifiedItem":null)") == std::string::npos);
        Assert::IsTrue(json.find(R"("status":"Ok")") != std::string::npos);
    }

    TEST_METHOD(scanResultWrapsRepairItemsAndCollisions)
    {
        RepairItem item;
        item.status = LinkStatus::Missing;
        AliasCollision collision;
        collision.alias = L"codex.exe";
        collision.executables.push_back(PackageExe{LR"(C:\A\codex.exe)"});
        collision.executables.push_back(PackageExe{LR"(C:\B\codex.exe)"});

        const std::string json = toJsonScanResult({item}, {collision});

        Assert::IsTrue(json.find(R"("command":"scan")") != std::string::npos);
        Assert::IsTrue(json.find(R"("schemaVersion":1)") != std::string::npos);
        Assert::IsTrue(json.find("repairItems") != std::string::npos);
        Assert::IsTrue(json.find("collisions") != std::string::npos);
    }

    TEST_METHOD(scanResultWithNoItemsProducesEmptyArrays)
    {
        const std::string json = toJsonScanResult({}, {});

        Assert::IsTrue(json.find(R"("repairItems":[])") != std::string::npos);
        Assert::IsTrue(json.find(R"("collisions":[])") != std::string::npos);
    }

    TEST_METHOD(fixResultWrapsResultsAndCollisions)
    {
        SymlinkRepairResult result;
        result.outcome = SymlinkRepairOutcome::Created;

        const std::string json = toJsonFixResult({result}, {});

        Assert::IsTrue(json.find(R"("command":"fix")") != std::string::npos);
        Assert::IsTrue(json.find("results") != std::string::npos);
        Assert::IsTrue(json.find(R"("outcome":"Created")") != std::string::npos);
    }
};
} // namespace syncwingetlink::tests
