// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ps5_pup.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <system_error>

#include <fmt/format.h>
#include <libdeflate.h>

namespace PS5Pup {

namespace {

constexpr std::array<u8, 4> kPupMagic = {0x54, 0x14, 0xF5, 0xEE};
constexpr u64 kPupHeaderSize = 0x20;
constexpr u64 kPupEntrySize = 0x20;
constexpr u64 kFlagInfo = 0x1;              // holds the block table of entry "id"
constexpr u64 kFlagCompressed = 0x8;        // zlib
constexpr u64 kFlagBlocked = 0x800;         // stored in blocks, each zlib or raw
constexpr u64 kDigestSize = 32;             // per block, before the block extents
constexpr u64 kMaxWholeEntry = 64ull << 20; // compressed in one piece

// SLB2: a 0x20-byte header, then 0x30-byte entries; data in 512-byte blocks.
constexpr std::array<u8, 4> kSlb2Magic = {'S', 'L', 'B', '2'};
constexpr u64 kSlb2HeaderSize = 0x20;
constexpr u64 kSlb2EntrySize = 0x30;
constexpr u64 kSlb2BlockSize = 512;
constexpr size_t kSlb2NameSize = 32;

// exFAT boot sector and directory entries.
constexpr std::string_view kExfatSignature = "EXFAT   ";
constexpr u64 kExfatSignatureOffset = 3;
constexpr u64 kExfatBootSize = 512;
constexpr u8 kExfatTypeEnd = 0x00;
constexpr u8 kExfatTypeFile = 0x85;
constexpr u8 kExfatTypeStream = 0xC0;
constexpr u8 kExfatTypeName = 0xC1;
constexpr u16 kExfatAttrDirectory = 0x10;
constexpr u8 kExfatNoFatChain = 0x2;
constexpr u64 kExfatEntrySize = 32;
constexpr u32 kExfatFirstCluster = 2;
constexpr u32 kExfatLastCluster = 0xFFFFFFF6;
constexpr u64 kExfatMaxDirectory = 64ull << 20;

constexpr u64 kCopyChunk = 1ull << 20;

// offset + size <= limit, without overflowing.
bool InRange(u64 offset, u64 size, u64 limit) {
    return offset <= limit && size <= limit - offset;
}

template <typename T>
T Get(const u8* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

bool IsSafeName(std::string_view name) {
    if (name.empty() || name.size() > 255 || name == "." || name == "..") {
        return false;
    }
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

std::string Utf16ToUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        u32 c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 &&
            s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[i + 1] - 0xDC00);
            ++i;
        }
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

class FileSource final : public Source {
public:
    explicit FileSource(const std::filesystem::path& path) : m_file(path, std::ios::binary) {
        std::error_code ec;
        m_size = std::filesystem::file_size(path, ec);
        if (ec) {
            m_size = 0;
        }
    }

    [[nodiscard]] bool IsOpen() const {
        return static_cast<bool>(m_file);
    }

    u64 Size() const override {
        return m_size;
    }

    bool Read(u64 offset, void* dst, u64 size) const override {
        if (!InRange(offset, size, m_size)) {
            return false;
        }
        std::scoped_lock lock{m_mutex};
        m_file.clear();
        m_file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        return m_file && m_file.read(static_cast<char*>(dst), static_cast<std::streamsize>(size)) &&
               static_cast<u64>(m_file.gcount()) == size;
    }

private:
    mutable std::ifstream m_file;
    mutable std::mutex m_mutex;
    u64 m_size = 0;
};

class RangeSource final : public Source {
public:
    RangeSource(std::shared_ptr<const Source> parent, u64 start, u64 size)
        : m_parent(std::move(parent)), m_start(start), m_size(size) {}

    u64 Size() const override {
        return m_size;
    }

