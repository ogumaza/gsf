// SPDX-License-Identifier: MIT

// The players that a game with Nintendo R&D2's driver plays its sound effects on.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "agblib/agb/rom.h"

namespace gsf::rip
{

// A game's table of sound effects: a word for each of the game's sounds. The low halfword is a track of the first sound
// effects' sequence. With bit 27 set, the game plays that track or one of the next two in turn. The game requests the
// track on `player`, or on the player that the word's third byte gives where `player` is kPlayerInEntry.
struct Rd2EffectTable
{
    static constexpr int kPlayerInEntry = -1;

    uint32_t address = 0;
    uint32_t count = 0;
    int player = kPlayerInEntry;
};

// The player that the game requests each of the first `tracks` tracks of the first sound effects' sequence on: that of
// the first entry of `tables` that plays the track, in their order, or -1 for a track that no entry plays.
std::vector<int> Rd2EffectPlayers(const agblib::agb::Rom& rom, std::span<const Rd2EffectTable> tables, int tracks);

} // namespace gsf::rip
