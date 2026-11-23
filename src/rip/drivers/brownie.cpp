// SPDX-License-Identifier: MIT

// Brownie Brown's driver, as Sword of Mana and Magical Vacation play it. agblib finds the tables and the per-frame
// routine of the driver, reads the channels of the sounds and works out the loop or the end of each song. A profile of
// each release gives the routines that the game calls. Their addresses come from the code of the game.
//
// The game calls its sound init, and requests a sound by its number. The driver starts the sound at the end of the next
// frame. The handler of the vertical blank in the game calls the per-frame routine. The sounds that use the PSG
// channels of the music are the music, and each gets a mini. The others are sound effects. With --effects, they get
// minis in a game whose profile has them: the stub requests a sound effect in the same way as the music.
//
// In the Sword of Mana revision, the init sets up Timer 0 and the DMAs of the FIFOs. The game copies the mixer to
// IWRAM. Each time the FIFOs have played 16 points, the DMA interrupts run the mixer once: the mixer acknowledges both.
// The stub runs it for the DMA 1 interrupt. In the Magical Vacation revision, each FIFO plays one sample at the rate of
// its timer. The interrupt of DMA 1 or DMA 2 gives FIFO A or FIFO B the next 4 points of the sample. The handler of the
// vertical blank in the stub runs the per-frame routine with the interrupts off. The handlers of the games do the same.
//
// How far a sample plays in a frame depends on the order of interrupts. In the Magical Vacation revision it depends on
// the rates of the notes too. The model runs neither. The library therefore keeps the whole of each sample that a run
// read part of. It also keeps the code of the driver whole: from the lowest routine in the cartridge that the stub
// calls to the last byte that a run read within kCodeTail bytes after the highest.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/system.h"
#include "agblib/brownie/driver.h"
#include "agblib/sequence_export/brownie/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace brownie = agblib::brownie;

// The routines of a release and the state that the game sets before its first sound. A routine is ARM code unless bit 0
// of its address is set.
struct BrownieProfile
{
    std::string_view code = {};
    uint8_t version = 0;
    uint32_t frame = 0;              // the driver's per-frame routine, as detection finds it
    uint32_t init = 0;               // the game's sound init
    uint32_t request = 0;            // the driver's request of a sound: r0 = the sound
    std::vector<uint32_t> fifo_irqs; // the routines of the DMA 1 interrupt and, in one game, the DMA 2 interrupt
    uint16_t waitcnt = 0;            // WAITCNT as the game sets it
    bool effects = false;            // whether gsf rips the sound effects
};

// Sword of Mana, US. The sound init of the game (0x080A1560) calls the init of the driver. It then copies the mixer
// from 0x080A0B74 and 0x080A1040 to 0x03003150 and 0x03003750 with DMA 3. The game requests a sound through 0x080A153C.
// That routine drops sounds 0 to 0x2F, the music, while the byte at 0x02000000 is set. Otherwise it calls the request
// of the driver. The stub calls that request directly. The interrupts of the game run the mixer at 0x03003150 about 17
// times a frame. The handler of the vertical blank (0x08001038) calls the per-frame routine through 0x080A15A0.
//
// Magical Vacation, Japan. The sound init of the game is 0x0805B574. The game calls it in frames 2 and 614 after the
// boot. It requests a sound through 0x0805B564, and that routine calls the request of the driver. The routines of the
// DMA 1 and DMA 2 interrupts go through 0x0805B58C and 0x0805B598.
//
// Both games set WAITCNT to 0x45B7. The sound effects of Sword of Mana play on the channels of the driver for sound
// effects, through the same request.
const std::vector<BrownieProfile>& Profiles()
{
    static const std::vector<BrownieProfile> kProfiles = {
        {
            .code = "AVSE",
            .frame = 0x080A1900,
            .init = 0x080A1561,
            .request = 0x080A16EC,
            .fifo_irqs = {0x03003150},
            .waitcnt = 0x45B7,
            .effects = true,
        },
        {
            .code = "AMVJ",
            .frame = 0x0805C2C8,
            .init = 0x0805B575,
            .request = 0x0805BEAC,
            .fifo_irqs = {0x0805BFD8, 0x0805C150},
            .waitcnt = 0x45B7,
        },
    };
    return kProfiles;
}

