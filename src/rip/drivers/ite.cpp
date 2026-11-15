// SPDX-License-Identifier: MIT

// ITE Media's driver, a player of 4-channel modules in a bank of songs and samples. agblib finds the driver's init and
// the bank that the game passes it, reads the songs and walks their rows for each song's loop or end. The game's other
// calls come from a profile of each known release.
//
// The driver mixes into a ring of 1920 samples in IWRAM, which DMA 1 plays through FIFO A at 16384 Hz, and restarts
// DMA 1 at the ring's start once every 7 frames. Each frame the game calls the routine that counts the frames to the
// restart, then the one that runs the song's ticks and mixes the frame's samples. The game starts a song through its
// routine that sets the music's volume for the song after the driver's start routine. The stub makes the same calls,
// with the volumes in a table.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/ite/driver.h"
#include "xsflib/rip/length.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace ite = agblib::ite;

// A game's calls of the driver besides its init, all Thumb code, for one release: the game code and the software
// version in the cartridge's header, and the init's address.
struct IteProfile
{
    std::string_view code = {};
    uint8_t version = 0;
    uint32_t init = 0;
    uint16_t waitcnt = 0;   // WAITCNT as the game sets it
    uint16_t soundbias = 0; // SOUNDBIAS while the music plays
    uint32_t effects = 0;   // the routine that takes the sound effects' table and their count
    uint32_t effect_table = 0;
    uint32_t effect_count = 0;
    uint32_t effect_slots = 0; // the routine that sets the sound effects' slots, and their count
    uint32_t slot_count = 0;
    uint32_t output_on = 0;
    uint32_t start = 0;  // the driver's start of a song, which takes the song's number
    uint32_t volume = 0; // the routine that takes the music's volume and the sound effects'
    uint32_t count = 0;  // the frame routine that counts the frames to DMA 1's restart
    uint32_t mix = 0;    // the frame routine that runs the ticks and mixes
    uint32_t ring_count = 0;
    uint32_t ring_last_count = 0;
    uint32_t ring_write = 0;
    std::vector<uint32_t> volumes; // the music's volume by song
    const GameTags* tags = nullptr;
    std::vector<std::pair<int, long long>> ends; // the last sound of each song that ends, in ms from the set's start
};

// The driver's full volume. The game sets the sound effects to it.
constexpr uint32_t kFullVolume = 0x100;

// The ring's count of frames to DMA 1's restart, and its write position, at the start of the frame that restarts DMA 1.
// The game is in that state once every 7 frames. The driver's init leaves the count and the write position at 0. The
// first frame's samples would then land where DMA 1 already reads. With these values, the stub's first frame restarts
// DMA 1, and its samples go 288 samples behind the DMA. The driver's output depends a little on the count at which a
// song starts: from count 0, song 27 differs in two groups of 16 bytes and jingle 22 in 33 short runs. The cause isn't
// known. The game starts a song at any count, and the stub always starts at 5.
constexpr uint32_t kRingCount = 5;
constexpr uint32_t kRingWrite = 0x120;

// WAITCNT and SOUNDBIAS.
constexpr uint32_t kWaitCnt = 0x04000204;
constexpr uint32_t kSoundBias = 0x04000088;

// The software version in the cartridge's header.
constexpr uint32_t kVersionByte = 0x080000BC;

// How long a walk of a song's rows may run: 400 s of the driver's mix.
constexpr uint64_t kMaxWalkSamples = 16384ull * 400;

