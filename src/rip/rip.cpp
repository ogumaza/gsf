// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "ntdlib/rounding.h"
#include "xsflib/bytes.h"

#include "gsf/gsf_file.h"
#include "rip/model.h"

namespace gsf::rip
{
namespace
{

// Where the stub goes in an image that fills the 32 MiB: in the run of the last byte value at the end of the image, or
// else in the longest run of 0x00 or 0xFF after the header. Of equal runs, the last one counts. Such a run is the least
// likely to be data that the game reads. The stub starts at the first multiple of 4 in the run. Returns nothing when
// the run is too short for the stub.
std::optional<uint32_t> PaddingRoom(const std::vector<uint8_t>& rom, uint32_t size)
{
    const auto room = [size](size_t start, size_t end) -> std::optional<uint32_t>
    {
        const size_t aligned = ntdlib::AlignUp(start, 4);
        if (aligned + size > end)
        {
            return std::nullopt;
        }

        return uint32_t(aligned);
    };

    size_t end = rom.size();
    size_t longest_start = 0;
    size_t longest_end = 0;
    while (end > kHeaderSize)
    {
        const uint8_t value = rom[end - 1];
        size_t start = end;
        while (start > kHeaderSize && rom[start - 1] == value)
        {
            start--;
        }

        if (end == rom.size())
        {
            if (auto offset = room(start, end))
            {
                return offset;
            }
        }
        else if ((value == 0x00 || value == 0xFF) && end - start > longest_end - longest_start)
        {
            longest_start = start;
            longest_end = end;
        }

        end = start;
    }

    return room(longest_start, longest_end);
}

// Whether the trim keeps the byte at `offset` of the image: the header, the stub and the bytes that a run read.
bool Kept(const SetImage& set, const std::vector<uint8_t>& reads, size_t offset)
{
    const size_t stub_first = set.stub.address - kRomBase;
    const size_t stub_last = stub_first + set.stub.code.size();
    return offset < kHeaderSize || (offset >= stub_first && offset < stub_last) ||
           (offset < reads.size() && reads[offset]);
}

} // namespace

std::optional<SetImage> PlaceStub(const std::vector<uint8_t>& rom, const StubPlan& plan, uint32_t first_song,
                                  std::string& error)
{
    if (rom.size() < kHeaderSize)
    {
        error = "the image is shorter than a cartridge's header";
        return std::nullopt;
    }

    if (std::optional<std::string> song_error = SongNumberError(first_song))
    {
        error = std::move(*song_error);
        return std::nullopt;
    }

    // The stub goes after the image where there's room. It then replaces no byte of the game.
    const uint32_t size = StubSize(plan);
    const size_t end = ntdlib::AlignUp(rom.size(), 4);
    std::optional<uint32_t> offset;
    size_t room_end = kMaxRomSize;
    if (!StubInImage(rom.size(), plan))
    {
        offset = uint32_t(end);
    }
    else
    {
        offset = PaddingRoom(rom, size);
        room_end = rom.size();
    }

    if (!offset)
    {
        error = "the image fills the cartridge's 32 MiB and has no room for the stub";
        return std::nullopt;
    }

    SetImage set;
    set.stub = BuildStub(plan, kRomBase + *offset);
    if (*offset + set.stub.code.size() > room_end)
    {
        throw std::logic_error("the stub at its place is larger than StubSize() gave");
    }

    set.image = rom;
    set.image.resize(std::max<size_t>(rom.size(), *offset + set.stub.code.size()), 0);
    std::copy(set.stub.code.begin(), set.stub.code.end(), set.image.begin() + *offset);
    xsflib::StoreLe32(set.image.data(), BranchWord(kRomBase, set.stub.address));
    xsflib::StoreLe16(&set.image[kSongAddress - kRomBase], uint16_t(first_song));

    return set;
}

std::optional<std::string> SongNumberError(uint32_t song)
{
    if (song <= kMaxSong)
    {
        return std::nullopt;
    }

    return "song " + std::to_string(song) + " is above " + std::to_string(kMaxSong) +
           ", the highest number a mini can store";
}

bool StubInImage(size_t size, const StubPlan& plan)
{
    return ntdlib::AlignUp(size, 4) + StubSize(plan) > kMaxRomSize;
}

uint64_t CoverageFrames(const std::optional<xsflib::rip::Timing>& timing, double margin_seconds)
{
    double seconds = kUntimedSeconds;
    if (timing)
    {
        const double length = double(timing->length_ms) / 1000.0;
        seconds = length + double(timing->fade_ms) / 1000.0;
        if (timing->fade_ms > 0)
        {
            seconds += length / 2;
        }
    }

    return uint64_t(std::ceil((seconds + margin_seconds) * agblib::agb::kFrameRate));
}

Coverage CoverSong(const agblib::agb::Rom& rom, const Driver& driver, const StubPlan& plan, uint32_t song,
                   uint64_t frames)
{
    Coverage coverage;
    ModelRun run(rom, plan, song, &coverage.reads);
    driver.PrepareModel(run);
    while (run.Frames() < frames)
    {
        run.Frame();
    }

    coverage.dmas = run.TimedDmas();
    coverage.dma_starts = run.TimedDmaStarts();
    return coverage;
}

void AddReads(std::vector<uint8_t>& all, const std::vector<uint8_t>& reads)
{
    if (all.size() < reads.size())
    {
        all.resize(reads.size());
    }

    for (size_t i = 0; i < reads.size(); i++)
    {
        all[i] |= reads[i];
    }
}

std::optional<std::string> CheckReplacedBytes(const SetImage& set, const std::vector<uint8_t>& reads)
{
    // The set replaces these bytes whatever the trim. --no-trim doesn't help.
    for (size_t i = 0; i < 4 && i < reads.size(); i++)
    {
        if (reads[i])
        {
            return "the music reads the cartridge's first word, which branches to the stub, at " +
                   agblib::agb::Hex(kRomBase + uint32_t(i)) + ". --no-trim doesn't help";
        }
    }

    for (size_t i = kSongAddress - kRomBase; i < kSongAddress - kRomBase + 2 && i < reads.size(); i++)
    {
        if (reads[i])
        {
            return "the music reads the header's bytes where the song's number goes, at " +
                   agblib::agb::Hex(kRomBase + uint32_t(i)) + ". --no-trim doesn't help";
        }
    }

    const size_t first = set.stub.address - kRomBase;
    const size_t last = std::min(reads.size(), first + set.stub.code.size());
    for (size_t i = first; i < last; i++)
    {
        if (reads[i])
        {
            return "the music reads the padding where the stub goes, at " + agblib::agb::Hex(kRomBase + uint32_t(i));
        }
    }

    return std::nullopt;
}

std::optional<std::string> CheckTimedDmas(const SetImage& set, const std::vector<uint8_t>& reads,
                                          const std::set<TimedDma>& dmas)
{
    for (const TimedDma& dma : dmas)
    {
        const DmaBytes bytes = FirstTransfer(dma);
        for (size_t i = bytes.offset; i < size_t(bytes.offset) + bytes.size && i < set.image.size(); i++)
        {
            if (!Kept(set, reads, i))
            {
                return "DMA " + std::to_string(dma.channel) + " reads " + agblib::agb::Hex(kRomBase + uint32_t(i)) +
                       " at its timed transfers, and the trim leaves that byte out";
            }
        }
    }

    return std::nullopt;
}

uint64_t Trim(SetImage& set, const std::vector<uint8_t>& reads)
{
    uint64_t kept = 0;
    for (size_t i = 0; i < set.image.size(); i++)
    {
        if (Kept(set, reads, i))
        {
            kept++;
        }
        else
        {
            set.image[i] = 0;
        }
    }

    return kept;
}

xsflib::rip::Timing TaggedTiming(xsflib::rip::Timing timing, std::optional<long long> last_sound_ms)
{
    if (timing.fade_ms == 0 && last_sound_ms)
    {
        timing.length_ms = *last_sound_ms;
    }

    return timing;
}

std::string ProductCode(const std::string& code)
{
    const auto alphanumeric = [](char c)
    {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (code.size() != 4 || !std::all_of(code.begin(), code.end(), alphanumeric))
    {
        return "";
    }

    // The regions that the last letter of a game code gives.
    static constexpr std::pair<char, const char*> kRegions[] = {
        {'J', "JPN"}, {'E', "USA"}, {'P', "EUR"}, {'D', "NOE"}, {'F', "FRA"}, {'I', "ITA"},
        {'S', "ESP"}, {'H', "HOL"}, {'U', "AUS"}, {'K', "KOR"}, {'C', "CHN"},
    };
    for (const auto& [letter, region] : kRegions)
    {
        if (code.back() == letter)
        {
            return "AGB-" + code + "-" + region;
        }
    }

    return "AGB-" + code;
}

std::vector<uint8_t> LibraryFile(const SetImage& set, const xsflib::psf::Tags& tags)
{
    Section section;
    section.entry = kRomBase;
    section.address = kRomBase;
    section.data = set.image;
    return WriteGsf(section, tags);
}

std::vector<uint8_t> MiniFile(uint32_t song, const std::string& library, xsflib::psf::Tags tags)
{
    if (song > kMaxSong)
    {
        throw std::logic_error("a mini stores a song's number in 16 bits");
    }

    Section section;
    section.entry = kRomBase;
    section.address = kSongAddress;
    xsflib::AppendLe16(section.data, uint16_t(song));
    tags["_lib"] = library;
    return WriteGsf(section, tags);
}

} // namespace gsf::rip
