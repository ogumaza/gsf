// SPDX-License-Identifier: MIT

// ITE Media's driver, a player of 4-channel modules in a bank of songs and samples. agblib finds the init of the driver
// and the bank that the game passes it. It reads the songs and walks their rows for the loop or the end of each song. A
// profile of each known release gives the other calls of the game.
//
// The driver mixes into a ring of 1920 samples in IWRAM. DMA 1 plays the ring through FIFO A at 16384 Hz. The driver
// restarts DMA 1 at the start of the ring once every 7 frames. Each frame, the game calls the routine that counts the
// frames to the restart. It then calls the routine that runs the ticks of the song and mixes the samples of the frame.
// The game starts a song through the start routine of the driver, then through its routine that sets the volume of the
// music for the song. The stub makes the same calls, with the volumes in a table.

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
#include "agblib/agb/system.h"
#include "agblib/ite/driver.h"
#include "xsflib/rip/length.h"

#include "rip/driver.h"

namespace gsf::rip
{
namespace
{

namespace ite = agblib::ite;

// The calls of the driver in a game besides its init, all Thumb code, for one release: the game code and the software
// version in the cartridge header, and the address of the init.
struct IteProfile
{
    std::string_view code = {};
    uint8_t version = 0;
    uint32_t init = 0;
    uint16_t waitcnt = 0;   // WAITCNT as the game sets it
    uint16_t soundbias = 0; // SOUNDBIAS while the music plays
    uint32_t effects = 0;   // the routine that takes the table of sound effects and their count
    uint32_t effect_table = 0;
    uint32_t effect_count = 0;
    uint32_t effect_slots = 0; // the routine that sets the slots of the sound effects, and their count
    uint32_t slot_count = 0;
    uint32_t output_on = 0;
    uint32_t start = 0;  // the start of a song in the driver: it takes the number of the song
    uint32_t volume = 0; // the routine that takes the volume of the music and that of the sound effects
    uint32_t count = 0;  // the frame routine that counts the frames to the restart of DMA 1
    uint32_t mix = 0;    // the frame routine that runs the ticks and mixes
    uint32_t ring_count = 0;
    uint32_t ring_last_count = 0;
    uint32_t ring_write = 0;
    std::vector<uint32_t> volumes; // the volume of the music by song
    std::vector<SongEnd> ends;     // the last sound of each song that ends
};

// The full volume of the driver. The game sets the sound effects to it.
constexpr uint32_t kFullVolume = 0x100;

// The count of frames to the restart of DMA 1, and the write position in the ring, at the start of the frame that
// restarts DMA 1. The game is in that state once every 7 frames. The init of the driver leaves the count and the write
// position at 0. The samples of the first frame would then land where DMA 1 already reads. With these values, the first
// frame of the stub restarts DMA 1, and its samples go 288 samples behind the DMA. The output of the driver depends a
// little on the count at which a song starts: from count 0, song 27 differs in two groups of 16 bytes and jingle 22 in
// 33 short runs. The cause isn't known. The game starts a song at any count, and the stub always starts at 5.
constexpr uint32_t kRingCount = 5;
constexpr uint32_t kRingWrite = 0x120;

// How long a walk of the rows of a song may run: 400 s of the mix of the driver.
constexpr uint64_t kMaxWalkSamples = 16384ull * 400;

// Hugo: The Evil Mirror. A run of the game in mGBA 0.11 showed where the game starts each song. The run took
// screenshots, and patched the state of the game where a place is hard to reach:
//
// - 0: the main menu (0x08009210), the high scores (0x08006D1A) and the credits (0x0800493C).
// - 1 to 3: the levels of each world (0x08011D6A, the world + 1). 14 to 16, faster versions of them: the last level of
//   the world, its boss (0x08011A46, when the byte at 0x03003FFC is set), and any level whose timer runs out
//   (0x08025958). The Time Monster then comes with jingle 23 (0x0802595E).
// - 7 to 9: the title card of each world (0x08005504, 0x08005552, 0x080055A0). 10 to 12: the start of a level
//   (0x08011A56). 24 to 26: the end of a level (0x08012252 to 0x08012262).
// - 5: a new high score (0x08006D28). 13: a bonus object (0x0800E756). 17: the key (0x0800E88C, 0x0800FF4C and,
//   by a reading of the code, 0x08012898). 18: the blue crystal (0x0800E900). 19 to 21: an extra heart at 8,000, 20,000
//   and 35,000 points (0x0800FF32, table at 0x08047338). 22: the last monster generator frozen (0x08010840). 27: game
//   over (0x08007C88, 0x0800563E).
//
// Songs 4 and 6 have no patterns, and no code starts them. A player first hears the songs in this order: 0, 7, 10, 1,
// 13, 22, 24, 17, 18, 19, 23, 14, 8, 11, 2, 25, 20, 15, 9, 12, 3, 26, 21, 16, 27, 5. The set lists them by number.

// The volume of the music that the game routine at 0x08010668 sets for each song (jump table at 0x080106C4).
std::vector<uint32_t> HugoVolumes()
{
    std::vector<uint32_t> volumes(28, kFullVolume);
    volumes[1] = volumes[3] = volumes[14] = volumes[16] = 0xCF;
    volumes[2] = volumes[15] = 0x80;
    return volumes;
}

// The last sound of each song that ends: the last byte that isn't 0 in FIFO A when mGBA 0.11 plays the set from boot.
// In the game, the music returns a frame after the F00 of a jingle and cuts the jingle short. The set plays each jingle
// out. The samples ring on after the F00 for up to 3.1 s.
//
// Hugo: The Evil Mirror, US. The sound init of the game (0x08010310) calls the init of the driver with the bank, then
// the routines of the sound effects. The game turns the output on (0x0801F628) before its first song. The FMV player of
// its intro leaves SOUNDBIAS at 0x4200. The title screen has WAITCNT at 0x4316. Each frame the game calls 0x0801F698,
// then 0x0801F6F0. It calls them from the handler of the vertical blank at the title screen, and from the main loop in
// a level.
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
            Operation::Store(agblib::agb::kRegWaitCnt, {profile_.waitcnt}, 2),
            Operation::Store(agblib::agb::kRegSoundBias, {profile_.soundbias}, 2),
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

