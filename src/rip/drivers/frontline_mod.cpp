// SPDX-License-Identifier: MIT

// Frontline Studios' MOD player, the older player of the F U H Sound System. Hardcore Pool, Garfield: The Search for
// Pooky and Games Explosion! play their music on it. agblib finds the player and its mixer in the image of IWRAM that
// the start-up code of the game copies. It gives the set-up of the mixer in each game, the start routine and the songs:
// the module, the order and the fade step of each start.
//
// The stub makes the copy of the image that the start-up code makes, and the calls of the game that set up the mixer
// and the sound registers. It then starts the module of the song at its order through the start routine of the game,
// with the values in tables by song. Each vertical blank, the interrupt handler of the game calls a game routine. That
// routine restarts DMA 1 on the buffer that the mixer filled in the last frame, runs the player, then runs the mixer.
// The handler of the stub calls the same routine. No other interrupt touches the sound, and the mix finishes within the
// frame. The stub therefore plays the bytes that the game plays.
//
// The runs of the model run the code of the game on the model of the console, with the same calls. The DMA reads the
// buffers of the mixer in IWRAM, never the cartridge. The library still keeps the whole table of periods, and the whole
// of each sample, instrument record and sample record that a run read part of.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "agblib/frontline_mod/driver.h"
#include "agblib/sequence_export/agb/settings.h"
#include "agblib/sequence_export/frontline_mod/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace frontline_mod = agblib::frontline_mod;

using agblib::agb::Hex;
using agblib::agb::kIwramBase;

// The game routine that the interrupt handler calls at each vertical blank, by the game code and the version of the
// agblib entry for the game (DriverInfo::game_code and game_version). A release that differs from another only in the
// cartridge header shares the entry of that release. The routine reads the context of the mixer, the address of the
// buffers, the player and the mixer from its literals. The driver compares them with what agblib found. Hardcore Pool
// and Garfield have the same Thumb routine at 0x08000190, called from the handler of the game. Games Explosion! has an
// ARM routine at 0x0803D38C that calls the restart of the DMA at 0x0803D24C.
struct FrameRoutine
{
    std::string_view code;
    uint8_t version = 0;
    uint32_t routine = 0; // Thumb with bit 0 set
    uint32_t context = 0; // the literals: the context, the buffers, the player and the mixer
    uint32_t output = 0;
    uint32_t player = 0;
    uint32_t mixer = 0;
};

constexpr FrameRoutine kFrameRoutines[] = {
    {"BHOP", 0, 0x08000191, 0x080001D8, 0x080001DC, 0x080001E8, 0x080001EC},
    {"BGOE", 0, 0x08000191, 0x080001D8, 0x080001DC, 0x080001E8, 0x080001EC},
    {"BG7E", 0, 0x0803D38C, 0x0803D3C8, 0x0803D3CC, 0x0803D3D0, 0x0803D3D4},
};

// The table of periods: a 16.16 step for each period index.
constexpr uint32_t kPeriodCount = 7680;

// The sizes of an instrument record and of a sample record.
constexpr uint32_t kInstrumentSize = 16;
constexpr uint32_t kSampleSize = 8;

// The songs follow the numbering of agblib: module by module, the starts that the game makes, then the parts that no
// start reaches.
class FrontlineModDriver : public SongListDriver<frontline_mod::DriverInfo>
{
public:
    FrontlineModDriver(const agblib::agb::Rom& rom, frontline_mod::DriverInfo info, uint32_t frame)
        : SongListDriver(std::move(info)), rom_(rom), frame_(frame), samples_(DriverSamples())
    {
        info_.log.push_back("start " + Hex(info_.start) + ", the game's routine of each vertical blank " + Hex(frame_));
        LogWarnings();

        // The songs that no call of the game starts are titled "(Unused)" and follow the others.
        const auto started = [&](int song)
        {
            return !info_.songs[size_t(song)].unanchored;
        };
        AddSongsUnusedLast(tags_, 0, int(info_.songs.size()), "Song", started);
    }

    std::string Name() const override
    {
        return "Frontline Studios' MOD player";
    }

