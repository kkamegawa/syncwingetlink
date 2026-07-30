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
#include <vector>

int wmain(int argc, wchar_t* argv[])
{
    // First statement, before anything else runs: restricts dependent-DLL search to
    // the system directory. Most of this process's imports (kernel32, combase,
    // shell32, ...) are already KnownDLLs, so the practical exposure this closes is
    // small - inexpensive defense-in-depth ahead of the M8 unsigned single-exe
    // release, not a fix for a demonstrated vulnerability (docs/adr-phase-5.md
    // ADR-0024).
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);

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
        std::fwprintf(stderr, L"unexpected error: %hs\n", error.what());
        return 3;
    }
    catch (...)
    {
        std::fwprintf(stderr, L"unexpected error (not a std::exception)\n");
        return 3;
    }
}