    std::optional<long long> LastSoundMs(int song) const override
    {
        return EndOfSong(profile_.ends, song);
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

        // Two passes of a loop, then the fade; or the end of the song.
        const double frame_ms = 1000.0 / agblib::agb::kFrameRate;
        if (flow.kind == ite::Flow::Kind::kLoop)
        {
            const uint64_t pass = flow.loop_end - flow.loop_start;
            const uint64_t passes = flow.loop_start + xsflib::rip::kLengthLoops * pass;
            const auto pass_frames = uint64_t(std::llround(ite::FramesOfSamples(pass)));
            return xsflib::rip::PassesAt(Frames(passes), pass_frames, frame_ms, kFadeMs);
        }

        // The samples of a song can ring on after its F00. The timing then ends at the last sound. The coverage then
        // runs past that sound.
        if (flow.kind == ite::Flow::Kind::kEnd)
        {
            uint64_t end = Frames(flow.end);
            if (const std::optional<long long> last = LastSoundMs(song))
            {
                end = std::max(end, uint64_t(std::ceil(double(*last) / frame_ms)));
            }

            return xsflib::rip::EndAt(end, frame_ms);
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

} // namespace

std::unique_ptr<Driver> FindIte(const agblib::agb::Rom& rom, std::string& error)
{
    ite::DriverInfo info;
    if (!ite::DetectDriver(rom, info, error))
    {
        return nullptr;
    }

    const std::string code = rom.GameCode();
    const uint8_t version = rom.Version();
    const IteProfile* profile = FindProfile({&HugoProfile(), 1}, code, version, &IteProfile::init, info.init);
    if (!profile)
    {
        error = NoProfileError("ITE Media's driver", GameWithRoutine(code, version, "init", info.init));
        return nullptr;
    }

    return std::make_unique<IteDriver>(rom, std::move(info), *profile);
}

} // namespace gsf::rip
