// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <zlib.h>

#include "common/io_file.h"
#include "core/file_format/pkg.h"
#include "core/file_format/pkg_type.h"

namespace {
constexpr u32 PkgMagic = 0x7F434E54;
constexpr u32 PfscMagic = 0x43534650;
constexpr size_t PfsBlockSize = 0x10000;
constexpr size_t PfsInodeStride = 0xA8;
constexpr size_t PfsDirentHeaderSize = 4 * sizeof(s32);

bool IsRangeWithin(u64 offset, u64 size, u64 total_size) {
    return offset <= total_size && size <= total_size - offset;
}

bool DecompressPFSC(std::span<const char> compressed_data, std::span<char> decompressed_data) {
    if (compressed_data.empty() ||
        compressed_data.size() > std::numeric_limits<uInt>::max() ||
        decompressed_data.size() > std::numeric_limits<uInt>::max()) {
        return false;
    }

    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return false;
    }

    stream.avail_in = static_cast<uInt>(compressed_data.size());
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(compressed_data.data()));
    stream.avail_out = static_cast<uInt>(decompressed_data.size());
    stream.next_out = reinterpret_cast<Bytef*>(decompressed_data.data());

    const int result = inflate(&stream, Z_FINISH);
    const bool success = result == Z_STREAM_END && stream.total_out == decompressed_data.size();
    const int end_result = inflateEnd(&stream);
    return success && end_result == Z_OK;
}

std::optional<u64> GetPFSCOffset(std::span<const u8> pfs_image) {
    if (pfs_image.size() < sizeof(PfscMagic)) {
        return std::nullopt;
    }

    for (size_t offset = 0x20000; offset <= pfs_image.size() - sizeof(PfscMagic);
         offset += 0x10000) {
        u32 value;
        std::memcpy(&value, pfs_image.data() + offset, sizeof(value));
        if (value == PfscMagic) {
            return offset;
        }
    }
    return std::nullopt;
}

bool IsRelativeTo(const std::filesystem::path& base, const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(base);
    if (relative.empty()) {
        return path == base;
    }
    if (relative.is_absolute()) {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const auto& component) {
        return component == "..";
    });
}

bool NormalizeContainedPath(const std::filesystem::path& root,
                            const std::filesystem::path& candidate,
                            std::filesystem::path& normalized_candidate,
                            std::string& failreason) {
    std::error_code error;
    const auto absolute_root = std::filesystem::absolute(root, error);
    if (error) {
        failreason = "Failed to resolve extraction root: " + error.message();
        return false;
    }
    const auto absolute_candidate = std::filesystem::absolute(candidate, error);
    if (error) {
        failreason = "Failed to resolve extraction path: " + error.message();
        return false;
    }

    const auto lexical_root = absolute_root.lexically_normal();
    normalized_candidate = absolute_candidate.lexically_normal();
    if (!IsRelativeTo(lexical_root, normalized_candidate)) {
        failreason = "PFS destination is outside the extraction root";
        return false;
    }

    const auto canonical_root = std::filesystem::weakly_canonical(lexical_root, error);
    if (error) {
        failreason = "Failed to resolve extraction root: " + error.message();
        return false;
    }
    const auto canonical_candidate = std::filesystem::weakly_canonical(normalized_candidate, error);
    if (error) {
        failreason = "Failed to resolve extraction path: " + error.message();
        return false;
    }
    if (!IsRelativeTo(canonical_root, canonical_candidate)) {
        failreason = "PFS destination resolves outside the extraction root";
        return false;
    }
    normalized_candidate = canonical_candidate;
    return true;
}

bool CreateContainedDirectories(const std::filesystem::path& root,
                                const std::filesystem::path& directory,
                                std::string& failreason) {
    std::filesystem::path normalized_directory;
    if (!NormalizeContainedPath(root, directory, normalized_directory, failreason)) {
        return false;
    }

    std::error_code error;
    std::filesystem::create_directories(normalized_directory, error);
    if (error) {
        failreason = "Failed to create extraction directory: " + error.message();
        return false;
    }

    return NormalizeContainedPath(root, normalized_directory, normalized_directory, failreason);
}

struct PfsDirentHeader {
    s32 inode;
    s32 type;
    s32 name_length;
    s32 entry_size;
};
static_assert(sizeof(PfsDirentHeader) == PfsDirentHeaderSize);

bool ReadPfsDirent(std::span<const char> block, size_t offset, PfsDirentHeader& dirent,
                   std::string& name, bool& end_of_entries, std::string& failreason) {
    if (offset > block.size() || block.size() - offset < sizeof(dirent)) {
        failreason = "Truncated PFS directory entry header";
        return false;
    }

    std::memcpy(&dirent, block.data() + offset, sizeof(dirent));
    end_of_entries = dirent.inode == 0;
    name.clear();
    if (end_of_entries) {
        return true;
    }
    if (dirent.inode < 0 || dirent.entry_size < static_cast<s32>(sizeof(dirent)) ||
        static_cast<size_t>(dirent.entry_size) > block.size() - offset) {
        failreason = "Invalid PFS directory entry inode or size";
        return false;
    }
    if (dirent.name_length <= 0 || dirent.name_length > 512 ||
        static_cast<size_t>(dirent.name_length) >
            static_cast<size_t>(dirent.entry_size) - sizeof(dirent)) {
        failreason = "Invalid PFS directory entry name length";
        return false;
    }

    name.assign(block.data() + offset + sizeof(dirent), static_cast<size_t>(dirent.name_length));
    return true;
}

