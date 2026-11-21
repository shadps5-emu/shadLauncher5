// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <memory>

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "common/path_util.h"
#include "progress_dialog.h"
#include "pup_viewer_dialog.h"

namespace {

constexpr int RelPathRole = Qt::UserRole;
constexpr int IsDirRole = Qt::UserRole + 1;

} // namespace

PupViewerDialog::PupViewerDialog(std::filesystem::path pup_path, QWidget* parent)
    : QDialog(parent), m_pup_path(std::move(pup_path)) {
    std::string error;
    m_pup = PS5Pup::Pup::Open(m_pup_path, &error);

    SetupUi();

    if (!m_pup) {
        m_status_label->setText(
            tr("Failed to open the PUP: %1").arg(QString::fromStdString(error)));
        m_tree->setEnabled(false);
        return;
    }

    m_updating_checks = true;
    Populate(nullptr, m_pup->Root(), "");
    m_updating_checks = false;

    const auto& problems = m_pup->Problems();
    if (!problems.empty()) {
        QStringList lines;
        for (const std::string& problem : problems) {
            lines << QString::fromStdString(problem);
        }
        m_problems_label->setText(tr("%n item(s) could not be read fully; hover here for details.",
                                     "", static_cast<int>(problems.size())));
        m_problems_label->setToolTip(lines.join('\n'));
        m_problems_label->show();
    }
    UpdateStatusLabel();
}

PupViewerDialog::~PupViewerDialog() = default;

QString PupViewerDialog::PickPup(QWidget* parent) {
    return QFileDialog::getOpenFileName(
        parent, tr("Open PS5 System Update"), QString(),
        tr("Decrypted PS5 updates (*.PUP.dec *.pup.dec *.dec);;All files (*)"));
}

void PupViewerDialog::SetupUi() {
    QString pup_qpath;
    Common::FS::PathToQString(pup_qpath, m_pup_path);

    setWindowTitle(
        tr("PUP Viewer - %1").arg(QString::fromStdString(m_pup_path.filename().string())));
    resize(720, 560);

    auto* layout = new QVBoxLayout(this);

    auto* path_label = new QLabel(pup_qpath, this);
    path_label->setWordWrap(true);
    layout->addWidget(path_label);

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({tr("Name"), tr("Type"), tr("Size")});
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setUniformRowHeights(true);
    layout->addWidget(m_tree, /*stretch=*/1);

    connect(m_tree, &QTreeWidget::itemChanged, this, &PupViewerDialog::OnItemChanged);

    m_problems_label = new QLabel(this);
    m_problems_label->hide();
    layout->addWidget(m_problems_label);

    m_status_label = new QLabel(this);
    layout->addWidget(m_status_label);

    auto* button_row = new QHBoxLayout();
    auto* select_all = new QPushButton(tr("Select All"), this);
    auto* select_none = new QPushButton(tr("Select None"), this);
    connect(select_all, &QPushButton::clicked, this, &PupViewerDialog::OnSelectAll);
    connect(select_none, &QPushButton::clicked, this, &PupViewerDialog::OnSelectNone);
    button_row->addWidget(select_all);
    button_row->addWidget(select_none);
    button_row->addStretch(1);

    auto* extract_selected = new QPushButton(tr("Extract Selected..."), this);
    auto* extract_all = new QPushButton(tr("Extract All..."), this);
    connect(extract_selected, &QPushButton::clicked, this, &PupViewerDialog::OnExtractSelected);
    connect(extract_all, &QPushButton::clicked, this, &PupViewerDialog::OnExtractAll);
    button_row->addWidget(extract_selected);
    button_row->addWidget(extract_all);
    layout->addLayout(button_row);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
    layout->addWidget(box);
}

