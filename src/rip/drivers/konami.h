// SPDX-License-Identifier: MIT

// The profiles of the games with Konami's driver that gsf rips, the plan of the stub for a profile, and the bytes of
// the samples that a rip keeps whole.

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/konami/driver.h"

#include "rip/driver.h"
#include "rip/stub.h"

namespace gsf::rip
{

// The routines of a release, all Thumb code except a mixer in IWRAM, and the state that the game sets before its first
// song.
struct KonamiProfile
{
    std::string_view code = {};
    uint8_t version = 0;
    uint32_t song_table = 0;                 // the song table, as detection finds it
    uint32_t init = 0;                       // the driver's init
    std::vector<uint32_t> init_args = {};    // the arguments of the init as the game passes them
    uint32_t mono = 0;                       // if not 0, the switch to mono output: the game calls it with 0
    uint32_t rate = 0;                       // if not 0, the rate setter: the game calls it with rate_setting
    uint32_t rate_setting = 0;               // the argument of the rate setter: an entry of the timer table
    uint32_t request = 0;                    // the driver's request of a song: r0 = the song, then request_args
    std::vector<uint32_t> request_args = {}; // the request's other arguments
    uint32_t start = 0;   // the routine that the main loop calls each frame: it starts a requested song
    uint32_t frame = 0;   // the per-frame entry: the handler of a display interrupt in the game calls it
    uint32_t dma1 = 0;    // the routine that the DMA 1 interrupt runs: the mixer, or a routine that calls it
    uint16_t waitcnt = 0; // WAITCNT as the game sets it
};

// The profile of the game with the game code `code` and the software version `version`, whose song table is at
// `song_table`, or null for a game that no profile covers.
const KonamiProfile* FindKonamiProfile(std::string_view code, uint8_t version, uint32_t song_table);

// The CPU cycles of one block of the mixer: from the timer table of the driver for a profile that calls the rate
// setter, or else from the rate of the mixer.
double KonamiBlockCycles(const agblib::agb::Rom& rom, const agblib::konami::DriverInfo& info,
                         const KonamiProfile& profile);

// The plan of the stub for `profile`: the set-up of the driver in the game, the request of the song, and the per-frame
// entry and the start in the main loop. The DMA 1 routine runs once a block of the mixer, every `block_cycles` CPU
// cycles.
StubPlan KonamiPlan(const KonamiProfile& profile, double block_cycles);

// The bytes of every sample that the songs can play, to keep whole: the header and the points of each sample, from the
// sample table and the table of the live stage.
std::vector<RomSpan> KonamiSampleSpans(const agblib::agb::Rom& rom, const agblib::konami::DriverInfo& info);

} // namespace gsf::rip