// Hugo: The Evil Mirror. The credits name David Filskov and Thomas Bendt for the music and sound effects, ITE Media as
// the developer and Namco's team for the US release. The game names no song. A run of the game in mGBA 0.11, with
// screenshots and the game's state patched where a place is hard to reach, showed where it starts each song:
//
// - 0: the main menu (0x08009210), the high scores (0x08006D1A) and the credits (0x0800493C).
// - 1 to 3: the levels of each world (0x08011D6A, the world + 1). 14 to 16, faster versions of them: the world's last
//   level, its boss (0x08011A46, when the byte at 0x03003FFC is set), and any level whose timer runs out
//   (0x08025958), where the Time Monster comes with jingle 23 (0x0802595E).
// - 7 to 9: each world's title card (0x08005504, 0x08005552, 0x080055A0). 10 to 12: the start of a level
//   (0x08011A56). 24 to 26: a level's end (0x08012252 to 0x08012262).
// - 5: a new high score (0x08006D28). 13: a bonus object (0x0800E756). 17: the key (0x0800E88C, 0x0800FF4C and,
//   by a reading of the code, 0x08012898). 18: the blue crystal (0x0800E900). 19 to 21: an extra heart at 8,000, 20,000
//   and 35,000 points (0x0800FF32, table at 0x08047338). 22: the last monster generator frozen (0x08010840). 27: game
//   over (0x08007C88, 0x0800563E).
//
// Songs 4 and 6 have no patterns, and no code starts them. The set orders the songs as a player first hears them.
const GameTags kHugoTags = {
    .game = "Hugo: The Evil Mirror",
    .artist = "David Filskov, Thomas Bendt",
    .year = "2002-09-17",
    .copyright = "ITE Media, Namco",
    .comment = {},
    .titles = {"Main Menu, High Score, Credits",
               "Beaver Fort",
               "Squirrel Tree",
               "Viking Camp",
               "",
               "New High Score",
               "",
               "Beaver Fort Intro",
               "Squirrel Tree Intro",
               "Viking Camp Intro",
               "Beaver Fort Start",
               "Squirrel Tree Start",
               "Viking Camp Start",
               "Power-Up",
               "Beaver Fort Boss, Beaver Fort Time Out",
               "Squirrel Tree Boss, Squirrel Tree Time Out",
               "Viking Camp Boss, Viking Camp Time Out",
               "Key",
               "Crystal",
               "Extra Heart 1",
               "Extra Heart 2",
               "Extra Heart 3",
               "Generators Frozen",
               "Time Monster",
               "Beaver Fort Clear",
               "Squirrel Tree Clear",
               "Viking Camp Clear",
               "Game Over"},
    .order = {0, 7, 10, 1, 13, 22, 24, 17, 18, 19, 23, 14, 8, 11, 2, 25, 20, 15, 9, 12, 3, 26, 21, 16, 27, 5},
    .credits = {},
};

// The music's volume that the game's routine at 0x08010668 sets for each song (jump table at 0x080106C4).
std::vector<uint32_t> HugoVolumes()
{
    std::vector<uint32_t> volumes(28, kFullVolume);
    volumes[1] = volumes[3] = volumes[14] = volumes[16] = 0xCF;
    volumes[2] = volumes[15] = 0x80;
    return volumes;
}

// The last sound of each song that ends: the last byte that isn't 0 in FIFO A when mGBA 0.11 plays the set from boot.
// In the game, the music returns a frame after a jingle's F00 and cuts the jingle short. The set plays each jingle out.
// The samples ring on after the F00 for up to 3.1 s.
//
// Hugo: The Evil Mirror, US. The game's sound init (0x08010310) calls the driver's init with the bank, then the sound
// effects' routines. The game turns the output on (0x0801F628) before its first song, and its intro's FMV player
// leaves SOUNDBIAS at 0x4200. Its title screen has WAITCNT at 0x4316. Each frame it calls 0x0801F698, then 0x0801F6F0,
// from the handler of the vertical blank at the title screen and from the main loop in a level.
const IteProfile& HugoProfile()
{
    static const IteProfile kProfile = {
        .code = "AHJE",
        .init = 0x0801F4DC,
        .waitcnt = 0x4316,
        .soundbias = 0x4200,
        .effects = 0x08020A90,
        .effect_table = 0x080473CC,
        .effect_count = 196,
        .effect_slots = 0x08020AA4,
        .slot_count = 3,
        .output_on = 0x0801F628,
        .start = 0x080205CC,
        .volume = 0x0801F5EC,
        .count = 0x0801F698,
        .mix = 0x0801F6F0,
        .ring_count = 0x030003E4,
        .ring_last_count = 0x03000B78,
        .ring_write = 0x03000B1C,
        .volumes = HugoVolumes(),
        .tags = &kHugoTags,
        .ends = {{5, 3200},
                 {7, 7332},
                 {8, 8025},
                 {9, 8044},
                 {10, 2255},
                 {11, 2211},
                 {12, 2837},
                 {13, 2923},
                 {17, 4688},
                 {18, 5702},
                 {19, 3248},
                 {20, 3655},
                 {21, 3477},
                 {22, 2984},
                 {23, 3093},
                 {24, 4916},
                 {25, 4027},
                 {26, 4710},
                 {27, 7310}},
    };
    return kProfile;
}

