// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

#include "common/assert.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "core/file_format/psf.h"

static const std::unordered_map<std::string_view, u32> psf_known_max_sizes = {
    {"ACCOUNT_ID", 8},  {"CATEGORY", 4},  {"DETAIL", 1024},       {"FORMAT", 4},
    {"MAINTITLE", 128}, {"PARAMS", 1024}, {"SAVEDATA_BLOCKS", 8}, {"SAVEDATA_DIRECTORY", 32},
    {"SUBTITLE", 128},  {"TITLE_ID", 12},
};
static inline u32 get_max_size(std::string_view key, u32 default_value) {
    if (const auto& v = psf_known_max_sizes.find(key); v != psf_known_max_sizes.end()) {
        return v->second;
    }
    return default_value;
}

bool PSF::Open(const std::filesystem::path& filepath) try {
    using namespace std::chrono;
    if (std::filesystem::exists(filepath)) {
        const auto t = std::filesystem::last_write_time(filepath);
        const auto rel =
            duration_cast<seconds>(t - std::filesystem::file_time_type::clock::now()).count();
        const auto tp = system_clock::to_time_t(system_clock::now() + seconds{rel});
        last_write = system_clock::from_time_t(tp);
    }

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        return false;
    }

    const u64 psf_size = file.GetSize();
    if (psf_size > std::numeric_limits<size_t>::max()) {
        LOG_ERROR(Core, "PSF file size cannot be represented in memory: {}", psf_size);
        return false;
    }

    const size_t buffer_size = static_cast<size_t>(psf_size);
    std::vector<u8> psf;
    if (buffer_size > psf.max_size()) {
        LOG_ERROR(Core, "PSF file exceeds maximum supported buffer size: {}", psf_size);
        return false;
    }
    psf.resize(buffer_size);
    if (!file.Seek(0)) {
        LOG_ERROR(Core, "Failed to seek to the start of PSF file");
        return false;
    }
    if (file.Read(psf) != psf.size()) {
        LOG_ERROR(Core, "Failed to read complete PSF file");
        return false;
    }
    file.Close();
    return Open(psf);
} catch (const std::bad_alloc&) {
    LOG_ERROR(Core, "Insufficient memory to read PSF file");
    return false;
} catch (const std::length_error&) {
    LOG_ERROR(Core, "PSF file exceeds supported memory limits");
    return false;
}

