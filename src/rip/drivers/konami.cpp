// SPDX-License-Identifier: MIT

// Konami's driver, as the Yu-Gi-Oh! games of 2005 and 2006 and the other games of that revision play it. agblib finds
// the tables of the driver, reads its songs and samples, and works out the loop or the end of each song. A profile of
// each release gives the routines that the game calls. Their addresses come from the code of the game.
//
// The game calls the init of the driver with a buffer in EWRAM and with the addresses of the entries of the DMA 1 and
// DMA 2 interrupts in its table of interrupt routines. The init copies the mixer to IWRAM, and starts Timer 0 at the
// rate of the mixer and the DMAs of both FIFOs. It puts the address of the mixer in the entry of DMA 1. Some games then
// set another rate through the rate setter of the driver. The rate setter takes the reload of Timer 0 from the timer
// table of the driver. Each time FIFO A has played 16 points, the DMA 1 interrupt runs the mixer. The mixer makes the
// next 16 points of both FIFOs. The game requests a song through the request of the driver. Once a frame, it calls the
// per-frame entry of the driver with the interrupts on, and the routine that starts a requested song and steps the
// sequencer. The stub makes the same calls, both in its main loop.
//
// The DMA 1 interrupt can stop the per-frame entry part way. The voices that the entry has updated by then reach one
// block of the mixer, and the rest reach the next block. Which voices those are depends on the phase: where the blocks
// of the mixer fall against the display. Timer 0 starts in the init. Three things set the phase: the state of the
// console when the code of the cartridge starts, the cycles of the start-up code before the init, and the interrupt
// that calls the entry. The stub calls the init when it reaches it, and the boot of each player sets the phase. A
// change of a voice can then reach the output one block earlier or later than in the game: 0.76 ms at 21,024 Hz. gsfrip
// doesn't derive the phase of the game. Some games start the driver without a wait for the display. The exit of the
// BIOS sets their phase, and no image contains it. The model counts no cycles.
//
// How far the mixer plays into a sample by the end of the run of a song depends on the order of the interrupts. The
// model doesn't run that order. The library therefore keeps the whole of each sample that a run read part of. It also
// keeps the code of the driver whole: from the lowest routine that the stub calls to the last byte that a run read
// within kCodeTail bytes after the highest.

#include "rip/drivers/konami.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/system.h"
#include "agblib/konami/driver.h"
#include "agblib/sequence_export/konami/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace konami = agblib::konami;

