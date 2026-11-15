// SPDX-License-Identifier: MIT

// MP2K, the driver of Nintendo's SDK for the GBA. agblib's detection finds the song table, and FindGameRoutines() the
// routines from their shapes: the init, m4aSongNumStart, SoundMain and m4aSoundVSync.
//
// The game calls the init, and starts a song with m4aSongNumStart. The handler of the vertical blank calls
// m4aSoundVSync, which restarts the sound DMA at the buffer's end, and then SoundMain, which runs the players and the
// mixer for the frame.
//
// The game's code can set the driver's mode again after the init (DriverInfo::game_modes). Some games also set up a
// song before they start it, with another number of DirectSound channels or a reverb (song_settings, and the channels
// of SongChannels()). One gives the player a volume after the start (SongVolume()). The stub makes the same calls of
// m4aSoundMode, m4aMPlayImmInit and m4aMPlayVolumeControl. A table by song gives each call its values.
//
// In Camelot's version of the driver, the boot code copies a mixer to IWRAM. A sound module in the game's code wraps
// the driver: an init, a routine that plays a sound by number, and a per-frame routine that moves the music's volume
// and tempo toward the module's targets. The handler of the vertical blank calls m4aSoundVSync. Each frame, the main
// loop copies the DMA counter for SoundMain and calls the per-frame routine, SoundMain and the mixer's output stage. A
// profile of each game gives the module's routines.

#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/thumb.h"
#include "agblib/mp2k/driver.h"
#include "agblib/mp2k/game_driver.h"
#include "agblib/mp2k/song.h"
#include "agblib/sequence_export/mp2k/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace mp2k = agblib::mp2k;

// How far past the highest routine that the stub calls the driver's code can reach: the SDK's library places CgbSound
// and the commands' routines after m4aSongNumStart.
constexpr uint32_t kCodeTail = 0x2000;

// The tracks that a call of m4aMPlayVolumeControl changes: all of them.
constexpr uint32_t kAllTracks = 0xFFFF;

// The bytes of an entry of the music player table: the player's MusicPlayerInfo, its tracks and their count.
constexpr uint32_t kPlayerEntrySize = 12;

// The calls of m4aSongNumStart, m4aMPlayImmInit and m4aMPlayVolumeControl in a routine of the game that starts a song
// and sets the volume of its player. agblib doesn't find the two volume routines. A row matches only where its first
// call is one of m4aSongNumStart.
struct VolumeCalls
{
    std::string_view game_code;
    uint32_t start_call;
    uint32_t imm_init_call;
    uint32_t volume_call;
};

constexpr VolumeCalls kVolumeCalls[] = {
    // Steel Empire: the routine at 0x08005C54 starts a song, then gives its player the volume of the song's halfword
    // in the table at 0x080460FC for all tracks (0xFFFF).
    {"BKTP", 0x08005CC4, 0x08005CDE, 0x08005CF8},
};

// The routines of Camelot's sound module in a game, where the module's table of exports holds them: a veneer of
// ldr r4, =routine; bx r4 for each, 8 bytes apart.
struct CamelotModule
{
    std::string_view game_code;
    uint32_t exports;
    uint32_t init;  // the module's init: export 1, which calls m4aSoundInit
    uint32_t play;  // the routine that plays a sound by number: export 2
    uint32_t frame; // the per-frame routine: export 17
};

constexpr CamelotModule kCamelotModules[] = {
    // Golden Sun: The Lost Age: the boot code calls export 1 at 0x0801322A, and the game plays its music and sounds
    // through export 2 (1,032 calls). The main loop's sound routine at 0x080134B0 calls export 17, then SoundMain
    // (0x08000630) and the output stage.
    {"AGFE", 0x081C0000, 0x081C0C1C, 0x081C0CB0, 0x081C0E30},
};

