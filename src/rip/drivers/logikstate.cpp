// SPDX-License-Identifier: MIT

// Logik State's driver, GBAModPlay. The driver plays modules in the manner of ProTracker. agblib finds the modules,
// their banks of samples, the play routine, the volume of the music that the game starts with, and whether the game
// switches the sound off. A profile of each known release gives the other calls of the game.
//
// The game calls the init of the driver with the memory for the code and the buffers of the mixer. It then calls the
// play routine with a module, an order and a bank of samples, then the routine that switches the sound on. The play
// routine sets the master volume, the volume of the music and the volume of the sound effects to full. After it, the
// game sets the volume of the music from its options. The frame routine runs once a frame, from the handler of the
// vertical blank or from the main loop of the game. The stub makes the same calls and keeps a table of the module, the
// order and the bank of each song.
//
// The play routine of the GBAMOD1 build takes a range of orders in place of a bank. Its init takes the memory from the
// allocator of the game. The game keeps the scale of a slide to a note in a variable that it sets for each module.

#include "rip/drivers/logikstate.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/sound_registers.h"
#include "agblib/logikstate/driver.h"
#include "agblib/sequence_export/logikstate/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace logikstate = agblib::logikstate;

// Aero the Acro-Bat: Rascal Rival Revenge. In the European release, the game plays a module in the story of the intro
// (0x0801BD7C), the title screen (0x0801C7B0), the card that names an act (0x0801E894), the pause menu (0x08022AB8),
// the game over (0x08022684), the ending and its credits (0x08023938), and each level through the table of 25 modules.
// No pointer in the image names module 4, and no code plays it. Order 2 of module 20 follows a jump that skips it, and
// no call starts there: song 22 starts it. A player first hears the songs in this order: the intro, the title screen,
// the card of the act and the pause menu, the levels in the order of the table at 0x083576C8 (the acts, bonus and boss
// of each world), the game over, the ending, then the two unused songs. That is songs 0, 5, 20, 13, 17, 3, 6, 1, 9, 7,
// 8, 14, 10, 18, 11, 12, 15, 16, 21, 19, 2, 4, 22. The set lists them by number.

// Module 20 of Aero from order 2.
constexpr LogikStateOrder kAeroOrders[] = {{20, 2}};

// The game over of Aero (module 19) sounds for 18 rows, then loops on a silent pattern. The analysis of agblib counts
// that pattern as part of the song. The length of the song is the end of its last sound, in milliseconds from the start
// of the song, and its fade is 1 s.
constexpr SongEnd kAeroEnds[] = {{19, 1808}};

const LogikStateProfile kProfiles[] = {
    // Aero the Acro-Bat: Rascal Rival Revenge, Europe. The game stores the volume of the music at 0x030024C0 and passes
    // it to the routine at 0x08026F9A. The handler of the vertical blank calls the frame routine from 0x08029702.
    {.code = "AAOP",
     .play = 0x08103770,
     .init = 0x08103840,
     .memory = 0x03005D50,
     .frame = 0x081037D8,
     .frame_in_handler = true,
     .sound_on = 0x081037A8,
     .orders = kAeroOrders,
     .ends = kAeroEnds},
    // Aero the Acro-Bat: Rascal Rival Revenge, US: the same code 0x358 bytes lower. The game passes the volume of the
    // music to the routine at 0x08026CE6. The handler calls the frame routine from 0x0802944E.
    {.code = "AAOE",
     .play = 0x08103418,
     .init = 0x081034E8,
     .memory = 0x03005D50,
     .frame = 0x08103480,
     .frame_in_handler = true,
     .sound_on = 0x08103450,
     .orders = kAeroOrders,
     .ends = kAeroEnds},
    // Zapper, US. The boot (0x08000872 to 0x080008AA) sets the three volumes to 0, and the title screen sets the master
    // volume to full. The main loop calls the frame routine from 0x08000994.
    {.code = "AZPE",
     .play = 0x080E466C,
     .init = 0x080E4780,
     .memory = 0x03000180,
     .frame = 0x080E46DC,
     .sound_on = 0x080E46A8,
     .master_volume = 0x080E46B8,
     .music_volume = 0x080E46C4},
    // Pinball Challenge Deluxe, Europe, with GBAMOD1 modules. The init (0x08004234) takes its buffers from the
    // allocator of the game. The next address of the allocator is 0x03000000 when the game calls the init. The play
    // routine sets the output up when it first runs. Its start routine (0x080044BE) takes the volume of the music from
    // the word at 0x02000434. The game sets that word from its options. The handler of the vertical blank calls the
    // game routine at 0x080088D8. That routine restarts the sound DMA, runs the frame routine (0x080048A0) and swaps
    // the buffers.
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

        // Some games switch the sound off and on again after the init of the driver, such as Aero at 0x0802C45C.
        // mGBA 0.11 then clears the volumes in SOUNDCNT_H, and the FIFOs play at 50%. The stub makes the same store to
        // SOUNDCNT_X for such a game. The set then plays at the level of the game, whichever way a player treats
        // SOUNDCNT_H.
        if (info_.sound_off_call)
        {
            plan.start.push_back(Operation::Store(agblib::agb::kRegSoundCntX, {0}, 2));
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

    std::optional<long long> LastSoundMs(int song) const override
    {
        return EndOfSong(profile_.ends, song);
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
    return FindProfile(kProfiles, code, version, &LogikStateProfile::play, play_routine);
}

std::unique_ptr<Driver> FindLogikState(const agblib::agb::Rom& rom, std::string& error)
{
    logikstate::DriverInfo info;
    if (!logikstate::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    const LogikStateProfile* profile = FindLogikStateProfile(code, version, info.play_routine);
    if (!profile)
    {
        error =
            NoProfileError("Logik State's driver", GameWithRoutine(code, version, "play routine", info.play_routine));
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
