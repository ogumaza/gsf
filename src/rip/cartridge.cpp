// SPDX-License-Identifier: MIT

#include "rip/cartridge.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "xsflib/files.h"

#include "rip/driver.h"

namespace gsf::rip
{

std::vector<FoundDriver> LoadCartridge(const std::string& path, agblib::agb::Rom& rom, std::string& error)
{
    if (!rom.Load(xsflib::Utf8(std::filesystem::absolute(xsflib::PathFromUtf8(path))), error))
    {
        return {};
    }

    if (rom.FromGsf())
    {
        error = "a GSF set, not a cartridge image";
        return {};
    }

    return FindDrivers(rom, error);
}

std::vector<uint8_t> CartridgeImage(const agblib::agb::Rom& rom)
{
    return std::vector<uint8_t>(rom.Ptr(agblib::agb::kRomBase), rom.Ptr(agblib::agb::kRomBase) + rom.Size());
}

} // namespace gsf::rip
