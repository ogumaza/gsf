// SPDX-License-Identifier: MIT

// Tests of the bytes of Nintendo R&D1's driver that a rip keeps whole (rip/drivers/rd1.h), on a cartridge laid out by
// hand: one bank with a sample's instrument and a drum kit whose key plays a PSG channel.

#include "rip/drivers/rd1.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "agblib/agb/rom.h"
#include "agblib/rd1/driver.h"
#include "xsflib/bytes.h"
#include "xsflib/testing/check.h"

#include "rip/driver.h"
#include "rom_spans.h"

namespace
{

using agblib::agb::kRomBase;
using gsf::rip::RomSpan;
using gsf::testing::HasSpan;

// The places of the bank's tables, records and sample in the cartridge.
constexpr uint32_t kBankTable = 0x100;
constexpr uint32_t kPrograms = 0x200;
constexpr uint32_t kSampleRecord = 0x400;
constexpr uint32_t kDrumRecord = 0x440;
constexpr uint32_t kDrumTable = 0x460;
constexpr uint32_t kPsgRecord = 0x480;
constexpr uint32_t kHeader = 0x600;
constexpr uint32_t kPoints = 0x700;
constexpr uint32_t kPointCount = 0x40;
constexpr int kDrumKey = 36;

std::vector<uint8_t> MakeImage()
{
    std::vector<uint8_t> image(0x800);
    const auto word = [&](uint32_t offset, uint32_t value)
    {
        xsflib::StoreLe32(&image[offset], value);
    };

    word(kBankTable, kRomBase + kPrograms);
    word(kPrograms, kRomBase + kSampleRecord);
    word(kPrograms + 4, kRomBase + kDrumRecord);

    image[kSampleRecord] = 'A';
    word(kSampleRecord + 4, kRomBase + kHeader);
    word(kSampleRecord + 0x1C, 0x00123456);

    image[kDrumRecord] = 'R';
    image[kDrumRecord + 1] = kDrumKey;
    word(kDrumRecord + 4, kRomBase + kDrumTable);
    word(kDrumTable, kRomBase + kPsgRecord);

    image[kPsgRecord] = 'P';

    word(kHeader, kPointCount);
    word(kHeader + 20, kRomBase + kPoints);

    return image;
}

void TestWholeSpans()
{
    // Each record whole by its kind, the sample's header and its points.
    agblib::agb::Rom rom;
    rom.Assign(MakeImage());
    agblib::rd1::DriverInfo info;
    info.bank_table = kRomBase + kBankTable;
    info.bank_count = 1;

    const std::vector<RomSpan> spans = gsf::rip::Rd1WholeSpans(rom, info);
    XSFLIB_CHECK(spans.size() == 5);
    XSFLIB_CHECK(HasSpan(spans, kRomBase + kSampleRecord, 32));
    XSFLIB_CHECK(HasSpan(spans, kRomBase + kDrumRecord, 8));
    XSFLIB_CHECK(HasSpan(spans, kRomBase + kPsgRecord, 36));
    XSFLIB_CHECK(HasSpan(spans, kRomBase + kHeader, 24));
    XSFLIB_CHECK(HasSpan(spans, kRomBase + kPoints, kPointCount));

    // A run that read the record's start but not its last field, like Rhythm Tengoku's song 584: the trim keeps the
    // record whole, and nothing that no run read any of.
    std::vector<uint8_t> reads(0x800);
    std::fill_n(reads.begin() + kSampleRecord, 0x10, uint8_t{1});
    gsf::rip::KeepWholeSpans(reads, spans);
    XSFLIB_CHECK(std::all_of(reads.begin() + kSampleRecord, reads.begin() + kSampleRecord + 32,
                             [](uint8_t mark) { return mark != 0; }));
    XSFLIB_CHECK(std::count(reads.begin(), reads.end(), uint8_t{1}) == 32);
}

void TestKeepWholeSpansGrows()
{
    // A span that runs past the marks grows them to its end.
    std::vector<uint8_t> reads(0x10);
    reads[0xC] = 1;
    const std::vector<RomSpan> spans = {{kRomBase + 0x8, 0x10}, {kRomBase + 0x20, 4}};
    gsf::rip::KeepWholeSpans(reads, spans);
    XSFLIB_CHECK(reads.size() == 0x18);
    XSFLIB_CHECK(std::count(reads.begin(), reads.end(), uint8_t{1}) == 0x10);
}

} // namespace

int main()
{
    TestWholeSpans();
    TestKeepWholeSpansGrows();

    return xsflib::testing::Summary("rd1");
}
