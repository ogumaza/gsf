// SPDX-License-Identifier: MIT

// Quintet's driver, as Banpresto's Super Robot Taisen games and the games with music files play it. The agblib
// detection finds the songs, the samples and the music files. A profile for each game gives the routines that the game
// calls: its sound init, the call after it where the driver loads music files, the loader, the play routine and the
// per-frame routine. Their addresses come from the code of each game.
//
// The game calls the sound init. A game with music files then makes the call after it. The game starts a song with the
// play routine and the number of the song in its file, after the loader has read the file. The handler of the vertical
// blank in the game runs the per-frame routine. The driver doesn't mix: the DMA of each FIFO reads a sample from the
// cartridge. The model runs no DMA. The library therefore keeps the whole of each sample whose header a run read.
//
// gsf rips the songs. agblib numbers the sound effects after the songs of each file. With --effects, a Super Robot
// Taisen game also gets a mini for each sound effect. The game routine that plays a sound effect finds it in the file
// of sound effects by its index, and calls the routine of the driver for sound effects. The effect plays on the 6
// channels of the driver for sound effects.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/quintet/driver.h"
#include "agblib/quintet/instruments.h"
#include "agblib/sequence_export/quintet/convert.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace quintet = agblib::quintet;

// The routines of the driver in a game, all Thumb code. `after_init` and `loader` are 0 in a game without music files.
// `play_effect` is 0 in a game whose sound effects gsf doesn't rip.
struct Profile
{
    const char* code = "";
    uint32_t sound_init = 0;
    uint32_t after_init = 0;
    uint32_t loader = 0;
    uint32_t play = 0;
    uint32_t frame = 0;
    uint32_t play_effect = 0;
};

constexpr Profile kProfiles[] = {
    // The Super Robot Taisen games: the sound init of the game, its play routine of song r0, its per-frame routine and
    // its routine that plays sound effect r0. The play routines walk the file of the music or of the sound effects to
    // the song, then call the routine of the driver: 0x0805D1FC and 0x0805D254 in ASRJ, 0x080713E0 and 0x08071438 in
    // A6SJ, 0x08057294 and 0x080572EC in AJ9J, and 0x0807D250 and 0x0807D2B8 in B6JJ.
    {"ASRJ", 0x080034F0, 0, 0, 0x08003550, 0x08003544, 0x08003594},
    {"A6SJ", 0x08004A30, 0, 0, 0x08004A94, 0x08004A84, 0x08004AD8},
    {"AJ9J", 0x08004AB4, 0, 0, 0x08004B18, 0x08004B08, 0x08004B5C},
    {"B6JJ", 0x080083D0, 0, 0, 0x08008440, 0x08008430, 0x08008498},

    // The games with music files: the init of the driver, the call of the game after it, the loader of the file whose
    // first word r0 gives, the play routine of song r0 of that file and the per-frame routine.
    {"A2PJ", 0x0803FB38, 0x08042784, 0x0803FE28, 0x0803FF18, 0x08041C78},
    {"A8DJ", 0x08082B4C, 0x0808572C, 0x08082E3C, 0x08082F2C, 0x08084C20},
    {"AAIJ", 0x080A8118, 0x080AACF8, 0x080A8408, 0x080A84F8, 0x080AA1EC},
    {"ABIJ", 0x080A8118, 0x080AACF8, 0x080A8408, 0x080A84F8, 0x080AA1EC},
    {"ABRE", 0x0805E814, 0x080613F4, 0x0805EB04, 0x0805EBF4, 0x080608E8},
    {"ARPE", 0x08044FBC, 0x08047B9C, 0x080452AC, 0x0804539C, 0x08047090},
    {"ACVE", 0x08044FD0, 0x08047BB0, 0x080452C0, 0x080453B0, 0x080470A4},
    {"AJOJ", 0x08048E14, 0x0804B9F4, 0x08049104, 0x080491F4, 0x0804AEE8},
    {"APJJ", 0x080D4864, 0x080D7444, 0x080D4B54, 0x080D4C44, 0x080D6938},
    {"AVDJ", 0x080C8960, 0x080CB5AC, 0x080C8C50, 0x080C8D40, 0x080CAAA0},
    {"BPDJ", 0x08159098, 0x0815BC78, 0x08159388, 0x08159478, 0x0815B16C},
};

class QuintetDriver : public Driver
{
public:
    QuintetDriver(const agblib::agb::Rom& rom, std::vector<quintet::DriverInfo> files, const Profile& profile)
        : rom_(rom), files_(std::move(files)), profile_(profile)
    {
        log_ = files_.front().log;
        log_.push_back(std::string("the game's routines: ") + profile_.code);
    }