bool PSF::Open(const std::vector<u8>& psf_buffer) try {
    entry_list.clear();
    map_binaries.clear();
    map_strings.clear();
    map_integers.clear();

    if (psf_buffer.size() < sizeof(PSFHeader)) {
        LOG_ERROR(Core, "Truncated PSF header");
        return false;
    }

    const u8* psf_data = psf_buffer.data();
    PSFHeader header{};
    std::memcpy(&header, psf_data, sizeof(header));

    if (header.magic != PSF_MAGIC) {
        LOG_ERROR(Core, "Invalid PSF magic number");
        return false;
    }
    if (header.version != PSF_VERSION_1_1 && header.version != PSF_VERSION_1_0) {
        LOG_ERROR(Core, "Unsupported PSF version: 0x{:08x}", header.version);
        return false;
    }

    const size_t buffer_size = psf_buffer.size();
    const u64 entry_count_u64 = header.index_table_entries;
    const size_t entry_table_start = sizeof(PSFHeader);
    if (entry_count_u64 > (buffer_size - entry_table_start) / sizeof(PSFRawEntry)) {
        LOG_ERROR(Core, "PSF index table extends past the end of the file");
        return false;
    }
    const size_t entry_count = static_cast<size_t>(entry_count_u64);
    const size_t entry_table_end = entry_table_start + entry_count * sizeof(PSFRawEntry);

    const u64 key_table_offset_u64 = header.key_table_offset;
    const u64 data_table_offset_u64 = header.data_table_offset;
    if (key_table_offset_u64 < entry_table_end || key_table_offset_u64 > buffer_size ||
        data_table_offset_u64 < key_table_offset_u64 || data_table_offset_u64 > buffer_size) {
        LOG_ERROR(Core, "Invalid PSF key or data table offset");
        return false;
    }
    const size_t key_table_offset = static_cast<size_t>(key_table_offset_u64);
    const size_t data_table_offset = static_cast<size_t>(data_table_offset_u64);

    // Validate every record before allocating strings or copying entry payloads.
    for (size_t i = 0; i < entry_count; i++) {
        PSFRawEntry raw_entry{};
        const size_t entry_offset = entry_table_start + i * sizeof(PSFRawEntry);
        std::memcpy(&raw_entry, psf_data + entry_offset, sizeof(raw_entry));

        const size_t key_offset = raw_entry.key_offset;
        if (key_offset >= data_table_offset - key_table_offset) {
            LOG_ERROR(Core, "PSF entry {} has a key offset outside the key table", i);
            return false;
        }
        const char* key = reinterpret_cast<const char*>(psf_data + key_table_offset + key_offset);
        const size_t key_length = data_table_offset - key_table_offset - key_offset;
        if (key[0] == '\0' || std::memchr(key, '\0', key_length) == nullptr) {
            LOG_ERROR(Core, "PSF entry {} has an unterminated key", i);
            return false;
        }

        const size_t data_offset = raw_entry.data_offset;
        if (data_offset > buffer_size - data_table_offset) {
            LOG_ERROR(Core, "PSF entry {} has a data offset outside the file", i);
            return false;
        }
        const size_t data_length = raw_entry.param_len;
        if (data_length > buffer_size - data_table_offset - data_offset ||
            data_length > raw_entry.param_max_len) {
            LOG_ERROR(Core, "PSF entry {} has an invalid data length", i);
            return false;
        }

        switch (static_cast<PSFEntryFmt>(raw_entry.param_fmt.Raw())) {
        case PSFEntryFmt::Binary:
            break;
        case PSFEntryFmt::Text:
            if (data_length == 0 ||
                std::memchr(psf_data + data_table_offset + data_offset, '\0', data_length) ==
                    nullptr) {
                LOG_ERROR(Core, "PSF entry {} has an invalid or unterminated string", i);
                return false;
            }
            break;
        case PSFEntryFmt::Integer:
            if (data_length != sizeof(s32)) {
                LOG_ERROR(Core, "PSF integer entry {} has invalid size {}", i, data_length);
                return false;
            }
            break;
        default:
            LOG_ERROR(Core, "PSF entry {} has unknown format 0x{:04x}", i,
                      raw_entry.param_fmt.Raw());
            return false;
        }
    }

    for (size_t i = 0; i < entry_count; i++) {
        PSFRawEntry raw_entry{};
        const size_t entry_offset = entry_table_start + i * sizeof(PSFRawEntry);
        std::memcpy(&raw_entry, psf_data + entry_offset, sizeof(raw_entry));

        Entry& entry = entry_list.emplace_back();
        const size_t key_offset = raw_entry.key_offset;
        const char* key = reinterpret_cast<const char*>(psf_data + key_table_offset + key_offset);
        entry.key.assign(key);
        entry.param_fmt = static_cast<PSFEntryFmt>(raw_entry.param_fmt.Raw());
        entry.max_len = raw_entry.param_max_len;

        const size_t data_offset = raw_entry.data_offset;
        const size_t data_length = raw_entry.param_len;
        const u8* data = psf_data + data_table_offset + data_offset;

        switch (entry.param_fmt) {
        case PSFEntryFmt::Binary: {
            std::vector<u8> value(data_length);
            if (data_length != 0) {
                std::memcpy(value.data(), data, data_length);
            }
            map_binaries.emplace(i, std::move(value));
        } break;
        case PSFEntryFmt::Text: {
            const char* terminator = static_cast<const char*>(std::memchr(data, '\0', data_length));
            const auto* text_data = reinterpret_cast<const char*>(data);
            const size_t string_length = static_cast<size_t>(terminator - text_data);
            map_strings.emplace(i, std::string(text_data, string_length));
        } break;
        case PSFEntryFmt::Integer: {
            s32 integer{};
            std::memcpy(&integer, data, sizeof(integer));
            map_integers.emplace(i, integer);
        } break;
        default:
            // The validation pass rejects all other formats.
            return false;
        }
    }
    return true;
} catch (const std::bad_alloc&) {
    entry_list.clear();
    map_binaries.clear();
    map_strings.clear();
    map_integers.clear();
    LOG_ERROR(Core, "Insufficient memory to parse PSF data");
    return false;
} catch (const std::length_error&) {
    entry_list.clear();
    map_binaries.clear();
    map_strings.clear();
    map_integers.clear();
    LOG_ERROR(Core, "PSF data exceeds supported memory limits");
    return false;
}

bool PSF::Encode(const std::filesystem::path& filepath) const {
    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Write);
    if (!file.IsOpen()) {
        return false;
    }

    last_write = std::chrono::system_clock::now();

    const auto psf_buffer = Encode();
    const size_t written = file.Write(psf_buffer);
    if (written != psf_buffer.size()) {
        LOG_ERROR(Core, "Failed to write PSF file. Written {} Expected {}", written,
                  psf_buffer.size());
    }
    return written == psf_buffer.size();
}

std::vector<u8> PSF::Encode() const {
    std::vector<u8> psf_buffer;
    Encode(psf_buffer);
    return psf_buffer;
}

