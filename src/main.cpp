// SPDX-License-Identifier: MIT

// Entry point only - no logic beyond wiring argv into cli::run() and returning its exit
// code. All real logic lives in syncwingetlink.core (cli/Dispatch.{h,cpp} and below);
// this file cannot grow beyond that because a C++ MSTest project is a DLL and cannot
// link an executable's object files (docs/adr.md ADR-0002).

#include "cli/Dispatch.h"

#include "core/ComApartment.h"

#include <Windows.h>

#include <cstdio>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

namespace
{
// error.what() follows this codebase's UTF-8 diagnostic-text convention
// (docs/adr-phase-5.md ADR-0021); printing it via "%hs" would instead decode it
// through the CRT's current narrow locale/codepage, which can garble non-ASCII text.
// Decoded explicitly here rather than pulling in cli::Console for this one
// last-resort message.
void printUnexpectedError(std::string_view utf8Message)
{
    int required = 0;
    if (!utf8Message.empty())
    {
        required = ::MultiByteToWideChar(CP_UTF8, 0, utf8Message.data(),
                                         static_cast<int>(utf8Message.size()), nullptr, 0);
    }

    if (required <= 0)
    {
        std::fwprintf(stderr, L"unexpected error (could not decode message)\n");
        return;
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    const int written =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8Message.data(),
                              static_cast<int>(utf8Message.size()), wide.data(), required);
    if (written <= 0)
    {
        std::fwprintf(stderr, L"unexpected error (could not decode message)\n");
        return;
    }

    std::fwprintf(stderr, L"unexpected error: %ls\n", wide.c_str());
}
} // namespace

int wmain(int argc, wchar_t* argv[])
{
    // First statement, before anything else runs: restricts dependent-DLL search to
    // the system directory. Most of this process's imports (kernel32, combase,
    // shell32, ...) are already KnownDLLs, so the practical exposure this closes is
    // small - inexpensive defense-in-depth ahead of the M8 unsigned single-exe
    // release, not a fix for a demonstrated vulnerability (docs/adr-phase-5.md
    // ADR-0024). Its failure is not fatal to the process (the restriction is simply
    // not applied), but is worth reporting since it silently changes this hardening
    // posture.
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        std::fwprintf(stderr,
                      L"warning: could not restrict DLL search directories "
                      L"(GetLastError=%lu)\n",
                      ::GetLastError());
    }

    try
    {
        // Constructed exactly once, before any other core call - RuleSet::parse()
        // (winrt::Windows::Data::Json, ADR-0011) and WingetComSource (which no longer
        // constructs its own, per #56) both depend on the process already having an
        // initialized apartment by the time cli::run() reaches them.
        const syncwingetlink::ComApartment apartment;

        std::vector<std::wstring> args;
        args.reserve(static_cast<std::size_t>(argc > 0 ? argc - 1 : 0));
        for (int i = 1; i < argc; ++i)
        {
            args.emplace_back(argv[i]);
        }

        return syncwingetlink::cli::run(args);
    }
    catch (const std::exception& error)
    {
        // Last-resort net: cli::run() is documented to catch every exception type its
        // own dependencies can throw internally, so reaching here means something
        // escaped that contract - a defensive backstop, not a normal path
        // (docs/adr-phase-5.md ADR-0024). Exit code 3 is the closest documented fit
        // for a condition dispatch did not anticipate closely enough to name.
        printUnexpectedError(error.what());
        return 3;
    }
    catch (...)
    {
        std::fwprintf(stderr, L"unexpected error (not a std::exception)\n");
        return 3;
    }
}
