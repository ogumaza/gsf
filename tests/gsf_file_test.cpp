// SPDX-License-Identifier: MIT

// Tests of GSF's files (gsf/gsf_file.h): a section's program, and a set of written files as agblib's Rom loads it.

#include "gsf/gsf_file.h"

#include <cstdint>
#include <string>
#include <vector>

#include "agblib/agb/files.h"
#include "agblib/agb/rom.h"
#include "agblib/testing/agb_cart.h"
#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"

namespace
{

using agblib::agb::Utf8;
using agblib::testing::agb_files::TempPath;
using gsf::Section;

void TestSection()
{
    Section section;
    section.entry = 0x08000000;
    section.address = 0x08000104;
    section.data = {1, 2, 3};

    const std::vector<uint8_t> program = gsf::SectionProgram(section);

    // The start, the address and the size, each a little-endian word, then the data.
    const std::vector<uint8_t> expected = {0x00, 0x00, 0x00, 0x08, 0x04, 0x01, 0x00, 0x08, 3, 0, 0, 0, 1, 2, 3};
    XSFLIB_CHECK(program == expected);
}

void TestWrittenSetLoads()
{
    // A library of 16 bytes and a mini that changes the word at 0x08000008.
    Section library;
    library.data = std::vector<uint8_t>(16, 0xAA);
    Section mini;
    mini.address = 0x08000008;
    mini.data = {5, 0, 0, 0};
    xsflib::psf::Tags mini_tags;
    mini_tags["_lib"] = "game.gsflib";
    mini_tags["title"] = "Song 5";
    std::string error;
    XSFLIB_REQUIRE(agblib::agb::WriteFile(Utf8(TempPath("game.gsflib")), gsf::WriteGsf(library, {}), error));
    XSFLIB_REQUIRE(
        agblib::agb::WriteFile(Utf8(TempPath("0005 Song 5.minigsf")), gsf::WriteGsf(mini, mini_tags), error));
    agblib::agb::Rom rom;

    const bool loaded = rom.Load(Utf8(TempPath("0005 Song 5.minigsf")), error);

    XSFLIB_REQUIRE(loaded);
    XSFLIB_CHECK(rom.FromGsf() && rom.GsfLibrary() == Utf8(TempPath("game.gsflib")));
    XSFLIB_CHECK(rom.GsfEntry() == gsf::kRomBase);
    XSFLIB_CHECK(rom.GsfReserved().empty());
    XSFLIB_REQUIRE(rom.Size() == 16);
    XSFLIB_CHECK(rom.U8(0x08000007) == 0xAA && rom.U32(0x08000008) == 5 && rom.U8(0x0800000C) == 0xAA);
}

void TestWriteGsfHasNoReserved()
{
    Section section;
    section.data = std::vector<uint8_t>(8, 0x11);

    xsflib::psf::PsfFile written;
    XSFLIB_REQUIRE(!xsflib::psf::ParsePsf(gsf::WriteGsf(section, {}), written));

    // gsf writes no reserved area.
    XSFLIB_CHECK(written.version == gsf::kGsfVersion);
    XSFLIB_CHECK(written.reserved.empty());
    XSFLIB_CHECK(written.program == gsf::SectionProgram(section));
}

} // namespace

int main()
{
    TestSection();
    TestWrittenSetLoads();
    TestWriteGsfHasNoReserved();

    return xsflib::testing::Summary("GSF files");
}
