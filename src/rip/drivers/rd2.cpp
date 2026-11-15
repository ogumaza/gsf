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
// gsf rips the sequences, each as the first music number that plays it gives it. The sound effects get no minis.

#include <cstdint>
#include <memory>
#include <optional>
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
// `yoshi_mute` are 0 in a game without them.
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
};

// A Link to the Past (AZLE) sets its R&D2 driver up only once the player has chosen a file and the game. In mGBA, the
// game then calls its sound init (0x08129E60) and its play-music routine (0x0812A044). The routine looks the music
// number up in a table of sequences (0x08198C50), requests the sequence on music player 0x12 and records the music. It
// sets no voices, volume or muted tracks, so the stub makes the request itself for every sequence.
constexpr Profile kProfiles[] = {
    {"AA2E", 0x0809B258, 0x0809D840, 0x0809D82C, 0x0809BA74, 0x0809BA80, 0x13, 0x0809B348, 0x0809B3CC},
    {"A3AE", 0x0812C214, 0x0812EEBC, 0x0812EEA8, 0x0812CF50, 0x0812CF5C, 0x13, 0x0812C3B4},
    {"AX4E", 0x080E4414, 0x080E6D94, 0x080E6D80, 0x080E4DDC, 0x080E4DE8, 0x13, 0x080E4598},
    {"AZLE", 0x08129E60, 0x0812C718, 0x0812C704, 0x0812A798, 0x0812A7A4, 0x12},
};

class Rd2Driver : public Driver
{
public:
    Rd2Driver(const agblib::agb::Rom& rom, rd2::DriverInfo info, const Profile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile)
    {
        info_.log.push_back(std::string("the game's routines: ") + profile_.code);
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

    StubPlan Plan() const override
    {
        // Each song's routine: the play-music routine with its music number, or the request.
        std::vector<uint32_t> play_music;
        std::vector<uint32_t> yoshi_mute;
        std::vector<uint32_t> request;
        std::vector<uint32_t> music;
        for (int song = 0; song < SongCount(); song++)
        {
            const rd2::MusicSettings settings = rd2::VersionSettings(info_, song, 0);
            const bool by_music = settings.music >= 0 && profile_.play_music;
            play_music.push_back(by_music ? profile_.play_music | 1 : 0);
            yoshi_mute.push_back(by_music && profile_.yoshi_mute ? profile_.yoshi_mute | 1 : 0);
            request.push_back(by_music ? 0 : profile_.request | 1);
            music.push_back(by_music ? uint32_t(settings.music) : 0);
        }

        StubPlan plan;
        plan.start = {
            Operation::Call(profile_.sound_init | 1),
            Operation::CallFromTable(play_music, {SongTable(music)}),
            Operation::CallFromTable(yoshi_mute, {{0}}),
            Operation::CallFromTable(request, {{profile_.music_player}, {0, true}}),
            Operation::Call(profile_.commit | 1),
        };
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(profile_.vblank | 1)}}};
        plan.frame = {Operation::Call(profile_.frame | 1)};
        return plan;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::rd2::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    rd2::DriverInfo info_;
    const Profile& profile_;
};

} // namespace

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
