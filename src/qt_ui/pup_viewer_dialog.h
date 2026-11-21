// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QDialog>

#include "core/file_format/ps5_pup.h"

class QLabel;
class QTreeWidget;
class QTreeWidgetItem;

class PupViewerDialog : public QDialog {
    Q_OBJECT
public:
    explicit PupViewerDialog(std::filesystem::path pup_path, QWidget* parent = nullptr);
    ~PupViewerDialog() override;
    static QString PickPup(QWidget* parent);

private:
    void SetupUi();
    void Populate(QTreeWidgetItem* parent_item, const PS5Pup::Node& node,
                  const std::string& rel_path);
    void SetChildrenCheckState(QTreeWidgetItem* item, Qt::CheckState state);
    void UpdateAncestorCheckState(QTreeWidgetItem* item);
    void CollectCheckedFiles(QTreeWidgetItem* item, std::vector<std::string>& out) const;
    [[nodiscard]] std::vector<std::string> GetCheckedFiles() const;
    void UpdateStatusLabel();

    void OnItemChanged(QTreeWidgetItem* item, int column);
    void OnSelectAll();
    void OnSelectNone();
    void OnExtractSelected();
    void OnExtractAll();
    void RunExtraction(const std::vector<std::string>& rel_paths,
                       const std::filesystem::path& output_dir);

    std::filesystem::path m_pup_path;
    std::shared_ptr<const PS5Pup::Pup> m_pup;
    QString m_open_error;

    QTreeWidget* m_tree = nullptr;
    QLabel* m_status_label = nullptr;
    QLabel* m_problems_label = nullptr;
    bool m_updating_checks = false;
};
