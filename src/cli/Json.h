// SPDX-License-Identifier: MIT

#pragma once

#include "core/Model.h"
#include "core/SymlinkService.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace syncwingetlink::cli
{
// Escapes text as the body of a JSON string (without the surrounding quotes), returning
// UTF-8 bytes. `"`, `\`, and every C0 control character are escaped per RFC 8259 - the
// six named two-character escapes (\", \\, \b, \f, \n, \r, \t) where they apply, and
// \u00XX for every other 0x00-0x1F code unit. Every other code point is emitted as its
// ordinary UTF-8 encoding.
//
// Surrogate policy (explicit, not left to whatever a general-purpose conversion
// function defaults to - docs/adr-phase-5.md ADR-0022): a valid UTF-16 surrogate pair is
// combined and encoded as the one non-BMP code point it represents. An unpaired high or
// low surrogate - possible in a Windows file name, since Windows paths are technically
// WTF-16, not strict UTF-16 - is replaced with U+FFFD (REPLACEMENT CHARACTER) rather
// than silently dropped or left as invalid UTF-8 a JSON parser would reject outright.
[[nodiscard]] std::string escapeJsonString(std::wstring_view text);

// escapeJsonString(text), wrapped in double quotes.
[[nodiscard]] std::string toJsonString(std::wstring_view text);

// Named distinctly from toJsonString(std::wstring_view) rather than overloaded on it:
// std::filesystem::path's converting constructor makes a std::wstring an equally viable
// implicit argument for either overload, which is an unresolvable ambiguity, not merely
// a style preference.
[[nodiscard]] std::string toJsonPathString(const std::filesystem::path& path);

[[nodiscard]] std::string toJsonBool(bool value);

// Renders one RepairItem as a JSON object. Every string field is sanitized via
// cli::sanitizeForDisplay() (Console.h) before escaping - JSON and console output share
// the one sanitization boundary rather than defining a second, possibly divergent, one.
//
//   {
//     "executable": "<path>",
//     "alias": "<name>.exe",
//     "linkPath": "<path>",
//     "status": "Ok" | "Missing" | "Broken" | "Mismatch",
//     "entryKind": "None" | "RegularFile" | "SymbolicLink" | "OtherReparsePoint",
//     "existingTarget": "<path>" | null
//   }
[[nodiscard]] std::string toJson(const RepairItem& item);

// Renders one AliasCollision as a JSON object:
//   { "alias": "<name>.exe", "executables": ["<path>", ...] }
[[nodiscard]] std::string toJson(const AliasCollision& collision);

// Renders the result of one repairLink() call as a JSON object. verifiedItem is the
// post-creation re-inspection, present only when outcome is Created or ReplacedBroken.
//
//   {
//     "item": <RepairItem>,
//     "outcome": "WouldCreate" | "WouldReplaceBroken" | "Created" | "ReplacedBroken" |
//                "SkippedOk" | "RefusedMismatch",
//     "verifiedItem": <RepairItem> | null
//   }
[[nodiscard]] std::string toJson(const SymlinkRepairResult& result);

// Full `scan` output: {"schemaVersion":1,"command":"scan","repairItems":[...],
// "collisions":[...]}. schemaVersion follows rules.json's own precedent (docs/rules.md)
// for a stable, versioned document shape; docs/PLAN.md documents the full schema this
// mirrors. Note that this function only produces the JSON text - ensuring stdout
// carries nothing else when --json is set is dispatch's (#56) responsibility, not this
// module's (docs/adr-phase-5.md's security contract, "the `--json` stream purity"
// rule).
[[nodiscard]] std::string toJsonScanResult(const std::vector<RepairItem>& repairItems,
                                          const std::vector<AliasCollision>& collisions);

// Full `fix` output: {"schemaVersion":1,"command":"fix","results":[...],
// "collisions":[...]}. collisions here are the candidates dispatch excluded from
// repair before running SymlinkService::repairLink() on anything (docs/adr-phase-5.md
// ADR-0021's collision-exclusion note) - they never appear inside `results`.
[[nodiscard]] std::string
toJsonFixResult(const std::vector<SymlinkRepairResult>& results,
                const std::vector<AliasCollision>& collisions);
} // namespace syncwingetlink::cli
