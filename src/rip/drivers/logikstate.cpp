// SPDX-License-Identifier: MIT

// Logik State's driver, GBAModPlay. The driver plays modules in the manner of ProTracker. agblib finds the modules,
// their banks of samples, the play routine, the music's volume that the game starts with and whether the game switches
// the sound off. The game's other calls come from a profile of each known release.
//
// The game calls the driver's init with the memory for the mixer's code and buffers, then the play routine with a
// module, an order and a bank of samples, then the routine that switches the sound on. The play routine sets the master
// volume, the music's volume and the sound effects' volume to full. After it, the game sets the music's volume from its
// options. The frame routine runs once a frame, from the handler of the vertical blank or from the game's main loop.
// The stub makes the same calls and keeps a table of each song's module, order and bank.
//
// The GBAMOD1 build's play routine takes a range of orders in place of a bank. Its init takes the memory from the
// game's allocator, and the game keeps the scale of a slide to a note in a variable that it sets for each module.

#include "rip/drivers/logikstate.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/logikstate/driver.h"
#include "agblib/sequence_export/logikstate/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace logikstate = agblib::logikstate;

// Aero the Acro-Bat: Rascal Rival Revenge. The credits name Jon Wells for the music and sound effects, Atomic Planet
// Entertainment as the developer and Metro3D as the publisher. The songs arrange the music of the game's first
// version, by Rick Fox. Module 8 plays Khachaturian's "Sabre Dance" with another B section, and module 5 is loosely
// based on Fučík's "Entry of the Gladiators". Each title names where the game plays the song's module. In the European
// release: the story of the intro (0x0801BD7C), the title screen (0x0801C7B0), the card that names an act (0x0801E894),
// the pause menu (0x08022AB8), the game over (0x08022684), the ending and its credits (0x08023938), and each level
// through the table of 25 modules. The act's card names the level. No pointer in the image names module 4, and no code
// plays it. Module 20's order 2 follows a jump that skips it, and no call starts there: song 22 starts it. The set
// orders the songs as a player first hears them: the intro, the title screen, the act's card and the pause menu, the
// levels in the order of the table at 0x083576C8 (each world's acts, bonus and boss), the game over, the ending, then
// the two unused songs.
const GameTags kAeroTags = {
    .game = "Aero the Acro-Bat: Rascal Rival Revenge",
    .artist = "Rick Fox",
    .year = "2002-06-21",
    .copyright = "Atomic Planet Entertainment, Metro3D",
    .comment = "Arranged by Jon Wells",
    .titles = {"Intro, Woods Act 4",
               "Circus Bonus, Funpark Boss",
               "Ending",
               "Circus Act 4",
               "Unused Song",
               "Title Screen, Circus Act 1",
               "Circus Act 5, Museum Bonus",
               "Funpark Act 1, Woods Act 3",
               "Funpark Act 2, Funpark Bonus",
               "Circus Boss",
               "Funpark Act 4",
               "Woods Act 1",
               "Woods Act 2",
               "Circus Act 2",
               "Funpark Act 3, Museum Act 3",
               "Museum Act 1",
               "Museum Act 4, Museum Act 5",
               "Circus Act 3, Museum Act 2",
               "Funpark Act 5",
               "Game Over",
               "Act Start, Pause",
               "Museum Boss",
               "Act Start, Pause, Part 2 (Unused)"},
    .order = {0, 5, 20, 13, 17, 3, 6, 1, 9, 7, 8, 14, 10, 18, 11, 12, 15, 16, 21, 19, 2, 4, 22},
    .credits = {{5, "Rick Fox",
                 "Arranged by Jon Wells. Based on \"Entry of the Gladiators\" by Julius Fu\u010D\u00EDk"},
                {8, "Aram Khachaturian, Rick Fox", "Arranged by Jon Wells"}},
};

// Aero's module 20 from order 2.
constexpr LogikStateOrder kAeroOrders[] = {{20, 2}};

// Aero's game over (module 19) sounds for 18 rows, then loops on a silent pattern, which agblib's analysis counts as
// part of the song. The song's length is the end of its last sound, in milliseconds from the song's start, and its
// fade is 1 s.
constexpr LogikStateEnd kAeroEnds[] = {{19, 1808}};