bool IsSafePfsName(s32 type, const std::string& name, std::string& failreason) {
    if (type == PFS_CURRENT_DIR) {
        if (name == ".") {
            return true;
        }
        failreason = "Invalid current-directory PFS entry";
        return false;
    }
    if (type == PFS_PARENT_DIR) {
        if (name == "..") {
            return true;
        }
        failreason = "Invalid parent-directory PFS entry";
        return false;
    }
    if (type != PFS_FILE && type != PFS_DIR) {
        failreason = "Unsupported PFS directory entry type";
        return false;
    }
    if (name.empty() || name == "." || name == ".." ||
        name.find_first_of("/\\:") != std::string::npos || name.find('\0') != std::string::npos ||
        std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32; })) {
        failreason = "Unsafe PFS path component";
        return false;
    }
#ifdef _WIN32
    if (name.find_first_of("<>\"|?*") != std::string::npos || name.back() == '.' ||
        name.back() == ' ') {
        failreason = "Invalid Windows PFS path component";
        return false;
    }

    std::string windows_name = name;
    std::transform(windows_name.begin(), windows_name.end(), windows_name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const auto extension_separator = windows_name.find('.');
    std::string device_name = windows_name.substr(0, extension_separator);
    while (!device_name.empty() && (device_name.back() == '.' || device_name.back() == ' ')) {
        device_name.pop_back();
    }
    const auto is_reserved_port = [&](std::string_view prefix) {
        if (!device_name.starts_with(prefix)) {
            return false;
        }
        const std::string_view suffix{device_name.data() + prefix.size(),
                                      device_name.size() - prefix.size()};
        return (suffix.size() == 1 && suffix[0] >= '1' && suffix[0] <= '9') ||
               suffix == "\xB9" || suffix == "\xB2" || suffix == "\xB3" ||
               suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3";
    };
    if (device_name == "CON" || device_name == "PRN" || device_name == "AUX" ||
        device_name == "NUL" || device_name == "CONIN$" || device_name == "CONOUT$" ||
        is_reserved_port("COM") || is_reserved_port("LPT")) {
        failreason = "Windows device names are not valid PFS output paths";
        return false;
    }
#endif

    const std::filesystem::path component(name);
    if (component.is_absolute() || component.has_root_path() || component.has_parent_path() ||
        component.filename() != component) {
        failreason = "Unsafe PFS path component";
        return false;
    }
    return true;
}
} // namespace

PKG::PKG() = default;

PKG::~PKG() = default;

bool PKG::Open(const std::filesystem::path& filepath, std::string& failreason) try {
    sfo.clear();
    pkgFlags.clear();

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        failreason = "Failed to open PKG file";
        return false;
    }
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader) || file.Read(pkgheader) != 1) {
        failreason = "PKG header is truncated";
        return false;
    }
    if (pkgheader.magic != PkgMagic) {
        failreason = "Invalid PKG header magic";
        return false;
    }

    const u64 declared_size = pkgheader.pkg_size;
    if (declared_size < sizeof(PKGHeader) || declared_size > pkgSize ||
        declared_size > static_cast<u64>(std::numeric_limits<s64>::max()) ||
        !IsRangeWithin(pkgheader.pkg_content_offset, pkgheader.pkg_content_size, declared_size)) {
        failreason = "Invalid PKG size or content range";
        return false;
    }

    const u64 table_offset = pkgheader.pkg_table_entry_offset;
    const u64 table_count = pkgheader.pkg_table_entry_count;
    if (table_count > std::numeric_limits<u64>::max() / sizeof(PKGEntry)) {
        failreason = "PKG table entry count overflows its byte range";
        return false;
    }
    const u64 table_size = table_count * sizeof(PKGEntry);
    if (!IsRangeWithin(table_offset, table_size, declared_size)) {
        failreason = "PKG table entry range is outside the file";
        return false;
    }

    for (const auto& flag : flagNames) {
        if (isFlagSet(pkgheader.pkg_content_flags, flag.first)) {
            if (!pkgFlags.empty()) {
                pkgFlags += ", ";
            }
            pkgFlags += flag.second;
        }
    }

    // Find title id: it is part of pkg_content_id starting at offset 0x40.
    if (!file.Seek(0x47) || file.ReadRaw<char>(pkgTitleID, sizeof(pkgTitleID)) != sizeof(pkgTitleID)) {
        failreason = "Failed to read PKG title ID";
        return false;
    }

    if (!file.Seek(static_cast<s64>(table_offset))) {
        failreason = "Failed to seek to PKG table entry offset";
        return false;
    }

    bool found_sfo = false;
    u64 sfo_offset = 0;
    u64 sfo_size = 0;
    for (u64 i = 0; i < table_count; ++i) {
        PKGEntry entry{};
        if (file.Read(entry) != 1) {
            failreason = "PKG table entry is truncated";
            return false;
        }
        const u64 entry_offset = entry.offset;
        const u64 entry_size = entry.size;
        if (!IsRangeWithin(entry_offset, entry_size, declared_size)) {
            failreason = "PKG entry range is outside the file";
            return false;
        }

        if (!found_sfo && GetEntryNameByType(entry.id) == "param.sfo") {
            if (entry_size == 0 || entry_size > std::numeric_limits<size_t>::max()) {
                failreason = "Invalid param.sfo entry size";
                return false;
            }
            sfo_offset = entry_offset;
            sfo_size = entry_size;
            found_sfo = true;
        }
    }
    if (!found_sfo) {
        failreason = "PKG is missing param.sfo";
        return false;
    }
    if (!file.Seek(static_cast<s64>(sfo_offset))) {
        failreason = "Failed to seek to param.sfo offset";
        return false;
    }
    sfo.resize(static_cast<size_t>(sfo_size));
    if (file.ReadRaw<u8>(sfo.data(), sfo.size()) != sfo.size()) {
        failreason = "param.sfo entry is truncated";
        sfo.clear();
        return false;
    }

    return true;
} catch (const std::bad_alloc&) {
    sfo.clear();
    failreason = "Insufficient memory to read PKG metadata";
    return false;
} catch (const std::length_error&) {
    sfo.clear();
    failreason = "PKG metadata exceeds supported memory limits";
    return false;
} catch (const std::filesystem::filesystem_error&) {
    sfo.clear();
    failreason = "PKG filesystem operation failed";
    return false;
}

