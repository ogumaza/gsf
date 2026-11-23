// SPDX-License-Identifier: MIT

// The cartridge image that a rip starts from: its loading, and the sound drivers that gsf finds in it. Every tool that
// finds a game's drivers loads the image through these functions.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"

#include "rip/driver.h"

namespace gsf::rip
{

// Loads the cartridge image at `path` (UTF-8) into `rom` and finds its sound drivers (FindDrivers()). `rom` has to
// outlive the result. Returns none and sets `error` when the image can't be loaded, when it's a GSF set, or when it has
// no driver that gsf rips.
std::vector<FoundDriver> LoadCartridge(const std::string& path, agblib::agb::Rom& rom, std::string& error);

// The bytes of the cartridge in `rom`, from kRomBase.
std::vector<uint8_t> CartridgeImage(const agblib::agb::Rom& rom);

} // namespace gsf::rip