    bool Read(u64 offset, void* dst, u64 size) const override {
        return InRange(offset, size, m_size) && m_parent->Read(m_start + offset, dst, size);
    }

private:
    std::shared_ptr<const Source> m_parent;
    u64 m_start;
    u64 m_size;
};

bool Inflate(const std::vector<u8>& in, u8* out, u64 out_size) {
    libdeflate_decompressor* d = libdeflate_alloc_decompressor();
    if (d == nullptr) {
        return false;
    }
    size_t in_used = 0;
    size_t actual = 0;
    const libdeflate_result res = libdeflate_zlib_decompress_ex(
        d, in.data(), in.size(), out, static_cast<size_t>(out_size), &in_used, &actual);
    libdeflate_free_decompressor(d);
    return res == LIBDEFLATE_SUCCESS && actual == out_size;
}

struct PupEntry {
    u32 index = 0;
    u64 flags = 0;
    u64 offset = 0;                 // in the PUP
    u64 stored_size = 0;            // in the PUP
    u64 size = 0;                   // decoded
    std::optional<u32> block_table; // index of the info entry

    [[nodiscard]] u64 Id() const {
        return flags >> 20;
    }
    [[nodiscard]] bool IsInfo() const {
        return (flags & kFlagInfo) != 0;
    }
    [[nodiscard]] bool IsCompressed() const {
        return (flags & kFlagCompressed) != 0;
    }
    [[nodiscard]] bool IsBlocked() const {
        return (flags & kFlagBlocked) != 0;
    }
    [[nodiscard]] u64 BlockSize() const {
        return 1ull << (((flags >> 12) & 0xF) + 12);
    }
};

class PupEntrySource final : public Source {
public:
    PupEntrySource(std::shared_ptr<const Source> pup, const PupEntry& entry, const PupEntry* info)
        : m_pup(std::move(pup)), m_entry(entry) {
        if (!m_entry.IsCompressed()) {
            m_ok = InRange(m_entry.offset, m_entry.size, m_pup->Size());
            return;
        }
        if (!m_entry.IsBlocked()) {
            m_ok = m_entry.size <= kMaxWholeEntry &&
                   InRange(m_entry.offset, m_entry.stored_size, m_pup->Size());
            return;
        }
        if (info == nullptr) {
            m_problem = "it is stored in blocks but has no block table";
            return;
        }
        const u64 block_size = m_entry.BlockSize();
        const u64 count = (m_entry.size + block_size - 1) / block_size;
        const u64 table = count * (kDigestSize + 8);
        std::vector<u8> extents(count * 8);
        if (info->stored_size < table || info->IsCompressed() ||
            !m_pup->Read(info->offset + count * kDigestSize, extents.data(), extents.size())) {
            m_problem = "its block table cannot be read";
            return;
        }
        m_extents.resize(count * 2);
        for (u64 i = 0; i < count * 2; ++i) {
            m_extents[i] = Get<u32>(extents.data() + i * 4);
        }
        m_ok = true;
    }

    [[nodiscard]] bool Ok() const {
        return m_ok;
    }
    [[nodiscard]] const std::string& Problem() const {
        return m_problem;
    }

    u64 Size() const override {
        return m_entry.size;
    }

