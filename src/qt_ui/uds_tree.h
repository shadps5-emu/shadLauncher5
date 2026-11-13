// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <map>

#include <QCoreApplication>
#include <QString>
#include <QStringList>

#include "common/types.h"
#include "core/file_format/uds.h"

class QTreeWidget;
class QTreeWidgetItem;

// Builds the Activities (UDS) definition tree shown by the PKG info dialog and
// the UDS viewer: Events, Stats, Stat rules and Enums.
class UdsTree {
    Q_DECLARE_TR_FUNCTIONS(UdsTree)

public:
    // Extra column filled by the UDS viewer. Without it the tree keeps the
    // two columns "Definition" and "Details".
    struct Live {
        // Saved "udsStats" from the user's trophy state file, by stat id.
        const std::map<s64, u64>* stat_values = nullptr;
        // Mark each rule as used or skipped by shadPS5.
        bool emulator_marker = false;
    };

    // Section header ("Events (3)"): bold, spanning every column.
    static QTreeWidgetItem* AddSection(QTreeWidget* tree, const QString& label, std::size_t count);
    static QTreeWidgetItem* AddRow(QTreeWidgetItem* parent, const QStringList& columns);

    // Appends the four sections to `tree`. Does not clear it or set headers.
    static void Populate(QTreeWidget* tree, const UdsDefinitions& uds, const Live* live = nullptr);

    // "3 event(s) · 2 stat(s) ..." plus the NP ID / configuration and any
    // problems, as rich text for a label.
    static QString SummaryHtml(const UdsDefinitions& uds);
    // The same counts and ids as one plain line, for reports.
    static QString SummaryText(const UdsDefinitions& uds);

    // "used" or "skipped: <reason>".
    static QString SupportText(const UdsDefinitions& uds, const UdsRule& rule);

    static QString Str(std::string_view text);

private:
    static QStringList CountParts(const UdsDefinitions& uds);
    static QStringList IdParts(const UdsDefinitions& uds);
};
