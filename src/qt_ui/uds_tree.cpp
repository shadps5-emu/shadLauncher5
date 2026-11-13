// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "uds_tree.h"

#include <QFont>
#include <QTreeWidget>
#include <QTreeWidgetItem>

QString UdsTree::Str(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

QTreeWidgetItem* UdsTree::AddSection(QTreeWidget* tree, const QString& label, std::size_t count) {
    auto* item = new QTreeWidgetItem(tree, {tr("%1 (%2)").arg(label).arg(count)});
    QFont f = item->font(0);
    f.setBold(true);
    item->setFont(0, f);
    item->setFirstColumnSpanned(true);
    return item;
}

QTreeWidgetItem* UdsTree::AddRow(QTreeWidgetItem* parent, const QStringList& columns) {
    auto* item = new QTreeWidgetItem(parent, columns);
    for (int i = 0; i < columns.size(); ++i) {
        item->setToolTip(i, columns[i]);
    }
    return item;
}

QString UdsTree::SupportText(const UdsDefinitions& uds, const UdsRule& rule) {
    const UdsStat* stat = uds.FindStat(rule.output_stat_id);
    const QString aggregation = stat != nullptr ? Str(stat->aggregation) : QString();
    const QString type = stat != nullptr ? Str(stat->data_type) : QString();
    QString reason;
    switch (uds.CheckEmulatorSupport(rule)) {
    case UdsRuleSupport::Used:
        return tr("used");
    case UdsRuleSupport::Condition:
        reason = rule.condition.empty() ? tr("has a condition")
                                        : tr("has a condition (%1)").arg(Str(rule.condition));
        break;
    case UdsRuleSupport::Input:
        reason = tr("input %1 is not an event property").arg(Str(rule.input));
        break;
    case UdsRuleSupport::NoStat:
        reason = tr("stat #%1 is not defined").arg(rule.output_stat_id);
        break;
    case UdsRuleSupport::Aggregation:
        reason = tr("%1 aggregation").arg(aggregation);
        break;
    case UdsRuleSupport::DataType:
        reason = tr("%1 data type").arg(type);
        break;
    case UdsRuleSupport::Int64Latest:
        reason = tr("int64 stat without max aggregation and a minValue");
        break;
    case UdsRuleSupport::Range:
        reason = tr("stat range is not 0 to the %1 limit").arg(type);
        break;
    case UdsRuleSupport::InitialValue:
        reason = tr("initial value %1 is out of range").arg(Str(stat->initial_value));
        break;
    }
    return tr("skipped: %1").arg(reason);
}

void UdsTree::Populate(QTreeWidget* tree, const UdsDefinitions& uds, const Live* live) {
    auto* events = AddSection(tree, tr("Events"), uds.events.size());
    for (const auto& ev : uds.events) {
        auto* item =
            AddRow(events, {Str(ev.name),
                            QStringList{Str(ev.type), Str(ev.group)}.join(QStringLiteral(", "))});
        for (const auto& p : ev.properties) {
            AddRow(item, {Str(p.path), Str(p.data_type)});
        }
    }

    auto* stats = AddSection(tree, tr("Stats"), uds.stats.size());
    for (const auto& st : uds.stats) {
        QStringList details{Str(st.data_type), Str(st.aggregation)};
        if (!st.enum_source_id.empty()) {
            details << tr("values from %1").arg(Str(st.enum_source_id));
        }
        QStringList columns{QStringLiteral("#%1 %2").arg(st.id).arg(Str(st.name)),
                            details.join(QStringLiteral(", "))};
        if (live != nullptr && live->stat_values != nullptr) {
            const auto it = live->stat_values->find(st.id);
            columns << (it != live->stat_values->end() ? QString::number(it->second) : QString());
        }
        auto* item = AddRow(stats, columns);
        item->setToolTip(0, tr("Group: %1, source: %2").arg(Str(st.group), Str(st.source_id)));
    }

    auto* rules = AddSection(tree, tr("Stat rules"), uds.rules.size());
    for (const auto& r : uds.rules) {
        QString when = tr("on %1").arg(Str(r.event));
        if (!r.condition.empty()) {
            when += tr(" if %1").arg(Str(r.condition));
        }
        QStringList columns{QStringLiteral("#%1 %2").arg(r.id).arg(when),
                            tr("%1 → stat #%2 %3")
                                .arg(Str(r.input))
                                .arg(r.output_stat_id)
                                .arg(Str(r.output_stat_name))};
        const bool marker = live != nullptr && live->emulator_marker;
        if (marker) {
            columns << SupportText(uds, r);
        }
        auto* item = AddRow(rules, columns);
        if (marker && uds.CheckEmulatorSupport(r) != UdsRuleSupport::Used) {
            QFont f = item->font(2);
            f.setItalic(true);
            item->setFont(2, f);
        }
    }

    auto* enums = AddSection(tree, tr("Enums"), uds.enums.size());
    for (const auto& en : uds.enums) {
        auto* item =
            AddRow(enums, {QStringLiteral("#%1 %2").arg(en.id).arg(Str(en.source_id)),
                           tr("%n value(s)", nullptr, static_cast<int>(en.values.size()))});
        for (const auto& v : en.values) {
            AddRow(item, {Str(v), QString()});
        }
    }
    events->setExpanded(true);
    stats->setExpanded(true);
}

QStringList UdsTree::CountParts(const UdsDefinitions& uds) {
    return {tr("%n event(s)", nullptr, static_cast<int>(uds.events.size())),
            tr("%n stat(s)", nullptr, static_cast<int>(uds.stats.size())),
            tr("%n rule(s)", nullptr, static_cast<int>(uds.rules.size())),
            tr("%n enum(s)", nullptr, static_cast<int>(uds.enums.size()))};
}

QStringList UdsTree::IdParts(const UdsDefinitions& uds) {
    QStringList ids;
    if (!uds.np_comm_id.empty()) {
        ids << tr("NP ID %1").arg(Str(uds.np_comm_id));
    }
    if (uds.configuration_id != 0) {
        ids << tr("configuration %1").arg(uds.configuration_id);
    }
    return ids;
}

QString UdsTree::SummaryHtml(const UdsDefinitions& uds) {
    const QStringList ids = IdParts(uds);
    QString text = QStringLiteral("<b>%1</b>").arg(CountParts(uds).join(QStringLiteral(" · ")));
    if (!ids.isEmpty()) {
        text += QStringLiteral("<br>") + ids.join(QStringLiteral(" · ")).toHtmlEscaped();
    }
    if (!uds.problems.empty()) {
        QStringList p;
        for (const auto& s : uds.problems) {
            p << Str(s);
        }
        text += QStringLiteral("<br><span style=\"color:#d9534f\">%1</span>")
                    .arg(p.join(QStringLiteral("; ")).toHtmlEscaped());
    }
    return text;
}

QString UdsTree::SummaryText(const UdsDefinitions& uds) {
    const QStringList ids = IdParts(uds);
    QString text = CountParts(uds).join(QStringLiteral(", "));
    if (!ids.isEmpty()) {
        text += QStringLiteral(" (") + ids.join(QStringLiteral(", ")) + QLatin1Char(')');
    }
    return text;
}
