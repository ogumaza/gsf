// SPDX-License-Identifier: MIT

// Nintendo R&D2's driver, as Super Mario Advance 2, 3 and 4 and A Link to the Past play it.
// agblib's detection finds the sequences and the settings that the game's play-music routine gives each of them. A
// profile for each game gives the routines that the game calls: its sound init, the driver's request and the call that
// hands the requests to the next frame, the driver's VBlank routine and its per-frame routine, and the game's
// play-music routine where it has one. Each game's code gives them.
//
// The game calls its sound init. A sequence that the game plays by a music number starts through the game's
// play-music routine, which also sets the voices, the volume and the muted tracks that the game plays it with. In Super
// Mario Advance 2, the routine for the tracks that play only with Yoshi follows, with the player on foot. Any other
// sequence starts through the driver's request on the music player. The stub then hands the request to the driver. The
// handler of the vertical blank runs the VBlank routine, which restarts timer 0 and the output DMAs, and the main loop
// runs the per-frame routine.
//
// gsf rips the sequences, each as the first music number that plays it gives it. With --effects, a game whose profile
// gives its request of a sound effect gets a mini for each sound effect too: a track of one of the sound effects'
// sequences. The stub requests it on the player that the game's tables give it, and hands the request to the driver.

#include "rip/drivers/rd2.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/rd2/driver.h"
#include "agblib/sequence_export/rd2/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace rd2 = agblib::rd2;

// The routines of a game's driver, all Thumb code, and the player that the game plays its music on. `play_music` and
// `yoshi_mute` are 0 in a game without them. `effect_request` is 0 in a game whose sound effects gsf doesn't rip.
struct Profile
{
    const char* code = "";
    uint32_t sound_init = 0;
    uint32_t request = 0;
    uint32_t commit = 0;
    uint32_t vblank = 0;
    uint32_t frame = 0;
    uint32_t music_player = 0x12;
    uint32_t play_music = 0;
    uint32_t yoshi_mute = 0;
    // The driver's request of a sound effect: r0 = the player, r1 = the sequence, r2 = the track.
    uint32_t effect_request = 0;
    std::span<const Rd2EffectTable> effect_tables = {};
};

// A Link to the Past's tables of sound effects. The game's sound effect routine (0x0812A324) takes a sound from the
// first. The third byte of its word is the player, 0 to 14. The routine skips the request while a sound of higher
// priority plays on the player. With a position, it then sets the player's pan and volume (0x0812C8DC, 0x0812C884);
// without one, the pan is the middle (0x40) and the volume full (0x80). The routine at 0x0812A4F8 takes a sound from
// the second, and requests it on player 0x10 or 0x11, whichever of its two slots is free.
constexpr Rd2EffectTable kAzleEffectTables[] = {{0x08198C94, 125}, {0x08198E88, 8, 0x10}};

// A Link to the Past (AZLE) sets its R&D2 driver up only once the player has chosen a file and the game. In mGBA, the
// game then calls its sound init (0x08129E60) and its play-music routine (0x0812A044). The routine looks the music
// number up in a table of sequences (0x08198C50), requests the sequence on music player 0x12 and records the music. It
// sets no voices, volume or muted tracks, so the stub makes the request itself for every sequence. The game requests
// its sound effects only from the first sound effects' sequence, through the driver's request at 0x0812C73C. The only
// routine that requests the second sequence's (0x0812A5F8) has no caller.
constexpr Profile kProfiles[] = {
    {"AA2E", 0x0809B258, 0x0809D840, 0x0809D82C, 0x0809BA74, 0x0809BA80, 0x13, 0x0809B348, 0x0809B3CC},
    {"A3AE", 0x0812C214, 0x0812EEBC, 0x0812EEA8, 0x0812CF50, 0x0812CF5C, 0x13, 0x0812C3B4},
    {"AX4E", 0x080E4414, 0x080E6D94, 0x080E6D80, 0x080E4DDC, 0x080E4DE8, 0x13, 0x080E4598},
    {"AZLE", 0x08129E60, 0x0812C718, 0x0812C704, 0x0812A798, 0x0812A7A4, 0x12, 0, 0, 0x0812C73C, kAzleEffectTables},
};