    bool Read(u64 offset, void* dst, u64 size) const override {
        if (!m_ok || !InRange(offset, size, m_entry.size)) {
            return false;
        }
        if (!m_entry.IsCompressed()) {
            return m_pup->Read(m_entry.offset + offset, dst, size);
        }
        std::scoped_lock lock{m_mutex};
        if (!m_entry.IsBlocked()) {
            if (m_cache_block != 0) {
                std::vector<u8> in(m_entry.stored_size);
                m_cache.resize(m_entry.size);
                if (!m_pup->Read(m_entry.offset, in.data(), in.size()) ||
                    !Inflate(in, m_cache.data(), m_entry.size)) {
                    return false;
                }
                m_cache_block = 0;
            }
            std::memcpy(dst, m_cache.data() + offset, size);
            return true;
        }
        const u64 block_size = m_entry.BlockSize();
        auto* out = static_cast<u8*>(dst);
        while (size > 0) {
            const u64 block = offset / block_size;
            if (!LoadBlock(block)) {
                return false;
            }
            const u64 in_block = offset - block * block_size;
            const u64 n = std::min(size, m_cache.size() - in_block);
            std::memcpy(out, m_cache.data() + in_block, n);
            out += n;
            offset += n;
            size -= n;
        }
        return true;
    }

private:
    bool LoadBlock(u64 block) const {
        if (m_cache_block == block) {
            return true;
        }
        const u64 block_size = m_entry.BlockSize();
        const u64 length = std::min(block_size, m_entry.size - block * block_size);
        const u64 start = m_entry.offset + m_extents[block * 2];
        const u64 stored = m_extents[block * 2 + 1];
        m_cache.resize(length);
        m_cache_block = UINT64_MAX;
        if (stored >= length) {
            // stored as it is
            if (!m_pup->Read(start, m_cache.data(), length)) {
                return false;
            }
        } else {
            if (start > m_pup->Size()) {
                return false;
            }
            std::vector<u8> in(std::min(stored, m_pup->Size() - start));
            if (!m_pup->Read(start, in.data(), in.size()) || !Inflate(in, m_cache.data(), length)) {
                return false;
            }
        }
        m_cache_block = block;
        return true;
    }

    std::shared_ptr<const Source> m_pup;
    PupEntry m_entry;
    std::vector<u64> m_extents; // offset in the entry, stored size; per block
    bool m_ok = false;
    std::string m_problem;
    mutable std::mutex m_mutex;
    mutable std::vector<u8> m_cache;
    mutable u64 m_cache_block = UINT64_MAX;
};

class ExfatFileSource final : public Source {
public:
    struct Run {
        u64 offset;
        u64 size;
    };

    ExfatFileSource(std::shared_ptr<const Source> image, std::vector<Run> runs, u64 valid, u64 size)
        : m_image(std::move(image)), m_runs(std::move(runs)), m_valid(valid), m_size(size) {}

    u64 Size() const override {
        return m_size;
    }

    bool Read(u64 offset, void* dst, u64 size) const override {
        if (!InRange(offset, size, m_size)) {
            return false;
        }
        auto* out = static_cast<u8*>(dst);
        if (offset + size > m_valid) {
            const u64 zero_from = std::max(offset, m_valid);
            std::memset(out + (zero_from - offset), 0, offset + size - zero_from);
            if (offset >= m_valid) {
                return true;
            }
            size = m_valid - offset;
        }
        u64 run_start = 0;
        for (const Run& run : m_runs) {
            if (size == 0) {
                break;
            }
            if (offset < run_start + run.size) {
                const u64 in_run = offset - run_start;
                const u64 n = std::min(size, run.size - in_run);
                if (!m_image->Read(run.offset + in_run, out, n)) {
                    return false;
                }
                out += n;
                offset += n;
                size -= n;
            }
            run_start += run.size;
        }
        return size == 0;
    }

private:
    std::shared_ptr<const Source> m_image;
    std::vector<Run> m_runs;
    u64 m_valid;
    u64 m_size;
};

class TreeBuilder {
public:
    explicit TreeBuilder(std::vector<std::string>& problems) : m_problems(problems) {}

    void Expand(Node& node, const std::string& path) {
        std::array<u8, kExfatBootSize> head{};
        const u64 n = std::min<u64>(head.size(), node.data->Size());
        if (n < kSlb2Magic.size() || !node.data->Read(0, head.data(), n)) {
            return;
        }
        if (std::memcmp(head.data(), kSlb2Magic.data(), kSlb2Magic.size()) == 0) {
            ReadSlb2(node, path);
        } else if (n == kExfatBootSize && IsExfat(head.data())) {
            ReadExfat(node, path);
        }
    }

private:
    static bool IsExfat(const u8* boot) {
        return std::memcmp(boot + kExfatSignatureOffset, kExfatSignature.data(),
                           kExfatSignature.size()) == 0 &&
               boot[510] == 0x55 && boot[511] == 0xAA;
    }

    void Problem(const std::string& path, const std::string& what) {
        m_problems.push_back(fmt::format("{}: {}", path, what));
    }

