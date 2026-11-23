// SPDX-License-Identifier: MIT

// GSF, the PSF format of Game Boy Advance music, and the writer of its files. A GSF file is a PSF file of version 0x22.
// The format sets the reserved area aside for a save state. No version of the format defines the layout of that state.
// gsf writes no reserved area. The program is a section: the address at which the CPU starts, the address that the data
// goes to, the size of the data and the data. The library of a set contains the cartridge image of the game, cut down
// to what the music reads, with a stub that starts a song. Each mini contains the bytes that select its song. A player
// loads the files of a set in the order of the PSF format. The section of each file goes over the image that the
// earlier files made. The player then starts the CPU. agblib's agb::Rom loads a set this way.

#pragma once

#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "xsflib/psf/psf.h"

namespace gsf
{

// The PSF version of GSF.
constexpr uint8_t kGsfVersion = 0x22;

using agblib::agb::kRomBase;

// The largest cartridge: 32 MiB.
using agblib::agb::kMaxRomSize;

// The bytes of a section's header: the start, the address and the size.
constexpr uint32_t kSectionHeaderSize = 12;

// A section of a GSF file's program.
struct Section
{
    uint32_t entry = kRomBase;   // where the CPU starts
    uint32_t address = kRomBase; // where the data goes
    std::vector<uint8_t> data;
};

// Returns the program that contains `section`.
std::vector<uint8_t> SectionProgram(const Section& section);

// Writes a GSF file of `section` with `tags`.
std::vector<uint8_t> WriteGsf(const Section& section, const xsflib::psf::Tags& tags);

} // namespace gsf
