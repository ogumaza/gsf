// SPDX-License-Identifier: MIT

#include "gsf/gsf_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "xsflib/bytes.h"
#include "xsflib/psf/psf.h"
#include "xsflib/psf/set.h"

namespace gsf
{

std::vector<uint8_t> SectionProgram(const Section& section)
{
    std::vector<uint8_t> program;
    program.reserve(kSectionHeaderSize + section.data.size());
    xsflib::psf::Put32(program, section.entry);
    xsflib::psf::Put32(program, section.address);
    xsflib::psf::Put32(program, uint32_t(section.data.size()));
    program.insert(program.end(), section.data.begin(), section.data.end());

    return program;
}

std::optional<std::string> ParseSection(const std::vector<uint8_t>& program, Section& section)
{
    if (program.size() < kSectionHeaderSize)
    {
        return "the program is shorter than a section's header";
    }

    const uint32_t size = xsflib::LoadLe32(&program[8]);
    if (size > program.size() - kSectionHeaderSize || size > kMaxImageSize)
    {
        return "the section is larger than the program holds";
    }

    section.entry = xsflib::LoadLe32(&program[0]);
    section.address = xsflib::LoadLe32(&program[4]);
    section.data.assign(program.begin() + kSectionHeaderSize, program.begin() + kSectionHeaderSize + size);

    return std::nullopt;
}

std::vector<uint8_t> WriteGsf(const Section& section, const xsflib::psf::Tags& tags)
{
    return xsflib::psf::WritePsf(kGsfVersion, {}, SectionProgram(section), tags);
}

uint32_t RegionBase(uint32_t address)
{
    return address >> 24 == kMultibootBase >> 24 ? kMultibootBase : kRomBase;
}

std::optional<std::string> LoadSet(const std::string& path, const xsflib::psf::FileReader& reader, LoadedSet& out)
{
    out = LoadedSet{};
    bool first = true;

    const auto parse = [](const std::string& file_path, const std::vector<uint8_t>& data, int,
                          xsflib::psf::PsfFile& psf) -> std::optional<std::string>
    {
        if (auto err = xsflib::psf::ParsePsf(data, psf))
        {
            return file_path + ": " + *err;
        }

        if (psf.version != kGsfVersion)
        {
            return file_path + ": not a GSF file";
        }

        return std::nullopt;
    };

    const auto add = [&out, &first](const std::string& file_path, const std::vector<uint8_t>&, int,
                                    const xsflib::psf::PsfFile& psf) -> std::optional<std::string>
    {
        Section section;
        if (auto err = ParseSection(psf.program, section))
        {
            return file_path + ": " + *err;
        }

        if (first)
        {
            out.entry = section.entry;
            first = false;
        }

        // No version of the format defines the reserved area's layout, so the set keeps it as it is.
        if (!psf.reserved.empty())
        {
            out.reserved.push_back(psf.reserved);
        }

        // A section's place in the image is its address within its region. The image grows to hold it.
        const size_t at = section.address & (kMaxImageSize - 1);
        if (at + section.data.size() > kMaxImageSize)
        {
            return file_path + ": the section runs past the region's 32 MiB";
        }

        if (out.image.size() < at + section.data.size())
        {
            out.image.resize(at + section.data.size());
        }

        std::copy(section.data.begin(), section.data.end(), out.image.begin() + ptrdiff_t(at));

        return std::nullopt;
    };

    xsflib::psf::PsfFile top;
    if (auto err = xsflib::psf::WalkSet(path, reader, "/\\", top, parse, add))
    {
        return err;
    }

    out.tags = std::move(top.tags);

    return std::nullopt;
}

} // namespace gsf
