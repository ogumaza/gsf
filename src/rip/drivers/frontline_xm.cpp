// SPDX-License-Identifier: MIT

// Frontline Studios' XM-like player, the newer player of the F U H Sound System. Chicken Shoot and Chicken Shoot 2 play
// their music on it. agblib finds the routines and the tables of the player. It gives the set-up of the player in each
// game, and the songs: the module, the order list, the last order and the master volume of each.
//
// The stub makes the call of the game that sets up the player. It then starts the output and runs the routine of the
// vertical blank once. That routine restarts the sound DMA and sets the samples a frame for the rate. The frames of the
// game do the same before its music starts. The stub then makes the calls of the game that start a song: the load of
// the module, the play of the list, the switch of the music, the master volume and the start of the output, with the
// values in tables by song. Each vertical blank, the interrupt handler of the game calls the routine that restarts the
// DMA on the buffer that the mixer filled and swaps the buffers. The main loop of the game then calls the frame
// routine. That routine runs the player and mixes the next buffer. The stub does the same. No other interrupt touches
// the sound, and the mix finishes within the frame. The stub therefore plays the bytes that the game plays.
//
// The runs of the model run the code of the game on the model of the console, with the same calls. The DMA reads the
// buffers of the mixer in IWRAM, never the cartridge. The library still keeps the code of the player, its tables of
// periods, steps and waves, and the whole of each instrument that a run read part of.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/frontline_xm/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_xm/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace frontline_xm = agblib::frontline_xm;

// The load takes the address of the module header less 4.
constexpr uint32_t kLoadOffset = 4;

// The sizes of the tables of the player: the place in the table of steps for each of 7,680 periods, and 768 steps. The
// three waves have frontline_xm::kWaveSize bytes each.
constexpr uint32_t kPeriodsSize = 2 * 7680;
constexpr uint32_t kStepsSize = 4 * 768;

// The bytes after the mix routine, the highest routine of the player, that contain the tick routines, the handlers of
// the effects and their literals.
constexpr uint32_t kCodeTail = 0x1000;

class FrontlineXmDriver : public Driver
{
public:
    FrontlineXmDriver(const agblib::agb::Rom& rom, frontline_xm::DriverInfo info)
        : rom_(rom), info_(std::move(info)), spans_(DriverSpans())
    {
        for (const std::string& warning : info_.warnings)
        {
            info_.log.push_back("warning: " + warning);
        }
    }

    std::string Name() const override
    {
        return "Frontline Studios' XM-like player";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    // The songs follow the list of agblib: the song of the title screen, then the lists of the levels.
    int SongCount() const override
    {
        return int(info_.songs.size());
    }

    bool HasSong(int song) const override
    {
        return song >= 0 && song < SongCount();
    }

    StubPlan Plan() const override
    {
        std::vector<uint32_t> modules;
        std::vector<uint32_t> lists;
        std::vector<uint32_t> lasts;
        std::vector<uint32_t> volumes;
        for (const frontline_xm::Song& song : info_.songs)
        {
            modules.push_back(info_.modules[size_t(song.module)].address - kLoadOffset);
            lists.push_back(song.list);
            lasts.push_back(song.last);
            volumes.push_back(song.volume);
        }

        const frontline_xm::Library& lib = info_.library;
        StubPlan plan;
        plan.start = {
            Operation::Call(info_.sound_init),
            Operation::Call(lib.output),
            Operation::Call(lib.swap),
            Operation::Call(lib.load, {SongTable(modules), {0}}),
            Operation::Call(lib.play, {SongTable(lists), {0}, SongTable(lasts)}),
            Operation::Call(lib.music, {{1}}),
            Operation::Call(lib.volume, {SongTable(volumes)}),
            Operation::Call(lib.output),
        };
        plan.handlers = {{kIrqVblank, {Operation::Call(lib.swap)}, 1.0}};
        plan.frame = {Operation::Call(lib.frame)};
        return plan;
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        const frontline_xm::Library& lib = info_.library;
        MarkCodeSpan(reads, {lib.load, lib.play, lib.music, lib.volume, lib.output, lib.frame, lib.swap, lib.mix},
                     kCodeTail);
        KeepWholeSpans(reads, spans_);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::frontline_xm::InspectSong(rom_, info_, song, settings));
    }

private:
    // The tables of the player, and the spans of the instruments of each module: each entry of the instrument table,
    // and the record of the instrument with its points. The entries run until one that doesn't name an instrument in
    // the cartridge.
    std::vector<RomSpan> DriverSpans() const
    {
        const frontline_xm::Library& lib = info_.library;
        std::vector<RomSpan> spans = {{lib.periods, kPeriodsSize}, {lib.steps, kStepsSize}};
        for (const uint32_t wave : lib.waves)
        {
            spans.push_back({wave, frontline_xm::kWaveSize});
        }

        const auto instrument = [&](uint32_t record) -> std::vector<RomSpan>
        {
            if (!rom_.Contains(record, frontline_xm::kInstrumentPoints))
            {
                return {};
            }

            const uint32_t end = std::max(rom_.U32(record + frontline_xm::kInstrumentLength),
                                          rom_.U32(record + frontline_xm::kInstrumentLoopEnd));
            const uint32_t points =
                uint32_t((uint64_t(end) + (1u << frontline_xm::kPositionShift) - 1) >> frontline_xm::kPositionShift);
            const uint32_t size = frontline_xm::kInstrumentPoints + points;
            if (!rom_.Contains(record, size))
            {
                return {};
            }

            return {{record, size}};
        };
        for (const frontline_xm::Module& module : info_.modules)
        {
            AddInstrumentSpans(rom_, module.instruments, false, instrument, spans);
        }

        return spans;
    }

    const agblib::agb::Rom& rom_;
    frontline_xm::DriverInfo info_;
    std::vector<RomSpan> spans_;
};

} // namespace

std::unique_ptr<Driver> FindFrontlineXm(const agblib::agb::Rom& rom, std::string& error)
{
    frontline_xm::DriverInfo info;
    if (!frontline_xm::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    return std::make_unique<FrontlineXmDriver>(rom, std::move(info));
}

} // namespace gsf::rip