    std::string Name() const override
    {
        return "Quintet's driver";
    }

    const std::vector<std::string>& Log() const override
    {
        return log_;
    }

    int SongCount() const override
    {
        const quintet::DriverInfo& last = files_.back();
        return last.first_song + quintet::SongCount(last);
    }

    bool HasSong(int song) const override
    {
        if (song < 0 || song >= SongCount())
        {
            return false;
        }

        const quintet::DriverInfo& file = FileOf(song);
        return size_t(song - file.first_song) < file.song_addresses.size();
    }

    bool HasEffect(int song) const override
    {
        if (!profile_.play_effect || song < 0 || song >= SongCount())
        {
            return false;
        }

        const quintet::DriverInfo& file = FileOf(song);
        return quintet::EffectIndex(file, song - file.first_song) >= 0;
    }

    StubPlan Plan() const override
    {
        return MakePlan(false);
    }

    StubPlan PlanWithEffects() const override
    {
        return MakePlan(profile_.play_effect != 0);
    }

    void CompleteReads(std::vector<uint8_t>& reads) const override
    {
        for (const quintet::DriverInfo& file : files_)
        {
            for (int bank = 0; bank < 2; bank++)
            {
                for (size_t number = 0; number < file.sample_addresses[size_t(bank)].size(); number++)
                {
                    const std::optional<quintet::Sample> sample = quintet::ReadSample(rom_, file, bank, int(number));
                    // The driver reads the header just before the data: the size, then the rate.
                    if (sample && AnyRead(reads, {sample->header, sample->data - sample->header}) &&
                        rom_.Contains(sample->data, sample->size))
                    {
                        MarkSpan(reads, {sample->data, sample->size});
                    }
                }
            }
        }
    }

    // The DMA of each FIFO reads from the data of a sample, and CompleteReads() keeps the whole of each sample.
    bool KeepsTimedDmaReads() const override
    {
        return true;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        const quintet::DriverInfo& file = FileOf(song);
        const agblib::sequence_export::agb::ConvertSettings settings;
        return TimingOfReport(
            agblib::sequence_export::quintet::InspectSong(rom_, file, song - file.first_song, settings));
    }

private:
    // The plan of the stub: the sound init of the game and the calls after it, then the start of the song. With
    // `effects`, the plan starts a sound effect through the game routine for sound effects instead.
    StubPlan MakePlan(bool effects) const
    {
        // The number of each mini selects the file and the song in it, or the sound effect.
        std::vector<uint32_t> file_words;
        std::vector<uint32_t> indices;
        std::vector<uint32_t> routines;
        const std::vector<uint32_t>& words = files_.front().music_files;
        for (int song = 0; song < SongCount(); song++)
        {
            const size_t file = quintet::FileOfSong(files_, song);
            const int index = song - files_[file].first_song;
            const int effect = effects ? quintet::EffectIndex(files_[file], index) : -1;
            file_words.push_back(file < words.size() ? words[file] : 0);
            indices.push_back(uint32_t(effect >= 0 ? effect : index));
            routines.push_back((effect >= 0 ? profile_.play_effect : profile_.play) | 1);
        }

        StubPlan plan;
        plan.start.push_back(Operation::Call(profile_.sound_init | 1));
        if (profile_.after_init)
        {
            plan.start.push_back(Operation::Call(profile_.after_init | 1));
        }

        if (profile_.loader)
        {
            plan.start.push_back(Operation::Call(profile_.loader | 1, {SongTable(file_words)}));
        }

        if (effects)
        {
            plan.start.push_back(Operation::CallFromTable(routines, {SongTable(indices)}));
        }
        else
        {
            plan.start.push_back(Operation::Call(profile_.play | 1, {SongTable(indices)}));
        }

        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(profile_.frame | 1)}}};
        return plan;
    }

    const quintet::DriverInfo& FileOf(int song) const
    {
        return files_[quintet::FileOfSong(files_, song)];
    }

    const agblib::agb::Rom& rom_;
    std::vector<quintet::DriverInfo> files_;
    const Profile& profile_;
    std::vector<std::string> log_;
};

} // namespace

std::unique_ptr<Driver> FindQuintet(const agblib::agb::Rom& rom, std::string& error)
{
    quintet::DriverInfo info;
    if (!quintet::DetectDriver(rom, quintet::DriverOverrides{}, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    if (const Profile* profile = FindProfile<Profile>(kProfiles, code))
    {
        return std::make_unique<QuintetDriver>(rom, quintet::ReadMusicFiles(rom, info), *profile);
    }

    error = NoProfileError("Quintet's driver", code);
    return nullptr;
}

} // namespace gsf::rip