const LogikStateProfile kProfiles[] = {
    // Aero the Acro-Bat: Rascal Rival Revenge, Europe. The game stores the music's volume at 0x030024C0 and passes it
    // to the routine at 0x08026F9A. The handler of the vertical blank calls the frame routine at 0x08029702.
    {.code = "AAOP",
     .play = 0x08103770,
     .init = 0x08103840,
     .memory = 0x03005D50,
     .frame = 0x081037D8,
     .frame_in_handler = true,
     .sound_on = 0x081037A8,
     .tags = &kAeroTags,
     .orders = kAeroOrders,
     .ends = kAeroEnds},
    // Aero the Acro-Bat: Rascal Rival Revenge, US: the same code 0x358 bytes lower. The game passes the music's volume
    // to the routine at 0x08026CE6, and the handler calls the frame routine at 0x0802944E.
    {.code = "AAOE",
     .play = 0x08103418,
     .init = 0x081034E8,
     .memory = 0x03005D50,
     .frame = 0x08103480,
     .frame_in_handler = true,
     .sound_on = 0x08103450,
     .tags = &kAeroTags,
     .orders = kAeroOrders,
     .ends = kAeroEnds},
    // Zapper, US. Its boot (0x08000872 to 0x080008AA) sets the three volumes to 0, and its title screen sets the master
    // volume to full. The main loop calls the frame routine at 0x08000994.
    {.code = "AZPE",
     .play = 0x080E466C,
     .init = 0x080E4780,
     .memory = 0x03000180,
     .frame = 0x080E46DC,
     .sound_on = 0x080E46A8,
     .master_volume = 0x080E46B8,
     .music_volume = 0x080E46C4},
    // Pinball Challenge Deluxe, Europe, with GBAMOD1 modules. The init (0x08004234) takes its buffers from the game's
    // allocator, whose next address is 0x03000000 when the game calls it. The play routine sets the output up when it
    // first runs, and its start routine (0x080044BE) takes the music's volume from the word at 0x02000434, which the
    // game sets from its options. The game's routine at 0x080088D8, which the handler of the vertical blank calls,
    // restarts the sound DMA, runs the frame routine (0x080048A0) and swaps the buffers.
    {.code = "APLP",
     .play = 0x0800568C,
     .init = 0x08004234,
     .frame = 0x080088D8,
     .frame_in_handler = true,
     .sound_on = 0x0800DDD0,
     .heap = 0x02000BA0,
     .heap_start = 0x03000000,
     .slide_scale = 0x020006F0,
     .music_level = 0x02000434},
};

// SOUNDCNT_X. Some games switch the sound off and on again after the driver's init, such as Aero at 0x0802C45C. mGBA
// 0.11 then clears the volumes in SOUNDCNT_H, and the FIFOs play at 50%. The stub makes the same store for such a
// game. The set then plays at the game's level, whichever way a player treats SOUNDCNT_H.
constexpr uint32_t kSoundCntX = 0x04000084;

// The software version in the cartridge's header.
constexpr uint32_t kVersionByte = 0x080000BC;

class LogikStateDriver : public Driver
{
public:
    LogikStateDriver(const agblib::agb::Rom& rom, logikstate::DriverInfo info, const LogikStateProfile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile)
    {
        const uint32_t memory = profile.memory ? profile.memory : profile.heap_start;
        info_.log.push_back("init at " + agblib::agb::Hex(profile.init) + " with its memory at " +
                            agblib::agb::Hex(memory) + ", frame routine at " + agblib::agb::Hex(profile.frame));
    }

    std::string Name() const override
    {
        return "Logik State's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return int(info_.songs.size());
    }

    bool HasSong(int song) const override
    {
        logikstate::Module module;
        return song >= 0 && song < SongCount() &&
               logikstate::ReadModule(rom_, info_.songs[size_t(song)].module, module);
    }