// The module of the game in `rom`, where its table of exports gives the profile's routines, or nothing.
std::optional<CamelotModule> FindCamelotModule(const agblib::agb::Rom& rom)
{
    constexpr uint32_t kVeneer = 0x47204C00; // ldr r4, [pc, #0]; bx r4
    const auto exported = [&rom](uint32_t exports, int index, uint32_t routine)
    {
        const uint32_t at = exports + 8 * uint32_t(index);
        return rom.Contains(at, 8) && rom.U32(at) == kVeneer && rom.U32(at + 4) == (routine | 1);
    };
    for (const CamelotModule& game : kCamelotModules)
    {
        if (rom.GameCode() == game.game_code && exported(game.exports, 1, game.init) &&
            exported(game.exports, 2, game.play) && exported(game.exports, 17, game.frame))
        {
            return game;
        }
    }

    return std::nullopt;
}

// What the game's code gives each song: a mode for m4aSoundMode before the start (0 for no call), and the player and
// the volume after it (a volume of kFullPlayerVolume for no call).
struct SongSetup
{
    std::vector<uint32_t> modes;
    std::vector<uint32_t> players;
    std::vector<uint32_t> volumes;
    bool any_mode = false;
    bool any_volume = false;
};

SongSetup SetupOfSongs(const agblib::agb::Rom& rom, const mp2k::DriverInfo& info)
{
    SongSetup setup;
    for (int song = 0; song < info.song_count; song++)
    {
        uint32_t mode = 0;
        for (const mp2k::SongSetting& setting : info.song_settings)
        {
            if (setting.song == song)
            {
                mode = setting.mode;
                break;
            }
        }

        const int channels = mp2k::SongChannels(rom, info, song);
        if (!(mode & 0xF00) && channels != info.max_channels)
        {
            mode |= uint32_t(channels) << 8;
        }

        const uint32_t entry = info.song_table + 8 * uint32_t(song);
        const uint32_t player = info.player_table + kPlayerEntrySize * rom.U16(entry + 4);
        const int volume = mp2k::SongVolume(info, song);
        setup.modes.push_back(mode);
        setup.players.push_back(rom.Contains(player, 4) ? rom.U32(player) : 0);
        setup.volumes.push_back(uint32_t(volume));
        setup.any_mode = setup.any_mode || mode != 0;
        setup.any_volume = setup.any_volume || volume != mp2k::kFullPlayerVolume;
    }

    return setup;
}

// m4aMPlayImmInit and m4aMPlayVolumeControl.
struct VolumeRoutines
{
    uint32_t imm_init = 0;
    uint32_t volume_control = 0;
};

// The volume routines that the game in `rom` calls, from its row of kVolumeCalls, or nothing.
std::optional<VolumeRoutines> FindVolumeRoutines(const agblib::agb::Rom& rom, const mp2k::GameRoutines& routines)
{
    for (const VolumeCalls& game : kVolumeCalls)
    {
        if (rom.GameCode() == game.game_code && agblib::agb::BlTarget(rom, game.start_call) == routines.song_start)
        {
            const VolumeRoutines found = {agblib::agb::BlTarget(rom, game.imm_init_call),
                                          agblib::agb::BlTarget(rom, game.volume_call)};
            if (found.imm_init && found.volume_control)
            {
                return found;
            }
        }
    }

    return std::nullopt;
}

// A table of `routine` for the songs where `values` differs from `none`, and 0 for the others.
std::vector<uint32_t> RoutineWhere(uint32_t routine, const std::vector<uint32_t>& values, uint32_t none)
{
    std::vector<uint32_t> routines;
    for (const uint32_t value : values)
    {
        routines.push_back(value != none ? routine | 1 : 0);
    }

    return routines;
}

class Mp2kDriver : public Driver
{
public:
    Mp2kDriver(const agblib::agb::Rom& rom, mp2k::DriverInfo info, mp2k::GameRoutines routines, SongSetup setup,
               std::optional<VolumeRoutines> volume, std::optional<CamelotModule> camelot, uint32_t sound_info)
        : rom_(rom),
          info_(std::move(info)),
          routines_(routines),
          setup_(std::move(setup)),
          volume_(volume),
          camelot_(camelot),
          sound_info_(sound_info)
    {
    }

