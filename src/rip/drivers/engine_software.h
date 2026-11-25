// SPDX-License-Identifier: MIT

// The stub's operations for the steps of the game's code that agblib gives for Engine Software's driver: the game's
// set-up, the same for every song, and each song's start.

#pragma once

#include <span>
#include <vector>

#include "agblib/engine_software/driver.h"

#include "rip/stub.h"

namespace gsf::rip
{

// The stub's operations for `steps`: a call for each call, and a store for each store.
std::vector<Operation> StepOperations(std::span<const agblib::engine_software::GameStep> steps);

// The stub's operations for each song's start, `starts` by song: at each place in the steps, a call of each song's
// routine from a table, with each argument from a table. A song with fewer steps skips the call at the places past its
// last. Throws std::logic_error for a store, or for a call of more than four arguments.
std::vector<Operation> SongStartOperations(const std::vector<std::vector<agblib::engine_software::GameStep>>& starts);

} // namespace gsf::rip