    StubPlan Plan() const override
    {
        std::vector<uint32_t> modules;
        std::vector<uint32_t> orders;
        std::vector<uint32_t> banks;
        std::vector<uint32_t> first_orders;
        std::vector<uint32_t> last_orders;
        std::vector<uint32_t> slide_scales;
        for (const logikstate::Song& song : info_.songs)
        {
            modules.push_back(song.module);
            orders.push_back(uint32_t(song.order));
            banks.push_back(song.bank);
            first_orders.push_back(uint32_t(song.first_order));
            last_orders.push_back(uint32_t(song.last_order));
            slide_scales.push_back(song.note_slide_scale);
        }

        StubPlan plan;
        if (profile_.heap)
        {
            plan.start.push_back(Operation::Store(profile_.heap, {profile_.heap_start}, 4));
        }
        plan.start.push_back(profile_.memory ? Operation::Call(profile_.init | 1, {{profile_.memory}})
                                             : Operation::Call(profile_.init | 1));
        if (profile_.music_level)
        {
            plan.start.push_back(Operation::Store(profile_.music_level, {uint32_t(info_.music_volume)}, 4));
        }
        if (info_.format == logikstate::Format::kGbaMod1)
        {
            plan.start.push_back(Operation::Store(profile_.slide_scale, SongTable(slide_scales), 4));
            plan.start.push_back(Operation::Call(profile_.play | 1, {SongTable(modules), SongTable(orders),
                                                                     SongTable(first_orders), SongTable(last_orders)}));
        }
        else
        {
            plan.start.push_back(
                Operation::Call(profile_.play | 1, {SongTable(modules), SongTable(orders), SongTable(banks)}));
        }
        if (profile_.master_volume)
        {
            plan.start.push_back(Operation::Call(profile_.master_volume | 1, {{uint32_t(logikstate::kFullVolume)}}));
        }
        const uint32_t music_volume = info_.music_volume_routine ? info_.music_volume_routine : profile_.music_volume;
        if (music_volume)
        {
            plan.start.push_back(Operation::Call(music_volume | 1, {{uint32_t(info_.music_volume)}}));
        }
        if (info_.sound_off_call)
        {
            plan.start.push_back(Operation::Store(kSoundCntX, {0}, 2));
        }
        plan.start.push_back(Operation::Call(profile_.sound_on | 1));

        const Operation frame = Operation::Call(profile_.frame | 1);
        if (profile_.frame_in_handler)
        {
            plan.interrupts = kIrqVblank;
            plan.handlers = {{kIrqVblank, {frame}}};
        }
        else
        {
            plan.frame = {frame};
        }
        return plan;
    }

    const GameTags* Tags() const override
    {
        return profile_.tags;
    }

    std::optional<long long> LastSoundMs(int song) const override
    {
        for (const LogikStateEnd& end : profile_.ends)
        {
            if (end.song == song)
            {
                return end.ms;
            }
        }

        return std::nullopt;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::logikstate::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    logikstate::DriverInfo info_;
    LogikStateProfile profile_;
};

} // namespace

const LogikStateProfile* FindLogikStateProfile(std::string_view code, uint8_t version, uint32_t play_routine)
{
    for (const LogikStateProfile& profile : kProfiles)
    {
        if (profile.code == code && profile.version == version && profile.play == play_routine)
        {
            return &profile;
        }
    }

    return nullptr;
}

std::unique_ptr<Driver> FindLogikState(const agblib::agb::Rom& rom, std::string& error)
{
    logikstate::DriverInfo info;
    if (!logikstate::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.U8(kVersionByte);
    const LogikStateProfile* profile = FindLogikStateProfile(code, version, info.play_routine);
    if (!profile)
    {
        error = "Logik State's driver: the game " + code + " (version " + std::to_string(version) +
                ") with its play routine at " + agblib::agb::Hex(info.play_routine) +
                " is in no profile that gsf has, so the routines that it calls are unknown";
        return nullptr;
    }

    for (const LogikStateOrder& order : profile->orders)
    {
        if (order.song < 0 || size_t(order.song) >= info.songs.size())
        {
            error = "Logik State's driver: the profile of " + code + " names module " + std::to_string(order.song) +
                    ", which the game doesn't have";
            return nullptr;
        }

        logikstate::Song song = info.songs[size_t(order.song)];
        song.order = order.order;
        info.songs.push_back(song);
    }

    return std::make_unique<LogikStateDriver>(rom, std::move(info), *profile);
}

} // namespace gsf::rip
