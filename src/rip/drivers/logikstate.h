// SPDX-License-Identifier: MIT

// The profiles of the games with Logik State's driver that gsf rips: the routines that each game calls besides the play
// routine that agblib finds, and the memory that it gives the init.

#pragma once

#include <cstdint>
#include <span>
#include <string_view>

namespace gsf::rip
{

struct GameTags;

// A song that starts a module at another order than the game's: module `song` of agblib's songs, from `order`. Such a
// song comes after agblib's songs.
struct LogikStateOrder
{
    int song = 0;
    int order = 0;
};

// A song that ends with a loop of silence, where agblib's length analysis gives the end of the loop's first pass: the
// end of its last sound, measured on the console or an emulator, in milliseconds from the song's start. The length tag
// gives it. The coverage still runs for the analysis's length.
struct LogikStateEnd
{
    int song = 0;
    int ms = 0;
};

// A game's calls of the driver. All of the routines are Thumb code. A profile is for one release: the game code and
// the software version in the cartridge's header, and the play routine's address.
struct LogikStateProfile
{
    std::string_view code = {};
    uint8_t version = 0;
    uint32_t play = 0;
    uint32_t init = 0;
    uint32_t memory = 0;
    uint32_t frame = 0;
    bool frame_in_handler = false; // the handler of the vertical blank calls the frame routine, not the main loop
    uint32_t sound_on = 0;
    uint32_t master_volume = 0; // a routine that the game calls with the full volume after the play routine, or 0
    uint32_t music_volume = 0;  // the music's volume routine, where agblib doesn't find it
    uint32_t heap = 0;          // the word where the game's allocator keeps its next address, or 0
    uint32_t heap_start = 0;    // the address there when the game calls the init
    uint32_t slide_scale = 0;   // GBAMOD1: the variable for the scale of a slide to a note
    uint32_t music_level = 0;   // a word that the play routine reads the music's volume from, or 0
    const GameTags* tags = nullptr;
    std::span<const LogikStateOrder> orders = {}; // the songs at other orders
    std::span<const LogikStateEnd> ends = {};
};

// The profile of the game with the game code `code` and the software version `version`, whose play routine is at
// `play_routine`, or null for a game that gsf doesn't know.
const LogikStateProfile* FindLogikStateProfile(std::string_view code, uint8_t version, uint32_t play_routine);

} // namespace gsf::rip