    std::string Name() const override
    {
        return "MP2K";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    // An entry of the song table can name a header with no tracks, or a music player that plays none. Such a song is
    // silent in the game too.
    bool HasSong(int song) const override
    {
        mp2k::SongHeader header;
        return mp2k::ReadSong(rom_, info_, song, header) && mp2k::SongPlayer(rom_, info_, song).track_count > 0;
    }

    StubPlan Plan() const override
    {
        if (camelot_)
        {
            return CamelotPlan();
        }

        StubPlan plan;
        plan.start = {Operation::Call(routines_.init | 1)};
        for (const uint32_t mode : routines_.sound_mode ? info_.game_modes : std::vector<uint32_t>{})
        {
            plan.start.push_back(Operation::Call(routines_.sound_mode | 1, {{mode}}));
        }

        if (setup_.any_mode)
        {
            plan.start.push_back(Operation::CallFromTable(RoutineWhere(routines_.sound_mode, setup_.modes, 0),
                                                          {SongTable(setup_.modes)}));
        }

        plan.start.push_back(Operation::Call(routines_.song_start | 1, {{0, true}}));
        if (setup_.any_volume)
        {
            const uint32_t full = mp2k::kFullPlayerVolume;
            plan.start.push_back(Operation::CallFromTable(RoutineWhere(volume_->imm_init, setup_.volumes, full),
                                                          {SongTable(setup_.players)}));
            plan.start.push_back(
                Operation::CallFromTable(RoutineWhere(volume_->volume_control, setup_.volumes, full),
                                         {SongTable(setup_.players), {kAllTracks}, SongTable(setup_.volumes)}));
        }

        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(routines_.vsync | 1), Operation::Call(routines_.main | 1)}}};
        return plan;
    }

    // SoundMain stops mixing early when the vertical count shows that the frame has run late. The model's count
    // stays at the start of the vertical blank, as on the console when the handler runs on time.
    void PrepareModel(ModelRun& run) const override
    {
        run.Gba().HoldVcount(agblib::agb::kVblankLine);
    }

    // CgbSound turns a PSG channel's note off when NR52 shows that the channel's length counter has stopped it. The
    // model has no length counters, so its runs never take that path, and the library keeps the driver's code whole.
    // Camelot's SoundMain and m4aSoundVSync are far from the module and the rest of the driver. Each part gets a
    // separate span. The module's span starts at its table of exports.
    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        if (camelot_)
        {
            MarkCodeSpan(reads, {routines_.main, routines_.vsync}, kCodeTail);
            MarkCodeSpan(reads,
                         {camelot_->exports, camelot_->init, camelot_->play, camelot_->frame, routines_.song_start},
                         kCodeTail);
            return;
        }

        MarkCodeSpan(reads, {routines_.init, routines_.song_start, routines_.main, routines_.vsync}, kCodeTail);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::mp2k::InspectSong(rom_, info_, song, settings));
    }

