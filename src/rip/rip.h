// SPDX-License-Identifier: MIT

// The pieces of a rip: the stub in the cartridge's image, the coverage run of each song, the trim, and the set's files.

#pragma once

#include <cstddef>
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

// The bytes at the start of the cartridge that a set keeps whole: the header. Its first word branches to the stub.
constexpr uint32_t kHeaderSize = 0xC0;

// The image of a set: the cartridge image from 0x08000000, with the stub in place and the first word branching to it.
struct SetImage
{
    std::vector<uint8_t> image;
    Stub stub;
};

// Puts the stub for `plan` after the end of `rom`, or in its padding when the image fills the 32 MiB. The padding is
// the run of the last byte value at the end of the image, or else the longest run of 0x00 or 0xFF. The library starts
// song `first_song`. Its number goes at kSongAddress. Returns nothing and sets `error` when there's no room, or for a
// song above kMaxSong.
std::optional<SetImage> PlaceStub(const std::vector<uint8_t>& rom, const StubPlan& plan, uint32_t first_song,
                                  std::string& error);

// The error for song `song` when a mini can't store its number: "song N is above M, the highest number a mini can
// store". Returns nothing for a song up to kMaxSong.
std::optional<std::string> SongNumberError(uint32_t song);

// Whether PlaceStub() puts the stub for `plan` inside an image of `size` bytes: in its padding, when no room is left
// after it in the 32 MiB. The stub then replaces bytes of the image, and a rip has to check that the music reads none.
bool StubInImage(size_t size, const StubPlan& plan);

// The frames that a song's coverage run plays: its length and fade, half its length more for a song that loops, and
// `margin_seconds`. A song without a timing plays kUntimedSeconds.
constexpr double kUntimedSeconds = 300.0;
uint64_t CoverageFrames(const std::optional<xsflib::rip::Timing>& timing, double margin_seconds);

// What a song's run on the model covered: a byte for each byte of the cartridge, 1 where the run read it, and the timed
// DMAs that it started with a source in the cartridge, with the count of their starts.
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

// Returns an error when a run read a byte that the set replaced, or nothing: the first word (the branch to the stub),
// the bytes of the header at kSongAddress, or a byte of the stub in the padding of the image.
std::optional<std::string> CheckReplacedBytes(const SetImage& set, const std::vector<uint8_t>& reads);

// Returns an error when the trim would zero a byte that one of `dmas` reads in its first transfer, or nothing. The
// model runs no timed DMA. `reads` therefore contains none of their reads.
std::optional<std::string> CheckTimedDmas(const SetImage& set, const std::vector<uint8_t>& reads,
                                          const std::set<TimedDma>& dmas);

// Zeroes each byte of the image that no run read, except the header and the stub. Returns the bytes kept.
uint64_t Trim(SetImage& set, const std::vector<uint8_t>& reads);

// The timing that a song's length tags come from: `timing`, with the length of a song that ends at `last_sound_ms`
// where a profile gives it. The coverage keeps `timing` itself: the commands can read on past the last sound.
xsflib::rip::Timing TaggedTiming(xsflib::rip::Timing timing, std::optional<long long> last_sound_ms);

// The product code of a cartridge with the game code `code`: "AGB-", the code, "-" and the region that the code's last
// letter gives, such as "EUR" for 'P', or "AGB-" and a code whose region gsf doesn't know. Returns an empty string for
// a code that isn't four letters and digits.
std::string ProductCode(const std::string& code);

// The file of the library, and the file of a mini that starts song `song` with `library`, the name of the library. The
// section of the mini is the number of the song at kSongAddress. MiniFile() throws std::logic_error for a song above
// kMaxSong.
std::vector<uint8_t> LibraryFile(const SetImage& set, const xsflib::psf::Tags& tags);
std::vector<uint8_t> MiniFile(uint32_t song, const std::string& library, xsflib::psf::Tags tags);

} // namespace gsf::rip
