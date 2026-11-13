// SPDX-License-Identifier: MIT

// The pieces of a rip: the stub in the cartridge's image, the coverage run of each song, the trim, and the set's files.

#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "agblib/agb/rom.h"
#include "xsflib/psf/psf.h"
#include "xsflib/rip/length.h"

#include "rip/driver.h"
#include "rip/model.h"
#include "rip/stub.h"

namespace gsf::rip
{

// The bytes at the cartridge's start that a set keeps whole: the header, whose first word branches to the stub.
constexpr uint32_t kHeaderSize = 0xC0;

// A set's image: the cartridge's image from 0x08000000, with the stub in place and the first word branching to it.
struct SetImage
{
    std::vector<uint8_t> image;
    Stub stub;
};

// Puts the stub for `plan` after the end of `rom`, or in the padding at its end when the image fills the 32 MiB. The
// library starts song `first_song`. Returns nothing and sets `error` when there's no room.
std::optional<SetImage> PlaceStub(const std::vector<uint8_t>& rom, const StubPlan& plan, uint32_t first_song,
                                  std::string& error);

// The frames that a song's coverage run plays: its length and fade, half its length more for a song that loops, and
// `margin_seconds`. A song without a timing plays kUntimedSeconds.
constexpr double kUntimedSeconds = 300.0;
uint64_t CoverageFrames(const std::optional<xsflib::rip::Timing>& timing, double margin_seconds);

// What a song's run on the model covered: a byte for each byte of the cartridge, 1 where the run read it, and the
// timed DMAs that it started with a source in the cartridge, with the count of their starts.
struct Coverage
{
    std::vector<uint8_t> reads;
    std::set<TimedDma> dmas;
    uint64_t dma_starts = 0;
};

// Runs song `song` of `driver`'s plan on the model for `frames` frames.
Coverage CoverSong(const agblib::agb::Rom& rom, const Driver& driver, const StubPlan& plan, uint32_t song,
                   uint64_t frames);

// Adds the marks of `reads` to `all`.
void AddReads(std::vector<uint8_t>& all, const std::vector<uint8_t>& reads);

// Returns an error when a run read a byte that the stub replaced, or nothing.
std::optional<std::string> CheckStubRoom(const SetImage& set, const std::vector<uint8_t>& reads);

// Returns an error when the trim would zero a byte that one of `dmas` reads in its first transfer, or nothing. The
// model runs no timed DMA, so `reads` has none of their reads.
std::optional<std::string> CheckTimedDmas(const SetImage& set, const std::vector<uint8_t>& reads,
                                          const std::set<TimedDma>& dmas);

// Zeroes each byte of the image that no run read, except the header and the stub. Returns the bytes kept.
uint64_t Trim(SetImage& set, const std::vector<uint8_t>& reads);

// The length and fade tags of a song: for a song that loops, its length to the second and its fade in seconds; for one
// that ends, its end to the millisecond, or `last_sound_ms` where it's given, and its tail of kTailMs as the fade.
struct LengthTags
{
    std::string length;
    std::string fade;
};
LengthTags TagsOfTiming(const xsflib::rip::Timing& timing, std::optional<long long> last_sound_ms = std::nullopt);

// A name for a file of a set that is `title` with the characters that file systems refuse replaced by '_'.
std::string FileTitle(const std::string& title);

// The product code of a cartridge with the game code `code`: "AGB-", the code, "-" and the region that the code's last
// letter gives, such as "EUR" for 'P', or "AGB-" and a code whose region gsf doesn't know. Returns an empty string for
// a code that isn't four letters and digits.
std::string ProductCode(const std::string& code);

// The library's file, and a mini's file that starts song `song` with `library`, the library's name.
std::vector<uint8_t> LibraryFile(const SetImage& set, const xsflib::psf::Tags& tags);
std::vector<uint8_t> MiniFile(const SetImage& set, uint32_t song, const std::string& library, xsflib::psf::Tags tags);

} // namespace gsf::rip