// The control of Timer 0, the control of Timer 1 one timer further on, and the bit that runs a timer.
constexpr uint32_t kTimerControl = agblib::agb::kRegTimer0Reload + 2;
constexpr uint32_t kTimerStride = 4;
constexpr uint32_t kTimerOn = 0x80;

// The points that the mixer makes for each FIFO at a time, and that each interrupt gives a FIFO in the Magical Vacation
// revision.
constexpr uint32_t kMixerPoints = 16;
constexpr uint32_t kInterruptPoints = 4;

// How far the code and the tables of the driver go past the start of the highest routine that the stub calls: the data
// reader, the routines of the commands and their table.
constexpr uint32_t kCodeTail = 0x1000;

// The points of the sample whose record is at `record`, or nothing where `record` isn't the record of a sample. Each
// record that the tables of the driver name is just before its points.
std::optional<RomSpan> SampleAt(const agblib::agb::Rom& rom, uint32_t record)
{
    if (!rom.Contains(record, brownie::kSampleRecordBytes))
    {
        return std::nullopt;
    }

    const uint32_t start = rom.U32(record);
    const uint32_t end = rom.U32(record + 4);
    const uint32_t loop = rom.U32(record + 8);
    if (start != record + brownie::kSampleRecordBytes || loop < start || end < loop ||
        !rom.Contains(start, end - start))
    {
        return std::nullopt;
    }

    return RomSpan{start, end - start};
}

// The points of each sample in the sample table. The table has no count: it ends before the first entry that names no
// sample, or a set with an entry that names none.
std::vector<RomSpan> SamplePoints(const agblib::agb::Rom& rom, const brownie::DriverInfo& info)
{
    const bool sets = info.revision == brownie::Revision::kSwordOfMana;
    std::set<std::pair<uint32_t, uint32_t>> found;
    for (uint32_t entry = info.sample_table; rom.Contains(entry, 4); entry += 4)
    {
        std::vector<uint32_t> records;
        if (!sets)
        {
            records.push_back(rom.U32(entry));
        }
        else if (const uint32_t set = rom.U32(entry); rom.Contains(set, 4 * brownie::kSetSamples))
        {
            for (uint32_t i = 0; i < brownie::kSetSamples; i++)
            {
                records.push_back(rom.U32(set + 4 * i));
            }
        }

        std::vector<RomSpan> samples;
        for (const uint32_t record : records)
        {
            if (const std::optional<RomSpan> sample = SampleAt(rom, record))
            {
                samples.push_back(*sample);
            }
        }

        if (samples.empty() || samples.size() < records.size())
        {
            break;
        }

        for (const RomSpan& sample : samples)
        {
            found.insert({sample.address, sample.size});
        }
    }

    return SpansOf(found);
}

class BrownieDriver : public Driver
{
public:
    BrownieDriver(const agblib::agb::Rom& rom, brownie::DriverInfo info, const BrownieProfile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile), samples_(SamplePoints(rom_, info_))
    {
        info_.log.push_back("the game's routines: " + std::string(profile_.code));
        info_.log.push_back("samples: " + std::to_string(samples_.size()));
    }

    std::string Name() const override
    {
        return "Brownie Brown's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return info_.log;
    }

    int SongCount() const override
    {
        return info_.song_count;
    }

    // The music: a sound that uses one of the PSG channels of the music. Sound 0 can't play: in the queue of the
    // driver, 0 means no request.
    bool HasSong(int song) const override
    {
        const std::optional<bool> music = IsMusic(song);
        return music && *music;
    }

    // A sound effect: a sound with channels, none of them a channel of the music, in a game whose profile has sound
    // effects.
    bool HasEffect(int song) const override
    {
        const std::optional<bool> music = profile_.effects ? IsMusic(song) : std::nullopt;
        return music && !*music;
    }

