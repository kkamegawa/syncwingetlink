// SPDX-License-Identifier: MIT

#include <CppUnitTest.h>

#include <core/LinkInspector.h>

#include "TempDirectory.h"

#include <Windows.h>

#include <cstring>
#include <filesystem>
#include <fstream>
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

// --- inspectLink(): the production, filesystem-backed adapter (issue #46).
//
// The branches reachable without symlink privilege (absence, a regular file, a
// directory, another reparse point) are covered unconditionally. The branches that need
// a real IO_REPARSE_TAG_SYMLINK entry (Ok/Broken/Mismatch-different-file, and the
// executable-disappears scenario) attempt real symlink creation and report
// Assert::Inconclusive() if it fails - this environment has neither Developer Mode nor
// elevation, so SeCreateSymbolicLinkPrivilege is unavailable even with the
// unprivileged-create flag, but a privileged CI/dev environment running this same suite
// gains full coverage automatically. See docs/adr-phase-3.md ADR-0016.
TEST_CLASS(InspectLinkTests)
{
public:
    TEST_METHOD(absentEntryIsMissing)
    {
        const TempDirectory temp(L"inspect-link-absent");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item =
            inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Missing);
        Assert::IsTrue(item.entryKind == LinkEntryKind::None);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(regularFileIsMismatch)
    {
        const TempDirectory temp(L"inspect-link-file");
        const std::filesystem::path linkPath = temp.createFile(L"codex.exe");
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item =
            inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::RegularFile);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(directoryIsMismatch)
    {
        const TempDirectory temp(L"inspect-link-dir");
        const std::filesystem::path linkPath = temp.createDirectory(L"codex.exe");
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item =
            inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::RegularFile);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(nonSymlinkReparsePointIsMismatch)
    {
        const TempDirectory temp(L"inspect-link-junction");
        const std::filesystem::path targetDir = temp.createDirectory(L"target-dir");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        Assert::IsTrue(createJunction(targetDir, linkPath));
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item =
            inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::OtherReparsePoint);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(fieldsArePassedThroughUnchanged)
    {
        const TempDirectory temp(L"inspect-link-passthrough");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");
        const std::filesystem::path executablePath = executable.path;

        const RepairItem item =
            inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::AreEqual(executablePath.native(), item.executable.path.native());
        Assert::AreEqual(std::wstring(kAlias), item.alias);
        Assert::AreEqual(linkPath.native(), item.linkPath.native());
    }

    TEST_METHOD(directorySymbolicLinkIsMismatchEvenWithAMissingTarget)
    {
        const TempDirectory temp(L"inspect-link-dir-symlink");
        // A missing target is the dangerous shape: decoding it as a symbolic link
        // would classify Broken, and repairing Broken deletes with DeleteFileW - which
        // cannot remove a directory entry.
        const std::filesystem::path missingTarget = temp.path() / L"gone-dir";
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createDirectorySymlink(missingTarget, linkPath))
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item = inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::OtherReparsePoint);
        Assert::IsFalse(item.existingTarget.has_value());
    }

    TEST_METHOD(healthySymbolicLinkIsOk)
    {
        const TempDirectory temp(L"inspect-link-ok");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createFileSymlink(executablePath, linkPath))
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = executablePath;

        const RepairItem item = inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Ok);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkToAMissingTargetIsBroken)
    {
        const TempDirectory temp(L"inspect-link-broken");
        const std::filesystem::path missingTarget = temp.path() / L"does-not-exist.exe";
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createFileSymlink(missingTarget, linkPath))
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item = inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Broken);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
    }

    TEST_METHOD(symbolicLinkToADifferentExistingFileIsMismatch)
    {
        const TempDirectory temp(L"inspect-link-mismatch");
        const std::filesystem::path otherFile = temp.createFile(L"other.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createFileSymlink(otherFile, linkPath))
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item = inspectLink(executable, std::wstring(kAlias), linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
    }

    TEST_METHOD(expectedExecutableDisappearingDuringInspectionIsAnError)
    {
        const TempDirectory temp(L"inspect-link-disappear");
        // The symlink's real target stays present throughout - only the *expected*
        // executable, a separate file, disappears before inspectLink() runs. That is
        // what distinguishes this from symbolicLinkToAMissingTargetIsBroken above: the
        // decoded target itself resolves fine, so inspectLink() must reach the identity
        // comparison and fail trying to open executable, not report Broken.
        const std::filesystem::path otherExistingFile = temp.createFile(L"other.exe");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createFileSymlink(otherExistingFile, linkPath))
        {
            // CppUnitTestFramework (native C++ MSTest) has no Assert::Inconclusive -
            // logging and returning is the closest equivalent: the test stays green
            // here and exercises this branch for real wherever symlink privilege is
            // available.
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        std::filesystem::remove(executablePath);

        PackageExe executable;
        executable.path = executablePath;

        Assert::ExpectException<LinkInspectionError>([&] {
            (void)inspectLink(executable, std::wstring(kAlias), linkPath);
        });
    }
};

// #62: non-ASCII target comparison through the real, filesystem-backed inspectLink().
// The fixture alias combines Japanese katakana (no case distinction) with U+1F600 (a
// non-BMP character also with no case distinction, sidestepping any mismatch between
// this codebase's CompareStringOrdinal-based comparisons and NTFS's own $UpCase table -
// see SmokeTests.cpp's matching round-trip tests for the full reasoning). Written with
// \uXXXX/\UXXXXXXXX escapes only; no raw non-ASCII bytes in this file.
TEST_CLASS(NonAsciiInspectLinkTests)
{
public:
    TEST_METHOD(nonAsciiRegularFileIsMismatch)
    {
        // No symlink privilege needed - runs unconditionally, unlike the Ok/Broken
        // cases below.
        const TempDirectory temp(L"inspect-link-non-ascii-file");
        const std::wstring alias = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600.exe";
        const std::filesystem::path linkPath = temp.createFile(alias);
        PackageExe executable;
        executable.path = temp.createFile(L"codex-real.exe");

        const RepairItem item = inspectLink(executable, alias, linkPath);

        Assert::IsTrue(item.status == LinkStatus::Mismatch);
        Assert::IsTrue(item.entryKind == LinkEntryKind::RegularFile);
    }

    TEST_METHOD(nonAsciiHealthySymbolicLinkIsOk)
    {
        const TempDirectory temp(L"inspect-link-non-ascii-ok");
        const std::wstring alias = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600.exe";
        const std::wstring stem = alias.substr(0, alias.size() - 4); // strip ".exe"
        const std::filesystem::path executablePath = temp.createFile(stem + L"-real.exe");
        const std::filesystem::path linkPath = temp.path() / alias;
        if (!createFileSymlink(executablePath, linkPath))
        {
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = executablePath;

        const RepairItem item = inspectLink(executable, alias, linkPath);

        Assert::IsTrue(item.status == LinkStatus::Ok);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::IsTrue(item.existingTarget->native() == executablePath.native());
    }

    TEST_METHOD(nonAsciiBrokenSymbolicLinkIsBroken)
    {
        const TempDirectory temp(L"inspect-link-non-ascii-broken");
        const std::wstring alias = L"\u30C6\u30B9\u30C8\u30C4\u30FC\u30EB\U0001F600.exe";
        const std::wstring stem = alias.substr(0, alias.size() - 4); // strip ".exe"
        const std::filesystem::path staleTarget = temp.path() / (stem + L"-old.exe");
        const std::filesystem::path linkPath = temp.path() / alias;
        if (!createFileSymlink(staleTarget, linkPath))
        {
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }
        PackageExe executable;
        executable.path = temp.createFile(stem + L"-real.exe");

        const RepairItem item = inspectLink(executable, alias, linkPath);

        Assert::IsTrue(item.status == LinkStatus::Broken);
        Assert::IsTrue(item.entryKind == LinkEntryKind::SymbolicLink);
        Assert::IsTrue(item.existingTarget.has_value());
        Assert::IsTrue(item.existingTarget->native() == staleTarget.native());
    }
};

namespace
{
[[nodiscard]] RepairItem makeCollisionCandidate(std::wstring alias,
                                                const std::filesystem::path& executablePath)
{
    RepairItem item;
    item.alias = std::move(alias);
    item.executable.path = executablePath;
    item.linkPath = std::filesystem::path(LR"(C:\Links\)") / item.alias;
    item.status = LinkStatus::Ok;
    return item;
}
} // namespace

// --- detectAliasCollisions(): pure, no filesystem or symlink privilege required
// (issue #47). A colliding alias is reported separately from LinkStatus specifically so
// M6/M7 can warn and require an explicit choice instead of ever sending it into an
// automatic repair path - see AliasCollision's own documentation in core/Model.h. ---
TEST_CLASS(AliasCollisionTests)
{
public:
    TEST_METHOD(twoDistinctExecutablesForOneAliasCollide)
    {
        const std::vector<RepairItem> items{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\B\codex.exe)"),
        };

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(1), collisions.size());
        Assert::AreEqual(std::wstring(L"codex.exe"), collisions[0].alias);
        Assert::AreEqual(std::size_t(2), collisions[0].executables.size());
    }

    TEST_METHOD(threeDistinctExecutablesForOneAliasCollide)
    {
        const std::vector<RepairItem> items{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\B\codex.exe)"),
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\C\codex.exe)"),
        };

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(1), collisions.size());
        Assert::AreEqual(std::size_t(3), collisions[0].executables.size());
    }

    TEST_METHOD(aliasesDifferingOnlyByCaseCollide)
    {
        const std::vector<RepairItem> items{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"CODEX.EXE", LR"(C:\Packages\B\codex.exe)"),
        };

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(1), collisions.size());
        Assert::AreEqual(std::size_t(2), collisions[0].executables.size());
    }

    TEST_METHOD(distinctAliasesDoNotCollide)
    {
        const std::vector<RepairItem> items{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"other.exe", LR"(C:\Packages\B\other.exe)"),
        };

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(0), collisions.size());
    }

    TEST_METHOD(repeatedCopiesOfOneExecutableDoNotCollide)
    {
        const std::vector<RepairItem> items{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"CODEX.exe", LR"(C:\Packages\A\CODEX.exe)"),
        };

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(0), collisions.size());
    }

    TEST_METHOD(noItemsYieldsNoCollisions)
    {
        const std::vector<RepairItem> items;

        Assert::AreEqual(std::size_t(0), detectAliasCollisions(items).size());
    }

    TEST_METHOD(outputIsStableRegardlessOfInputOrder)
    {
        const std::vector<RepairItem> forward{
            makeCollisionCandidate(L"aa.exe", LR"(C:\Packages\A\aa.exe)"),
            makeCollisionCandidate(L"aa.exe", LR"(C:\Packages\B\aa.exe)"),
            makeCollisionCandidate(L"zz.exe", LR"(C:\Packages\C\zz.exe)"),
            makeCollisionCandidate(L"zz.exe", LR"(C:\Packages\D\zz.exe)"),
        };
        std::vector<RepairItem> reversed(forward.rbegin(), forward.rend());

        const std::vector<AliasCollision> forwardResult = detectAliasCollisions(forward);
        const std::vector<AliasCollision> reversedResult = detectAliasCollisions(reversed);

        Assert::AreEqual(std::size_t(2), forwardResult.size());
        Assert::AreEqual(forwardResult.size(), reversedResult.size());
        for (std::size_t i = 0; i < forwardResult.size(); ++i)
        {
            Assert::AreEqual(forwardResult[i].alias, reversedResult[i].alias);
            Assert::AreEqual(forwardResult[i].executables.size(),
                             reversedResult[i].executables.size());
            for (std::size_t j = 0; j < forwardResult[i].executables.size(); ++j)
            {
                Assert::AreEqual(forwardResult[i].executables[j].path.native(),
                                 reversedResult[i].executables[j].path.native());
            }
        }
        // Alphabetical by alias, independent of the (reverse-alphabetical) input order.
        Assert::AreEqual(std::wstring(L"aa.exe"), forwardResult[0].alias);
        Assert::AreEqual(std::wstring(L"zz.exe"), forwardResult[1].alias);
    }

    TEST_METHOD(representativeAliasCasingIsStableRegardlessOfInputOrder)
    {
        // A collision caused entirely by a case difference: lowercase-first and
        // uppercase-first orderings must still agree on which casing is reported,
        // since std::sort is not stable and the two spellings compare equal under
        // ordinal case-insensitive comparison alone.
        const std::vector<RepairItem> lowercaseFirst{
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
            makeCollisionCandidate(L"CODEX.EXE", LR"(C:\Packages\B\codex.exe)"),
        };
        const std::vector<RepairItem> uppercaseFirst{
            makeCollisionCandidate(L"CODEX.EXE", LR"(C:\Packages\B\codex.exe)"),
            makeCollisionCandidate(L"codex.exe", LR"(C:\Packages\A\codex.exe)"),
        };

        const std::vector<AliasCollision> lowercaseFirstResult =
            detectAliasCollisions(lowercaseFirst);
        const std::vector<AliasCollision> uppercaseFirstResult =
            detectAliasCollisions(uppercaseFirst);

        Assert::AreEqual(std::size_t(1), lowercaseFirstResult.size());
        Assert::AreEqual(std::size_t(1), uppercaseFirstResult.size());
        Assert::AreEqual(lowercaseFirstResult[0].alias, uppercaseFirstResult[0].alias);
    }
};