// Yu-Gi-Oh! Duel Monsters Expert 2006, Japan. The start-up code of the game (0x080F4CA4) sets WAITCNT to 0x4014. The
// routine at 0x0810E57C also sets the wait of SRAM. WAITCNT is then 0x4017. The game puts its table of interrupt
// routines at 0x03000000. It calls the init (0x080F4DAC) with the entries of that table for DMA 1 and DMA 2 and with
// the buffer 0x02006F10. The init puts 0x03004C0C in the entry of DMA 1. The game requests a song through 0x080F9B6C.
// That routine keeps the number of the song at 0x0200AF10, and calls the request 0x0810CFE4 unless the song already
// plays. The request calls 0x0810CFA0 with r1 = 0. The handler of the vertical blank (0x080F4AA8) calls the per-frame
// entry 0x0810CAC8. The main loop calls 0x080F4C08 once a frame, and that routine calls 0x0810CE30.
//
// The other games make the same calls, each from its code below: the call of the init and its arguments, the call of
// the per-frame entry in the handler of the vertical blank, the call of the start in the main loop, and WAITCNT while
// the music plays. In mGBA, DMA 1 of each game runs the routine that the profile gives.
//
//     game  init call    arguments                          per-frame entry  start       WAITCNT
//     A8NJ  0x080004A2   0x030000F4                         0x080006EA       0x0800075A  0x4014
//     A8RJ  0x080002D6   0x030069B4                         0x0800056C       0x08000540  0x4017
//     AKEJ  0x0800C8AC   0, 0                               0x0800C902       0x0800C8F6  0x4017
//     AVAJ  0x0802CDB6   0x03000024                         0x0802CBC2       0x0802CC44  0x4017
//     B3MJ  0x0800051A   0x030000FC, 0x03000100, 0x0203BD00 0x080007A2       0x08000836  0x4017
//     B4GJ  0x080002D4   0x03006AB4, 0x03006AB8, 0x0203B000 0x08000578       0x0800054C  0x4017
//     B4SJ  0x080002D4   0x03006AB4, 0x03006AB8, 0x0203B000 0x08000578       0x0800054C  0x4017
//     BMAJ  0x080004CC   0x030000FC, 0x03000100, 0x0203BD00 0x08000726       0x080007AE  0x4017
//     BRME  0x080002D8   0x03005AF4, 0x03005AF8             0x08000D12       0x08000394  0x4017
//     BY7E  0x0811AF5C   0x03000024, 0x03000028, 0x02014820 0x0811AC1A       0x0811AD9A  0x4017
//     BYGE  0x080E1B2C   0x03000024, 0x03000028, 0x020061C0 0x080E18E6       0x080E198E  0x4017
//     BYWP  0x08083320   0x03000024, 0x03000028, 0x020114C0 0x08082FC0       0x08083144  0x4014
//
// The games of the Hunter X Hunter revision (A8NJ, A8RJ, AVAJ) pass r1 = 0 to the request.
//
// The init of AKEJ takes no entries. The handler of the DMA 1 interrupt in the game calls 0x080784E0 (at 0x0800C90E).
// That routine calls the mixer and restarts the DMAs. The init of BRME puts 0x0802BF90 in the entry of DMA 1: a routine
// that calls the mixer.
//
// The request of AKEJ takes r1 = 0xF0. The call of the game at 0x0800CA66 passes that value for a song with no fade.
// The code of BRME requests every song through 0x0802C3CC. That routine keeps the number of the song and clears the
// fade.
//
// A8RJ calls the rate setter of the driver (0x0809823C) with 2 after the init (0x080002DC). B4GJ and B4SJ call theirs
// (0x080D6F4C, 0x080D6F50) with 2 from 0x080002DA: the mixer runs at 15,768 Hz. AVAJ calls 0x0803DFD0 with 0 at boot
// (0x0802CDBC). The other games call their rate setter only from their init.
//
// B3MJ and BMAJ set their output from an option. Started with no save, B3MJ sets the byte of the option to 1
// (0x08003A50) and then calls 0x08009878 with 0 (from 0x08003C52). BMAJ calls 0x08013AB0 with 0 (from 0x0800362E). With
// 0, the routine sends both FIFOs to both sides (SOUNDCNT_H 0x330E).
const std::vector<KonamiProfile>& Profiles()
{
    static const std::vector<KonamiProfile> kProfiles = {
        {
            .code = "BY6J",
            .song_table = 0x08194210,
            .init = 0x0810B471,
            .init_args = {0x03000020, 0x03000024, 0x02006F10},
            .request = 0x0810CFE5,
            .start = 0x0810CE31,
            .frame = 0x0810CAC9,
            .dma1 = 0x03004C0C,
            .waitcnt = 0x4017,
        },
        {
            .code = "A8NJ",
            .song_table = 0x0805F1B0,
            .init = 0x08024CF5,
            .init_args = {0x030000F4},
            .request = 0x0802644D,
            .request_args = {0},
            .start = 0x080262ED,
            .frame = 0x08025FD1,
            .dma1 = 0x03001D1C,
            .waitcnt = 0x4014,
        },
        {
            .code = "A8RJ",
            .song_table = 0x089AC770,
            .init = 0x080983B1,
            .init_args = {0x030069B4},
            .rate = 0x0809823D,
            .rate_setting = 2,
            .request = 0x080999B5,
            .request_args = {0},
            .start = 0x08099855,
            .frame = 0x080995A5,
            .dma1 = 0x03000000,
            .waitcnt = 0x4017,
        },
        {
            .code = "AKEJ",
            .song_table = 0x08099050,
            .init = 0x080774E9,
            .init_args = {0, 0},
            .request = 0x080788FD,
            .request_args = {0xF0},
            .start = 0x080787AD,
            .frame = 0x08078555,
            .dma1 = 0x080784E1,
            .waitcnt = 0x4017,
        },
        {
            .code = "AVAJ",
            .song_table = 0x0811F210,
            .init = 0x0803E145,
            .init_args = {0x03000024},
            .rate = 0x0803DFD1,
            .rate_setting = 0,
            .request = 0x0803F6AD,
            .request_args = {0},
            .start = 0x0803F54D,
            .frame = 0x0803F2A9,
            .dma1 = 0x03004E6C,
            .waitcnt = 0x4017,
        },
        {
            .code = "B3MJ",
            .song_table = 0x080A9AA0,
            .init = 0x08009B4D,
            .init_args = {0x030000FC, 0x03000100, 0x0203BD00},
            .mono = 0x08009879,
            .request = 0x0800B661,
            .start = 0x0800B4B1,
            .frame = 0x0800B149,
            .dma1 = 0x03001EB0,
            .waitcnt = 0x4017,
        },
        {
            .code = "B4GJ",
            .song_table = 0x0901B340,
            .init = 0x080D71C1,
            .init_args = {0x03006AB4, 0x03006AB8, 0x0203B000},
            .rate = 0x080D6F4D,
            .rate_setting = 2,
            .request = 0x080D8C3D,
            .start = 0x080D8A89,
            .frame = 0x080D8721,
            .dma1 = 0x03000000,
            .waitcnt = 0x4017,
        },
        {
            .code = "B4SJ",
            .song_table = 0x09016F80,
            .init = 0x080D71C5,
            .init_args = {0x03006AB4, 0x03006AB8, 0x0203B000},
            .rate = 0x080D6F51,
            .rate_setting = 2,
            .request = 0x080D8C41,
            .start = 0x080D8A8D,
            .frame = 0x080D8725,
            .dma1 = 0x03000000,
            .waitcnt = 0x4017,
        },
        {
            .code = "BMAJ",
            .song_table = 0x08039DD0,
            .init = 0x08013D7D,
            .init_args = {0x030000FC, 0x03000100, 0x0203BD00},
            .mono = 0x08013AB1,
            .request = 0x08015841,
            .start = 0x08015639,
            .frame = 0x080152F5,
            .dma1 = 0x03001E10,
            .waitcnt = 0x4017,
        },
        {
            .code = "BRME",
            .song_table = 0x0809A7E0,
            .init = 0x0802AF4D,
            .init_args = {0x03005AF4, 0x03005AF8},
            .request = 0x0802C3CD,
            .start = 0x0802C29D,
            .frame = 0x0802C019,
            .dma1 = 0x0802BF91,
            .waitcnt = 0x4017,
        },
        {
            .code = "BY7E",
            .song_table = 0x081B40B0,
            .init = 0x0813C541,
            .init_args = {0x03000024, 0x03000028, 0x02014820},
            .request = 0x0813E091,
            .start = 0x0813DEDD,
            .frame = 0x0813DB75,
            .dma1 = 0x03004788,
            .waitcnt = 0x4017,
        },
        {
            .code = "BYGE",
            .song_table = 0x08191970,
            .init = 0x080EF235,
            .init_args = {0x03000024, 0x03000028, 0x020061C0},
            .request = 0x080F0DA1,
            .start = 0x080F0BED,
            .frame = 0x080F0885,
            .dma1 = 0x030006C8,
            .waitcnt = 0x4017,
        },
        {
            .code = "BYWP",
            .song_table = 0x0893C7D0,
            .init = 0x08096695,
            .init_args = {0x03000024, 0x03000028, 0x020114C0},
            .request = 0x08098139,
            .start = 0x08097F2D,
            .frame = 0x08097BE9,
            .dma1 = 0x03004F18,
            .waitcnt = 0x4014,
        },
    };
    return kProfiles;
}