    void ReadSlb2(Node& node, const std::string& path) {
        const Source& src = *node.data;
        std::array<u8, kSlb2HeaderSize> header{};
        if (!src.Read(0, header.data(), header.size())) {
            return Problem(path, "the SLB2 header cannot be read");
        }
        const u32 count = Get<u32>(header.data() + 12);
        if (!InRange(kSlb2HeaderSize, u64{count} * kSlb2EntrySize, src.Size())) {
            return Problem(path, fmt::format("the SLB2 header lists {} files", count));
        }
        std::vector<u8> table(u64{count} * kSlb2EntrySize);
        if (!src.Read(kSlb2HeaderSize, table.data(), table.size())) {
            return Problem(path, "the SLB2 file table cannot be read");
        }
        std::vector<Node> children;
        for (u32 i = 0; i < count; ++i) {
            const u8* e = table.data() + u64{i} * kSlb2EntrySize;
            const u64 start = u64{Get<u32>(e)} * kSlb2BlockSize;
            const u64 size = Get<u32>(e + 4);
            const char* name_ptr = reinterpret_cast<const char*>(e + 16);
            const std::string name(name_ptr, strnlen(name_ptr, kSlb2NameSize));
            if (!IsSafeName(name) || !InRange(start, size, src.Size())) {
                Problem(path, fmt::format("file {} ({}) is left out", i, name));
                continue;
            }
            Node child;
            child.name = name;
            child.size = size;
            child.data = std::make_shared<RangeSource>(node.data, start, size);
            Expand(child, path + "/" + name);
            children.push_back(std::move(child));
        }
        node.kind = NodeKind::Slb2;
        node.children = std::move(children);
    }

    void ReadExfat(Node& node, const std::string& path) {
        ExfatReader reader(node.data);
        std::string error;
        std::vector<Node> children;
        if (!reader.Read(children, error, m_problems, path)) {
            return Problem(path, error);
        }
        node.kind = NodeKind::Exfat;
        node.children = std::move(children);
    }

    class ExfatReader {
    public:
        explicit ExfatReader(std::shared_ptr<const Source> image) : m_image(std::move(image)) {}

        bool Read(std::vector<Node>& out, std::string& error, std::vector<std::string>& problems,
                  const std::string& path) {
            std::array<u8, kExfatBootSize> boot{};
            if (!m_image->Read(0, boot.data(), boot.size())) {
                error = "the exFAT boot sector cannot be read";
                return false;
            }
            const u64 fat_sector = Get<u32>(boot.data() + 80);
            const u64 fat_sectors = Get<u32>(boot.data() + 84);
            const u64 heap_sector = Get<u32>(boot.data() + 88);
            m_cluster_count = Get<u32>(boot.data() + 92);
            const u32 root = Get<u32>(boot.data() + 96);
            const u8 sector_shift = boot[108];
            const u8 cluster_shift = boot[109];
            if (sector_shift < 9 || sector_shift > 12 || sector_shift + cluster_shift > 25) {
                error = "the exFAT sector or cluster size is invalid";
                return false;
            }
            m_heap = heap_sector << sector_shift;
            m_cluster_size = 1ull << (sector_shift + cluster_shift);
            m_fat.resize((fat_sectors << sector_shift) / 4);
            if (m_fat.size() < m_cluster_count + kExfatFirstCluster ||
                !m_image->Read(fat_sector << sector_shift, m_fat.data(), m_fat.size() * 4)) {
                error = "the exFAT allocation table cannot be read";
                return false;
            }
            m_problems = &problems;
            std::vector<ExfatFileSource::Run> runs;
            if (!Chain(root, std::nullopt, false, runs)) {
                error = "the exFAT root folder's clusters are invalid";
                return false;
            }
            return ReadDirectory(runs, path, out, 0, error);
        }

