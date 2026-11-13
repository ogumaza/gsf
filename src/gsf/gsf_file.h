// SPDX-License-Identifier: MIT

// GSF, the PSF format of the Game Boy Advance's music. A GSF file is a PSF file of version 0x22. The format keeps the
// reserved area for a save state, whose layout no version of the format defines. gsf writes no reserved area, and a
// load keeps any that a file has. The program is a section: the address at which the CPU starts, the address that the
// data goes to, the data's size and the data. A set's library holds the game's cartridge image, cut down to what the
// music reads, with a stub that starts a song. Each mini holds the bytes that pick its song. A player loads a set's
// files in the order of the PSF format, each one's section over the image that the files before it made, and starts
// the CPU.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"

namespace gsf
{

// The PSF version of GSF.
constexpr uint8_t kGsfVersion = 0x22;

// Where the cartridge and the work memory start. A set whose CPU starts in the work memory is a multiboot program.
constexpr uint32_t kRomBase = 0x08000000;
constexpr uint32_t kMultibootBase = 0x02000000;

// The largest cartridge: 32 MiB. A section's address is taken within that span of its region.
constexpr uint32_t kMaxImageSize = 0x02000000;

// The bytes of a section's header: the start, the address and the size.
constexpr uint32_t kSectionHeaderSize = 12;

// A section of a GSF file's program.
struct Section
{
    uint32_t entry = kRomBase;   // where the CPU starts
    uint32_t address = kRomBase; // where the data goes
    std::vector<uint8_t> data;
};

// Returns the program that holds `section`.
std::vector<uint8_t> SectionProgram(const Section& section);

// Reads the section in `program`. Returns the error, or nothing.
std::optional<std::string> ParseSection(const std::vector<uint8_t>& program, Section& section);

// Writes a GSF file of `section` with `tags`.
std::vector<uint8_t> WriteGsf(const Section& section, const xsflib::psf::Tags& tags);

// A set as a player loads it: where the CPU starts, the image that the sections make from the start of the region that
// the start is in, the tags of the file that was loaded, and the reserved area of each file that has one, in the order
// of loading.
struct LoadedSet
{
    uint32_t entry = kRomBase;
    std::vector<uint8_t> image;
    xsflib::psf::Tags tags;
    std::vector<std::vector<uint8_t>> reserved;
};

// Returns the start of the region that holds `address`: the cartridge's or the work memory's.
uint32_t RegionBase(uint32_t address);

// Loads the GSF file at `path` with the files that its _lib tags name. The first file loaded sets where the CPU starts.
// Each section goes into the image at its address within the region, and the image grows to hold it. A file's reserved
// area goes into `reserved`. Returns the error, or nothing.
std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, LoadedSet& out);

} // namespace gsf