// End-to-end coverage tying inspectLink() and detectAliasCollisions() together over a
// simulated multi-package scan - each is unit tested on its own (InspectLinkTests,
// AliasCollisionTests), but nothing else exercises the full M4 pipeline a real `scan`
// performs: probe every package's link, then look for collisions across the results.
// This is the "cross-component regression matrix" called for in the M4 plan (issue #48).
TEST_CLASS(LinkInspectionRegressionTests)
{
public:
    TEST_METHOD(aSimulatedScanClassifiesEveryStatusAndFindsExactlyOneCollision)
    {
        const TempDirectory temp(L"link-inspection-regression");
        const std::filesystem::path linksDir = temp.createDirectory(L"Links");

        // Package A: no link at all.
        const std::filesystem::path aExe = temp.createFile(L"PackageA\\a-real.exe");
        const std::filesystem::path aLink = linksDir / L"a.exe";

        // Package B: the link exists but is a plain file, not a symbolic link.
        const std::filesystem::path bExe = temp.createFile(L"PackageB\\b-real.exe");
        const std::filesystem::path bLink = temp.createFile(L"Links\\b.exe");

        // Packages C and D: two different executables that both resolved to the same
        // alias - a collision - and neither has a link created yet.
        const std::filesystem::path cExe = temp.createFile(L"PackageC\\shared-real.exe");
        const std::filesystem::path dExe = temp.createFile(L"PackageD\\shared-real.exe");
        const std::filesystem::path sharedLink = linksDir / L"shared.exe";

        PackageExe packageA;
        packageA.path = aExe;
        PackageExe packageB;
        packageB.path = bExe;
        PackageExe packageC;
        packageC.path = cExe;
        PackageExe packageD;
        packageD.path = dExe;

        std::vector<RepairItem> items;
        items.push_back(inspectLink(packageA, L"a.exe", aLink));
        items.push_back(inspectLink(packageB, L"b.exe", bLink));
        items.push_back(inspectLink(packageC, L"shared.exe", sharedLink));
        items.push_back(inspectLink(packageD, L"shared.exe", sharedLink));

        Assert::IsTrue(items[0].status == LinkStatus::Missing);
        Assert::IsTrue(items[1].status == LinkStatus::Mismatch);
        Assert::IsTrue(items[1].entryKind == LinkEntryKind::RegularFile);
        // C and D each observe the same (currently absent) shared link independently -
        // that is a Missing status for both, exactly like A. The collision is a
        // property of the alias resolution that produced two RepairItems for one
        // alias, not of either individual RepairItem's own status.
        Assert::IsTrue(items[2].status == LinkStatus::Missing);
        Assert::IsTrue(items[3].status == LinkStatus::Missing);

        const std::vector<AliasCollision> collisions = detectAliasCollisions(items);

        Assert::AreEqual(std::size_t(1), collisions.size());
        Assert::AreEqual(std::wstring(L"shared.exe"), collisions[0].alias);
        Assert::AreEqual(std::size_t(2), collisions[0].executables.size());

        // M4 is read-only: none of the above may have created, deleted, or modified
        // anything under Links.
        Assert::IsFalse(std::filesystem::exists(aLink));
        Assert::IsTrue(std::filesystem::exists(bLink));
        Assert::IsFalse(std::filesystem::exists(sharedLink));
        Assert::AreEqual(std::string("test"), [&] {
            std::ifstream stream(bLink, std::ios::binary);
            std::string content;
            stream >> content;
            return content;
        }());
    }

    TEST_METHOD(aHealthySymbolicLinkIsNeverMutatedByInspection)
    {
        const TempDirectory temp(L"link-inspection-regression-ok");
        const std::filesystem::path executablePath = temp.createFile(L"codex-real.exe");
        const std::filesystem::path linkPath = temp.path() / L"codex.exe";
        if (!createFileSymlink(executablePath, linkPath))
        {
            Logger::WriteMessage(
                L"Skipped: symbolic link creation needs Developer Mode or elevation, "
                L"neither of which is available here.\n");
            return;
        }

        PackageExe executable;
        executable.path = executablePath;
        const RepairItem before =
            inspectLink(executable, L"codex.exe", linkPath);
        Assert::IsTrue(before.status == LinkStatus::Ok);

        // Inspecting an already-Ok link a second time must be side-effect-free: the
        // same link, read again, still resolves to the same executable.
        const RepairItem after = inspectLink(executable, L"codex.exe", linkPath);

        Assert::IsTrue(after.status == LinkStatus::Ok);
        Assert::IsTrue(std::filesystem::exists(linkPath));
        Assert::IsTrue(after.existingTarget.has_value());
        Assert::AreEqual(before.existingTarget->native(), after.existingTarget->native());
    }
};
} // namespace syncwingetlink::tests
