// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "uds_viewer.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <set>
#include <system_error>

#include <QComboBox>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <nlohmann/json.hpp>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/file_format/npbind.h"
#include "core/file_sys/game_backend.h"
#include "core/user_settings.h"
#include "uds_tree.h"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view UdsRelDir = "sce_sys/uds";

std::optional<fs::path> Resolve(const std::vector<fs::path>& roots, std::string_view rel) {
    for (const auto& root : roots) {
        if (auto path = Core::FileSys::ResolveGameFilePath(root, rel)) {
            return path;
        }
    }
    return std::nullopt;
}

// *.ucp names in rel_dir across all roots, sorted.
std::vector<std::string> ListContainers(const std::vector<fs::path>& roots,
                                        std::string_view rel_dir) {
    std::set<std::string> names;
    for (const auto& root : roots) {
        for (const auto& entry : Core::FileSys::ListGameDir(root, rel_dir)) {
            if (!entry.is_directory && UCP::IsContainerFileName(entry.name)) {
                names.insert(entry.name);
            }
        }
    }
    return {names.begin(), names.end()};
}

std::vector<fs::path> Roots(const fs::path& game_path, const fs::path& update_path) {
    std::vector<fs::path> roots;
    if (!update_path.empty()) {
        roots.push_back(update_path);
    }
    if (!game_path.empty()) {
        roots.push_back(game_path);
    }
    return roots;
}

// "uds03.ucp" -> 3, the npbind.dat position the emulator pairs it with.
std::optional<std::size_t> LabelOf(std::string_view name) {
    if (name.size() != 9 || !name.starts_with("uds") || !name.ends_with(".ucp")) {
        return std::nullopt;
    }
    std::size_t label = 0;
    const auto digits = name.substr(3, 2);
    const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), label);
    return error == std::errc{} && end == digits.data() + digits.size()
               ? std::optional<std::size_t>(label)
               : std::nullopt;
}

std::optional<s64> ToNumber(std::string_view text) {
    s64 number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    return error == std::errc{} && end == text.data() + text.size() && !text.empty()
               ? std::optional<s64>(number)
               : std::nullopt;
}

QString ComparatorText(std::string_view comparator) {
    if (comparator == "ge") {
        return QStringLiteral("≥");
    }
    if (comparator == "gt") {
        return QStringLiteral(">");
    }
    if (comparator == "le") {
        return QStringLiteral("≤");
    }
    if (comparator == "lt") {
        return QStringLiteral("<");
    }
    if (comparator == "eq") {
        return QStringLiteral("=");
    }
    return UdsTree::Str(comparator);
}

std::string CurrentUserId(const QComboBox* combo) {
    if (combo != nullptr && combo->currentIndex() >= 0) {
        return std::to_string(combo->currentData().toInt());
    }
    return std::to_string(UserSettings.GetUserManager().GetDefaultUser().user_id);
}

} // namespace

bool UdsViewer::HasUds(const fs::path& game_path, const fs::path& update_path) {
    return !ListContainers(Roots(game_path, update_path), UdsRelDir).empty();
}

