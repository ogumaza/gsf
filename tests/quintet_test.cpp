// SPDX-License-Identifier: MIT

// Tests of Quintet's driver as gsf rips it, on agblib's synthetic cartridge of the driver with a Super Robot Taisen
// game's code in its header: the songs, the sound effects, and the stubs that start them.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "agblib/agb/rom.h"
#include "agblib/testing/quintet_cart.h"
#include "rip/driver.h"
#include "rip/stub.h"

namespace
{

namespace cart = agblib::testing::quintet_cart;

// The game code in the cartridge's header.
constexpr uint32_t kGameCode = cart::kRomBase + 0xAC;

// Super Robot Taisen A's play routine and its routine that plays a sound effect, both Thumb code.
constexpr uint32_t kPlay = 0x08003551;
constexpr uint32_t kPlayEffect = 0x08003595;

// A cartridge of the S revision with two songs and two sound effects, and `code` as its game code.
agblib::agb::Rom MakeRom(const std::string& code)
{
    cart::Cart image(agblib::quintet::Revision::kS);
    image.Songs(
        {cart::Song({cart::Channel().Bytes({0x00, 4}).End()}), cart::Song({cart::Channel().Bytes({0x00, 8}).End()})});
    image.Effects(
        {cart::Song({cart::Channel().Bytes({0x00, 4}).End()}), cart::Song({cart::Channel().Bytes({0x00, 2}).End()})});
    for (size_t i = 0; i < code.size(); i++)
    {
        image.Put8(kGameCode + uint32_t(i), uint8_t(code[i]));
    }

    return image.ToRom();
}

void TestEffects()
{
    // Songs 0 and 1 are the music, 2 and 3 the sound effects.
    std::string error;

    const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(MakeRom("ASRJ"), error);

    XSFLIB_CHECK(driver != nullptr);
    XSFLIB_CHECK(driver->SongCount() == 4);
    XSFLIB_CHECK(driver->HasSong(1) && !driver->HasEffect(1));
    XSFLIB_CHECK(!driver->HasSong(2) && driver->HasEffect(2) && driver->HasEffect(3));
    XSFLIB_CHECK(!driver->HasEffect(4));
}

void TestPlans()
{
    // The music's stub calls the play routine with the song's number in its file. The stub with the sound effects
    // calls the play routine for a song, and the game's routine for a sound effect with its number among them.
    std::string error;
    const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(MakeRom("ASRJ"), error);
    XSFLIB_CHECK(driver != nullptr);

    const gsf::rip::StubPlan music = driver->Plan();
    const gsf::rip::StubPlan effects = driver->PlanWithEffects();

    const gsf::rip::Operation& play = music.start.back();
    XSFLIB_CHECK(play.address == kPlay && play.routines.empty());
    XSFLIB_CHECK(play.values.size() == 1 && play.values[0].table == (std::vector<uint32_t>{0, 1, 2, 3}));
    const gsf::rip::Operation& start = effects.start.back();
    XSFLIB_CHECK(start.routines == (std::vector<uint32_t>{kPlay, kPlay, kPlayEffect, kPlayEffect}));
    XSFLIB_CHECK(start.values.size() == 1 && start.values[0].table == (std::vector<uint32_t>{0, 1, 0, 1}));
}

} // namespace

int main()
{
    TestEffects();
    TestPlans();

    return xsflib::testing::Summary("quintet");
}