bool PKG::Extract(const std::filesystem::path& filepath, const std::filesystem::path& extract,
                  std::string& failreason) try {
    fsTable.clear();
    iNodeBuf.clear();
    sectorMap.clear();
    extractPaths.clear();
    decNp.clear();
    pfsc_offset = 0;
    pkgpath = filepath;

    if (!NormalizeContainedPath(extract, extract, extract_path, failreason)) {
        return false;
    }

    Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
    if (!file.IsOpen()) {
        failreason = "Failed to open PKG file";
        return false;
    }
    pkgSize = file.GetSize();
    if (pkgSize < sizeof(PKGHeader) || file.Read(pkgheader) != 1) {
        failreason = "PKG header is truncated";
        return false;
    }
    if (pkgheader.magic != PkgMagic) {
        failreason = "Invalid PKG header magic";
        return false;
    }
    std::copy_n(reinterpret_cast<const char*>(pkgheader.pkg_content_id) + 7,
                sizeof(pkgTitleID), pkgTitleID);
    const std::string title_id(pkgTitleID, sizeof(pkgTitleID));
    if (!IsSafePfsName(PFS_DIR, title_id, failreason)) {
        failreason = "Invalid PKG title ID";
        return false;
    }
    std::filesystem::path pfs_root = extract_path;
    const auto parent_path = extract_path.parent_path();
    if (parent_path.filename().string() != title_id &&
        !extract_path.string().ends_with("-patch")) {
        pfs_root = parent_path / title_id;
    }
    if (!NormalizeContainedPath(extract_path, pfs_root, pfs_root, failreason)) {
        return false;
    }

    const u64 package_size = pkgheader.pkg_size;
    if (package_size < sizeof(PKGHeader) || package_size > pkgSize ||
        package_size > static_cast<u64>(std::numeric_limits<s64>::max()) ||
        !IsRangeWithin(pkgheader.pkg_content_offset, pkgheader.pkg_content_size, package_size)) {
        failreason = "Invalid PKG size or content range";
        return false;
    }

    const u64 table_offset = pkgheader.pkg_table_entry_offset;
    const u64 table_count = pkgheader.pkg_table_entry_count;
    if (table_count > std::numeric_limits<u64>::max() / sizeof(PKGEntry)) {
        failreason = "PKG table entry count overflows its byte range";
        return false;
    }
    const u64 table_size = table_count * sizeof(PKGEntry);
    if (!IsRangeWithin(table_offset, table_size, package_size)) {
        failreason = "PKG table entry range is outside the file";
        return false;
    }

    auto read_table_entry = [&](u64 index, PKGEntry& entry) {
        const u64 entry_offset = table_offset + index * sizeof(PKGEntry);
        if (!file.Seek(static_cast<s64>(entry_offset)) || file.Read(entry) != 1) {
            failreason = "PKG table entry is truncated";
            return false;
        }
        return true;
    };
    for (u64 i = 0; i < table_count; ++i) {
        PKGEntry entry{};
        if (!read_table_entry(i, entry)) {
            return false;
        }
        if (!IsRangeWithin(entry.offset, entry.size, package_size)) {
            failreason = "PKG entry range is outside the file";
            return false;
        }
        if ((entry.id == 0x400 || entry.id == 0x401 || entry.id == 0x402 || entry.id == 0x403) &&
            entry.size % CryptoPP::AES::BLOCKSIZE != 0) {
            const u64 padded_size =
                (entry.size + CryptoPP::AES::BLOCKSIZE - 1) / CryptoPP::AES::BLOCKSIZE *
                static_cast<u64>(CryptoPP::AES::BLOCKSIZE);
            if (!IsRangeWithin(entry.offset, padded_size, package_size)) {
                failreason = "Encrypted PKG entry padded range is outside the file";
                return false;
            }
        }
    }

    std::array<u8, 64> concatenated_ivkey_dk3{};
    std::array<u8, 32> seed_digest{};
    std::array<std::array<u8, 32>, 7> digest1{};
    std::array<std::array<u8, 256>, 7> key1{};
    std::array<u8, 256> imgkeydata{};
    bool have_dk3 = false;
    bool have_ekpfs_key = false;

    for (u64 i = 0; i < table_count; ++i) {
        PKGEntry entry{};
        if (!read_table_entry(i, entry)) {
            return false;
        }
        if (entry.id == 0x10) {
            constexpr u64 entry_keys_size = sizeof(seed_digest) + digest1.size() * sizeof(digest1[0]) +
                                            key1.size() * sizeof(key1[0]);
            if (entry.size < entry_keys_size) {
                failreason = "PKG entry keys are truncated";
                return false;
            }
            if (!file.Seek(static_cast<s64>(static_cast<u32>(entry.offset))) ||
                file.Read(seed_digest) != seed_digest.size()) {
                failreason = "Failed to read PKG entry keys";
                return false;
            }
            for (auto& digest : digest1) {
                if (file.Read(digest) != digest.size()) {
                    failreason = "Failed to read PKG entry key digests";
                    return false;
                }
            }
            for (auto& key : key1) {
                if (file.Read(key) != key.size()) {
                    failreason = "Failed to read PKG entry keys";
                    return false;
                }
            }
            if (!PKG::crypto.RSA2048Decrypt(dk3_, key1[3], true)) {
                failreason = "Failed to decode PKG DK3 key";
                return false;
            }
            have_dk3 = true;
        } else if (entry.id == 0x20) {
            if (!have_dk3) {
                failreason = "PKG image key is missing its DK3 key";
                return false;
            }
            if (entry.size < imgkeydata.size()) {
                failreason = "PKG image key entry is truncated";
                return false;
            }
            if (!file.Seek(static_cast<s64>(static_cast<u32>(entry.offset))) ||
                file.Read(imgkeydata) != imgkeydata.size()) {
                failreason = "Failed to read PKG image key";
                return false;
            }

            std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
            std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));
            PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3, ivKey);
            PKG::crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
            if (!PKG::crypto.RSA2048Decrypt(ekpfsKey, imgKey, false)) {
                failreason = "Failed to decode PKG PFS key";
                return false;
            }
            have_ekpfs_key = true;
        }
    }
    if (!have_dk3 || !have_ekpfs_key) {
        failreason = "PKG is missing required decryption keys";
        return false;
    }

    const u64 pfs_image_offset = pkgheader.pfs_image_offset;
    const u64 pfs_image_size = static_cast<u64>(pkgheader.pfs_cache_size) * 2;
    if (pfs_image_size == 0 || pfs_image_size > std::numeric_limits<size_t>::max() ||
        pfs_image_size % 0x1000 != 0 ||
        !IsRangeWithin(pfs_image_offset, pkgheader.pfs_image_size, package_size) ||
        pfs_image_size > pkgheader.pfs_image_size ||
        !IsRangeWithin(pfs_image_offset, pfs_image_size, package_size) ||
        !IsRangeWithin(pfs_image_offset, 0x370 + sizeof(std::array<u8, 16>), package_size)) {
        failreason = "Invalid PFS image range or size";
        return false;
    }

    std::array<u8, 16> seed{};
    if (!file.Seek(static_cast<s64>(pfs_image_offset + 0x370)) ||
        file.Read(seed) != seed.size()) {
        failreason = "Failed to read PFS seed";
        return false;
    }
    PKG::crypto.PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);

    std::vector<u8> pfs_encrypted(static_cast<size_t>(pfs_image_size));
    if (!file.Seek(static_cast<s64>(pfs_image_offset)) ||
        file.Read(pfs_encrypted) != pfs_encrypted.size()) {
        failreason = "Failed to read PFS image";
        return false;
    }
    std::vector<u8> pfs_decrypted(pfs_encrypted.size());
    PKG::crypto.decryptPFS(dataKey, tweakKey, pfs_encrypted, pfs_decrypted, 0);

    const auto found_pfsc_offset = GetPFSCOffset(pfs_decrypted);
    if (!found_pfsc_offset) {
        failreason = "PFSC marker was not found in the PFS image";
        return false;
    }
    pfsc_offset = *found_pfsc_offset;
    if (!IsRangeWithin(pfsc_offset, sizeof(PFSCHdr), pfs_decrypted.size())) {
        failreason = "PFSC header is truncated";
        return false;
    }
    std::vector<u8> pfsc(pfs_decrypted.begin() + static_cast<size_t>(pfsc_offset),
                         pfs_decrypted.end());

    PFSCHdr pfs_header{};
    std::memcpy(&pfs_header, pfsc.data(), sizeof(pfs_header));
    if (static_cast<u32>(pfs_header.magic) != PfscMagic || pfs_header.block_sz2 <= 0 ||
        pfs_header.data_length <= 0 || pfs_header.block_offsets < 0 ||
        static_cast<u64>(pfs_header.data_start) > pfsc.size()) {
        failreason = "Invalid PFSC header";
        return false;
    }

    const u64 data_length = static_cast<u64>(pfs_header.data_length);
    const u64 block_size = static_cast<u64>(pfs_header.block_sz2);
    if (data_length % block_size != 0) {
        failreason = "Invalid PFSC data length";
        return false;
    }
    const u64 block_count = data_length / block_size;
    if (block_count == 0 || block_count >= std::numeric_limits<size_t>::max()) {
        failreason = "Invalid PFSC block count";
        return false;
    }

    const u64 block_offsets = static_cast<u64>(pfs_header.block_offsets);
    if (block_offsets > pfsc.size() ||
        block_count + 1 > (pfsc.size() - static_cast<size_t>(block_offsets)) / sizeof(u64)) {
        failreason = "PFSC sector map is truncated";
        return false;
    }
    sectorMap.resize(static_cast<size_t>(block_count + 1));
    for (size_t i = 0; i < sectorMap.size(); ++i) {
        std::memcpy(&sectorMap[i], pfsc.data() + static_cast<size_t>(block_offsets) + i * sizeof(u64),
                    sizeof(u64));
    }
    if (sectorMap.empty() || sectorMap.front() > pfsc.size() || sectorMap.back() > pfsc.size()) {
        failreason = "PFSC sector map is outside the data range";
        return false;
    }
    for (size_t i = 0; i < static_cast<size_t>(block_count); ++i) {
        if (sectorMap[i + 1] <= sectorMap[i] ||
            sectorMap[i + 1] - sectorMap[i] > PfsBlockSize) {
            failreason = "Invalid PFSC sector range";
            return false;
        }
    }

    u64 inode_count = 0;
    u64 inode_blocks = 0;
    std::array<u8, sizeof(Inode)> pending_inode_bytes{};
    size_t pending_inode_size = 0;
    size_t inode_padding_remaining = 0;
    u64 dirent_count = 0;
    bool uroot_reached = false;
    bool root_mapped = false;
    bool dinode_reached = false;
    bool directory_complete = false;
    std::vector<std::filesystem::path> directories_to_create;
    std::vector<char> compressed_data;
    std::vector<char> decompressed_data(PfsBlockSize);

    for (u64 block_index = 0; block_index < block_count; ++block_index) {
        const u64 sector_offset = sectorMap[static_cast<size_t>(block_index)];
        const u64 sector_size = sectorMap[static_cast<size_t>(block_index + 1)] - sector_offset;
        if (sector_offset > pfsc.size() || sector_size > pfsc.size() - sector_offset ||
            sector_size == 0 || sector_size > PfsBlockSize) {
            failreason = "PFSC sector range is outside the data buffer";
            return false;
        }

        compressed_data.resize(static_cast<size_t>(sector_size));
        std::memcpy(compressed_data.data(), pfsc.data() + static_cast<size_t>(sector_offset),
                    static_cast<size_t>(sector_size));
        if (sector_size == PfsBlockSize) {
            std::memcpy(decompressed_data.data(), compressed_data.data(), PfsBlockSize);
        } else if (!DecompressPFSC(compressed_data, decompressed_data)) {
            failreason = "Failed to decompress PFSC sector";
            return false;
        }

        if (block_index == 0) {
            std::memcpy(&inode_count, decompressed_data.data() + 0x30, sizeof(u32));
            if (inode_count == 0 || inode_count > iNodeBuf.max_size()) {
                failreason = "Invalid PFS inode count";
                return false;
            }
            static_assert(sizeof(Inode) <= PfsInodeStride);
            const u64 inode_byte_count = inode_count * PfsInodeStride;
            inode_blocks = inode_byte_count / PfsBlockSize +
                           (inode_byte_count % PfsBlockSize != 0 ? 1 : 0);
            if (inode_blocks >= block_count) {
                failreason = "PFS inode table exceeds the sector map";
                return false;
            }
            iNodeBuf.reserve(static_cast<size_t>(inode_count));
        }

        if (block_index >= 1 && block_index <= inode_blocks) {
            size_t offset = 0;
            while (offset < decompressed_data.size() && iNodeBuf.size() < inode_count) {
                if (inode_padding_remaining != 0) {
                    const size_t padding =
                        std::min(inode_padding_remaining, decompressed_data.size() - offset);
                    offset += padding;
                    inode_padding_remaining -= padding;
                    continue;
                }

                const size_t bytes_to_copy =
                    std::min(sizeof(Inode) - pending_inode_size, decompressed_data.size() - offset);
                std::memcpy(pending_inode_bytes.data() + pending_inode_size,
                            decompressed_data.data() + offset, bytes_to_copy);
                offset += bytes_to_copy;
                pending_inode_size += bytes_to_copy;
                if (pending_inode_size == sizeof(Inode)) {
                    Inode node{};
                    std::memcpy(&node, pending_inode_bytes.data(), sizeof(node));
                    if (node.Mode == 0 || node.Size < 0) {
                        failreason = "Invalid PFS inode record";
                        return false;
                    }
                    if ((node.Mode & static_cast<u16>(InodeMode::file)) != 0) {
                        const u64 inode_sector_count = node.Blocks;
                        if (node.loc > block_count ||
                            inode_sector_count > block_count - node.loc ||
                            static_cast<u64>(node.Size) > inode_sector_count * PfsBlockSize) {
                            failreason = "PFS file inode has an invalid sector range";
                            return false;
                        }
                    }
                    iNodeBuf.push_back(node);
                    pending_inode_size = 0;
                    inode_padding_remaining = PfsInodeStride - sizeof(Inode);
                }
            }
        }

        const std::string_view flat_path_table(decompressed_data.data() + 0x10, 15);
        if (flat_path_table == "flat_path_table") {
            uroot_reached = true;
        }

        if (uroot_reached) {
            size_t offset = 0;
            while (offset + sizeof(PfsDirentHeader) <= decompressed_data.size()) {
                PfsDirentHeader dirent{};
                std::string name;
                bool end_of_entries = false;
                if (!ReadPfsDirent(decompressed_data, offset, dirent, name, end_of_entries,
                                   failreason)) {
                    return false;
                }
                if (end_of_entries) {
                    if (dirent_count >= inode_count) {
                        failreason = "PFS root inode index is out of range";
                        return false;
                    }
                    extractPaths[static_cast<u32>(dirent_count)] = pfs_root;
                    current_dir = pfs_root;
                    root_mapped = true;
                    uroot_reached = false;
                    break;
                }
                if (static_cast<u64>(dirent.inode) >= inode_count ||
                    !IsSafePfsName(dirent.type, name, failreason)) {
                    if (failreason.empty()) {
                        failreason = "Invalid PFS root directory entry";
                    }
                    return false;
                }
                ++dirent_count;
                offset += static_cast<size_t>(dirent.entry_size);
            }
        }

        const std::span<const char> decompressed_span(decompressed_data.data(),
                                                       decompressed_data.size());
        if (!dinode_reached && decompressed_data[0x10] == '.' &&
            decompressed_data[0x28] == '.' && decompressed_data[0x29] == '.') {
            PfsDirentHeader current_entry{};
            PfsDirentHeader parent_entry{};
            std::string current_name;
            std::string parent_name;
            bool end_of_entries = false;
            if (!ReadPfsDirent(decompressed_span, 0, current_entry, current_name, end_of_entries,
                               failreason) ||
                end_of_entries || !ReadPfsDirent(decompressed_span,
                                                 static_cast<size_t>(current_entry.entry_size),
                                                 parent_entry, parent_name, end_of_entries,
                                                 failreason) ||
                end_of_entries || current_entry.type != PFS_CURRENT_DIR || current_name != "." ||
                parent_entry.type != PFS_PARENT_DIR || parent_name != "..") {
                if (failreason.empty()) {
                    failreason = "Invalid PFS current/parent directory entries";
                }
                return false;
            }
            dinode_reached = true;
        }

        if (dinode_reached && root_mapped && !directory_complete) {
            size_t offset = 0;
            while (offset + sizeof(PfsDirentHeader) <= decompressed_data.size()) {
                PfsDirentHeader dirent{};
                std::string name;
                bool end_of_entries = false;
                if (!ReadPfsDirent(decompressed_span, offset, dirent, name, end_of_entries,
                                   failreason)) {
                    return false;
                }
                if (end_of_entries) {
                    break;
                }
                if (static_cast<u64>(dirent.inode) >= inode_count ||
                    !IsSafePfsName(dirent.type, name, failreason)) {
                    if (failreason.empty()) {
                        failreason = "Invalid PFS directory entry";
                    }
                    return false;
                }

                const u32 inode_index = static_cast<u32>(dirent.inode);
                std::filesystem::path destination;
                if (dirent.type == PFS_CURRENT_DIR) {
                    const auto current = extractPaths.find(inode_index);
                    if (current == extractPaths.end() ||
                        !NormalizeContainedPath(extract_path, current->second, destination,
                                                failreason)) {
                        if (failreason.empty()) {
                            failreason = "PFS current-directory inode has no path";
                        }
                        return false;
                    }
                    current_dir = destination;
                    extractPaths[inode_index] = destination;
                } else if (dirent.type == PFS_PARENT_DIR) {
                    if (current_dir.empty()) {
                        failreason = "PFS parent-directory entry has no current directory";
                        return false;
                    }
                    const auto parent = current_dir == pfs_root ? pfs_root
                                                                 : current_dir.parent_path();
                    if (!NormalizeContainedPath(extract_path, parent, destination, failreason)) {
                        return false;
                    }
                    extractPaths[inode_index] = destination;
                } else {
                    if (current_dir.empty()) {
                        failreason = "PFS file or directory has no current directory";
                        return false;
                    }
                    if (!NormalizeContainedPath(extract_path,
                                                current_dir / std::filesystem::path(name),
                                                destination, failreason)) {
                        return false;
                    }
                    extractPaths[inode_index] = destination;
                    if (dirent.type == PFS_DIR) {
                        directories_to_create.push_back(destination);
                    }
                    ++dirent_count;
                    if (dirent_count >= inode_count) {
                        failreason = "PFS directory entry count exceeds the inode count";
                        return false;
                    }
                    if (dirent_count + 1 == inode_count) {
                        directory_complete = true;
                    }
                }

                fsTable.push_back({std::move(name), inode_index, static_cast<u32>(dirent.type)});
                offset += static_cast<size_t>(dirent.entry_size);
            }
        }
    }

    if (!root_mapped || !dinode_reached || !directory_complete || uroot_reached ||
        iNodeBuf.size() != inode_count || pending_inode_size != 0) {
        failreason = "PFS inode or directory table is incomplete";
        return false;
    }
    for (const auto& table_entry : fsTable) {
        if (table_entry.inode >= iNodeBuf.size()) {
            failreason = "PFS directory entry inode index is out of range";
            return false;
        }
        const auto& inode = iNodeBuf[table_entry.inode];
        if ((table_entry.type == PFS_FILE && (inode.Mode & static_cast<u16>(InodeMode::file)) == 0) ||
            (table_entry.type == PFS_DIR && (inode.Mode & dir) == 0)) {
            failreason = "PFS directory entry does not match its inode type";
            return false;
        }
    }

    directories_to_create.insert(directories_to_create.begin(), pfs_root);
    for (const auto& directory : directories_to_create) {
        if (!CreateContainedDirectories(extract_path, directory, failreason)) {
            return false;
        }
    }

    const auto sce_sys = extract_path / "sce_sys";
    if (!CreateContainedDirectories(extract_path, sce_sys, failreason)) {
        return false;
    }
    for (u64 i = 0; i < table_count; ++i) {
        PKGEntry entry{};
        if (!read_table_entry(i, entry)) {
            return false;
        }
        const auto entry_name = GetEntryNameByType(entry.id);
        const std::string output_name = entry_name.empty()
                                            ? std::to_string(static_cast<u32>(entry.id))
                                            : std::string(entry_name);
        std::filesystem::path output_path;
        if (!NormalizeContainedPath(extract_path, sce_sys / output_name, output_path,
                                    failreason) ||
            !CreateContainedDirectories(extract_path, output_path.parent_path(), failreason)) {
            return false;
        }

        const bool encrypted_np_entry = entry.id == 0x400 || entry.id == 0x401 ||
                                        entry.id == 0x402 || entry.id == 0x403;
        const u64 entry_size = entry.size;
        const u64 padded_size = encrypted_np_entry
                                    ? (entry_size + CryptoPP::AES::BLOCKSIZE - 1) /
                                          CryptoPP::AES::BLOCKSIZE *
                                          static_cast<u64>(CryptoPP::AES::BLOCKSIZE)
                                    : entry_size;
        std::vector<u8> data(static_cast<size_t>(padded_size));
        if (!file.Seek(static_cast<s64>(static_cast<u32>(entry.offset))) ||
            file.Read(data) != data.size()) {
            failreason = "Failed to read PKG entry data";
            return false;
        }

        std::span<const u8> output_data(data);
        if (encrypted_np_entry) {
            std::array<u8, 64> entry_iv_key{};
            std::memcpy(entry_iv_key.data(), &entry, sizeof(entry));
            std::memcpy(entry_iv_key.data() + sizeof(entry), dk3_.data(), sizeof(dk3_));
            PKG::crypto.ivKeyHASH256(entry_iv_key, ivKey);
            decNp.resize(data.size());
            PKG::crypto.aesCbcCfb128DecryptEntry(ivKey, data, decNp);
            output_data = std::span<const u8>(decNp.data(), static_cast<size_t>(entry_size));
        }

        Common::FS::IOFile out(output_path, Common::FS::FileAccessMode::Write);
        if (!out.IsOpen()) {
            failreason = "Failed to open PKG output file";
            return false;
        }
        if (out.WriteRaw<u8>(output_data.data(), output_data.size()) != output_data.size() ||
            !out.Flush()) {
            failreason = "Failed to write complete PKG output file";
            return false;
        }
    }

    return true;
} catch (const std::bad_alloc&) {
    failreason = "Insufficient memory while extracting PKG";
    return false;
} catch (const std::length_error&) {
    failreason = "PKG data exceeds supported memory limits";
    return false;
} catch (const std::filesystem::filesystem_error&) {
    failreason = "PKG filesystem operation failed";
    return false;
}