class IteDriver : public Driver
{
public:
    IteDriver(const agblib::agb::Rom& rom, ite::DriverInfo info, const IteProfile& profile)
        : rom_(rom), info_(std::move(info)), profile_(profile)
    {
        info_.log.push_back("start at " + agblib::agb::Hex(profile.start) + ", frame routines at " +
                            agblib::agb::Hex(profile.count) + " and " + agblib::agb::Hex(profile.mix));
    }

    std::string Name() const override
    {
        return "ITE Media's driver";
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
        ite::Song header;
        return ite::ReadSong(rom_, info_.bank, song, header) && header.patterns > 0;
    }

    StubPlan Plan() const override
    {
        Value song;
        song.song = true;

        StubPlan plan;
        plan.start = {
            Operation::Store(kWaitCnt, {profile_.waitcnt}, 2),
            Operation::Store(kSoundBias, {profile_.soundbias}, 2),
            Operation::Call(info_.init | 1, {{0}, {info_.bank}}),
            Operation::Store(profile_.ring_count, {kRingCount}, 2),
            Operation::Store(profile_.ring_last_count, {kRingCount}, 2),
            Operation::Store(profile_.ring_write, {kRingWrite}, 4),
            Operation::Call(profile_.effects | 1, {{profile_.effect_table}, {profile_.effect_count}}),
            Operation::Call(profile_.effect_slots | 1, {{profile_.slot_count}}),
            Operation::Call(profile_.output_on | 1),
            Operation::Call(profile_.start | 1, {song}),
            Operation::Call(profile_.volume | 1, {SongTable(profile_.volumes), {kFullVolume}}),
        };
        plan.frame = {Operation::Call(profile_.count | 1), Operation::Call(profile_.mix | 1)};
        return plan;
    }

    const GameTags* Tags() const override
    {
        return profile_.tags;
    }

    std::optional<long long> LastSoundMs(int song) const override
    {
        for (const auto& [number, ms] : profile_.ends)
        {
            if (number == song)
            {
                return ms;
            }
        }

        return std::nullopt;
    }

    std::optional<xsflib::rip::Timing> SongTiming(int song) const override
    {
        ite::Song header;
        if (!ite::ReadSong(rom_, info_.bank, song, header))
        {
            return std::nullopt;
        }

        const ite::Flow flow = ite::WalkSong(rom_, info_.bank, header, kMaxWalkSamples);
        if (flow.notes == 0)
        {
            xsflib::rip::Timing timing;
            timing.silent = true;
            return timing;
        }

        // Two passes of a loop, then the fade; or the song's end, then the tail.
        const double frame_ms = 1000.0 / agblib::agb::kFrameRate;
        if (flow.kind == ite::Flow::Kind::kLoop)
        {
            const uint64_t passes = flow.loop_start + xsflib::rip::kLengthLoops * (flow.loop_end - flow.loop_start);
            return xsflib::rip::LoopAt(Frames(passes), frame_ms, kFadeMs);
        }

        // A song's samples can ring on after its F00. The timing then ends at the last sound, so that the coverage
        // runs past it.
        if (flow.kind == ite::Flow::Kind::kEnd)
        {
            uint64_t end = Frames(flow.end);
            if (const std::optional<long long> last = LastSoundMs(song))
            {
                end = std::max(end, uint64_t(std::ceil(double(*last) / frame_ms)));
            }

            return xsflib::rip::EndAt(end, frame_ms, kTailMs);
        }

        return std::nullopt;
    }

private:
    static uint64_t Frames(uint64_t samples)
    {
        return uint64_t(std::ceil(ite::FramesOfSamples(samples)));
    }

    const agblib::agb::Rom& rom_;
    ite::DriverInfo info_;
    const IteProfile& profile_;
};

const IteProfile* FindIteProfile(std::string_view code, uint8_t version, uint32_t init)
{
    const IteProfile& hugo = HugoProfile();
    if (hugo.code == code && hugo.version == version && hugo.init == init)
    {
        return &hugo;
    }

    return nullptr;
}

} // namespace

std::unique_ptr<Driver> FindIte(const agblib::agb::Rom& rom, std::string& error)
{
    ite::DriverInfo info;
    if (!ite::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.U8(kVersionByte);
    const IteProfile* profile = FindIteProfile(code, version, info.init);
    if (!profile)
    {
        error = "ITE Media's driver: the game " + code + " (version " + std::to_string(version) +
                ") with its init at " + agblib::agb::Hex(info.init) +
                " is in no profile that gsf has, so the routines that it calls are unknown";
        return nullptr;
    }

    return std::make_unique<IteDriver>(rom, std::move(info), *profile);
}

} // namespace gsf::rip
