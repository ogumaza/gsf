// SPDX-License-Identifier: MIT

// Quintet's driver, as Banpresto's Super Robot Taisen games and the games with music files play it. agblib's detection
// finds the songs, the samples and the music files. A profile for each game gives the routines that the game calls:
// its sound init, the call after it where the driver loads music files, the loader, the play routine and the per-frame
// routine. Each game's code gives them.
//
// The game calls the sound init, then, in a game with music files, the call after it. It starts a song with the play
// routine and the song's number in its file, after the loader has read the file. The game's handler of the vertical
// blank runs the per-frame routine. The driver doesn't mix: each FIFO's DMA reads a sample from the cartridge. The
// model runs no DMA, so the library keeps the whole of each sample whose header a run read.
//
// gsf rips the songs. The sound effects, which agblib numbers after each file's songs, get no minis.

#include <algorithm>
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

// The routines of a game's driver, all Thumb code. `after_init` and `loader` are 0 in a game without music files.
struct Profile
{
    const char* code = "";
    uint32_t sound_init = 0;
    uint32_t after_init = 0;
    uint32_t loader = 0;
    uint32_t play = 0;
    uint32_t frame = 0;
};

constexpr Profile kProfiles[] = {
    // The Super Robot Taisen games: the game's sound init, its play routine of song r0 and its per-frame routine.
    {"ASRJ", 0x080034F0, 0, 0, 0x08003550, 0x08003544},
    {"A6SJ", 0x08004A30, 0, 0, 0x08004A94, 0x08004A84},
    {"AJ9J", 0x08004AB4, 0, 0, 0x08004B18, 0x08004B08},
    {"B6JJ", 0x080083D0, 0, 0, 0x08008440, 0x08008430},

    // The games with music files: the driver's init, the game's call after it, the loader of the file whose first
    // word r0 gives, the play routine of song r0 of that file and the per-frame routine.
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

// The bytes of a sample's header that the driver reads: the size, then the rate.
constexpr uint32_t kSampleHeaderBytes = 6;

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

    StubPlan Plan() const override
    {
        // Each mini's number picks the file and the song in it.
        std::vector<uint32_t> file_words;
        std::vector<uint32_t> indices;
        const std::vector<uint32_t>& words = files_.front().music_files;
        for (int song = 0; song < SongCount(); song++)
        {
            const size_t file = quintet::FileOfSong(files_, song);
            file_words.push_back(file < words.size() ? words[file] : 0);
            indices.push_back(uint32_t(song - files_[file].first_song));
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

        plan.start.push_back(Operation::Call(profile_.play | 1, {SongTable(indices)}));
        plan.interrupts = kIrqVblank;
        plan.handlers = {{kIrqVblank, {Operation::Call(profile_.frame | 1)}}};
        return plan;
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
                    if (sample && Read(reads, sample->header, kSampleHeaderBytes))
                    {
                        Mark(reads, sample->data, sample->size);
                    }
                }
            }
        }
    }

    // Each FIFO's DMA reads from a sample's data, and CompleteReads() keeps the whole of each sample.
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
    const quintet::DriverInfo& FileOf(int song) const
    {
        return files_[quintet::FileOfSong(files_, song)];
    }

    // Whether a run read any of the `size` bytes at `address`.
    static bool Read(const std::vector<uint8_t>& reads, uint32_t address, uint32_t size)
    {
        const size_t first = address - agblib::agb::kRomBase;
        for (size_t i = first; i < first + size && i < reads.size(); i++)
        {
            if (reads[i])
            {
                return true;
            }
        }

        return false;
    }

    // Marks the `size` bytes at `address`.
    void Mark(std::vector<uint8_t>& reads, uint32_t address, uint32_t size) const
    {
        if (!rom_.Contains(address, size))
        {
            return;
        }

        const size_t first = address - agblib::agb::kRomBase;
        if (reads.size() < first + size)
        {
            reads.resize(first + size);
        }

        std::fill_n(reads.begin() + std::ptrdiff_t(first), size, uint8_t{1});
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
    for (const Profile& profile : kProfiles)
    {
        if (code == profile.code)
        {
            return std::make_unique<QuintetDriver>(rom, quintet::ReadMusicFiles(rom, info), profile);
        }
    }

    error = "Quintet's driver: the game " + code +
            " is in no profile that gsf has, so the routines that it calls are "
            "unknown";
    return nullptr;
}

} // namespace gsf::rip
