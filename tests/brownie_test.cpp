// SPDX-License-Identifier: MIT

// Tests of the detection of Brownie Brown's driver, on agblib's synthetic cartridge of the driver. The cartridge's
// per-frame routine lies where no release's does, so the profiles of the releases don't match it.

#include <cstdint>
#include <memory>
#include <string>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/testing/brownie_cart.h"
#include "xsflib/testing/check.h"

#include "rip/driver.h"

namespace
{

namespace cart = agblib::testing::brownie_cart;

// The game code in the cartridge's header.
constexpr uint32_t kGameCode = agblib::agb::kRomBase + 0xAC;

agblib::agb::Rom MakeRom(const std::string& code)
{
    cart::Cart image;
    for (size_t i = 0; i < code.size(); i++)
    {
        image.Put8(kGameCode + uint32_t(i), uint8_t(code[i]));
    }
    return image.ToRom();
}

void TestNoProfile()
{
    // Sword of Mana's game code, with the per-frame routine elsewhere: the driver, but no profile.
    std::string error;
    const std::unique_ptr<gsf::rip::Driver> driver = gsf::rip::FindDriver(MakeRom("AVSE"), error);
    XSFLIB_CHECK(driver == nullptr);
    XSFLIB_CHECK(error.find("Brownie Brown's driver") != std::string::npos);
    XSFLIB_CHECK(error.find("AVSE") != std::string::npos);
    XSFLIB_CHECK(error.find(agblib::agb::Hex(cart::kFrame)) != std::string::npos);
    XSFLIB_CHECK(error.find("no profile") != std::string::npos);
}

} // namespace

int main()
{
    TestNoProfile();

    return xsflib::testing::Summary("brownie");
}