void PupViewerDialog::Populate(QTreeWidgetItem* parent_item, const PS5Pup::Node& node,
                               const std::string& rel_path) {
    std::vector<const PS5Pup::Node*> children;
    for (const PS5Pup::Node& child : node.children) {
        children.push_back(&child);
    }
    if (parent_item != nullptr) {
        std::stable_sort(children.begin(), children.end(), [](const auto* a, const auto* b) {
            if (a->IsDirectory() != b->IsDirectory()) {
                return a->IsDirectory();
            }
            return a->name < b->name;
        });
    }

    const QIcon folder_icon = style()->standardIcon(QStyle::SP_DirIcon);
    const QIcon file_icon = style()->standardIcon(QStyle::SP_FileIcon);

    for (const PS5Pup::Node* child : children) {
        const std::string child_rel = rel_path.empty() ? child->name : rel_path + "/" + child->name;

        QString type;
        switch (child->kind) {
        case PS5Pup::NodeKind::Folder:
            type = tr("Folder");
            break;
        case PS5Pup::NodeKind::Slb2:
            type = tr("SLB2 container");
            break;
        case PS5Pup::NodeKind::Exfat:
            type = tr("exFAT image");
            break;
        case PS5Pup::NodeKind::File:
        default:
            type = tr("File");
            break;
        }

        auto* item = new QTreeWidgetItem();
        item->setText(0, QString::fromStdString(child->name));
        item->setText(1, type);
        if (child->kind != PS5Pup::NodeKind::Folder) {
            item->setText(2, QLocale().formattedDataSize(static_cast<qint64>(child->size)));
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        }
        item->setIcon(0, child->IsDirectory() ? folder_icon : file_icon);
        item->setData(0, RelPathRole, QString::fromStdString(child_rel));
        item->setData(0, IsDirRole, child->IsDirectory());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, Qt::Unchecked);

        if (parent_item) {
            parent_item->addChild(item);
        } else {
            m_tree->addTopLevelItem(item);
        }

        if (child->IsDirectory()) {
            Populate(item, *child, child_rel);
        }
    }
}

void PupViewerDialog::SetChildrenCheckState(QTreeWidgetItem* item, Qt::CheckState state) {
    for (int i = 0; i < item->childCount(); ++i) {
        QTreeWidgetItem* child = item->child(i);
        child->setCheckState(0, state);
        if (child->data(0, IsDirRole).toBool()) {
            SetChildrenCheckState(child, state);
        }
    }
}

void PupViewerDialog::UpdateAncestorCheckState(QTreeWidgetItem* item) {
    while (item) {
        bool any_checked = false;
        bool any_unchecked = false;
        for (int i = 0; i < item->childCount(); ++i) {
            switch (item->child(i)->checkState(0)) {
            case Qt::Checked:
                any_checked = true;
                break;
            case Qt::Unchecked:
                any_unchecked = true;
                break;
            case Qt::PartiallyChecked:
                any_checked = true;
                any_unchecked = true;
                break;
            }
        }

        Qt::CheckState new_state = Qt::Unchecked;
        if (any_checked && !any_unchecked) {
            new_state = Qt::Checked;
        } else if (any_checked && any_unchecked) {
            new_state = Qt::PartiallyChecked;
        }
        item->setCheckState(0, new_state);

        item = item->parent();
    }
}

void PupViewerDialog::OnItemChanged(QTreeWidgetItem* item, int column) {
    if (column != 0 || m_updating_checks || !item) {
        return;
    }

    m_updating_checks = true;

    if (item->data(0, IsDirRole).toBool()) {
        const Qt::CheckState state = item->checkState(0);
        if (state != Qt::PartiallyChecked) {
            SetChildrenCheckState(item, state);
        }
    }

    UpdateAncestorCheckState(item->parent());

    m_updating_checks = false;
    UpdateStatusLabel();
}

void PupViewerDialog::CollectCheckedFiles(QTreeWidgetItem* item,
                                          std::vector<std::string>& out) const {
    if (!item->data(0, IsDirRole).toBool()) {
        if (item->checkState(0) == Qt::Checked) {
            out.push_back(item->data(0, RelPathRole).toString().toStdString());
        }
        return;
    }
    for (int i = 0; i < item->childCount(); ++i) {
        CollectCheckedFiles(item->child(i), out);
    }
}

std::vector<std::string> PupViewerDialog::GetCheckedFiles() const {
    std::vector<std::string> files;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        CollectCheckedFiles(m_tree->topLevelItem(i), files);
    }
    return files;
}

void PupViewerDialog::UpdateStatusLabel() {
    const auto checked = GetCheckedFiles();
    if (checked.empty()) {
        m_status_label->setText(tr("No files selected."));
    } else {
        m_status_label->setText(tr("%1 file(s) selected.").arg(checked.size()));
    }
}

void PupViewerDialog::OnSelectAll() {
    m_updating_checks = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        SetChildrenCheckState(item, Qt::Checked);
        item->setCheckState(0, Qt::Checked);
    }
    m_updating_checks = false;
    UpdateStatusLabel();
}