bool PKG::ExtractFiles(const int index) try {
    if (index < 0 || static_cast<size_t>(index) >= fsTable.size()) {
        return false;
    }

    const auto& table_entry = fsTable[static_cast<size_t>(index)];
    if (table_entry.inode >= iNodeBuf.size()) {
        return false;
    }
    if (table_entry.type != PFS_FILE) {
        return true;
    }

    const auto path = extractPaths.find(table_entry.inode);
    if (path == extractPaths.end()) {
        return false;
    }
    const auto& inode = iNodeBuf[table_entry.inode];
    if ((inode.Mode & static_cast<u16>(InodeMode::file)) == 0 || inode.Size < 0) {
        return false;
    }

    const u64 file_size = static_cast<u64>(inode.Size);
    const u64 sector_loc = inode.loc;
    const u64 block_count = inode.Blocks;
    if (sectorMap.empty() || sector_loc > sectorMap.size() - 1 ||
        block_count > sectorMap.size() - 1 - sector_loc ||
        file_size > block_count * PfsBlockSize || (file_size != 0 && block_count == 0)) {
        return false;
    }

    std::string failreason;
    std::filesystem::path output_path;
    if (!NormalizeContainedPath(extract_path, path->second, output_path, failreason) ||
        !CreateContainedDirectories(extract_path, output_path.parent_path(), failreason) ||
        !NormalizeContainedPath(extract_path, output_path, output_path, failreason)) {
        return false;
    }

    Common::FS::IOFile pkg_file(pkgpath, Common::FS::FileAccessMode::Read);
    if (!pkg_file.IsOpen()) {
        return false;
    }
    Common::FS::IOFile inflated(output_path, Common::FS::FileAccessMode::Write);
    if (!inflated.IsOpen()) {
        return false;
    }

    constexpr size_t pfs_buffer_size = 0x11000;
    const u64 pfs_image_size = static_cast<u64>(pkgheader.pfs_cache_size) * 2;
    std::vector<u8> encrypted_pfs(pfs_buffer_size);
    std::vector<u8> decrypted_pfs(pfs_buffer_size);
    std::vector<char> compressed_data;
    std::vector<char> decompressed_data(PfsBlockSize);
    u64 size_decompressed = 0;

    for (u64 block = 0; block < block_count && size_decompressed < file_size; ++block) {
        const u64 map_index = sector_loc + block;
        const u64 sector_offset = sectorMap[static_cast<size_t>(map_index)];
        const u64 next_sector_offset = sectorMap[static_cast<size_t>(map_index + 1)];
        if (next_sector_offset <= sector_offset ||
            pfsc_offset > pfs_image_size || next_sector_offset > pfs_image_size - pfsc_offset) {
            return false;
        }
        const u64 sector_size = next_sector_offset - sector_offset;
        if (sector_size == 0 || sector_size > PfsBlockSize ||
            pfsc_offset > std::numeric_limits<u64>::max() - sector_offset) {
            return false;
        }

        const u64 relative_offset = pfsc_offset + sector_offset;
        const u64 aligned_offset = relative_offset & ~u64(0xFFF);
        const u64 previous_data = relative_offset - aligned_offset;
        if (!IsRangeWithin(aligned_offset, encrypted_pfs.size(), pfs_image_size) ||
            !IsRangeWithin(pkgheader.pfs_image_offset, aligned_offset + encrypted_pfs.size(),
                           pkgheader.pkg_size)) {
            return false;
        }
        const u64 file_offset = pkgheader.pfs_image_offset + aligned_offset;
        const u64 current_sector = aligned_offset / 0x1000;
        if (!pkg_file.Seek(static_cast<s64>(file_offset)) ||
            pkg_file.Read(encrypted_pfs) != encrypted_pfs.size()) {
            return false;
        }

        PKG::crypto.decryptPFS(dataKey, tweakKey, encrypted_pfs, decrypted_pfs, current_sector);
        if (previous_data + sector_size > decrypted_pfs.size()) {
            return false;
        }
        compressed_data.resize(static_cast<size_t>(sector_size));
        std::memcpy(compressed_data.data(),
                    decrypted_pfs.data() + static_cast<size_t>(previous_data),
                    static_cast<size_t>(sector_size));
        if (sector_size == PfsBlockSize) {
            std::memcpy(decompressed_data.data(), compressed_data.data(), PfsBlockSize);
        } else if (!DecompressPFSC(compressed_data, decompressed_data)) {
            return false;
        }

        const u64 write_size = std::min<u64>(PfsBlockSize, file_size - size_decompressed);
        if (inflated.WriteRaw<u8>(decompressed_data.data(), static_cast<size_t>(write_size)) !=
            write_size) {
            return false;
        }
        size_decompressed += write_size;
    }

    return size_decompressed == file_size && inflated.Flush();
} catch (const std::bad_alloc&) {
    return false;
} catch (const std::length_error&) {
    return false;
} catch (const std::filesystem::filesystem_error&) {
    return false;
}
