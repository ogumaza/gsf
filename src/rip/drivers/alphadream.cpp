// SPDX-License-Identifier: MIT

// AlphaDream's driver, as Mario & Luigi: Superstar Saga and two Hamtaro games play it. agblib finds the routines from
// the code: the mixer's setup, the driver's init and its state, the start of a song on a player, the per-frame
// routine, the mix and VBlank's swap of the mixer's halves.
//
// The game stores the state's address at 0x03007FF0 and calls the setup and the init in its order. It starts its music
// on player 1. VBlank's interrupt swaps the halves, and the main loop runs the per-frame routine, then the mix.

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/alphadream/driver.h"
#include "agblib/alphadream/game_driver.h"
#include "agblib/sequence_export/alphadream/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace alphadream = agblib::alphadream;

// The player that the game starts its music on.
constexpr uint32_t kMusicPlayer = 1;

// Where the game keeps the address of the driver's state.
constexpr uint32_t kStatePointer = 0x03007FF0;

class AlphaDreamDriver : public Driver
{
public:
    AlphaDreamDriver(const agblib::agb::Rom& rom, alphadream::DriverInfo info, alphadream::GameRoutines routines)
        : rom_(rom), info_(std::move(info)), routines_(routines)
    {
    }

    std::string Name() const override
    {
        return "AlphaDream's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    bool HasSong(int song) const override
    {
        alphadream::Song read;
        return alphadream::ReadSong(rom_, info_, song, read);
    }

    StubPlan Plan() const override
    {
        const alphadream::GameRoutines& r = routines_;
        const Operation setup = Operation::Call(r.setup | 1);
        const Operation init = Operation::Call(r.init | 1, {{r.state}});

        StubPlan plan;
        plan.start.push_back(Operation::Store(kStatePointer, {r.state}, 4));
        plan.start.push_back(r.init_first ? init : setup);
        plan.start.push_back(r.init_first ? setup : init);
        plan.start.push_back(Operation::Call(r.start | 1, {{kMusicPlayer}, {0, true}}));
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(r.swap | 1)}}};
        plan.frame = {Operation::Call(r.frame | 1), Operation::Call(r.mix | 1)};
        return plan;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::alphadream::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    alphadream::DriverInfo info_;
    alphadream::GameRoutines routines_;
};

} // namespace

std::unique_ptr<Driver> FindAlphaDream(const agblib::agb::Rom& rom, std::string& error)
{
    alphadream::DriverInfo info;
    if (!alphadream::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const std::optional<alphadream::GameRoutines> routines = alphadream::FindGameRoutines(rom, info);
    if (!routines)
    {
        error = "AlphaDream's driver: the routines that start and run a song aren't laid out as in the known versions";
        return nullptr;
    }

    return std::make_unique<AlphaDreamDriver>(rom, std::move(info), *routines);
}

} // namespace gsf::rip
