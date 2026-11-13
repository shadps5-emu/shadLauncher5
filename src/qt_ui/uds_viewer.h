// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <QDialog>

#include "common/types.h"
#include "core/file_format/ucp.h"
#include "core/file_format/uds.h"

class QComboBox;
class QTabWidget;

// Shows a title's Activities (UDS) definitions from sce_sys/uds/udsNN.ucp,
// the stats and trophy progress saved for the chosen user, and which
// extraction rules shadPS5 applies. Container udsNN belongs to the trophy set
// at position NN of npbind.dat, as in the emulator.
class UdsViewer : public QDialog {
    Q_OBJECT

public:
    UdsViewer(const QString& game_name, const std::filesystem::path& game_path,
              const std::filesystem::path& update_path, QWidget* parent = nullptr);

    // True if the game (or its update) has any sce_sys/uds/*.ucp.
    static bool HasUds(const std::filesystem::path& game_path,
                       const std::filesystem::path& update_path);

private:
    struct Container {
        std::string file_name;  // "uds00.ucp"
        std::string np_comm_id; // empty when npbind.dat has no entry for it
        UdsDefinitions uds;
        bool readable = false;
        std::optional<TrophySet> trophies;
    };

    struct SavedState {
        bool found = false;
        std::filesystem::path path;
        std::map<s64, u64> stats;             // "udsStats"
        std::map<std::string, bool> unlocked; // by trophy id
        std::map<std::string, u64> progress;  // by trophy id
    };

    void LoadContainers();
    void Refresh();
    QWidget* BuildPage(const Container& container, const SavedState& state);
    SavedState ReadState(const std::string& np_comm_id) const;

    std::vector<std::filesystem::path> m_roots; // update first, then base
    std::vector<Container> m_containers;
    QComboBox* m_user_combo = nullptr;
    QTabWidget* m_tabs = nullptr;
};
