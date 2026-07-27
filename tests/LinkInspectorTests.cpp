// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/LinkInspector.h>

#include "TempDirectory.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace syncwingetlink::tests
{
namespace
{
[[nodiscard]] PackageExe makeExecutable()
{
    PackageExe exe;
    exe.path = LR"(C:\Packages\Codex\codex-x86_64-pc-windows-msvc.exe)";
    return exe;
}

constexpr std::wstring_view kAlias = L"codex.exe";
constexpr std::wstring_view kLinkPath = LR"(C:\Links\codex.exe)";

void writeU16(std::vector<std::byte>& buffer, std::size_t offset, std::uint16_t value)
{
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void writeU32(std::vector<std::byte>& buffer, std::size_t offset, std::uint32_t value)
{
    std::memcpy(buffer.data() + offset, &value, sizeof(value));
}

void writeWideString(std::vector<std::byte>& buffer, std::size_t byteOffset,
                     std::wstring_view text)
{
    std::memcpy(buffer.data() + byteOffset, text.data(), text.size() * sizeof(wchar_t));
}

// Builds a well-formed IO_REPARSE_TAG_SYMLINK buffer with substituteName placed right
// after the fixed fields (PrintName is left empty/unused - decodeSymbolicLinkTarget()
// never reads it).
[[nodiscard]] std::vector<std::byte> makeSymlinkBuffer(std::wstring_view substituteName,
                                                       std::uint32_t flags)
{
    const auto substituteNameLength =
        static_cast<std::uint16_t>(substituteName.size() * sizeof(wchar_t));
    const auto reparseDataLength = static_cast<std::uint16_t>(12 + substituteNameLength);

    std::vector<std::byte> buffer(8 + reparseDataLength, std::byte{0});
    writeU32(buffer, 0, IO_REPARSE_TAG_SYMLINK);
    writeU16(buffer, 4, reparseDataLength);
    writeU16(buffer, 8, 0); // SubstituteNameOffset
    writeU16(buffer, 10, substituteNameLength);
    writeU16(buffer, 12, 0); // PrintNameOffset
    writeU16(buffer, 14, 0); // PrintNameLength
    writeU32(buffer, 16, flags);
    writeWideString(buffer, 20, substituteName);
    return buffer;
}
} // namespace

TEST_CLASS(LinkInspectorTests)
{
public:
    // --- Classification table rows (docs/adr-phase-3.md ADR-0014) ---

    TEST_METHOD(noEntryIsMissingWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::None, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Missing);
        Assert::IsTrue(item.entryKind == LinkEntryKind::None);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(regularFileIsMismatchWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::RegularFile, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::RegularFile);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(nonSymlinkReparsePointIsMismatchWithNoExistingTarget)
    {
        const LinkObservation observation{LinkEntryKind::OtherReparsePoint, std::nullopt,
                                          TargetRelation::NotApplicable};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::OtherReparsePoint);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkWithAbsentTargetIsBroken)
    {
        const std::filesystem::path decodedTarget = LR"(C:\Packages\Codex\old-name.exe)";
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::Missing};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Broken);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::AreEqual(decodedTarget.native(), item.existingTarget->native());
    }

    TEST_METHOD(symbolicLinkResolvingToExpectedFileIsOk)
    {
        const std::filesystem::path decodedTarget = makeExecutable().path;
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::SameFile};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Ok);
        Assert::IsTrue(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkResolvingToAnotherExistingFileIsMismatch)
    {
        const std::filesystem::path decodedTarget =
            LR"(C:\Packages\OtherPackage\other.exe)";
        const LinkObservation observation{LinkEntryKind::SymbolicLink, decodedTarget,
                                          TargetRelation::DifferentFile};

        const RepairItem item =
            classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                        std::filesystem::path(kLinkPath));

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::AreEqual(decodedTarget.native(), item.existingTarget->native());
    }

    // --- Passthrough fields ---

    TEST_METHOD(executableAliasAndLinkPathArePassedThroughUnchanged)
    {
        const LinkObservation observation{LinkEntryKind::None, std::nullopt,
                                          TargetRelation::NotApplicable};
        const PackageExe executable = makeExecutable();

        const RepairItem item = classifyLink(observation, executable, std::wstring(kAlias),
                                             std::filesystem::path(kLinkPath));

        Assert::AreEqual(executable.path.native(), item.executable.path.native());
        Assert::AreEqual(std::wstring(kAlias), item.alias);
        Assert::AreEqual(std::filesystem::path(kLinkPath).native(), item.linkPath.native());
    }

    // --- Invalid observation combinations must fail explicitly ---

    TEST_METHOD(noEntryWithADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::None,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(regularFileWithATargetRelationIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::RegularFile, std::nullopt,
                                          TargetRelation::SameFile};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(otherReparsePointWithADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::OtherReparsePoint,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(symbolicLinkWithoutADecodedTargetIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::SymbolicLink, std::nullopt,
                                          TargetRelation::Missing};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(symbolicLinkLeftAtNotApplicableIsRejected)
    {
        const LinkObservation observation{LinkEntryKind::SymbolicLink,
                                          std::filesystem::path(LR"(C:\somewhere.exe)"),
                                          TargetRelation::NotApplicable};

        Assert::ExpectException<std::invalid_argument>([&] {
            (void)classifyLink(observation, makeExecutable(), std::wstring(kAlias),
                               std::filesystem::path(kLinkPath));
        });
    }

    // --- LinkInspectionError ---

    TEST_METHOD(linkInspectionErrorCarriesOperationPathAndWin32ErrorCode)
    {
        const std::filesystem::path path = LR"(C:\Links\codex.exe)";
        const LinkInspectionError error("DeviceIoControl", path, 5u);

        Assert::AreEqual(std::string("DeviceIoControl"), error.operation());
        Assert::AreEqual(path.native(), error.path().native());
        Assert::AreEqual(5u, error.win32ErrorCode());
        Assert::IsTrue(std::string(error.what()).find("DeviceIoControl") !=
                       std::string::npos);
    }
};

// --- decodeSymbolicLinkTarget(): pure buffer parsing, no filesystem or symlink
// privilege required (issue #45) ---
TEST_CLASS(ReparseTargetDecoderTests)
{
public:
    TEST_METHOD(absoluteDriveTargetIsDecoded)
    {
        const std::vector<std::byte> buffer =
            makeSymlinkBuffer(LR"(\??\C:\Packages\Codex\codex.exe)", 0);

        const auto result =
            decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));

        Assert::IsTrue(result.has_value());
        Assert::AreEqual(std::filesystem::path(LR"(C:\Packages\Codex\codex.exe)").native(),
                         result->native());
    }

    TEST_METHOD(absoluteUncTargetIsDecoded)
    {
        const std::vector<std::byte> buffer =
            makeSymlinkBuffer(LR"(\??\UNC\server\share\codex.exe)", 0);

        const auto result =
            decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));

        Assert::IsTrue(result.has_value());
        Assert::AreEqual(std::filesystem::path(LR"(\\server\share\codex.exe)").native(),
                         result->native());
    }

    TEST_METHOD(relativeTargetResolvesAgainstTheLinkParentNotTheWorkingDirectory)
    {
        constexpr std::uint32_t kSymlinkFlagRelative = 0x1;
        const std::vector<std::byte> buffer =
            makeSymlinkBuffer(LR"(..\OtherPackage\other.exe)", kSymlinkFlagRelative);

        const auto result = decodeSymbolicLinkTarget(
            buffer, std::filesystem::path(LR"(C:\Links\sub\codex.exe)"));

        Assert::IsTrue(result.has_value());
        Assert::AreEqual(std::filesystem::path(LR"(C:\Links\OtherPackage\other.exe)").native(),
                         result->native());
    }

    TEST_METHOD(alreadyExtendedLengthTargetIsNormalized)
    {
        const std::vector<std::byte> buffer =
            makeSymlinkBuffer(LR"(\\?\C:\Packages\Codex\codex.exe)", 0);

        const auto result =
            decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));

        Assert::IsTrue(result.has_value());
        Assert::AreEqual(std::filesystem::path(LR"(C:\Packages\Codex\codex.exe)").native(),
                         result->native());
    }

    TEST_METHOD(nonSymlinkTagIsNotAnErrorAndYieldsNullopt)
    {
        std::vector<std::byte> buffer(8, std::byte{0});
        writeU32(buffer, 0, IO_REPARSE_TAG_MOUNT_POINT);
        writeU16(buffer, 4, 0);

        const auto result =
            decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));

        Assert::IsFalse(result.has_value());
    }

    TEST_METHOD(bufferShorterThanTheCommonHeaderIsRejected)
    {
        const std::vector<std::byte> buffer(4, std::byte{0});

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(reparseDataLengthInconsistentWithBufferSizeIsRejected)
    {
        std::vector<std::byte> buffer = makeSymlinkBuffer(L"C:\\a.exe", 0);
        writeU16(buffer, 4, 0xFFFF); // claims far more data than the buffer actually holds

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(reparseDataLengthTooSmallForFixedFieldsIsRejected)
    {
        std::vector<std::byte> buffer(8 + 4, std::byte{0}); // 4 bytes of "fixed fields"
        writeU32(buffer, 0, IO_REPARSE_TAG_SYMLINK);
        writeU16(buffer, 4, 4);

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(oversizedSubstituteNameLengthIsRejected)
    {
        std::vector<std::byte> buffer = makeSymlinkBuffer(L"C:\\a.exe", 0);
        writeU16(buffer, 10, 0xFFFF); // SubstituteNameLength far past the path buffer

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(oddSubstituteNameLengthIsRejected)
    {
        std::vector<std::byte> buffer = makeSymlinkBuffer(L"C:\\a.exe", 0);
        writeU16(buffer, 10, 15); // odd byte length: not a whole number of UTF-16 units

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }

    TEST_METHOD(oddSubstituteNameOffsetIsRejected)
    {
        std::vector<std::byte> buffer = makeSymlinkBuffer(L"C:\\a.exe", 0);
        writeU16(buffer, 8, 1); // misaligned for a UTF-16 read

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)decodeSymbolicLinkTarget(buffer, std::filesystem::path(kLinkPath));
        });
    }
};

// --- readSymbolicLinkTarget(): the Win32 I/O paths reachable without symlink
// privilege. The privileged paths (a real symbolic link, Ok/Broken/Mismatch) are
// filesystem-backed tests owned by #46. ---
TEST_CLASS(SymbolicLinkTargetReaderTests)
{
public:
    TEST_METHOD(nonexistentPathThrowsWithTheCreateFileWOperation)
    {
        const TempDirectory temp(L"link-inspector-reader");
        const std::filesystem::path missing = temp.path() / L"does-not-exist.exe";

        try
        {
            (void)readSymbolicLinkTarget(missing);
            Assert::Fail(L"Expected a LinkInspectionError");
        }
        catch (const LinkInspectionError& error)
        {
            Assert::AreEqual(std::string("CreateFileW"), error.operation());
        }
    }

    TEST_METHOD(regularFileYieldsNulloptRatherThanAnError)
    {
        const TempDirectory temp(L"link-inspector-reader");
        const std::filesystem::path file = temp.createFile(L"codex.exe");

        const auto result = readSymbolicLinkTarget(file);

        Assert::IsFalse(result.has_value());
    }
};
} // namespace syncwingetlink::tests
