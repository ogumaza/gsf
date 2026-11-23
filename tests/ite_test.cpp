// SPDX-License-Identifier: MIT

// Tests of ITE Media's driver (rip/drivers/ite.cpp) on a synthetic cartridge with Hugo: The Evil Mirror's game code and
// the driver's init at the US release's address. The expected routines and values are those of the US release's
// profile. They come from a reading of the game's code, and runs of the game in mGBA 0.11 confirmed them.

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/testing/agb_cart.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"

#include "rip/driver.h"
#include "rip/stub.h"

namespace
{

using agblib::agb::kRomBase;
using agblib::testing::agb_files::CartImage;
using gsf::rip::Driver;
using gsf::rip::Operation;

constexpr size_t kCartSize = 0x30000;

// The driver's init and its literal pool, the game's call of the init and the bank.
constexpr uint32_t kInit = 0x0801F4DC;
constexpr uint32_t kPool = 0x0801F500;
constexpr uint32_t kCall = 0x08010348;
constexpr uint32_t kBankLiteral = 0x08010360;
constexpr uint32_t kBank = 0x08020000;

// A tick of 256 samples of the driver's mix.
constexpr uint16_t kTempo = 0xA2;

// A song of one channel and one pattern for each order.
struct TestSong
{
    std::vector<uint8_t> orders;
    int restart = 0;
    std::vector<std::vector<uint8_t>> tracks; // a pattern of 2 rows each
};

// The game's code `code`, the driver, and a bank of `songs`.
agblib::agb::Rom MakeRom(const char* code, const std::vector<TestSong>& songs)
{
    CartImage cart(kCartSize);
    cart.SetGameCode(code);

    // The init: push {r4-r7, lr}, then ldr r1, [pc, #32]. The load reads DMA3SAD from the pool.
    cart.Put16(kInit, 0xB5F0);
    cart.Put16(kInit + 2, 0x4908);
    const uint32_t pool[] = {0x040000D4, 0x08020D04, 0x03000A50, 0x84000030, 0x08020DC4, 0x030003F0, 0x8400003A};
    for (uint32_t i = 0; i < 7; i++)
    {
        cart.Put32(kPool + 4 * i, pool[i]);
    }

    // The game's call: ldr r1, [pc, #20] to load the bank, then bl to the init.
    cart.Put16(kCall, 0x4905);
    cart.PutBl(kCall + 2, kInit);
    cart.Put32(kBankLiteral, kBank);

    cart.Put16(kBank, uint16_t(songs.size()));
    cart.Put16(kBank + 2, 1);
    cart.Put32(kBank + 8, 0x400);
    uint32_t place = 0x800;
    for (size_t s = 0; s < songs.size(); s++)
    {
        const TestSong& song = songs[s];
        const auto patterns = uint16_t(song.tracks.size());
        cart.Put32(kBank + 0x10 + 4 * uint32_t(s), place);
        const uint32_t header = kBank + place;
        const uint16_t fields[] = {uint16_t(song.orders.size()), uint16_t(song.restart), 1, 1, kTempo, patterns};
        for (uint32_t f = 0; f < 6; f++)
        {
            cart.Put16(header + 2 * f, fields[f]);
        }

        const uint32_t order_list = header + 12 + 4 * uint32_t(patterns);
        cart.PutBytes(order_list, song.orders);
        uint32_t track = (order_list + uint32_t(song.orders.size()) + patterns + 3) & ~3u;
        for (uint32_t p = 0; p < patterns; p++)
        {
            cart.Put8(order_list + uint32_t(song.orders.size()) + p, 2);
            cart.Put32(header + 12 + 4 * p, track - kBank);
            cart.PutBytes(track, song.tracks[p]);
            track += uint32_t(song.tracks[p].size());
        }

        place = (track - kBank + 3) & ~3u;
    }

    return cart.ToRom();
}

// A note that waits a row, F00, and a row without a note.
const std::vector<uint8_t> kNote = {1, 0x01, 0x30};
const std::vector<uint8_t> kF00 = {0, 0x0C, 0x0F, 0x00};
const std::vector<uint8_t> kRest = {1, 0x00};

// Song 0 loops from order 1, songs 1 and 4 have no patterns, songs 2 and 5 end on their first row and song 3 plays no
// note.
const std::vector<uint8_t> kNoteAndF00 = {0, 0x0D, 0x30, 0x0F, 0x00};
const std::vector<TestSong> kSongs = {
    {{0, 1}, 1, {kNote, kNote}}, {{0}, 0, {}}, {{0}, 0, {kNoteAndF00}},
    {{0, 1}, 0, {kRest, kRest}}, {{0}, 0, {}}, {{0}, 0, {kNoteAndF00}},
};

uint32_t CallTarget(const Operation& operation)
{
    return operation.kind == Operation::Kind::kCall ? operation.address : 0;
}

void TestPlan()
{
    const agblib::agb::Rom rom = MakeRom("AHJE", kSongs);
    std::string error;

    const std::unique_ptr<Driver> driver = gsf::rip::FindDriver(rom, error);

    XSFLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(error.empty());
    XSFLIB_CHECK(driver->Name() == "ITE Media's driver");
    XSFLIB_CHECK(driver->SongCount() == 6);
    XSFLIB_CHECK(driver->HasSong(0) && !driver->HasSong(1) && driver->HasSong(2) && !driver->HasSong(4));

    // WAITCNT, SOUNDBIAS, the init with the bank, the ring's state, the sound effects, the output, the start and the
    // music's volume by song; each frame, the count of frames to DMA 1's restart and the mix.
    const gsf::rip::StubPlan plan = driver->Plan();
    XSFLIB_REQUIRE(plan.start.size() == 11);
    XSFLIB_CHECK(plan.start[0].kind == Operation::Kind::kStore && plan.start[0].address == agblib::agb::kRegWaitCnt &&
                 plan.start[0].values[0].constant == 0x4316);
    XSFLIB_CHECK(plan.start[1].address == agblib::agb::kRegSoundBias && plan.start[1].values[0].constant == 0x4200);
    XSFLIB_CHECK(CallTarget(plan.start[2]) == (kInit | 1) && plan.start[2].values[1].constant == kBank);
    XSFLIB_CHECK(plan.start[3].address == 0x030003E4 && plan.start[3].values[0].constant == 5);
    XSFLIB_CHECK(plan.start[4].address == 0x03000B78 && plan.start[4].values[0].constant == 5);
    XSFLIB_CHECK(plan.start[5].address == 0x03000B1C && plan.start[5].values[0].constant == 0x120 &&
                 plan.start[5].size == 4);
    XSFLIB_CHECK(CallTarget(plan.start[6]) == 0x08020A91 && plan.start[6].values[0].constant == 0x080473CC &&
                 plan.start[6].values[1].constant == 196);
    XSFLIB_CHECK(CallTarget(plan.start[7]) == 0x08020AA5 && plan.start[7].values[0].constant == 3);
    XSFLIB_CHECK(CallTarget(plan.start[8]) == 0x0801F629);
    XSFLIB_CHECK(CallTarget(plan.start[9]) == 0x080205CD && plan.start[9].values[0].song);
    XSFLIB_CHECK(CallTarget(plan.start[10]) == 0x0801F5ED && plan.start[10].values[1].constant == 0x100);

    const std::vector<uint32_t>& volumes = plan.start[10].values[0].table;
    XSFLIB_REQUIRE(volumes.size() == 28);
    XSFLIB_CHECK(volumes[0] == 0x100 && volumes[1] == 0xCF && volumes[2] == 0x80 && volumes[3] == 0xCF);
    XSFLIB_CHECK(volumes[14] == 0xCF && volumes[15] == 0x80 && volumes[16] == 0xCF && volumes[27] == 0x100);
    XSFLIB_REQUIRE(plan.frame.size() == 2);
    XSFLIB_CHECK(CallTarget(plan.frame[0]) == 0x0801F699 && CallTarget(plan.frame[1]) == 0x0801F6F1);
    XSFLIB_CHECK(plan.handlers.empty());
}

void TestTiming()
{
    const agblib::agb::Rom rom = MakeRom("AHJE", kSongs);
    std::string error;
    const std::unique_ptr<Driver> driver = gsf::rip::FindDriver(rom, error);
    XSFLIB_REQUIRE(driver != nullptr);
    const double frame_ms = 1000.0 / agblib::agb::kFrameRate;

    const std::optional<xsflib::rip::Timing> loop = driver->SongTiming(0);
    const std::optional<xsflib::rip::Timing> end = driver->SongTiming(2);
    const std::optional<xsflib::rip::Timing> silent = driver->SongTiming(3);
    const std::optional<xsflib::rip::Timing> rings = driver->SongTiming(5);

    // Song 0's loop runs from 512 samples to 1024: its intro and two passes take 1536 samples, 5.6 frames, and a pass
    // takes 512 samples, 1.9 frames. Song 2 ends on the tick after its F00, 256 samples in.
    const xsflib::rip::Timing expected_loop = xsflib::rip::PassesAt(6, 2, frame_ms, gsf::rip::kFadeMs);
    const xsflib::rip::Timing expected_end = xsflib::rip::EndAt(1, frame_ms);
    XSFLIB_REQUIRE(loop && end && silent);
    XSFLIB_CHECK(loop->length_ms == expected_loop.length_ms && loop->fade_ms == expected_loop.fade_ms);
    XSFLIB_CHECK(loop->loop == expected_loop.loop && loop->fade_ms == 7'000);
    XSFLIB_CHECK(end->length_ms == expected_end.length_ms && end->fade_ms == expected_end.fade_ms);
    XSFLIB_CHECK(!loop->silent && !end->silent && silent->silent);

    // Song 5's F00 comes as early as song 2's, but the profile gives song 5 a last sound at 3200 ms. Its timing runs
    // to that frame, and the coverage then covers the samples that ring on.
    const xsflib::rip::Timing expected_rings = xsflib::rip::EndAt(uint64_t(std::ceil(3200 / frame_ms)), frame_ms);
    XSFLIB_REQUIRE(rings);
    XSFLIB_CHECK(rings->length_ms == expected_rings.length_ms && rings->fade_ms == expected_rings.fade_ms);

    // The profile gives the last sound of each song that ends, such as the game over's, measured on the set in mGBA. A
    // song that loops gets none.
    XSFLIB_CHECK(driver->LastSoundMs(27) == 7310 && driver->LastSoundMs(5) == 3200 && !driver->LastSoundMs(0));
}

void TestNoTags()
{
    const agblib::agb::Rom rom = MakeRom("AHJE", kSongs);
    std::string error;

    const std::unique_ptr<Driver> driver = gsf::rip::FindDriver(rom, error);

    // The driver gives no titles and no order: the set lists the songs by number.
    XSFLIB_REQUIRE(driver != nullptr);
    XSFLIB_CHECK(driver->Tags() == nullptr);
}

void TestOtherRelease()
{
    // The European release (AHJP) has no profile.
    const agblib::agb::Rom rom = MakeRom("AHJP", kSongs);
    std::string error;

    const std::unique_ptr<Driver> driver = gsf::rip::FindDriver(rom, error);

    XSFLIB_CHECK(driver == nullptr);
    XSFLIB_CHECK(error.find("no profile") != std::string::npos);
}

void TestFindDrivers()
{
    // A game with one driver has one set, with no name, in the rip's folder.
    std::string error;
    const agblib::agb::Rom rom = MakeRom("AHJE", kSongs);
    const std::vector<gsf::rip::FoundDriver> drivers = gsf::rip::FindDrivers(rom, error);
    XSFLIB_REQUIRE(drivers.size() == 1);
    XSFLIB_CHECK(drivers[0].driver->Name() == "ITE Media's driver" && drivers[0].set.empty());

    // A game that the table of games with a driver for each part names has each of those drivers, or none.
    const agblib::agb::Rom parts = MakeRom("AZLE", kSongs);
    XSFLIB_CHECK(gsf::rip::FindDrivers(parts, error).empty());
    XSFLIB_CHECK(error.find("A Link to the Past") != std::string::npos);
}

} // namespace

int main()
{
    TestPlan();
    TestTiming();
    TestNoTags();
    TestOtherRelease();
    TestFindDrivers();

    return xsflib::testing::Summary("ite");
}
