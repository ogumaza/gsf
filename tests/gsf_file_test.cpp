// SPDX-License-Identifier: MIT

// Tests of GSF's files (gsf/gsf_file.h): a section's program, and a set loaded as a player loads it.

#include "gsf/gsf_file.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "xsflib/psf/psf.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

namespace
{

using gsf::LoadedSet;
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

    Section read;
    XSFLIB_CHECK(!gsf::ParseSection(program, read));
    XSFLIB_CHECK(read.entry == section.entry && read.address == section.address && read.data == section.data);

    // A program shorter than its header, or than the size it gives, is an error.
    XSFLIB_CHECK(gsf::ParseSection(std::vector<uint8_t>(11), read).has_value());
    std::vector<uint8_t> short_data = program;
    short_data.pop_back();
    XSFLIB_CHECK(gsf::ParseSection(short_data, read).has_value());
}

void TestLoadSet()
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

    const std::map<std::string, std::vector<uint8_t>> files = {
        {"set/game.gsflib", gsf::WriteGsf(library, {})},
        {"set/0005 Song 5.minigsf", gsf::WriteGsf(mini, mini_tags)},
    };

    LoadedSet set;
    const auto error = gsf::LoadSet("set/0005 Song 5.minigsf", xsflib::testing::MapReader(files), set);

    XSFLIB_REQUIRE(!error);
    XSFLIB_CHECK(set.entry == gsf::kRomBase);
    XSFLIB_REQUIRE(set.image.size() == 16);
    XSFLIB_CHECK(set.image[7] == 0xAA && set.image[8] == 5 && set.image[9] == 0 && set.image[12] == 0xAA);
    XSFLIB_CHECK(set.tags.at("title") == "Song 5");
    XSFLIB_CHECK(set.reserved.empty());

    // A section at 0x09xxxxxx, as sets of an image of 16 MiB or more had until the song's number moved into the
    // header, lands 16 MiB into the image.
    Section large;
    large.data = std::vector<uint8_t>(0x01000008, 0);
    Section high;
    high.address = 0x09000004;
    high.data = {1, 2, 3, 4};
    xsflib::psf::Tags high_tags;
    high_tags["_lib"] = "large.gsflib";
    const std::map<std::string, std::vector<uint8_t>> large_files = {
        {"large.gsflib", gsf::WriteGsf(large, {})},
        {"high.minigsf", gsf::WriteGsf(high, high_tags)},
    };
    LoadedSet loaded_large;
    XSFLIB_REQUIRE(!gsf::LoadSet("high.minigsf", xsflib::testing::MapReader(large_files), loaded_large));
    XSFLIB_REQUIRE(loaded_large.image.size() == 0x01000008);
    XSFLIB_CHECK(loaded_large.image[0x01000003] == 0 && loaded_large.image[0x01000004] == 1 &&
                 loaded_large.image[0x01000007] == 4);
}

void TestLoadSetKeepsReserved()
{
    // The library and the mini each have a reserved area. The set keeps both, in the order of loading.
    Section library;
    library.data = std::vector<uint8_t>(8, 0x11);
    Section mini;
    mini.address = 0x08000004;
    mini.data = {2, 0, 0, 0};
    xsflib::psf::Tags mini_tags;
    mini_tags["_lib"] = "game.gsflib";

    const std::map<std::string, std::vector<uint8_t>> files = {
        {"game.gsflib", xsflib::psf::WritePsf(gsf::kGsfVersion, {1, 2, 3}, gsf::SectionProgram(library), {})},
        {"song.minigsf", xsflib::psf::WritePsf(gsf::kGsfVersion, {9}, gsf::SectionProgram(mini), mini_tags)},
    };

    LoadedSet set;
    const auto error = gsf::LoadSet("song.minigsf", xsflib::testing::MapReader(files), set);

    XSFLIB_REQUIRE(!error);
    XSFLIB_REQUIRE(set.reserved.size() == 2);
    XSFLIB_CHECK(set.reserved[0] == std::vector<uint8_t>({1, 2, 3}) && set.reserved[1] == std::vector<uint8_t>({9}));
    XSFLIB_CHECK(set.image.size() == 8 && set.image[4] == 2 && set.image[3] == 0x11);

    // gsf writes no reserved area.
    xsflib::psf::PsfFile written;
    XSFLIB_REQUIRE(!xsflib::psf::ParsePsf(gsf::WriteGsf(library, {}), written));
    XSFLIB_CHECK(written.reserved.empty());
}

void TestLoadSetErrors()
{
    // A file of another PSF version, and a section that the image grows to hold.
    Section far;
    far.address = 0x08000100;
    far.data = {7};
    const std::map<std::string, std::vector<uint8_t>> files = {
        {"other.psf", xsflib::psf::WritePsf(0x01, {}, gsf::SectionProgram(far), {})},
        {"far.gsf", gsf::WriteGsf(far, {})},
    };

    LoadedSet set;
    const auto wrong = gsf::LoadSet("other.psf", xsflib::testing::MapReader(files), set);
    const auto grown = gsf::LoadSet("far.gsf", xsflib::testing::MapReader(files), set);

    XSFLIB_CHECK(wrong && wrong->find("not a GSF file") != std::string::npos);
    XSFLIB_CHECK(!grown && set.image.size() == 0x101 && set.image[0x100] == 7 && set.image[0] == 0);
}

void TestRegionBase()
{
    XSFLIB_CHECK(gsf::RegionBase(0x08001234) == gsf::kRomBase);
    XSFLIB_CHECK(gsf::RegionBase(0x02000000) == gsf::kMultibootBase);
}

} // namespace

int main()
{
    TestSection();
    TestLoadSet();
    TestLoadSetKeepsReserved();
    TestLoadSetErrors();
    TestRegionBase();

    return xsflib::testing::Summary("GSF files");
}