UdsViewer::UdsViewer(const QString& game_name, const fs::path& game_path,
                     const fs::path& update_path, QWidget* parent)
    : QDialog(parent), m_roots(Roots(game_path, update_path)) {
    setWindowTitle(tr("UDS Viewer") + QStringLiteral(" - ") + game_name);
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowFlag(Qt::WindowMaximizeButtonHint, true);
    resize(1000, 700);

    auto* layout = new QVBoxLayout(this);
    auto* top = new QHBoxLayout();
    top->addWidget(new QLabel(tr("User:"), this));
    m_user_combo = new QComboBox(this);
    const s32 default_user = UserSettings.GetUserManager().GetDefaultUser().user_id;
    for (const auto& user : UserSettings.GetUserManager().GetAllUsers()) {
        m_user_combo->addItem(QString::fromStdString(user.user_name), user.user_id);
        if (user.user_id == default_user) {
            m_user_combo->setCurrentIndex(m_user_combo->count() - 1);
        }
    }
    top->addWidget(m_user_combo);
    auto* reload = new QPushButton(tr("Reload"), this);
    reload->setToolTip(tr("Read the saved stats and trophy progress again."));
    top->addWidget(reload);
    top->addStretch();
    layout->addLayout(top);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    auto* close = new QPushButton(tr("Close"), this);
    auto* bottom = new QHBoxLayout();
    bottom->addStretch();
    bottom->addWidget(close);
    layout->addLayout(bottom);

    connect(m_user_combo, &QComboBox::currentIndexChanged, this, [this] { Refresh(); });
    connect(reload, &QPushButton::clicked, this, [this] { Refresh(); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    LoadContainers();
    Refresh();
}

void UdsViewer::LoadContainers() {
    m_containers.clear();

    std::vector<std::string> np_comm_ids;
    if (const auto npbind_path = Resolve(m_roots, "sce_sys/trophy2/npbind.dat")) {
        NPBindFile npbind;
        if (npbind.Load(npbind_path->string())) {
            np_comm_ids = npbind.GetNpCommIds();
        }
    }

    // Trophy containers, for the unlock conditions that read UDS stats.
    std::vector<fs::path> trophy_candidates;
    for (const auto& name : ListContainers(m_roots, Core::FileSys::TrophyRelDir)) {
        const std::string rel = std::string(Core::FileSys::TrophyRelDir) + "/" + name;
        if (const auto path = Resolve(m_roots, rel)) {
            trophy_candidates.push_back(*path);
        }
    }
    std::sort(trophy_candidates.begin(), trophy_candidates.end());

    for (const auto& name : ListContainers(m_roots, UdsRelDir)) {
        Container container;
        container.file_name = name;

        if (const auto path = Resolve(m_roots, std::string(UdsRelDir) + "/" + name)) {
            UCP ucp;
            container.readable = ucp.Open(*path) && container.uds.LoadFromUcp(ucp);
        }
        if (!container.readable) {
            LOG_WARNING(Loader, "Couldn't read UDS container {}", name);
        }

        const auto label = LabelOf(name);
        if (label && *label < np_comm_ids.size()) {
            container.np_comm_id = np_comm_ids[*label];
            const auto trophy_ucp = UCP::SelectContainerFor(trophy_candidates, container.np_comm_id,
                                                            static_cast<int>(*label));
            UCP ucp;
            TrophySet set;
            if (trophy_ucp && ucp.Open(*trophy_ucp) &&
                ucp.LoadTrophySet(set, EmulatorSettings.GetConsoleLanguage())) {
                container.trophies = std::move(set);
            }
        }
        m_containers.push_back(std::move(container));
    }
}

UdsViewer::SavedState UdsViewer::ReadState(const std::string& np_comm_id) const {
    SavedState state;
    if (np_comm_id.empty()) {
        return state;
    }
    state.path = EmulatorSettings.GetHomeDir() / CurrentUserId(m_user_combo) / "trophy" /
                 (np_comm_id + ".json");
    std::ifstream file(state.path);
    if (!file) {
        return state;
    }
    const auto j = nlohmann::json::parse(file, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) {
        return state;
    }
    state.found = true;

    if (const auto list = j.find("trophies"); list != j.end() && list->is_array()) {
        for (const auto& t : *list) {
            const auto id = t.is_object() ? t.find("id") : t.end();
            if (!t.is_object() || id == t.end() || !id->is_string()) {
                continue;
            }
            const auto key = id->get<std::string>();
            state.unlocked[key] = t.value("unlocked", false);
            if (const auto p = t.find("progress"); p != t.end() && p->is_number_unsigned()) {
                state.progress[key] = p->get<u64>();
            }
        }
    }
    if (const auto stats = j.find("udsStats"); stats != j.end() && stats->is_object()) {
        for (const auto& [key, value] : stats->items()) {
            const auto id = ToNumber(key);
            if (id && value.is_number_unsigned()) {
                state.stats[*id] = value.get<u64>();
            }
        }
    }
    return state;
}

QWidget* UdsViewer::BuildPage(const Container& container, const SavedState& state) {
    auto* page = new QWidget(m_tabs);
    auto* layout = new QVBoxLayout(page);

    auto* summary = new QLabel(page);
    summary->setWordWrap(true);
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(summary);

    if (!container.readable) {
        summary->setText(tr("The activity container could not be read."));
        layout->addStretch();
        return page;
    }

    QString text = UdsTree::SummaryHtml(container.uds);
    if (container.np_comm_id.empty()) {
        text += QStringLiteral("<br>") +
                tr("No trophy set in npbind.dat for this container, so shadPS5 does not load it.")
                    .toHtmlEscaped();
    } else if (state.found) {
        QString path;
        Common::FS::PathToQString(path, state.path);
        text += QStringLiteral("<br>") +
                tr("Saved state: %1").arg(QDir::toNativeSeparators(path)).toHtmlEscaped();
    } else {
        text += QStringLiteral("<br>") + tr("Nothing saved for this user yet.").toHtmlEscaped();
    }
    summary->setText(text);

    auto* tree = new QTreeWidget(page);
    tree->setColumnCount(3);
    tree->setHeaderLabels({tr("Definition"), tr("Details"), tr("State")});
    tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    tree->header()->resizeSection(0, 360);
    tree->header()->resizeSection(1, 300);
    tree->header()->setStretchLastSection(true);
    layout->addWidget(tree, 1);

    // Stats the emulator actually writes to, to flag trophies nothing feeds.
    std::set<s64> fed_stats;
    for (const auto& rule : container.uds.rules) {
        if (container.uds.CheckEmulatorSupport(rule) == UdsRuleSupport::Used) {
            fed_stats.insert(rule.output_stat_id);
        }
    }

    if (container.trophies) {
        std::vector<const TrophyDefinition*> linked;
        for (const auto& trophy : container.trophies->trophies) {
            if (trophy.has_unlock_condition) {
                linked.push_back(&trophy);
            }
        }
        auto* section = UdsTree::AddSection(tree, tr("Trophies"), linked.size());
        for (const auto* trophy : linked) {
            const auto& cond = trophy->unlock_condition;
            const auto stat_id = ToNumber(cond.uds_stat_id);
            const auto target = ToNumber(cond.target_value);
            QString condition =
                tr("stat #%1 %2 %3")
                    .arg(UdsTree::Str(cond.uds_stat_id), ComparatorText(cond.comparator),
                         UdsTree::Str(cond.target_value));
            if (stat_id) {
                if (const UdsStat* stat = container.uds.FindStat(*stat_id)) {
                    condition += QStringLiteral(" (%1)").arg(UdsTree::Str(stat->name));
                }
            }
            if (cond.progressive) {
                condition += tr(", progressive");
            }

            QString status;
            const auto unlocked = state.unlocked.find(trophy->id);
            if (unlocked != state.unlocked.end() && unlocked->second) {
                status = tr("unlocked");
            } else if (target && *target < 0) {
                status = tr("condition disabled");
            } else {
                // As the emulator: the higher of the saved max stat and the
                // reported progress, capped at the target.
                std::optional<u64> value;
                if (const auto saved = stat_id ? state.stats.find(*stat_id) : state.stats.end();
                    saved != state.stats.end()) {
                    value = saved->second;
                }
                if (const auto reported = state.progress.find(trophy->id);
                    reported != state.progress.end()) {
                    value = std::max(value.value_or(0), reported->second);
                }
                if (value && target) {
                    status =
                        tr("%1 / %2").arg(std::min(*value, static_cast<u64>(*target))).arg(*target);
                } else {
                    status = tr("locked");
                }
                if (stat_id && !fed_stats.contains(*stat_id)) {
                    status += tr(" · no supported rule writes this stat");
                }
            }

            const QString title = QStringLiteral("%1 %2 [%3]")
                                      .arg(UdsTree::Str(trophy->id), UdsTree::Str(trophy->name),
                                           UdsTree::Str(TrophySet::GradeName(trophy->grade)));
            UdsTree::AddRow(section, {title, condition, status});
        }
        section->setExpanded(true);
    }

    UdsTree::Live live;
    live.stat_values = &state.stats;
    live.emulator_marker = true;
    UdsTree::Populate(tree, container.uds, &live);
    return page;
}

void UdsViewer::Refresh() {
    const int current = m_tabs->currentIndex();
    while (m_tabs->count() > 0) {
        QWidget* page = m_tabs->widget(0);
        m_tabs->removeTab(0);
        page->deleteLater();
    }

    if (m_containers.empty()) {
        auto* label = new QLabel(tr("This game has no activity (UDS) data."), m_tabs);
        label->setAlignment(Qt::AlignCenter);
        m_tabs->addTab(label, tr("UDS"));
        return;
    }

    for (const auto& container : m_containers) {
        QString title = UdsTree::Str(container.file_name);
        if (!container.np_comm_id.empty()) {
            title += QStringLiteral(" (%1)").arg(UdsTree::Str(container.np_comm_id));
        }
        m_tabs->addTab(BuildPage(container, ReadState(container.np_comm_id)), title);
    }
    if (current >= 0 && current < m_tabs->count()) {
        m_tabs->setCurrentIndex(current);
    }
}