    StubPlan Plan() const override
    {
        StubPlan plan;
        plan.start = {
            Operation::Store(agblib::agb::kRegWaitCnt, {profile_.waitcnt}, 2),
            Operation::Call(profile_.init),
            Operation::Call(profile_.request, {{0, true}}),
        };

        // In the Sword of Mana revision, the mixer runs once for each 16 points of the FIFOs, at the rate of Timer 0.
        // In the Magical Vacation revision, the interrupt of each FIFO comes once for each 4 points at the rate of the
        // note. The model runs it at the fixed rate of DriverInfo::point_cycles. Detection reads no timer setting in
        // this revision and leaves that at its default of 1024 cycles (16,384 Hz).
        const uint32_t points = info_.revision == brownie::Revision::kSwordOfMana ? kMixerPoints : kInterruptPoints;
        const double per_frame = double(agblib::agb::kFrameCycles) / double(info_.point_cycles * points);
        constexpr uint16_t kFifoIrqs[] = {kIrqDma1, kIrqDma2};
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(profile_.frame)}}};
        for (size_t i = 0; i < profile_.fifo_irqs.size(); i++)
        {
            plan.interrupts |= kFifoIrqs[i];
            plan.handlers.push_back({kFifoIrqs[i], {Operation::Call(profile_.fifo_irqs[i])}, per_frame});
        }

        return plan;
    }

    // In the Magical Vacation revision, the interrupt of a FIFO comes only while the timer of the FIFO runs. The model
    // runs the routine at a fixed rate (Plan()). The run therefore skips the routine while the timer is off. Past the
    // end of a sample that doesn't loop, the routine would otherwise read on through the cartridge.
    void PrepareModel(ModelRun& run) const override
    {
        if (info_.revision != brownie::Revision::kMagicalVacation)
        {
            return;
        }

        run.on_call = [irqs = profile_.fifo_irqs](agblib::agb::System& gba, uint32_t routine)
        {
            for (size_t i = 0; i < irqs.size(); i++)
            {
                if (routine == irqs[i])
                {
                    return (gba.Read(kTimerControl + kTimerStride * uint32_t(i), 2) & kTimerOn) == 0;
                }
            }

            return false;
        };
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        // The routines in the cartridge. The mixer of Sword of Mana runs from IWRAM.
        std::vector<uint32_t> routines = {profile_.frame, profile_.init, profile_.request};
        std::copy_if(profile_.fifo_irqs.begin(), profile_.fifo_irqs.end(), std::back_inserter(routines),
                     [](uint32_t routine) { return routine >= agblib::agb::kRomBase; });
        const auto [lowest, highest] = std::minmax_element(routines.begin(), routines.end());
        MarkCodeSpan(reads, {*lowest, *highest}, kCodeTail);

        KeepWholeSpans(reads, samples_);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::brownie::InspectSong(rom_, info_, song, settings));
    }

private:
    // Whether sound `song` is music: whether it uses one of the PSG channels of the music. Nothing for a sound that
    // can't play or has no channels.
    std::optional<bool> IsMusic(int song) const
    {
        std::vector<brownie::SongChannel> channels;
        if (song <= 0 || !brownie::ReadSong(rom_, info_, song, channels) || channels.empty())
        {
            return std::nullopt;
        }

        return std::any_of(channels.begin(), channels.end(),
                           [](const brownie::SongChannel& c) { return c.channel < brownie::kFirstEffectChannel; });
    }

    const agblib::agb::Rom& rom_;
    brownie::DriverInfo info_;
    const BrownieProfile& profile_;
    std::vector<RomSpan> samples_;
};

} // namespace

std::unique_ptr<Driver> FindBrownie(const agblib::agb::Rom& rom, std::string& error)
{
    brownie::DriverInfo info;
    if (!brownie::DetectDriver(rom, brownie::DriverOverrides{}, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    const BrownieProfile* profile = FindProfile(Profiles(), code, version, &BrownieProfile::frame, info.frame_routine);
    if (!profile)
    {
        error = NoProfileError("Brownie Brown's driver",
                               GameWithRoutine(code, version, "per-frame routine", info.frame_routine));
        return nullptr;
    }

    return std::make_unique<BrownieDriver>(rom, std::move(info), *profile);
}

} // namespace gsf::rip
