// SPDX-License-Identifier: MIT

// Tests of Nintendo R&D2's driver as gsf rips it: the players of a game's tables of sound effects
// (rip/drivers/rd2.h), and the sound effects of A Link to the Past's profile on agblib's synthetic cartridge of the
// driver, which has none of the game's tables.

#include "rip/drivers/rd2.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/testing/rd2_cart.h"
#include "rip/driver.h"
#include "rip/stub.h"

namespace
{

using gsf::rip::Rd2EffectTable;

constexpr uint32_t kRomBase = 0x08000000;
constexpr uint32_t kTable = 0x100;
constexpr uint32_t kSlots = 0x200;

// The game code in the cartridge's header.
constexpr uint32_t kGameCode = kRomBase + 0xAC;

void TestEffectPlayers()
{
    // A table with the player in each entry, one of whose entries plays its track or the next two in turn, and a table
    // with one player for every entry.
    std::vector<uint8_t> image(0x400);
    xsflib::StoreLe32(&image[kTable], 0x210E0003);      // track 3 on player 14
    xsflib::StoreLe32(&image[kTable + 4], 0x29050005);  // tracks 5 to 7 in turn on player 5
    xsflib::StoreLe32(&image[kTable + 8], 0x21020003);  // track 3 again, on player 2
    xsflib::StoreLe32(&image[kTable + 12], 0x21020040); // a track past the sequence's
    xsflib::StoreLe32(&image[kSlots], 0x21000001);      // track 1 on the table's player
    agblib::agb::Rom rom;
    rom.Assign(image);
    const std::vector<Rd2EffectTable> tables = {{kRomBase + kTable, 4}, {kRomBase + kSlots, 1, 0x10}};

    const std::vector<int> players = gsf::rip::Rd2EffectPlayers(rom, tables, 9);

    XSFLIB_CHECK(players == (std::vector<int>{-1, 0x10, -1, 14, -1, 5, 5, 5, -1}));
}

void TestEffectTableOutsideRom()
{
    // A table that lies outside the cartridge names no player.
    agblib::agb::Rom rom;
    rom.Assign(std::vector<uint8_t>(0x100));
    const std::vector<Rd2EffectTable> tables = {{kRomBase + 0x100, 0x40}};

    const std::vector<int> players = gsf::rip::Rd2EffectPlayers(rom, tables, 2);

    XSFLIB_CHECK(players == (std::vector<int>{-1, -1}));
}

void TestEffectsOfProfile()
{
    // A Link to the Past's game code on the synthetic cartridge: one sequence, then a sound effects' sequence with an
    // empty entry between two tracks.
    using namespace agblib::testing::rd2_cart;
    Cart cart;
    cart.Sequences({{TrackData().Note(60, 12, 100).End()}});
    cart.Effects({{TrackData().Note(60, 12, 100).End(), {}, TrackData().Wait(1).End()}});
    const std::string code = "AZLE";
    for (size_t i = 0; i < code.size(); i++)
    {
        cart.Put8(kGameCode + uint32_t(i), uint8_t(code[i]));
    }

    std::string error;
    const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(cart.ToRom(), error);

    // Song 0 is the music. Songs 1 to 3 are the sound effects, apart from the empty entry. No table names them, so
    // they're unused and come after the music.
    XSFLIB_CHECK(driver != nullptr);
    XSFLIB_CHECK(driver->SongCount() == 4);
    XSFLIB_CHECK(driver->HasSong(0) && !driver->HasEffect(0));
    XSFLIB_CHECK(!driver->HasSong(1) && driver->HasEffect(1));
    XSFLIB_CHECK(!driver->HasEffect(2));
    XSFLIB_CHECK(driver->HasEffect(3));
    const gsf::rip::GameTags* tags = driver->Tags();
    XSFLIB_CHECK(tags != nullptr);
    XSFLIB_CHECK(tags->titles.size() == 4 && tags->titles[0].empty() && tags->titles[1] == "Effect 1 (Unused)");
    XSFLIB_CHECK(tags->order == (std::vector<int>{0, 1, 2, 3}));

    // The music's plan has no request of a sound effect. The plan with the sound effects adds one, before the call that
    // hands the requests over.
    const gsf::rip::StubPlan music = driver->Plan();
    const gsf::rip::StubPlan effects = driver->PlanWithEffects();
    XSFLIB_CHECK(music.start.size() == 5);
    XSFLIB_CHECK(effects.start.size() == 6);
    const gsf::rip::Operation& request = effects.start[4];
    XSFLIB_CHECK(request.routines == (std::vector<uint32_t>{0, 0x0812C73D, 0x0812C73D, 0x0812C73D}));
    XSFLIB_CHECK(request.values.size() == 3 && request.values[2].table == (std::vector<uint32_t>{0, 0, 1, 2}));
    XSFLIB_CHECK(effects.start[3].routines[1] == 0 && music.start[3].routines[1] != 0);
}

} // namespace

int main()
{
    TestEffectPlayers();
    TestEffectTableOutsideRom();
    TestEffectsOfProfile();

    return xsflib::testing::Summary("rd2");
}