    StubPlan Plan() const override
    {
        std::vector<uint32_t> modules;
        std::vector<uint32_t> orders;
        std::vector<uint32_t> steps;
        for (const frontline_mod::Song& song : info_.songs)
        {
            modules.push_back(info_.modules[size_t(song.module)].address);
            orders.push_back(song.order);
            steps.push_back(uint32_t(song.step));
        }

        StubPlan plan;
        plan.start.push_back(Operation::Copy(info_.image_base + (info_.copy_start - kIwramBase), info_.copy_start,
                                             info_.copy_end - info_.copy_start));
        for (const frontline_mod::GameCall& call : info_.init)
        {
            plan.start.push_back(Operation::Call(call.routine, Constants(call.arguments)));
        }
        plan.start.push_back(Operation::Call(info_.start, {SongTable(modules), SongTable(orders), SongTable(steps)}));
        plan.handlers = {{kIrqVblank, {Operation::Call(frame_)}, 1.0}};
        return plan;
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        const RomSpan periods = {info_.periods, 4 * kPeriodCount};
        KeepWholeSpans(reads, std::span<const RomSpan>(&periods, 1));
        KeepWholeSpans(reads, samples_);
    }

    // The titles of the songs that no call of the game starts, and the order of the set: the game's songs, then those.
    const GameTags* Tags() const override
    {
        return &tags_;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::frontline_mod::InspectSong(rom_, info_, song, settings));
    }

private:
    // The spans of the instruments of each module: each entry of the instrument table, the instrument record, the
    // sample record and the points. The entries run until one that doesn't name a sample in the cartridge. Entry 0 may
    // name none: instrument 0 in a cell keeps the last instrument.
    std::vector<RomSpan> DriverSamples() const
    {
        const auto instrument = [&](uint32_t record) -> std::vector<RomSpan>
        {
            const uint32_t sample =
                rom_.Contains(record, kInstrumentSize) ? rom_.U32(record + frontline_mod::kInstrumentSample) : 0;
            const uint32_t points =
                rom_.Contains(sample, kSampleSize) ? rom_.U32(sample + frontline_mod::kSamplePoints) : 0;
            const uint32_t length = points != 0 ? rom_.U32(sample + frontline_mod::kSampleLength) : 0;
            if (length == 0 || !rom_.Contains(points, length))
            {
                return {};
            }

            return {{record, kInstrumentSize}, {sample, kSampleSize}, {points, length}};
        };
        std::vector<RomSpan> spans;
        for (const frontline_mod::Module& module : info_.modules)
        {
            AddInstrumentSpans(rom_, module.instruments, true, instrument, spans);
        }

        return spans;
    }

    const agblib::agb::Rom& rom_;
    uint32_t frame_ = 0;
    std::vector<RomSpan> samples_;
    GameTags tags_;
};

// Returns the game routine of each vertical blank, or 0 with `error` set when the table has none for the game, or when
// its literals don't name what agblib found.
uint32_t FindFrameRoutine(const agblib::agb::Rom& rom, const frontline_mod::DriverInfo& info, std::string& error)
{
    for (const FrameRoutine& entry : kFrameRoutines)
    {
        if (entry.code != info.game_code || entry.version != info.game_version)
        {
            continue;
        }

        for (const auto& [literal, value] :
             {std::pair{entry.context, info.context}, std::pair{entry.output, info.output},
              std::pair{entry.player, info.player}, std::pair{entry.mixer, info.mixer}})
        {
            if (!rom.Contains(literal, 4) || rom.U32(literal) != value)
            {
                error = "Frontline Studios' MOD player: the routine at " + Hex(entry.routine) + ": its literal at " +
                        Hex(literal) + " isn't " + Hex(value);
                return 0;
            }
        }

        return entry.routine;
    }

    error = "Frontline Studios' MOD player: gsf doesn't know the routine that runs the player in game code " +
            info.game_code;
    return 0;
}

} // namespace

std::unique_ptr<Driver> FindFrontlineMod(const agblib::agb::Rom& rom, std::string& error)
{
    frontline_mod::DriverInfo info;
    if (!frontline_mod::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const uint32_t frame = FindFrameRoutine(rom, info, error);
    if (frame == 0)
    {
        return nullptr;
    }

    return std::make_unique<FrontlineModDriver>(rom, std::move(info), frame);
}

} // namespace gsf::rip
