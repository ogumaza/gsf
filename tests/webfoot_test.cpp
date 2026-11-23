// SPDX-License-Identifier: MIT

// Tests of the detection of Webfoot Technologies' driver, on agblib's synthetic cartridge of the driver. The cartridge
// has the driver's tables, the init that detection reads and a song, but not the play routine. The driver's search of
// the game's routines then fails, and gsf names what it couldn't find. The crt0's WAITCNT is read from made-up crt0s.

#include "rip/drivers/webfoot.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/testing/agb_cart.h"
#include "agblib/testing/webfoot_cart.h"
#include "xsflib/testing/check.h"

#include "rip/driver.h"

namespace
{

namespace cart = agblib::testing::webfoot_cart;

using agblib::webfoot::Build;

// A cartridge with one song, and `code` as its game code.
agblib::agb::Rom MakeRom(Build build, const std::string& code)
{
    cart::TestCart test(build);
    test.AddSample({});
    test.AddSong({cart::Pattern({cart::NoteEntry(0, 60, 1, 64)}, 4)}, 125, 6);
    test.SetGameCode(code);

    return test.ToRom();
}

void TestNoRoutines()
{
    // Both builds: the driver's song table, but no routines of the game's.
    for (const Build build : {Build::k2002, Build::k2003})
    {
        std::string error;
        const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(MakeRom(build, "ZZZE"), error);

        XSFLIB_CHECK(driver == nullptr);
        XSFLIB_CHECK(error.find("Webfoot Technologies' driver") != std::string::npos);
        XSFLIB_CHECK(error.find(agblib::agb::Hex(cart::kSongs)) != std::string::npos);
    }
}

void TestPartOutOfRange()
{
    // The cartridge has one song table: no part 1, and no part -1.
    const agblib::agb::Rom rom = MakeRom(Build::k2003, "ZZZE");
    std::string error;

    XSFLIB_CHECK(gsf::rip::FindWebfootPart(rom, 1, error) == nullptr);
    XSFLIB_CHECK(error.find("no table 1") != std::string::npos);
    XSFLIB_CHECK(gsf::rip::FindWebfootPart(rom, -1, error) == nullptr);
    XSFLIB_CHECK(error.find("no table -1") != std::string::npos);
}

void TestGameWithParts()
{
    // The game code of the cartridge with three programs: FindDrivers() looks for the driver of its first part, and
    // fails with the error of that part.
    std::string error;
    const std::vector<gsf::rip::FoundDriver> drivers = gsf::rip::FindDrivers(MakeRom(Build::k2003, "BUFE"), error);

    XSFLIB_CHECK(drivers.empty());
    XSFLIB_CHECK(error.find("Webfoot Technologies' driver") != std::string::npos);
    XSFLIB_CHECK(error.find(agblib::agb::Hex(cart::kSongs)) != std::string::npos);
}

// Returns an image of 0x400 bytes whose crt0, 0xC0 bytes in, is `code`, with `literal` at 0x200.
agblib::agb::Rom Crt0Rom(const std::vector<uint32_t>& code, uint32_t literal)
{
    agblib::testing::agb_files::CartImage image(0x400);
    for (size_t i = 0; i < code.size(); i++)
    {
        image.Put32(agblib::agb::kRomBase + 0xC0 + 4 * uint32_t(i), code[i]);
    }

    image.Put32(agblib::agb::kRomBase + 0x200, literal);

    return image.ToRom();
}

// Returns `ldr rD, [pc, #imm]` at `at` for the literal at 0x200.
uint32_t LoadLiteral(uint32_t d, uint32_t at)
{
    return 0xE59F0000 | (d << 12) | (0x200 - (at + 8));
}

void TestCrt0WaitCnt()
{
    // The later build's crt0: mov r0, #0x04000000; ldr r1, =0x4014; str r1, [r0, #0x204].
    const agblib::agb::Rom io_base = Crt0Rom({0xE3A00640, LoadLiteral(1, 0xC4), 0xE5801204}, 0x4014);
    // ldr r1, =0x04000204; mov r2, #0x14; str r2, [r1].
    const agblib::agb::Rom address = Crt0Rom({LoadLiteral(1, 0xC0), 0xE3A02014, 0xE5812000}, 0x04000204);
    // The same as the first, but `add r0, r0, #4` changes the base before the store.
    const agblib::agb::Rom moved = Crt0Rom({0xE3A00640, LoadLiteral(1, 0xC4), 0xE2800004, 0xE5801204}, 0x4014);
    // The Legacy of Goku's crt0 stores no WAITCNT: a stack pointer and a word of IWRAM.
    const agblib::agb::Rom none =
        Crt0Rom({0xE3A00012, 0xE121F000, 0xE3A00000, LoadLiteral(1, 0xCC), 0xE5810000}, 0x03000004);

    XSFLIB_CHECK(gsf::rip::Crt0WaitCnt(io_base, agblib::agb::kRomBase) == 0x4014);
    XSFLIB_CHECK(gsf::rip::Crt0WaitCnt(address, agblib::agb::kRomBase) == 0x14);
    XSFLIB_CHECK(gsf::rip::Crt0WaitCnt(moved, agblib::agb::kRomBase) == 0);
    XSFLIB_CHECK(gsf::rip::Crt0WaitCnt(none, agblib::agb::kRomBase) == 0);
}

} // namespace

int main()
{
    TestNoRoutines();
    TestPartOutOfRange();
    TestGameWithParts();
    TestCrt0WaitCnt();

    return xsflib::testing::Summary("webfoot");
}
