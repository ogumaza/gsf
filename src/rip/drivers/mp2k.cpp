// SPDX-License-Identifier: MIT

// MP2K, the driver of Nintendo's SDK for the GBA. agblib's detection finds the song table, and FindGameRoutines() the
// routines from their shapes: the init, m4aSongNumStart, SoundMain and m4aSoundVSync.
//
// The game calls the init, and starts a song with m4aSongNumStart. The handler of the vertical blank calls
// m4aSoundVSync, which restarts the sound DMA at the buffer's end, and then SoundMain, which runs the players and the
// mixer for the frame.
//
// gsf leaves out what the game's code gives some songs when it starts them (DriverInfo::song_settings and
// song_channels), and Camelot's version of the driver, whose mixer the game's boot code copies to IWRAM.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
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

class Mp2kDriver : public Driver
{
public:
    Mp2kDriver(const agblib::agb::Rom& rom, mp2k::DriverInfo info, mp2k::GameRoutines routines)
        : rom_(rom), info_(std::move(info)), routines_(routines)
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
        StubPlan plan;
        plan.start = {
            Operation::Call(routines_.init | 1),
            Operation::Call(routines_.song_start | 1, {{0, true}}),
        };
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
    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        MarkCodeSpan(reads, {routines_.init, routines_.song_start, routines_.main, routines_.vsync}, kCodeTail);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::mp2k::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    mp2k::DriverInfo info_;
    mp2k::GameRoutines routines_;
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

    if (routines.copy_size != 0)
    {
        error = "MP2K: Camelot's version of the driver, which gsf doesn't rip yet";
        return nullptr;
    }

    if (!info.song_settings.empty() || !info.song_channels.songs.empty())
    {
        error = "MP2K: the game's code sets up some songs when it starts them, which gsf doesn't follow yet";
        return nullptr;
    }

    return std::make_unique<Mp2kDriver>(rom, std::move(info), routines);
}

} // namespace gsf::rip