void PupViewerDialog::OnSelectNone() {
    m_updating_checks = true;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        QTreeWidgetItem* item = m_tree->topLevelItem(i);
        SetChildrenCheckState(item, Qt::Unchecked);
        item->setCheckState(0, Qt::Unchecked);
    }
    m_updating_checks = false;
    UpdateStatusLabel();
}

void PupViewerDialog::OnExtractSelected() {
    if (!m_pup) {
        return;
    }
    const auto files = GetCheckedFiles();
    if (files.empty()) {
        QMessageBox::information(this, tr("Extract Selected"),
                                 tr("Select one or more files to extract first."));
        return;
    }

    const QString output_path_str = QFileDialog::getExistingDirectory(
        this, tr("Extract Selected Files To"), QString(), QFileDialog::ShowDirsOnly);
    if (output_path_str.isEmpty()) {
        return;
    }

    RunExtraction(files, Common::FS::PathFromQString(output_path_str));
}

void PupViewerDialog::OnExtractAll() {
    if (!m_pup) {
        return;
    }
    const QString output_path_str = QFileDialog::getExistingDirectory(
        this, tr("Extract All Files To"), QString(), QFileDialog::ShowDirsOnly);
    if (output_path_str.isEmpty()) {
        return;
    }

    RunExtraction({}, Common::FS::PathFromQString(output_path_str));
}

void PupViewerDialog::RunExtraction(const std::vector<std::string>& rel_paths,
                                    const std::filesystem::path& output_dir) {
    const bool extract_all = rel_paths.empty();

    auto* progress = new ProgressDialog(tr("Extract from PUP"),
                                        extract_all ? tr("Extracting all files...")
                                                    : tr("Extracting selected files..."),
                                        tr("Cancel"), 0, 1000, /*delete_on_close=*/true, this);
    progress->SetValue(0);
    progress->show();

    auto cancel_flag = std::make_shared<std::atomic<bool>>(false);
    connect(progress, &QProgressDialog::canceled, this, [cancel_flag] { *cancel_flag = true; });

    struct ExtractResult {
        bool success = false;
        std::string error_message;
    };

    QPointer<ProgressDialog> progress_guard(progress);
    auto* watcher = new QFutureWatcher<ExtractResult>(this);

    connect(
        watcher, &QFutureWatcher<ExtractResult>::finished, this, [this, watcher, progress_guard]() {
            const ExtractResult result = watcher->result();
            watcher->deleteLater();

            if (progress_guard) {
                progress_guard->close();
            }

            if (!result.success) {
                if (result.error_message != "Canceled") {
                    QMessageBox::critical(this, tr("Extract from PUP"),
                                          tr("Failed to extract files:\n%1")
                                              .arg(QString::fromStdString(result.error_message)));
                }
                return;
            }

            QMessageBox::information(this, tr("Extract from PUP"), tr("Extraction finished."));
        });

    auto pup = m_pup;
    auto future = QtConcurrent::run([pup, output_dir, rel_paths, cancel_flag,
                                     progress_guard]() -> ExtractResult {
        std::string error_message;
        int last_percent = -1;
        std::string last_file;
        const auto progress_cb = [cancel_flag, progress_guard, &last_percent,
                                  &last_file](const PS5Pup::ExtractProgress& p) {
            const int percent =
                p.bytes_total > 0 ? static_cast<int>((p.bytes_done * 1000) / p.bytes_total) : 0;
            if (progress_guard && (percent != last_percent || p.current_file != last_file)) {
                last_percent = percent;
                last_file = p.current_file;
                QMetaObject::invokeMethod(
                    progress_guard.data(),
                    [progress_guard, percent, file = p.current_file]() {
                        if (!progress_guard) {
                            return;
                        }
                        progress_guard->SetValue(percent);
                        progress_guard->setLabelText(
                            QObject::tr("Extracting: %1").arg(QString::fromStdString(file)));
                    },
                    Qt::QueuedConnection);
            }
            return !cancel_flag->load();
        };

        const bool ok = PS5Pup::Extract(*pup, output_dir, rel_paths, progress_cb, &error_message);
        return ExtractResult{ok, ok ? std::string() : error_message};
    });

    watcher->setFuture(future);
}