// The player of a sound effect that the game's tables don't name.
constexpr int kDefaultEffectPlayer = 0;

// A bit of an entry of a game's table of sound effects: the game plays its track or one of the next two in turn.
constexpr uint32_t kTurnBit = 0x08000000;

// Whether a conversion's report warns of a sample that can't be read: one whose points lie outside the ROM. The driver
// plays such a sample from whatever the address holds on the console.
bool PlaysUnreadableSample(const agblib::sequence_export::agb::SongReport& report)
{
    for (const std::string& warning : report.warnings)
    {
        if (warning.find("can't be read") != std::string::npos)
        {
            return true;
        }
    }

    return false;
}

class Rd2Driver : public Driver
{
public:
    Rd2Driver(const agblib::agb::Rom& rom, rd2::DriverInfo info, const Profile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile)
    {
        info_.log.push_back(std::string("the game's routines: ") + profile_.code);
        if (profile_.effect_request)
        {
            FindEffects();
        }
    }

    std::string Name() const override
    {
        return "Nintendo R&D2's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return rd2::SongCount(info_);
    }

    bool HasSong(int song) const override
    {
        return song >= 0 && size_t(song) < info_.sequence_addresses.size() && !rd2::SongEffect(info_, song);
    }

    // A sound effect of a game with a request of sound effects in its profile, unless it plays a sample that can't be
    // read: the set can't play what the console's memory holds there.
    bool HasEffect(int song) const override
    {
        const int effect = song - int(info_.sequence_addresses.size());
        return effect >= 0 && size_t(effect) < effects_.size() && effects_[size_t(effect)];
    }

    StubPlan Plan() const override
    {
        return MakePlan(false);
    }

    StubPlan PlanWithEffects() const override
    {
        return MakePlan(profile_.effect_request != 0);
    }

    // The titles of the sound effects that no code of the game requests, and the order of the set: the music by number,
    // the sound effects that the game requests, then the others.
    const GameTags* Tags() const override
    {
        return profile_.effect_request ? &tags_ : nullptr;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::rd2::InspectSong(rom_, info_, song, settings));
    }

