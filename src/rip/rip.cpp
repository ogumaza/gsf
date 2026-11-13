// SPDX-License-Identifier: MIT

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "xsflib/bytes.h"

#include "gsf/gsf_file.h"
#include "rip/model.h"

namespace gsf::rip
{
namespace
{

// Where the stub goes in an image that fills the 32 MiB: the last 4-aligned run of padding, bytes of the image's last
// value, that holds it. Returns nothing when the run is too short.
std::optional<uint32_t> PaddingRoom(const std::vector<uint8_t>& rom, uint32_t size)
{
    const uint8_t pad = rom.back();
    size_t start = rom.size();
    while (start > kHeaderSize && rom[start - 1] == pad)
    {
        start--;
    }

    const size_t aligned = (start + 3) & ~size_t{3};
    if (aligned + size > rom.size())
    {
        return std::nullopt;
    }

    return uint32_t(aligned);
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

    // The stub goes after the image where there's room, so that it replaces nothing of the game's.
    const uint32_t size = StubSize(plan);
    const size_t end = (rom.size() + 3) & ~size_t{3};
    std::optional<uint32_t> offset;
    size_t room_end = kMaxImageSize;
    if (end + size <= kMaxImageSize)
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
    set.stub = BuildStub(plan, kRomBase + *offset, first_song);
    if (*offset + set.stub.code.size() > room_end)
    {
        throw std::logic_error("the stub at its place is larger than StubSize() gave");
    }

    set.image = rom;
    set.image.resize(std::max<size_t>(rom.size(), *offset + set.stub.code.size()), 0);
    std::copy(set.stub.code.begin(), set.stub.code.end(), set.image.begin() + *offset);
    xsflib::StoreLe32(set.image.data(), BranchWord(kRomBase, set.stub.address));

    return set;
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

std::optional<std::string> CheckStubRoom(const SetImage& set, const std::vector<uint8_t>& reads)
{
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

LengthTags TagsOfTiming(const xsflib::rip::Timing& timing, std::optional<long long> last_sound_ms)
{
    const auto clock = [](long long ms, bool milliseconds)
    {
        char text[32];
        if (milliseconds)
        {
            std::snprintf(text, sizeof text, "%lld:%02lld.%03lld", ms / 60000, ms / 1000 % 60, ms % 1000);
        }
        else
        {
            const long long seconds = (ms + 500) / 1000;
            std::snprintf(text, sizeof text, "%lld:%02lld", seconds / 60, seconds % 60);
        }

        return std::string(text);
    };

    if (timing.fade_ms > 0)
    {
        return {clock(timing.length_ms, false), std::to_string((timing.fade_ms + 500) / 1000)};
    }

    // The timing of a song that ends has the tail in its length.
    const auto tail = static_cast<long long>(kTailMs);
    const long long end = last_sound_ms ? *last_sound_ms : std::max(0LL, timing.length_ms - tail);
    return {clock(end, true), std::to_string(tail / 1000)};
}

std::string FileTitle(const std::string& title)
{
    std::string name;
    for (const char c : title)
    {
        const bool refused =
            static_cast<unsigned char>(c) < 0x20 || std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos;
        name += refused ? '_' : c;
    }

    // Windows drops a name's final dots and spaces.
    while (!name.empty() && (name.back() == '.' || name.back() == ' '))
    {
        name.back() = '_';
    }

    return name;
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

    // The regions of the game codes' last letters.
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

std::vector<uint8_t> MiniFile(const SetImage& set, uint32_t song, const std::string& library, xsflib::psf::Tags tags)
{
    Section section;
    section.entry = kRomBase;
    section.address = set.stub.song_address;
    xsflib::AppendLe32(section.data, song);
    tags["_lib"] = library;
    return WriteGsf(section, tags);
}

} // namespace gsf::rip
