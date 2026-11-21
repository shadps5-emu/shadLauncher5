// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "common/types.h"

namespace PS5Pup {

class Source {
public:
    virtual ~Source() = default;
    [[nodiscard]] virtual u64 Size() const = 0;
    virtual bool Read(u64 offset, void* dst, u64 size) const = 0;
};

enum class NodeKind {
    File,
    Folder, // a folder in an exFAT image
    Slb2,   // an SLB2 container, shown as a folder of its files
    Exfat,  // an exFAT image, shown as a folder of its files
};

struct Node {
    std::string name;
    NodeKind kind = NodeKind::File;
    u64 size = 0;                       // a file's size, or a container's own size
    std::shared_ptr<const Source> data; // files and containers
    std::vector<Node> children;         // folders and containers

    [[nodiscard]] bool IsDirectory() const {
        return kind != NodeKind::File;
    }
};

class Pup {
public:
    static std::unique_ptr<Pup> Open(const std::filesystem::path& path, std::string* error);

    [[nodiscard]] const Node& Root() const {
        return m_root;
    }
    [[nodiscard]] const Node* Find(std::string_view rel_path) const;
    [[nodiscard]] const std::vector<std::string>& Problems() const {
        return m_problems;
    }

private:
    Node m_root;
    std::vector<std::string> m_problems;
};

struct ExtractProgress {
    u64 bytes_done = 0;
    u64 bytes_total = 0;
    std::string current_file;
};

bool Extract(const Pup& pup, const std::filesystem::path& output_dir,
             const std::vector<std::string>& rel_paths,
             const std::function<bool(const ExtractProgress&)>& progress_cb,
             std::string* error_message);

} // namespace PS5Pup
