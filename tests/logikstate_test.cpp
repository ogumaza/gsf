// SPDX-License-Identifier: MIT

// Tests of the profiles of Logik State's driver (rip/drivers/logikstate.h). Each release of Aero the Acro-Bat: Rascal
// Rival Revenge has the driver's code at another address. The expected values are the addresses that a reading of each
// release's code gave. A profile matches only the release it was made for.

#include "rip/drivers/logikstate.h"

#include <cstdint>
#include <string>
#include <vector>

#include "xsflib/testing/check.h"

#include "rip/driver.h"

namespace
{

using gsf::rip::FindLogikStateProfile;
using gsf::rip::LogikStateProfile;

// A release's game code and routines as the reading of its code gave them. Both releases are version 0.
struct Release
{
    const char* code;
    uint32_t play;
    uint32_t init;
    uint32_t memory;
    uint32_t frame;
    uint32_t sound_on;
};

constexpr Release kEurope = {"AAOP", 0x08103770, 0x08103840, 0x03005D50, 0x081037D8, 0x081037A8};
constexpr Release kUs = {"AAOE", 0x08103418, 0x081034E8, 0x03005D50, 0x08103480, 0x08103450};

const LogikStateProfile* Find(const Release& release)
{
    return FindLogikStateProfile(release.code, 0, release.play);
}

void CheckRelease(const Release& release)
{
    const LogikStateProfile* profile = Find(release);
    XSFLIB_REQUIRE(profile != nullptr);
    XSFLIB_CHECK(profile->code == release.code && profile->version == 0);
    XSFLIB_CHECK(profile->play == release.play);
    XSFLIB_CHECK(profile->init == release.init);
    XSFLIB_CHECK(profile->memory == release.memory);
    XSFLIB_CHECK(profile->frame == release.frame);
    XSFLIB_CHECK(profile->frame_in_handler);
    XSFLIB_CHECK(profile->sound_on == release.sound_on);

    // The game sets no other volume and has no allocator: agblib finds the music's volume and its routine.
    XSFLIB_CHECK(profile->master_volume == 0 && profile->music_volume == 0);
    XSFLIB_CHECK(profile->heap == 0 && profile->music_level == 0);

    // Module 20 from order 2 is a song of its own. The profile gives the end of the game over's last sound.
    XSFLIB_REQUIRE(profile->orders.size() == 1);
    XSFLIB_CHECK(profile->orders[0].song == 20 && profile->orders[0].order == 2);
    XSFLIB_REQUIRE(profile->ends.size() == 1);
    XSFLIB_CHECK(profile->ends[0].song == 19 && profile->ends[0].ms == 1808);
}

void TestAeroReleases()
{
    CheckRelease(kEurope);
    CheckRelease(kUs);

    // The European release has the same driver 0x358 bytes higher, so each routine keeps its distance from the play
    // routine.
    constexpr uint32_t kShift = 0x358;
    XSFLIB_CHECK(kEurope.play - kUs.play == kShift);
    XSFLIB_CHECK(kEurope.init - kUs.init == kShift);
    XSFLIB_CHECK(kEurope.frame - kUs.frame == kShift);
    XSFLIB_CHECK(kEurope.sound_on - kUs.sound_on == kShift);
}

void TestAeroTags()
{
    // Both releases name the game, the credits and a title for each of the 22 modules and for module 20 from order 2.
    const gsf::rip::GameTags* europe = Find(kEurope)->tags;
    XSFLIB_REQUIRE(europe != nullptr);
    XSFLIB_CHECK(Find(kUs)->tags == europe);
    XSFLIB_CHECK(europe->game == "Aero the Acro-Bat: Rascal Rival Revenge");
    XSFLIB_CHECK(europe->artist == "Rick Fox" && europe->comment == "Arranged by Jon Wells");
    XSFLIB_CHECK(europe->year == "2002-06-21");
    XSFLIB_CHECK(europe->copyright == "Atomic Planet Entertainment, Metro3D");
    XSFLIB_CHECK(europe->titles.size() == 23);
    for (const std::string& title : europe->titles)
    {
        XSFLIB_CHECK(!title.empty() && title.find_first_of("/:") == std::string::npos);
    }

    // The title screen plays module 5, the game over module 19, and no code plays module 4 or module 20 from order 2.
    XSFLIB_CHECK(europe->titles[5] == "Title Screen, Circus Act 1");
    XSFLIB_CHECK(europe->titles[19] == "Game Over");
    XSFLIB_CHECK(europe->titles[4] == "Unused Song");
    XSFLIB_CHECK(europe->titles[22] == "Act Start, Pause, Part 2 (Unused)");

    // The set's order is the game's: the intro, the title screen, the act's card, the levels of the table at 0x083576C8
    // where each song first plays, the game over, the ending, then the unused songs.
    XSFLIB_CHECK(europe->order ==
                 std::vector<int>({0, 5, 20, 13, 17, 3, 6, 1, 9, 7, 8, 14, 10, 18, 11, 12, 15, 16, 21, 19, 2, 4, 22}));

    // Module 8 plays Khachaturian's "Sabre Dance", and module 5 is based on Fučík's "Entry of the Gladiators".
    XSFLIB_REQUIRE(europe->credits.size() == 2);
    XSFLIB_CHECK(europe->credits[0].song == 5 && europe->credits[0].artist == "Rick Fox");
    XSFLIB_CHECK(europe->credits[0].comment ==
                 "Arranged by Jon Wells. Based on \"Entry of the Gladiators\" by Julius Fu\xC4\x8D\xC3\xADk");
    XSFLIB_CHECK(europe->credits[1].song == 8 && europe->credits[1].artist == "Aram Khachaturian, Rick Fox");
    XSFLIB_CHECK(europe->credits[1].comment == "Arranged by Jon Wells");
}

void TestOtherGames()
{
    // Zapper and Pinball Challenge Deluxe have a profile each, with no songs at other orders.
    const LogikStateProfile* zapper = FindLogikStateProfile("AZPE", 0, 0x080E466C);
    const LogikStateProfile* pinball = FindLogikStateProfile("APLP", 0, 0x0800568C);
    XSFLIB_REQUIRE(zapper != nullptr && pinball != nullptr);
    XSFLIB_CHECK(zapper->orders.empty() && pinball->orders.empty());
    XSFLIB_CHECK(zapper->ends.empty() && pinball->ends.empty());
}

void TestNearMisses()
{
    // A profile needs the game code, the version and the play routine. Another game with Aero's play routine, another
    // version of Aero, one release's code with the other's routine, or another routine has none.
    XSFLIB_CHECK(FindLogikStateProfile("AZPE", 0, kEurope.play) == nullptr);
    XSFLIB_CHECK(FindLogikStateProfile(kEurope.code, 1, kEurope.play) == nullptr);
    XSFLIB_CHECK(FindLogikStateProfile(kEurope.code, 0, kUs.play) == nullptr);
    XSFLIB_CHECK(FindLogikStateProfile(kUs.code, 0, kEurope.play) == nullptr);
    XSFLIB_CHECK(FindLogikStateProfile(kEurope.code, 0, 0x08103774) == nullptr);
    XSFLIB_CHECK(FindLogikStateProfile("", 0, 0) == nullptr);
}

} // namespace

int main()
{
    TestAeroReleases();
    TestAeroTags();
    TestOtherGames();
    TestNearMisses();

    return xsflib::testing::Summary("Logik State's profiles");
}