void PSF::Encode(std::vector<u8>& psf_buffer) const {
    psf_buffer.resize(sizeof(PSFHeader) + sizeof(PSFRawEntry) * entry_list.size());

    {
        auto& header = *(PSFHeader*)psf_buffer.data();
        header.magic = PSF_MAGIC;
        header.version = PSF_VERSION_1_1;
        header.index_table_entries = entry_list.size();
    }

    const size_t key_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->key_table_offset = key_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.key_offset = psf_buffer.size() - key_table_offset;
        raw_entry.param_fmt.FromRaw(static_cast<u16>(entry.param_fmt));
        raw_entry.param_max_len = entry.max_len;
        std::ranges::copy(entry.key, std::back_inserter(psf_buffer));
        psf_buffer.push_back(0); // NULL terminator
    }

    const size_t data_table_offset = psf_buffer.size();
    ((PSFHeader*)psf_buffer.data())->data_table_offset = data_table_offset;
    for (size_t i = 0; i < entry_list.size(); i++) {
        if (psf_buffer.size() % 4 != 0) {
            std::ranges::fill_n(std::back_inserter(psf_buffer), 4 - psf_buffer.size() % 4, 0);
        }
        auto& raw_entry = ((PSFRawEntry*)(psf_buffer.data() + sizeof(PSFHeader)))[i];
        const Entry& entry = entry_list[i];
        raw_entry.data_offset = psf_buffer.size() - data_table_offset;

        s32 additional_padding = s32(raw_entry.param_max_len);

        switch (entry.param_fmt) {
        case PSFEntryFmt::Binary: {
            const auto& value = map_binaries.at(i);
            raw_entry.param_len = value.size();
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
        } break;
        case PSFEntryFmt::Text: {
            const auto& value = map_strings.at(i);
            raw_entry.param_len = value.size() + 1;
            additional_padding -= s32(raw_entry.param_len);
            std::ranges::copy(value, std::back_inserter(psf_buffer));
            psf_buffer.push_back(0); // NULL terminator
        } break;
        case PSFEntryFmt::Integer: {
            const auto& value = map_integers.at(i);
            raw_entry.param_len = sizeof(s32);
            additional_padding -= s32(raw_entry.param_len);
            const auto value_bytes = reinterpret_cast<const u8*>(&value);
            std::ranges::copy(value_bytes, value_bytes + sizeof(s32),
                              std::back_inserter(psf_buffer));
        } break;
        default:
            UNREACHABLE_MSG("Unknown PSF entry format");
        }
        ASSERT_MSG(additional_padding >= 0, "PSF entry max size mismatch");
        std::ranges::fill_n(std::back_inserter(psf_buffer), additional_padding, 0);
    }
}

std::optional<std::span<const u8>> PSF::GetBinary(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Binary) {
        return {};
    }
    return std::span{map_binaries.at(index)};
}

std::optional<std::string_view> PSF::GetString(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Text) {
        return {};
    }
    return std::string_view{map_strings.at(index)};
}

std::optional<s32> PSF::GetInteger(std::string_view key) const {
    const auto& [it, index] = FindEntry(key);
    if (it == entry_list.end() || it->param_fmt != PSFEntryFmt::Integer) {
        return {};
    }
    return map_integers.at(index);
}

void PSF::AddBinary(std::string key, std::vector<u8> value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add binary key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Binary, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size());
        map_binaries.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size());
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Binary;
    map_binaries.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddBinary(std::string key, uint64_t value, bool update) {
    std::vector<u8> data(8);
    std::memcpy(data.data(), &value, 8);
    return AddBinary(std::move(key), std::move(data), update);
}

void PSF::AddString(std::string key, std::string value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add string key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Text, "PSF: Change format is not supported");
        it->max_len = get_max_size(key, value.size() + 1);
        map_strings.at(index) = std::move(value);
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.max_len = get_max_size(key, value.size() + 1);
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Text;
    map_strings.emplace(entry_list.size() - 1, std::move(value));
}

void PSF::AddInteger(std::string key, s32 value, bool update) {
    auto [it, index] = FindEntry(key);
    bool exist = it != entry_list.end();
    if (exist && !update) {
        LOG_ERROR(Core, "PSF: Tried to add integer key that already exists: {}", key);
        return;
    }
    if (exist) {
        ASSERT_MSG(it->param_fmt == PSFEntryFmt::Integer, "PSF: Change format is not supported");
        it->max_len = sizeof(s32);
        map_integers.at(index) = value;
        return;
    }
    Entry& entry = entry_list.emplace_back();
    entry.key = std::move(key);
    entry.param_fmt = PSFEntryFmt::Integer;
    entry.max_len = sizeof(s32);
    map_integers.emplace(entry_list.size() - 1, value);
}

std::pair<std::vector<PSF::Entry>::iterator, size_t> PSF::FindEntry(std::string_view key) {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}

std::pair<std::vector<PSF::Entry>::const_iterator, size_t> PSF::FindEntry(
    std::string_view key) const {
    auto entry =
        std::ranges::find_if(entry_list, [&](const auto& entry) { return entry.key == key; });
    return {entry, std::distance(entry_list.begin(), entry)};
}