private:
    // The plan of the stub: the game's sound init, then a sequence's start through the play-music routine or the
    // request, or with `effects` a sound effect's request on its player, then the call that hands the request over.
    StubPlan MakePlan(bool effects) const
    {
        // Each song's routine: the play-music routine with its music number, the request, or the request of a sound
        // effect.
        std::vector<uint32_t> play_music;
        std::vector<uint32_t> yoshi_mute;
        std::vector<uint32_t> request;
        std::vector<uint32_t> music;
        std::vector<uint32_t> effect_request;
        std::vector<uint32_t> players;
        std::vector<uint32_t> sequences;
        std::vector<uint32_t> tracks;
        for (int song = 0; song < SongCount(); song++)
        {
            const rd2::MusicSettings settings = rd2::VersionSettings(info_, song, 0);
            const bool by_music = settings.music >= 0 && profile_.play_music;
            const rd2::Effect* effect = effects ? rd2::SongEffect(info_, song) : nullptr;
            play_music.push_back(by_music ? profile_.play_music | 1 : 0);
            yoshi_mute.push_back(by_music && profile_.yoshi_mute ? profile_.yoshi_mute | 1 : 0);
            request.push_back(by_music || effect ? 0 : profile_.request | 1);
            music.push_back(by_music ? uint32_t(settings.music) : 0);
            effect_request.push_back(effect ? profile_.effect_request | 1 : 0);
            players.push_back(effect ? uint32_t(EffectPlayer(*effect)) : 0);
            sequences.push_back(effect ? uint32_t(effect->sequence) : 0);
            tracks.push_back(effect ? uint32_t(effect->track) : 0);
        }

        StubPlan plan;
        plan.start = {
            Operation::Call(profile_.sound_init | 1),
            Operation::CallFromTable(play_music, {SongTable(music)}),
            Operation::CallFromTable(yoshi_mute, {{0}}),
            Operation::CallFromTable(request, {{profile_.music_player}, {0, true}}),
        };
        if (effects)
        {
            plan.start.push_back(Operation::CallFromTable(
                effect_request, {SongTable(players), SongTable(sequences), SongTable(tracks)}));
        }

        plan.start.push_back(Operation::Call(profile_.commit | 1));
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(profile_.vblank | 1)}}};
        plan.frame = {Operation::Call(profile_.frame | 1)};
        return plan;
    }

    // The player that the game's tables give a sound effect, or -1 for a sound effect that they don't name. Only the
    // first sound effects' sequence has tables.
    int TablePlayer(const rd2::Effect& effect) const
    {
        if (effect.sequence == 0 && size_t(effect.track) < players_.size())
        {
            return players_[size_t(effect.track)];
        }

        return -1;
    }

    // The player that the stub requests a sound effect on: the one that the game's tables give, or else
    // kDefaultEffectPlayer.
    int EffectPlayer(const rd2::Effect& effect) const
    {
        const int player = TablePlayer(effect);
        return player >= 0 ? player : kDefaultEffectPlayer;
    }

    // Finds the sound effects that a rip can have, the player of each, and the titles and order of the set.
    void FindEffects()
    {
        int first_tracks = 0;
        for (const rd2::Effect& effect : info_.sound_effects)
        {
            first_tracks += effect.sequence == 0 ? 1 : 0;
        }

        players_ = Rd2EffectPlayers(rom_, profile_.effect_tables, first_tracks);
        const int sequences = int(info_.sequence_addresses.size());
        const agblib::sequence_export::agb::ConvertSettings settings;
        std::vector<int> unused;
        tags_.titles.resize(size_t(SongCount()));
        for (int song = 0; song < sequences; song++)
        {
            tags_.order.push_back(song);
        }

        for (size_t i = 0; i < info_.sound_effects.size(); i++)
        {
            const rd2::Effect& effect = info_.sound_effects[i];
            const int song = sequences + int(i);
            const bool readable = effect.address && !PlaysUnreadableSample(agblib::sequence_export::rd2::InspectSong(
                                                        rom_, info_, song, settings));
            effects_.push_back(readable);
            const bool used = TablePlayer(effect) >= 0;
            if (!used)
            {
                tags_.titles[size_t(song)] = "Effect " + std::to_string(song) + " (Unused)";
                unused.push_back(song);
            }
            else
            {
                tags_.order.push_back(song);
            }
        }

        tags_.order.insert(tags_.order.end(), unused.begin(), unused.end());
    }

    const agblib::agb::Rom& rom_;
    rd2::DriverInfo info_;
    const Profile& profile_;
    std::vector<int> players_;  // the player of each track of the first sound effects' sequence, or -1
    std::vector<bool> effects_; // whether each sound effect can be ripped
    GameTags tags_;
};

} // namespace

std::vector<int> Rd2EffectPlayers(const agblib::agb::Rom& rom, std::span<const Rd2EffectTable> tables, int tracks)
{
    std::vector<int> players(size_t(std::max(tracks, 0)), -1);
    for (const Rd2EffectTable& table : tables)
    {
        for (uint32_t i = 0; i < table.count && rom.Contains(table.address + 4 * i, 4); i++)
        {
            const uint32_t entry = rom.U32(table.address + 4 * i);
            const int player =
                table.player == Rd2EffectTable::kPlayerInEntry ? int((entry >> 16) & 0xFF) : table.player;
            const uint32_t turns = (entry & kTurnBit) ? 3 : 1;
            for (uint32_t turn = 0; turn < turns; turn++)
            {
                const uint32_t track = (entry & 0xFFFF) + turn;
                if (track < players.size() && players[track] < 0)
                {
                    players[track] = player;
                }
            }
        }
    }

    return players;
}

std::unique_ptr<Driver> FindRd2(const agblib::agb::Rom& rom, std::string& error)
{
    rd2::DriverInfo info;
    if (!rd2::DetectDriver(rom, rd2::DriverOverrides{}, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    for (const Profile& profile : kProfiles)
    {
        if (code == profile.code)
        {
            return std::make_unique<Rd2Driver>(rom, std::move(info), profile);
        }
    }

    error = "Nintendo R&D2's driver: the game " + code +
            " is in no profile that gsf has, so the routines that it "
            "calls are unknown";
    return nullptr;
}

} // namespace gsf::rip