// The points that the mixer makes for each FIFO at a time.
constexpr double kMixerPoints = 16.0;

// How far the code and the tables of the driver go past the start of the highest routine that the stub calls: the
// commands of the sequencer, the output stage and the code of the mixer that the init copies.
constexpr uint32_t kCodeTail = 0x2000;

// The sample numbers of the table of the live stage (`DriverInfo::live_sample_table`).
constexpr int kLiveSamples = 0x8000;

// Adds the header and the points of each sample of the table at `table` that agblib can read, from sample number
// `first`. The span of a packed sample runs from its header, through its table of pairs, to `length` bytes after the
// start of its data. A packed stream takes at most one byte a point: the agblib layout counts it that way. A null entry
// or an entry that agblib can't read doesn't end the table. The walk ends after a run of konami::kInvalidSampleRun such
// entries, or after konami::kSampleNumbers entries.
void AddSamples(const agblib::agb::Rom& rom, const konami::DriverInfo& info, uint32_t table, int first,
                std::vector<RomSpan>& spans)
{
    int invalid = 0;
    for (int i = 0; i < konami::kSampleNumbers && invalid < konami::kInvalidSampleRun; i++)
    {
        const konami::SampleInfo sample = info.Sample(rom, first + i);
        if (!sample.valid || sample.length <= 0)
        {
            invalid++;
            continue;
        }

        invalid = 0;
        const uint32_t header = rom.U32(table + 4 * uint32_t(i));
        if (sample.packed)
        {
            spans.push_back({header, sample.data + uint32_t(sample.length) - header});
        }
        else
        {
            spans.push_back({header, konami::kSampleHeaderBytes});
            spans.push_back({sample.data, uint32_t(sample.length)});
        }
    }
}

} // namespace

