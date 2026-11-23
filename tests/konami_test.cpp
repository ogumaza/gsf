// SPDX-License-Identifier: MIT

// Tests of Konami's driver in gsf: its detection, on agblib's synthetic cartridge of the driver's tables, the stub's
// plan for a profile (rip/drivers/konami.h), and the samples that a rip keeps whole. The song table of the cartridge is
// at an address that no release uses. No profile then matches it.

#include "rip/drivers/konami.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/agb/system.h"
#include "agblib/konami/driver.h"
#include "agblib/testing/konami_cart.h"
#include "xsflib/testing/check.h"

#include "rip/driver.h"
#include "rip/stub.h"
#include "rom_spans.h"

namespace
{

namespace cart = agblib::testing::konami_cart;

using gsf::testing::HasSpan;

agblib::agb::Rom MakeRom(const std::string& code)
{
    cart::Image image = cart::ScanImage();
    image.SetGameCode(code);

    return image.ToRom();
}

void TestNoProfile()
{
    // Yu-Gi-Oh! Duel Monsters Expert 2006's game code, with the song table elsewhere: the driver, but no profile.
    std::string error;
    const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(MakeRom("BY6J"), error);
    XSFLIB_CHECK(driver == nullptr);
    XSFLIB_CHECK(error.find("Konami's driver") != std::string::npos);
    XSFLIB_CHECK(error.find("BY6J") != std::string::npos);
    XSFLIB_CHECK(error.find(agblib::agb::Hex(cart::kScanSongTable)) != std::string::npos);
    XSFLIB_CHECK(error.find("no profile") != std::string::npos);
}

// The index of the first call of `routine` among `operations`, or -1.
int CallIndex(const std::vector<gsf::rip::Operation>& operations, uint32_t routine)
{
    for (size_t i = 0; i < operations.size(); i++)
    {
        if (operations[i].kind == gsf::rip::Operation::Kind::kCall && operations[i].address == routine)
        {
            return int(i);
        }
    }

    return -1;
}

void TestRateCall()
{
    // Tennis no Ouji-sama 2003: Passion Red and 2004: Glorious Gold and Stylish Silver call the driver's rate setter
    // with 2 after the init (A8RJ 0x080002DC, B4GJ and B4SJ 0x080002DA). The stub makes the call between the init and
    // the request.
    struct Game
    {
        std::string_view code;
        uint32_t song_table;
        uint32_t rate;
    };
    for (const Game& game : {Game{"A8RJ", 0x089AC770, 0x0809823D}, Game{"B4GJ", 0x0901B340, 0x080D6F4D},
                             Game{"B4SJ", 0x09016F80, 0x080D6F51}})
    {
        const gsf::rip::KonamiProfile* profile = gsf::rip::FindKonamiProfile(game.code, 0, game.song_table);
        XSFLIB_CHECK(profile != nullptr);
        if (!profile)
        {
            continue;
        }

        const gsf::rip::StubPlan plan = gsf::rip::KonamiPlan(*profile, 12768.0);
        const int init = CallIndex(plan.start, profile->init);
        const int rate = CallIndex(plan.start, game.rate);
        const int request = CallIndex(plan.start, profile->request);
        XSFLIB_CHECK(init >= 0 && rate > init && request > rate);
        XSFLIB_CHECK(rate >= 0 && plan.start[size_t(rate)].values.size() == 1 &&
                     plan.start[size_t(rate)].values[0].constant == 2);
    }
}

void TestPlan()
{
    // Each profile's plan starts with WAITCNT, then calls the init, the mono switch and the rate setter where the
    // profile has them, and the request, with nothing between: the init starts when the stub reaches it. The main loop
    // runs the per-frame entry, then the start, and the handler runs only DMA 1's routine.
    struct Game
    {
        std::string_view code;
        uint32_t song_table;
    };
    for (const Game& game : {Game{"BY6J", 0x08194210}, Game{"A8RJ", 0x089AC770}, Game{"B3MJ", 0x080A9AA0}})
    {
        const gsf::rip::KonamiProfile* profile = gsf::rip::FindKonamiProfile(game.code, 0, game.song_table);
        XSFLIB_CHECK(profile != nullptr);
        if (!profile)
        {
            continue;
        }

        const gsf::rip::StubPlan plan = gsf::rip::KonamiPlan(*profile, 12768.0);
        std::vector<uint32_t> calls = {profile->init};
        if (profile->mono != 0)
        {
            calls.push_back(profile->mono);
        }

        if (profile->rate != 0)
        {
            calls.push_back(profile->rate);
        }

        calls.push_back(profile->request);

        XSFLIB_CHECK(plan.start.size() == calls.size() + 1);
        if (plan.start.size() != calls.size() + 1)
        {
            continue;
        }

        XSFLIB_CHECK(plan.start[0].kind == gsf::rip::Operation::Kind::kStore &&
                     plan.start[0].address == agblib::agb::kRegWaitCnt);
        for (size_t i = 0; i < calls.size(); i++)
        {
            XSFLIB_CHECK(plan.start[i + 1].kind == gsf::rip::Operation::Kind::kCall);
            XSFLIB_CHECK(plan.start[i + 1].address == calls[i]);
        }

        XSFLIB_CHECK(plan.frame.size() == 2 && CallIndex(plan.frame, profile->frame) == 0 &&
                     CallIndex(plan.frame, profile->start) == 1);
        XSFLIB_CHECK(plan.handlers.size() == 1 && plan.handlers[0].interrupt == gsf::rip::kIrqDma1);
        XSFLIB_CHECK(plan.handlers.size() == 1 && plan.handlers[0].per_frame == 22.0);
        XSFLIB_CHECK(plan.handlers.size() == 1 && plan.handlers[0].operations.size() == 1 &&
                     CallIndex(plan.handlers[0].operations, profile->dma1) == 0);
    }
}

void TestBlockCycles()
{
    // A block of the mixer is 16 periods of Timer 0. A profile that calls the rate setter takes the reload of its
    // setting from the driver's timer table: A8RJ's setting 2 is 0xFBD8, 15,768 Hz, and 16.5 blocks a frame. Without
    // the call, the period comes from the mixer's rate: BY6J's 21,024 Hz (0xFCE2) gives 22 blocks a frame.
    constexpr uint32_t kTimerTable = agblib::agb::kRomBase + 0x3C0;
    constexpr uint32_t kReloads[] = {0xFCE2, 0xFC64, 0xFBD8, 0xFB1A, 0xF9C4};
    cart::Image image = cart::ScanImage();
    for (uint32_t i = 0; i < std::size(kReloads); i++)
    {
        image.Put32(kTimerTable + 4 * i, kReloads[i]);
    }

    const agblib::agb::Rom rom = image.ToRom();
    agblib::konami::DriverInfo info;
    info.timer_table = kTimerTable;
    info.mix_rate = double(agblib::agb::kSecondCycles) / (0x10000 - kReloads[0]);

    const gsf::rip::KonamiProfile* rate = gsf::rip::FindKonamiProfile("A8RJ", 0, 0x089AC770);
    const gsf::rip::KonamiProfile* plain = gsf::rip::FindKonamiProfile("BY6J", 0, 0x08194210);
    XSFLIB_CHECK(rate != nullptr && plain != nullptr);
    if (!rate || !plain)
    {
        return;
    }

    XSFLIB_CHECK(gsf::rip::KonamiBlockCycles(rom, info, *rate) == 16.0 * (0x10000 - 0xFBD8));
    const gsf::rip::StubPlan slow = gsf::rip::KonamiPlan(*rate, gsf::rip::KonamiBlockCycles(rom, info, *rate));
    XSFLIB_CHECK(slow.handlers.size() == 1 && slow.handlers[0].per_frame == 16.5);

    // The mixer's rate is a fraction of the CPU clock. The period then comes back only to within rounding.
    XSFLIB_CHECK(std::abs(gsf::rip::KonamiBlockCycles(rom, info, *plain) - 16.0 * (0x10000 - 0xFCE2)) < 1e-6);
    const gsf::rip::StubPlan fast = gsf::rip::KonamiPlan(*plain, gsf::rip::KonamiBlockCycles(rom, info, *plain));
    XSFLIB_CHECK(fast.handlers.size() == 1 && std::abs(fast.handlers[0].per_frame - 22.0) < 1e-9);
}

void TestSampleSpans()
{
    // A null entry before a sample doesn't end the table: Hunter X Hunter's table has its first at entry 4. The second
    // sample of the scan cartridge stays whole, its header and its 16 points, with the first entry null.
    cart::Image image = cart::ScanImage();
    constexpr uint32_t kHeader1 = cart::kScanSampleTable + 8 + 12 + 32;
    image.Put32(cart::kScanSampleTable, 0);

    // A packed sample of the Live Start revision: a header of step, length with bit 31 and no loop, the
    // table of 32 pairs, and its data. It's kept from its header to its length after the data.
    constexpr uint32_t kPacked = agblib::agb::kRomBase + 0x300;
    constexpr uint32_t kPackedPoints = 40;
    image.Put32(cart::kScanSampleTable + 8, kPacked);
    image.Put32(kPacked, 0x1000);
    image.Put32(kPacked + 4, 0x80000000 | kPackedPoints);
    image.Put32(kPacked + 8, 0xFFFFFFFF);
    const agblib::agb::Rom rom = image.ToRom();

    agblib::konami::DriverInfo info;
    info.revision = agblib::konami::Revision::kLiveStart;
    info.sample_table = cart::kScanSampleTable;
    const std::vector<gsf::rip::RomSpan> spans = gsf::rip::KonamiSampleSpans(rom, info);
    XSFLIB_CHECK(HasSpan(spans, kHeader1, 12));
    XSFLIB_CHECK(HasSpan(spans, kHeader1 + 12, 16));
    XSFLIB_CHECK(HasSpan(spans, kPacked, 0x4C + kPackedPoints));
}

} // namespace

int main()
{
    TestNoProfile();
    TestRateCall();
    TestPlan();
    TestBlockCycles();
    TestSampleSpans();

    return xsflib::testing::Summary("konami");
}
