// SPDX-License-Identifier: MIT

#pragma once

namespace syncwingetlink
{
// RAII wrapper around CoInitializeEx(COINIT_MULTITHREADED). Tolerates
// RPC_E_CHANGED_MODE (the calling thread already initialized a different concurrency
// model) rather than throwing, since a static library must not assume it owns the
// process's apartment - a host process or a test runner may already have one.
// winrt::init_apartment() was deliberately not used here: it throws on exactly the
// RPC_E_CHANGED_MODE condition this type needs to tolerate.
//
// Apartment state is a process-wide concern, not something scoped to a single
// IPackageSource. main.cpp constructs the single process-wide ComApartment before any
// other core call (docs/adr-phase-5.md ADR-0024); WingetComSource no longer constructs
// its own (removed in #56, per the forward note this file and docs/adr-phase-2.md
// ADR-0009 carried since M2 - WingetComSource owned one only because main.cpp did not
// exist yet). rules/RuleSet.cpp's parse() still constructs its own independently: its
// need for an initialized apartment (winrt::Windows::Data::Json, ADR-0011) applies
// even to a --source fs or test-rule invocation that never touches WingetComSource at
// all, so it is not solely a stand-in for main.cpp's process-wide one.
class ComApartment
{
public:
    ComApartment();
    ~ComApartment();

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

private:
    bool m_owned;
};
} // namespace syncwingetlink
