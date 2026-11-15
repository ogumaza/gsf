// SPDX-License-Identifier: MIT

// Tests of the pieces of a rip (rip/rip.h): the stub's place, the coverage's length, the trim, the check of the timed
// DMAs and the set's files.

#include "rip/rip.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "agblib/agb/gba.h"
#include "agblib/agb/rom.h"
#include "xsflib/bytes.h"
#include "xsflib/rip/length.h"
#include "xsflib/testing/check.h"
#include "xsflib/testing/sets.h"

#include "gsf/gsf_file.h"
#include "rip/driver.h"
#include "rip/model.h"
#include "rip/stub.h"

namespace
{

using gsf::rip::Operation;
using gsf::rip::SetImage;
using gsf::rip::StubPlan;

constexpr uint32_t kRomBase = 0x08000000;

StubPlan SmallPlan()
{
    StubPlan plan;
    plan.start = {Operation::Call(0x08000201, {{1}, {0, true}})};
    plan.handlers = {{gsf::rip::kIrqVblank, {Operation::Call(0x08000301)}}};
    plan.frame = {Operation::Call(0x08000401)};
    return plan;
}

void TestPlaceStubAfterImage()
{
    // An image whose size isn't a multiple of 4: the stub starts at the next multiple.
    const std::vector<uint8_t> rom(0x1001, 0x55);
    std::string error;

    const std::optional<SetImage> set = gsf::rip::PlaceStub(rom, SmallPlan(), 2, error);

    XSFLIB_REQUIRE(set.has_value());
    XSFLIB_CHECK(set->stub.address == kRomBase + 0x1004);
    XSFLIB_CHECK(set->image.size() == 0x1004 + set->stub.code.size());
    XSFLIB_CHECK(!gsf::rip::StubInImage(rom.size(), SmallPlan()));
    XSFLIB_CHECK(set->image[0x1000] == 0x55 && set->image[0x1001] == 0 && set->image[4] == 0x55);
    XSFLIB_CHECK(xsflib::LoadLe32(set->image.data()) == gsf::rip::BranchWord(kRomBase, kRomBase + 0x1004));
    XSFLIB_CHECK(xsflib::LoadLe32(&set->image[set->stub.song_address - kRomBase]) == 2);
}

void TestPlaceStubInPadding()
{
    // A full 32 MiB image puts the stub in the padding at its end. One with too little padding has no room.
    std::vector<uint8_t> rom(gsf::kMaxImageSize, 0xFF);
    for (size_t i = 0; i < 0x1FFF001; i++)
    {
        rom[i] = 0x11;
    }
    std::string error;

    const std::optional<SetImage> set = gsf::rip::PlaceStub(rom, SmallPlan(), 0, error);

    // A constant that equals an address in the stub takes a word of the pool of its own, wherever the stub goes.
    StubPlan shared = SmallPlan();
    shared.start.push_back(Operation::Call(0x08000201, {{kRomBase + 0x1FFF004}, {kRomBase + 0x1FFF008}}));
    const std::optional<SetImage> placed = gsf::rip::PlaceStub(rom, shared, 0, error);

    // An image that ends in data puts the stub in its longest run of 0x00 or 0xFF. A longer run of another value isn't
    // padding.
    std::vector<uint8_t> data(gsf::kMaxImageSize, 0x11);
    std::fill(data.begin() + 0x100001, data.begin() + 0x101000, 0x00);
    std::fill(data.begin() + 0x1000000, data.begin() + 0x1100000, 0x22);
    std::fill(data.begin() + 0x1800000, data.begin() + 0x1800800, 0xFF);
    std::fill(data.end() - 3, data.end(), 0xFF);
    const std::optional<SetImage> inside = gsf::rip::PlaceStub(data, SmallPlan(), 0, error);

    // The longest run is too short for the stub.
    std::fill(data.begin() + 0x100001, data.begin() + 0x101000, 0x11);
    std::fill(data.begin() + 0x1800000, data.begin() + 0x1800800, 0x11);
    std::fill(data.begin() + 0x1800000, data.begin() + 0x1800020, 0xFF);
    const std::optional<SetImage> none = gsf::rip::PlaceStub(data, SmallPlan(), 0, error);

    XSFLIB_REQUIRE(set.has_value());
    XSFLIB_CHECK(set->stub.address == kRomBase + 0x1FFF004);
    XSFLIB_CHECK(gsf::rip::StubInImage(rom.size(), SmallPlan()));
    XSFLIB_CHECK(set->image.size() == gsf::kMaxImageSize);
    XSFLIB_CHECK(set->stub.code.size() == gsf::rip::StubSize(SmallPlan()));
    XSFLIB_REQUIRE(placed.has_value());
    XSFLIB_CHECK(placed->stub.code.size() == gsf::rip::StubSize(shared));
    XSFLIB_REQUIRE(inside.has_value());
    XSFLIB_CHECK(inside->stub.address == kRomBase + 0x100004);
    XSFLIB_CHECK(inside->image.size() == gsf::kMaxImageSize && inside->image[0x1FFFFFF] == 0xFF);
    XSFLIB_CHECK(!none && error.find("no room") != std::string::npos);
}

void TestCoverageFrames()
{
    // A loop of 60 s plays 60 s, a fade of 10 s, 30 s more and the margin. A song that ends plays its length and the
    // margin. A song without a timing plays 300 s and the margin.
    xsflib::rip::Timing loop;
    loop.length_ms = 60000;
    loop.fade_ms = 10000;
    xsflib::rip::Timing end;
    end.length_ms = 5000;

    XSFLIB_CHECK(gsf::rip::CoverageFrames(loop, 10) == uint64_t(std::ceil(110 * agblib::agb::kFrameRate)));
    XSFLIB_CHECK(gsf::rip::CoverageFrames(end, 10) == uint64_t(std::ceil(15 * agblib::agb::kFrameRate)));
    XSFLIB_CHECK(gsf::rip::CoverageFrames(std::nullopt, 0) == uint64_t(std::ceil(300 * agblib::agb::kFrameRate)));
}

void TestTrim()
{
    const std::vector<uint8_t> rom(0x400, 0x77);
    std::string error;
    std::optional<SetImage> set = gsf::rip::PlaceStub(rom, SmallPlan(), 0, error);
    XSFLIB_REQUIRE(set.has_value());
    std::vector<uint8_t> reads(0x300);
    reads[0x200] = 1;
    reads[0x2FF] = 1;
    std::vector<uint8_t> all;
    gsf::rip::AddReads(all, reads);

    const uint64_t kept = gsf::rip::Trim(*set, all);

    // The header, the bytes read and the stub stay. The rest are zeros.
    XSFLIB_CHECK(kept == gsf::rip::kHeaderSize + 2 + set->stub.code.size());
    XSFLIB_CHECK(set->image[gsf::rip::kHeaderSize - 1] == 0x77 && set->image[gsf::rip::kHeaderSize] == 0);
    XSFLIB_CHECK(set->image[0x200] == 0x77 && set->image[0x201] == 0 && set->image[0x2FF] == 0x77);
    XSFLIB_CHECK(set->image[0x3FF] == 0);
    XSFLIB_CHECK(xsflib::LoadLe32(set->image.data()) == gsf::rip::BranchWord(kRomBase, set->stub.address));
    XSFLIB_CHECK(!gsf::rip::CheckStubRoom(*set, all));
}

void TestCheckStubRoom()
{
    std::vector<uint8_t> rom(gsf::kMaxImageSize, 0xFF);
    rom[0x1000] = 0x11;
    std::string error;
    const std::optional<SetImage> set = gsf::rip::PlaceStub(rom, SmallPlan(), 0, error);
    XSFLIB_REQUIRE(set.has_value());

    // A run that read the padding where the stub went.
    std::vector<uint8_t> reads(gsf::kMaxImageSize);
    reads[set->stub.address - kRomBase + 8] = 1;

    XSFLIB_CHECK(gsf::rip::CheckStubRoom(*set, reads).has_value());
}

void TestSetFiles()
{
    const std::vector<uint8_t> rom(0x200, 0x33);
    std::string error;
    const std::optional<SetImage> set = gsf::rip::PlaceStub(rom, SmallPlan(), 0, error);
    XSFLIB_REQUIRE(set.has_value());
    xsflib::psf::Tags tags;
    tags["title"] = "Song 9";

    const std::map<std::string, std::vector<uint8_t>> files = {
        {"game.gsflib", gsf::rip::LibraryFile(*set, {})},
        {"0009 Song 9.minigsf", gsf::rip::MiniFile(*set, 9, "game.gsflib", tags)},
    };
    gsf::LoadedSet loaded;
    const auto problem = gsf::LoadSet("0009 Song 9.minigsf", xsflib::testing::MapReader(files), loaded);

    // The set gives the library's image with the mini's song number.
    XSFLIB_REQUIRE(!problem);
    XSFLIB_CHECK(loaded.entry == kRomBase);
    XSFLIB_REQUIRE(loaded.image.size() == set->image.size());
    XSFLIB_CHECK(xsflib::LoadLe32(&loaded.image[set->stub.song_address - kRomBase]) == 9);
    XSFLIB_CHECK(loaded.tags.at("_lib") == "game.gsflib" && loaded.tags.at("title") == "Song 9");
    std::vector<uint8_t> library_image = loaded.image;
    xsflib::StoreLe32(&library_image[set->stub.song_address - kRomBase], 0);
    XSFLIB_CHECK(library_image == set->image);
}

void TestTimedDmas()
{
    // DMA 1 for FIFO A from 0x08000100, DMA 3 at the vertical blank from 0x08000300 (8 halfwords), DMA 2 for FIFO B
    // from the work memory, and an immediate DMA 3 from the cartridge. The last two aren't timed DMAs that read it.
    std::vector<uint8_t> image(0x400, 0x44);
    StubPlan plan;
    plan.start = {
        Operation::Store(0x040000BC, {kRomBase + 0x100}, 4),
        Operation::Store(0x040000C0, {0x040000A0}, 4),
        Operation::Store(0x040000C6, {0xB640}, 2),
        Operation::Store(0x040000C8, {0x03000000}, 4),
        Operation::Store(0x040000CC, {0x040000A4}, 4),
        Operation::Store(0x040000D2, {0xB640}, 2),
        Operation::Store(0x040000D4, {kRomBase + 0x200}, 4),
        Operation::Store(0x040000D8, {0x02000000}, 4),
        Operation::Store(0x040000DC, {4}, 2),
        Operation::Store(0x040000DE, {0x8000}, 2),
        Operation::Store(0x040000D4, {kRomBase + 0x300}, 4),
        Operation::Store(0x040000DC, {8}, 2),
        Operation::Store(0x040000DE, {0x9000}, 2),
    };
    agblib::agb::Rom rom;
    rom.Assign(image);

    gsf::rip::ModelRun run(rom, plan, 0, nullptr);

    XSFLIB_REQUIRE(run.TimedDmas().size() == 2);
    XSFLIB_CHECK(run.TimedDmaStarts() == 2);
    const gsf::rip::TimedDma fifo = *run.TimedDmas().begin();
    const gsf::rip::TimedDma blank = *run.TimedDmas().rbegin();
    XSFLIB_CHECK(fifo.channel == 1 && fifo.source == kRomBase + 0x100 && fifo.control == 0xB640);
    XSFLIB_CHECK(blank.channel == 3 && blank.source == kRomBase + 0x300 && blank.count == 8);

    // A FIFO's transfer is four words. The blank's is its count of halfwords.
    const gsf::rip::DmaBytes fifo_bytes = gsf::rip::FirstTransfer(fifo);
    const gsf::rip::DmaBytes blank_bytes = gsf::rip::FirstTransfer(blank);
    XSFLIB_CHECK(fifo_bytes.offset == 0x100 && fifo_bytes.size == 16);
    XSFLIB_CHECK(blank_bytes.offset == 0x300 && blank_bytes.size == 16);

    // A source that stays reads one unit. One that counts down reads the units below it.
    gsf::rip::TimedDma fixed = blank;
    fixed.control = 0x9100;
    gsf::rip::TimedDma down = blank;
    down.control = 0x9080;
    XSFLIB_CHECK(gsf::rip::FirstTransfer(fixed).offset == 0x300 && gsf::rip::FirstTransfer(fixed).size == 2);
    XSFLIB_CHECK(gsf::rip::FirstTransfer(down).offset == 0x2F2 && gsf::rip::FirstTransfer(down).size == 16);

    // The trim has to keep each first transfer whole.
    std::string error;
    const std::optional<SetImage> set = gsf::rip::PlaceStub(image, SmallPlan(), 0, error);
    XSFLIB_REQUIRE(set.has_value());
    std::vector<uint8_t> reads(0x400);
    std::fill_n(reads.begin() + 0x100, 16, uint8_t{1});
    std::fill_n(reads.begin() + 0x300, 16, uint8_t{1});
    XSFLIB_CHECK(!gsf::rip::CheckTimedDmas(*set, reads, run.TimedDmas()));
    reads[0x30F] = 0;
    const std::optional<std::string> cut = gsf::rip::CheckTimedDmas(*set, reads, run.TimedDmas());
    XSFLIB_CHECK(cut && cut->find("DMA 3") != std::string::npos && cut->find("0x0800030F") != std::string::npos);
}

void TestTimedDmaStarts()
{
    // DMA 1 for FIFO A, with repeat, and DMA 3 at the vertical blank, without. A write to the control of DMA 1 while it
    // runs starts nothing, and the new source stays unread. DMA 1 starts again once the code clears its enable bit. DMA
    // 3 may have ended its one transfer before any write, so each write that sets its enable bit counts as a start,
    // twice in one frame too.
    std::vector<uint8_t> image(0x400, 0x44);
    agblib::agb::Rom rom;
    rom.Assign(image);
    StubPlan plan;
    plan.start = {
        Operation::Store(0x040000BC, {kRomBase + 0x100}, 4), Operation::Store(0x040000C6, {0xB640}, 2),
        Operation::Store(0x040000BC, {kRomBase + 0x180}, 4), Operation::Store(0x040000C6, {0xB640}, 2),
        Operation::Store(0x040000D4, {kRomBase + 0x300}, 4), Operation::Store(0x040000DC, {8}, 2),
        Operation::Store(0x040000DE, {0x9000}, 2),           Operation::Store(0x040000D4, {kRomBase + 0x340}, 4),
        Operation::Store(0x040000DE, {0x9000}, 2),
    };
    gsf::rip::ModelRun run(rom, plan, 0, nullptr);
    const uint64_t starts_before_frame = run.TimedDmaStarts();

    run.Frame();
    agblib::agb::System& gba = run.Gba();
    gba.Store(0x040000BC, 4, kRomBase + 0x200);
    gba.Store(0x040000C6, 2, 0xB640);
    gba.Store(0x040000D4, 4, kRomBase + 0x320);
    gba.Store(0x040000DE, 2, 0x9000);
    gba.Store(0x040000C6, 2, 0x0000);
    gba.Store(0x040000C6, 2, 0xB640);

    XSFLIB_CHECK(starts_before_frame == 3);
    XSFLIB_CHECK(run.TimedDmaStarts() == 5);
    std::vector<std::pair<int, uint32_t>> started;
    for (const gsf::rip::TimedDma& dma : run.TimedDmas())
    {
        started.emplace_back(dma.channel, dma.source - kRomBase);
    }
    XSFLIB_CHECK(started ==
                 (std::vector<std::pair<int, uint32_t>>{{1, 0x100}, {1, 0x200}, {3, 0x300}, {3, 0x320}, {3, 0x340}}));
}

void TestTagsOfTiming()
{
    // A loop: its length to the nearest second and a fade of 10 s. An end: the end to the millisecond, without the
    // tail, and the tail of 1 s as the fade.
    xsflib::rip::Timing loop;
    loop.length_ms = 100'004;
    loop.fade_ms = 10'000;
    xsflib::rip::Timing end;
    end.length_ms = 2'934;

    const gsf::rip::LengthTags loop_tags = gsf::rip::TagsOfTiming(loop);
    const gsf::rip::LengthTags end_tags = gsf::rip::TagsOfTiming(end);

    XSFLIB_CHECK(loop_tags.length == "1:40" && loop_tags.fade == "10");
    XSFLIB_CHECK(end_tags.length == "0:01.934" && end_tags.fade == "1");
    loop.length_ms = 59'500;
    XSFLIB_CHECK(gsf::rip::TagsOfTiming(loop).length == "1:00");

    // A song's last sound, where a profile gives it, replaces the end of its commands. A loop keeps its length.
    const gsf::rip::LengthTags sound = gsf::rip::TagsOfTiming(end, 1'808);
    XSFLIB_CHECK(sound.length == "0:01.808" && sound.fade == "1");
    XSFLIB_CHECK(gsf::rip::TagsOfTiming(loop, 1'808).length == "1:00");
}

void TestNames()
{
    XSFLIB_CHECK(gsf::rip::FileTitle("Title Screen, Circus Act 1") == "Title Screen, Circus Act 1");
    XSFLIB_CHECK(gsf::rip::FileTitle("A/B: C?.") == "A_B_ C__");

    // "AGB-", the game code and its region, from made-up game codes. A last letter that names no region adds none.
    XSFLIB_CHECK(gsf::rip::ProductCode("ZZZP") == "AGB-ZZZP-EUR");
    XSFLIB_CHECK(gsf::rip::ProductCode("ZZZE") == "AGB-ZZZE-USA");
    XSFLIB_CHECK(gsf::rip::ProductCode("ZZZJ") == "AGB-ZZZJ-JPN");
    XSFLIB_CHECK(gsf::rip::ProductCode("ZZZX") == "AGB-ZZZX");
    XSFLIB_CHECK(gsf::rip::ProductCode("AB C").empty() && gsf::rip::ProductCode("").empty());
}

void TestMarkCodeSpan()
{
    // Routines at 0x100 and 0x200, with a tail of 0x100. The span runs from the lowest routine to the last read within
    // the tail. A read past the tail stays as it is and doesn't move the span's end.
    std::vector<uint8_t> reads(0x800);
    reads[0x210] = 1;
    reads[0x2F0] = 1;
    reads[0x380] = 1;
    gsf::rip::MarkCodeSpan(reads, {kRomBase + 0x201, kRomBase + 0x101}, 0x100);

    XSFLIB_CHECK(reads[0xFF] == 0);
    XSFLIB_CHECK(std::all_of(reads.begin() + 0x100, reads.begin() + 0x2F1, [](uint8_t mark) { return mark == 1; }));
    XSFLIB_CHECK(reads[0x2F1] == 0 && reads[0x37F] == 0 && reads[0x380] == 1);

    // A routine outside the cartridge marks nothing.
    std::vector<uint8_t> other(0x800);
    other[0x110] = 1;
    gsf::rip::MarkCodeSpan(other, {0x03000001, kRomBase + 0x101}, 0x100);

    XSFLIB_CHECK(std::count(other.begin(), other.end(), 1) == 1);
}

} // namespace

int main()
{
    TestPlaceStubAfterImage();
    TestPlaceStubInPadding();
    TestCoverageFrames();
    TestTrim();
    TestCheckStubRoom();
    TestSetFiles();
    TestTimedDmas();
    TestTimedDmaStarts();
    TestTagsOfTiming();
    TestNames();
    TestMarkCodeSpan();

    return xsflib::testing::Summary("rip");
}