    private:
        bool Chain(u32 first, std::optional<u64> length, bool no_fat_chain,
                   std::vector<ExfatFileSource::Run>& runs) const {
            runs.clear();
            if (length && *length == 0) {
                return true;
            }
            const u64 max =
                length ? (*length + m_cluster_size - 1) / m_cluster_size : m_cluster_count;
            u64 cluster = first;
            u64 count = 0;
            while (count < max) {
                if (cluster < kExfatFirstCluster ||
                    cluster >= u64{kExfatFirstCluster} + m_cluster_count) {
                    if (!length && cluster > kExfatLastCluster) {
                        break; // end of chain
                    }
                    return false;
                }
                const u64 offset = m_heap + (cluster - kExfatFirstCluster) * m_cluster_size;
                if (!runs.empty() && runs.back().offset + runs.back().size == offset) {
                    runs.back().size += m_cluster_size;
                } else {
                    runs.push_back({offset, m_cluster_size});
                }
                ++count;
                cluster = no_fat_chain ? cluster + 1 : m_fat[cluster];
            }
            if (length) {
                // the last cluster is only partly used
                runs.back().size -= count * m_cluster_size - *length;
            }
            return true;
        }

        bool ReadDirectory(const std::vector<ExfatFileSource::Run>& runs, const std::string& path,
                           std::vector<Node>& out, int depth, std::string& error) {
            if (depth > 64) {
                error = "the exFAT folders are nested too deeply";
                return false;
            }
            u64 total = 0;
            for (const auto& run : runs) {
                total += run.size;
            }
            if (total > kExfatMaxDirectory) {
                error = "an exFAT folder is too large";
                return false;
            }
            std::vector<u8> dir(total);
            u64 pos = 0;
            for (const auto& run : runs) {
                if (!m_image->Read(run.offset, dir.data() + pos, run.size)) {
                    error = "an exFAT folder lies past the end of the image";
                    return false;
                }
                pos += run.size;
            }
            for (u64 at = 0; at + kExfatEntrySize <= dir.size();) {
                const u8* e = dir.data() + at;
                if (e[0] == kExfatTypeEnd) {
                    break;
                }
                const u64 secondary = e[1];
                const u8* stream = e + kExfatEntrySize;
                if (e[0] != kExfatTypeFile || secondary < 2 ||
                    at + (secondary + 1) * kExfatEntrySize > dir.size() ||
                    stream[0] != kExfatTypeStream) {
                    at += kExfatEntrySize;
                    continue;
                }
                const u16 attributes = Get<u16>(e + 4);
                const u8 flags = stream[1];
                const u8 name_length = stream[3];
                const u64 valid = Get<u64>(stream + 8);
                const u32 first = Get<u32>(stream + 20);
                const u64 length = Get<u64>(stream + 24);
                std::u16string name16;
                for (u64 i = 2; i <= secondary; ++i) {
                    const u8* n = e + i * kExfatEntrySize;
                    if (n[0] == kExfatTypeName) {
                        for (int c = 0; c < 15; ++c) {
                            name16 += static_cast<char16_t>(Get<u16>(n + 2 + c * 2));
                        }
                    }
                }
                name16.resize(std::min<size_t>(name_length, name16.size()));
                at += (secondary + 1) * kExfatEntrySize;

                const std::string name = Utf16ToUtf8(name16);
                const std::string full = path + "/" + name;
                std::vector<ExfatFileSource::Run> data;
                if (!IsSafeName(name)) {
                    m_problems->push_back(fmt::format("{}: left out (name)", full));
                    continue;
                }
                if (valid > length ||
                    !Chain(first, length, (flags & kExfatNoFatChain) != 0, data)) {
                    m_problems->push_back(fmt::format("{}: left out (clusters)", full));
                    continue;
                }
                Node node;
                node.name = name;
                if ((attributes & kExfatAttrDirectory) != 0) {
                    node.kind = NodeKind::Folder;
                    if (!ReadDirectory(data, full, node.children, depth + 1, error)) {
                        return false;
                    }
                } else {
                    node.size = length;
                    node.data =
                        std::make_shared<ExfatFileSource>(m_image, std::move(data), valid, length);
                }
                out.push_back(std::move(node));
            }
            return true;
        }