private:
    // The play routine starts song n of the song table for sound number n, through one of three paths: the music's,
    // the sound effects' (a free player among 4 to 7 for player 7) and a stop of the music before the start (0x50 to
    // 0x63). The numbers that it takes as commands (0x4E, 0x186 and 0x18B in Golden Sun: The Lost Age) are empty songs.
    // The game copies the DMA counter less one, or the period when the counter is 1, which puts the mixer a frame
    // further ahead. The stub copies the counter as it is, as agblib's GameDriver does: the FIFOs come out the same at
    // another offset. The game calls the output stage only when SoundMain returns 0. In mGBA, SoundMain returned 0 in
    // each of 1,800 frames of Golden Sun's song 5, and the stub calls the output stage every frame.
    StubPlan CamelotPlan() const
    {
        StubPlan plan;
        plan.start = {
            Operation::Copy(routines_.copy_source, routines_.copy_destination, routines_.copy_size),
            Operation::Call(camelot_->init | 1),
            Operation::Call(camelot_->play | 1, {{0, true}}),
        };
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(routines_.vsync | 1)}}};
        plan.frame = {
            Operation::Copy(sound_info_ + mp2k::kSoundInfoDmaCounter, routines_.counter_copy, 1),
            Operation::Call(camelot_->frame | 1),
            Operation::Call(routines_.main | 1),
            Operation::Call(routines_.output | 1, {{routines_.output_argument}}),
        };
        return plan;
    }

    const agblib::agb::Rom& rom_;
    mp2k::DriverInfo info_;
    mp2k::GameRoutines routines_;
    SongSetup setup_;
    std::optional<VolumeRoutines> volume_;
    std::optional<CamelotModule> camelot_;
    uint32_t sound_info_ = 0;
};

} // namespace

std::unique_ptr<Driver> FindMp2k(const agblib::agb::Rom& rom, std::string& error)
{
    mp2k::DriverInfo info;
    if (!mp2k::DetectDriver(rom, mp2k::DriverOverrides{}, info, error))
    {
        return nullptr;
    }

    mp2k::GameRoutines routines;
    if (!mp2k::FindGameRoutines(rom, info, routines, error))
    {
        error = "MP2K: " + error;
        return nullptr;
    }

    info.log.push_back("routines: init " + agblib::agb::Hex(routines.init) + ", m4aSoundVSync " +
                       agblib::agb::Hex(routines.vsync) + ", SoundMain " + agblib::agb::Hex(routines.main));
    std::optional<CamelotModule> camelot;
    uint32_t sound_info = 0;
    if (routines.copy_size != 0)
    {
        camelot = FindCamelotModule(rom);
        if (!camelot)
        {
            error = "MP2K: Camelot's version of the driver. gsf has no profile of this game's sound module";
            return nullptr;
        }

        // The SoundInfo's address, which the init stores, gives the DMA counter that the main loop copies.
        try
        {
            sound_info = mp2k::GameDriver(rom, info, routines).SoundInfo();
        }
        catch (const std::runtime_error& e)
        {
            error = std::string("MP2K: ") + e.what();
            return nullptr;
        }

        info.log.push_back("Camelot's sound module: init " + agblib::agb::Hex(camelot->init) + ", play " +
                           agblib::agb::Hex(camelot->play) + ", frame " + agblib::agb::Hex(camelot->frame));
    }

    // Camelot's plan plays each song through the module, which makes the game's set-up.
    SongSetup setup = camelot ? SongSetup{} : SetupOfSongs(rom, info);
    if (setup.any_mode && !routines.sound_mode)
    {
        error = "MP2K: the game's code sets up some songs, and m4aSoundMode wasn't found";
        return nullptr;
    }

    if (!info.game_modes.empty() && !routines.sound_mode && !camelot)
    {
        info.log.push_back(
            "the game's code sets the driver's mode after the init, and m4aSoundMode wasn't found: the "
            "set keeps the init's mode");
    }

    const std::optional<VolumeRoutines> volume = FindVolumeRoutines(rom, routines);
    if (setup.any_volume && !volume)
    {
        error = "MP2K: the game's code sets the volume of some songs, and its routines for that weren't found";
        return nullptr;
    }

    if (routines.sound_mode)
    {
        std::string text = "m4aSoundMode at " + agblib::agb::Hex(routines.sound_mode);
        for (size_t i = 0; i < info.game_modes.size(); i++)
        {
            text += (i == 0 ? "; the game's code sets " : ", ") + agblib::agb::Hex(info.game_modes[i]);
        }
        info.log.push_back(text + (info.game_modes.empty() ? "" : " after the init"));
    }

    return std::make_unique<Mp2kDriver>(rom, std::move(info), routines, std::move(setup), volume, camelot, sound_info);
}

} // namespace gsf::rip