std::vector<RomSpan> KonamiSampleSpans(const agblib::agb::Rom& rom, const konami::DriverInfo& info)
{
    std::vector<RomSpan> spans;
    if (info.sample_table)
    {
        AddSamples(rom, info, info.sample_table, 0, spans);
    }

    if (info.live_sample_table)
    {
        AddSamples(rom, info, info.live_sample_table, kLiveSamples, spans);
    }

    return spans;
}

double KonamiBlockCycles(const agblib::agb::Rom& rom, const konami::DriverInfo& info, const KonamiProfile& profile)
{
    // kMixerPoints periods of Timer 0. Where the game calls the rate setter, Timer 0 takes its reload from that entry
    // of the timer table of the driver.
    if (profile.rate != 0 && info.timer_table)
    {
        const uint32_t reload = rom.U32(info.timer_table + 4 * profile.rate_setting) & 0xFFFF;
        return kMixerPoints * double(0x10000 - reload);
    }

    return kMixerPoints * double(agblib::agb::kSecondCycles) / info.mix_rate;
}

StubPlan KonamiPlan(const KonamiProfile& profile, double block_cycles)
{
    const std::vector<Value> init_args = Constants(profile.init_args);
    std::vector<Value> request_args = Constants(profile.request_args);
    request_args.insert(request_args.begin(), Value{0, true});

    StubPlan plan;
    plan.start = {Operation::Store(agblib::agb::kRegWaitCnt, {profile.waitcnt}, 2)};
    plan.start.push_back(Operation::Call(profile.init, std::move(init_args)));
    if (profile.mono != 0)
    {
        plan.start.push_back(Operation::Call(profile.mono, {{0}}));
    }

    if (profile.rate != 0)
    {
        plan.start.push_back(Operation::Call(profile.rate, {{profile.rate_setting}}));
    }

    plan.start.push_back(Operation::Call(profile.request, std::move(request_args)));

    // The main loop runs the per-frame entry, then the start. As in the game, the DMA 1 interrupt can stop the entry.
    plan.interrupts = kIrqVblank | kIrqDma1;
    plan.handlers = {{kIrqDma1, {Operation::Call(profile.dma1)}, double(agblib::agb::kFrameCycles) / block_cycles}};
    plan.frame = {Operation::Call(profile.frame), Operation::Call(profile.start)};

    return plan;
}

namespace
{

class KonamiDriver : public Driver
{
public:
    KonamiDriver(const agblib::agb::Rom& rom, konami::DriverInfo info, const KonamiProfile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile), samples_(KonamiSampleSpans(rom_, info_))
    {
        info_.log.push_back("the game's routines: " + std::string(profile_.code));
        info_.log.push_back("sample spans kept whole: " + std::to_string(samples_.size()));
    }

    std::string Name() const override
    {
        return "Konami's driver";
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
        return song >= 0 && song < info_.song_count;
    }

    StubPlan Plan() const override
    {
        return KonamiPlan(profile_, KonamiBlockCycles(rom_, info_, profile_));
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        std::vector<uint32_t> routines = {profile_.init, profile_.request, profile_.start, profile_.frame};
        for (const uint32_t routine : {profile_.mono, profile_.rate, profile_.dma1})
        {
            if (rom_.Contains(routine & ~1u))
            {
                routines.push_back(routine);
            }
        }

        const auto [lowest, highest] = std::ranges::minmax(routines);
        MarkCodeSpan(reads, {lowest, highest}, kCodeTail);
        KeepWholeSpans(reads, samples_);
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(agblib::sequence_export::konami::InspectSong(rom_, info_, song, settings));
    }

private:
    const agblib::agb::Rom& rom_;
    konami::DriverInfo info_;
    const KonamiProfile& profile_;
    std::vector<RomSpan> samples_;
};

} // namespace

const KonamiProfile* FindKonamiProfile(std::string_view code, uint8_t version, uint32_t song_table)
{
    return FindProfile(Profiles(), code, version, &KonamiProfile::song_table, song_table);
}

std::unique_ptr<Driver> FindKonami(const agblib::agb::Rom& rom, std::string& error)
{
    konami::DriverInfo info;
    konami::DriverOverrides overrides;
    overrides.program = "gsfrip";
    if (!konami::DetectDriver(rom, overrides, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    const KonamiProfile* profile = FindKonamiProfile(code, version, info.song_table);
    if (!profile)
    {
        error = NoProfileError("Konami's driver", GameWithRoutine(code, version, "song table", info.song_table));
        return nullptr;
    }

    return std::make_unique<KonamiDriver>(rom, std::move(info), *profile);
}

} // namespace gsf::rip