        std::shared_ptr<const Source> m_image;
        std::vector<u32> m_fat;
        std::vector<std::string>* m_problems = nullptr;
        u64 m_heap = 0;
        u64 m_cluster_size = 0;
        u64 m_cluster_count = 0;
    };

    std::vector<std::string>& m_problems;
};

std::string GuessExtension(const Source& data) {
    std::array<u8, 16> head{};
    const u64 n = std::min<u64>(head.size(), data.Size());
    if (!data.Read(0, head.data(), n)) {
        return ".bin";
    }
    const std::string_view text(reinterpret_cast<const char*>(head.data()), n);
    if (text.starts_with("<?xml")) {
        return ".xml";
    }
    if (text.starts_with("{")) {
        return ".json";
    }
    if (text.starts_with("PK\3\4")) {
        return ".zip";
    }
    if (text.starts_with("SLB2")) {
        return ".bls";
    }
    if (text.starts_with("\x54\x14\xF5\xEE") || text.starts_with("\x4F\x15\x3D\x1D")) {
        return ".self";
    }
    return ".bin";
}

std::string EntryName(const PupEntry& entry, const Source& data) {
    switch (entry.Id() & 0xFFF) {
    case 0x004:
        return "mbr.bin";
    case 0x005:
        return "kernel.bin";
    case 0x00B:
        return "titania.bls";
    case 0x00E:
        return "eap_kbl.bin";
    case 0x010:
        return "emc_salina_c0.bls";
    case 0x012:
        return "usb_pdc_salina_c0.bls";
    case 0x103:
        return "oberon_sec_ldr_c0.bin";
    case 0x201:
        return "wlanbt.bin";
    case 0x203:
        return "ssd0.system_b";
    case 0x204:
        return "ssd0.system_ex_b";
    case 0x207:
        return "ssd0.preinst";
    case 0x7D0:
        return "bluray.bin";
    default:
        return fmt::format("entry_{}_0x{:x}{}", entry.index, entry.Id(), GuessExtension(data));
    }
}

std::filesystem::path PathFromUtf8(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

void CollectFiles(const Node& node, const std::string& path,
                  std::vector<std::pair<std::string, const Node*>>& out) {
    if (!node.IsDirectory()) {
        out.emplace_back(path, &node);
        return;
    }
    for (const Node& child : node.children) {
        CollectFiles(child, path.empty() ? child.name : path + "/" + child.name, out);
    }
}

} // namespace

std::unique_ptr<Pup> Pup::Open(const std::filesystem::path& path, std::string* error) {
    auto fail = [&](std::string why) -> std::unique_ptr<Pup> {
        if (error != nullptr) {
            *error = std::move(why);
        }
        return nullptr;
    };

    auto file = std::make_shared<FileSource>(path);
    if (!file->IsOpen()) {
        return fail("The file cannot be opened.");
    }
    std::array<u8, kPupHeaderSize> header{};
    if (!file->Read(0, header.data(), header.size()) ||
        std::memcmp(header.data(), kPupMagic.data(), kPupMagic.size()) != 0) {
        return fail("This is not a PS5 system update (PUP).");
    }
    const u16 count = Get<u16>(header.data() + 0x18);
    std::vector<u8> table(count * kPupEntrySize);
    if (count == 0 || !file->Read(kPupHeaderSize, table.data(), table.size())) {
        return fail("The PUP's entry table cannot be read.");
    }

    std::vector<PupEntry> entries(count);
    for (u32 i = 0; i < count; ++i) {
        const u8* e = table.data() + i * kPupEntrySize;
        PupEntry& entry = entries[i];
        entry.index = i;
        entry.flags = Get<u64>(e);
        entry.offset = Get<u64>(e + 8);
        entry.stored_size = Get<u64>(e + 16);
        entry.size = Get<u64>(e + 24);
        if (!InRange(entry.offset, entry.stored_size, file->Size())) {
            return fail("The PUP's entry table does not match the file. Only decrypted PUPs "
                        "(PS5UPDATE.PUP.dec) can be read.");
        }
    }
    for (const PupEntry& entry : entries) {
        if (entry.IsInfo() && entry.Id() < count) {
            entries[entry.Id()].block_table = entry.index;
        }
    }

    auto pup = std::unique_ptr<Pup>(new Pup());
    pup->m_root.kind = NodeKind::Folder;
    TreeBuilder builder(pup->m_problems);
    for (const PupEntry& entry : entries) {
        if (entry.IsInfo()) {
            continue;
        }
        const PupEntry* info = entry.block_table ? &entries[*entry.block_table] : nullptr;
        auto data = std::make_shared<PupEntrySource>(file, entry, info);
        if (!data->Ok()) {
            pup->m_problems.push_back(
                fmt::format("entry {}: left out, {}", entry.index,
                            data->Problem().empty() ? "it cannot be read" : data->Problem()));
            continue;
        }
        Node node;
        node.name = EntryName(entry, *data);
        node.size = entry.size;
        node.data = data;
        builder.Expand(node, node.name);
        pup->m_root.children.push_back(std::move(node));
    }
    if (pup->m_root.children.empty()) {
        return fail("The PUP has no entries that can be read. Only decrypted PUPs "
                    "(PS5UPDATE.PUP.dec) can be read.");
    }
    return pup;
}

const Node* Pup::Find(std::string_view rel_path) const {
    const Node* node = &m_root;
    size_t start = 0;
    while (start < rel_path.size()) {
        const size_t end = std::min(rel_path.find('/', start), rel_path.size());
        const std::string_view part = rel_path.substr(start, end - start);
        const auto it = std::find_if(node->children.begin(), node->children.end(),
                                     [&](const Node& child) { return child.name == part; });
        if (it == node->children.end()) {
            return nullptr;
        }
        node = &*it;
        start = end + 1;
    }
    return node;
}

bool Extract(const Pup& pup, const std::filesystem::path& output_dir,
             const std::vector<std::string>& rel_paths,
             const std::function<bool(const ExtractProgress&)>& progress_cb,
             std::string* error_message) {
    auto fail = [&](std::string why) {
        if (error_message != nullptr) {
            *error_message = std::move(why);
        }
        return false;
    };

    std::vector<std::pair<std::string, const Node*>> files;
    if (rel_paths.empty()) {
        CollectFiles(pup.Root(), "", files);
    }
    for (const std::string& rel : rel_paths) {
        const Node* node = pup.Find(rel);
        if (node == nullptr) {
            return fail(fmt::format("{} is not in the PUP.", rel));
        }
        CollectFiles(*node, rel, files);
    }

    ExtractProgress progress;
    for (const auto& [rel, node] : files) {
        progress.bytes_total += node->size;
    }

    std::vector<u8> buffer(kCopyChunk);
    for (const auto& [rel, node] : files) {
        progress.current_file = rel;
        if (progress_cb && !progress_cb(progress)) {
            return fail("Canceled");
        }
        const std::filesystem::path target = output_dir / PathFromUtf8(rel);
        std::error_code ec;
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) {
            return fail(fmt::format("Could not create {}: {}", target.parent_path().string(),
                                    ec.message()));
        }
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) {
            return fail(fmt::format("Could not write {}.", target.string()));
        }
        for (u64 done = 0; done < node->size;) {
            const u64 n = std::min<u64>(buffer.size(), node->size - done);
            if (!node->data->Read(done, buffer.data(), n)) {
                return fail(fmt::format("Could not read {} from the PUP.", rel));
            }
            if (!out.write(reinterpret_cast<const char*>(buffer.data()),
                           static_cast<std::streamsize>(n))) {
                return fail(fmt::format("Could not write {}.", target.string()));
            }
            done += n;
            progress.bytes_done += n;
            if (progress_cb && !progress_cb(progress)) {
                return fail("Canceled");
            }
        }
    }
    return true;
}

} // namespace PS5Pup
