// SPDX-FileCopyrightText: Copyright 2026 shadLauncher5 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "uds.h"

#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <system_error>

#include <nlohmann/json.hpp>

#include "core/file_format/ucp.h"

namespace {

using nlohmann::json;

std::string Str(const json& j, const char* key) {
    if (!j.is_object()) {
        return {};
    }
    const auto it = j.find(key);
    if (it == j.end() || it->is_null()) {
        return {};
    }
    return it->is_string() ? it->get<std::string>() : it->dump();
}

s64 Num(const json& j, const char* key) {
    if (!j.is_object()) {
        return 0;
    }
    const auto it = j.find(key);
    if (it == j.end()) {
        return 0;
    }
    if (it->is_number_integer()) {
        return it->get<s64>();
    }
    if (it->is_number()) {
        return static_cast<s64>(it->get<double>());
    }
    return 0;
}

const json& Array(const json& j, const char* key) {
    static const json kEmpty = json::array();
    if (!j.is_object()) {
        return kEmpty;
    }
    const auto it = j.find(key);
    return it != j.end() && it->is_array() ? *it : kEmpty;
}

std::optional<u64> Unsigned(std::string_view text) {
    u64 number = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), number);
    return error == std::errc{} && end == text.data() + text.size() && !text.empty()
               ? std::optional<u64>(number)
               : std::nullopt;
}

bool IsPropertyName(std::string_view input) {
    return input.size() > 2 && input.starts_with("$.") &&
           input.find_first_not_of(
               "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_", 2) ==
               std::string_view::npos;
}

bool ParseJson(std::string_view text, json& out) {
    out = json::parse(text, nullptr, /*allow_exceptions=*/false);
    return out.is_object();
}

} // namespace

bool UdsDefinitions::ParseEnums(std::string_view json_text) {
    json j;
    if (!ParseJson(json_text, j)) {
        return false;
    }
    if (np_comm_id.empty()) {
        np_comm_id = Str(j, "npCommunicationId");
    }
    enum_revision = Num(j, "enumDefinitionRevision");
    for (const auto& e : Array(j, "enumArray")) {
        UdsEnum en;
        en.id = Num(e, "enumId");
        en.group = Str(e, "definitionGroup");
        en.source_id = Str(e, "sourceId");
        for (const auto& v : Array(e, "enumValueArray")) {
            // Values are objects in some titles and plain strings in others.
            if (v.is_string()) {
                en.values.push_back(v.get<std::string>());
            } else {
                const std::string id = Str(v, "sourceId");
                en.values.push_back(id.empty() ? v.dump() : id);
            }
        }
        enums.push_back(std::move(en));
    }
    return true;
}

bool UdsDefinitions::ParseEvents(std::string_view json_text) {
    json j;
    if (!ParseJson(json_text, j)) {
        return false;
    }
    configuration_id = Num(j, "configurationId");
    for (const auto& e : Array(j, "events")) {
        UdsEvent ev;
        ev.name = Str(e, "eventName");
        ev.type = Str(e, "eventType");
        ev.group = Str(e, "definitionGroup");
        for (const auto& p : Array(e, "properties")) {
            ev.properties.push_back({Str(p, "property"), Str(p, "dataType")});
        }
        events.push_back(std::move(ev));
    }
    return true;
}

bool UdsDefinitions::ParseStats(std::string_view json_text) {
    json j;
    if (!ParseJson(json_text, j)) {
        return false;
    }
    if (np_comm_id.empty()) {
        np_comm_id = Str(j, "npCommunicationId");
    }
    stats_revision = Num(j, "statsDefinitionRevision");
    for (const auto& s : Array(j, "statDefinitionArray")) {
        UdsStat st;
        st.id = Num(s, "statId");
        st.name = Str(s, "statName");
        st.source_id = Str(s, "sourceId");
        st.group = Str(s, "definitionGroup");
        st.data_type = Str(s, "dataType");
        st.aggregation = Str(s, "aggregation");
        if (const auto it = s.find("enumDefinition"); it != s.end()) {
            st.enum_source_id = Str(*it, "sourceId");
        }
        st.initial_value = Str(s, "initialValue");
        st.min_value = Str(s, "minValue");
        st.max_value = Str(s, "maxValue");
        stats.push_back(std::move(st));
    }
    return true;
}

bool UdsDefinitions::ParseRules(std::string_view json_text) {
    json j;
    if (!ParseJson(json_text, j)) {
        return false;
    }
    if (np_comm_id.empty()) {
        np_comm_id = Str(j, "npCommunicationId");
    }
    extraction_revision = Num(j, "statsExtractionRevision");
    for (const auto& r : Array(j, "statsExtractionRuleArray")) {
        UdsRule rule;
        rule.id = Num(r, "ruleId");
        rule.source_id = Str(r, "sourceId");
        rule.group = Str(r, "definitionGroup");
        if (const auto c = r.find("condition"); c != r.end() && c->is_object()) {
            rule.event = Str(*c, "eventName");
            rule.filtered = c->size() != 1 || !c->contains("eventName");
            if (const auto p = c->find("property"); p != c->end() && p->is_object()) {
                rule.condition =
                    Str(*p, "path") + " " + Str(*p, "comparator") + " " + Str(*p, "value");
            }
        }
        if (const auto a = r.find("action"); a != r.end() && a->is_object()) {
            rule.input = Str(*a, "input");
            if (const auto o = a->find("output"); o != a->end() && o->is_object()) {
                rule.output_stat_id = Num(*o, "statId");
                rule.output_stat_name = Str(*o, "statName");
            }
        }
        rules.push_back(std::move(rule));
    }
    return true;
}

bool UdsDefinitions::LoadFromUcp(const UCP& ucp) {
    struct Part {
        const char* file;
        bool (UdsDefinitions::*parse)(std::string_view);
    };
    static constexpr Part kParts[] = {
        {"enum_definition.json", &UdsDefinitions::ParseEnums},
        {"events_definition.json", &UdsDefinitions::ParseEvents},
        {"stats_definition.json", &UdsDefinitions::ParseStats},
        {"stats_extraction.json", &UdsDefinitions::ParseRules},
    };
    bool any = false;
    for (const auto& part : kParts) {
        const auto data = ucp.ReadEntry(part.file);
        if (!data) {
            continue; // not every title ships every file
        }
        const std::string_view text(reinterpret_cast<const char*>(data->data()), data->size());
        if ((this->*part.parse)(text)) {
            any = true;
        } else {
            problems.push_back(std::string(part.file) + " could not be parsed");
        }
    }
    return any;
}

const UdsStat* UdsDefinitions::FindStat(s64 id) const {
    for (const auto& stat : stats) {
        if (stat.id == id) {
            return &stat;
        }
    }
    return nullptr;
}

UdsRuleSupport UdsDefinitions::CheckEmulatorSupport(const UdsRule& rule) const {
    if (rule.filtered || rule.event.empty()) {
        return UdsRuleSupport::Condition;
    }
    if (!IsPropertyName(rule.input)) {
        return UdsRuleSupport::Input;
    }
    const UdsStat* stat = FindStat(rule.output_stat_id);
    if (stat == nullptr || rule.output_stat_id < 0) {
        return UdsRuleSupport::NoStat;
    }
    if (stat->aggregation != "latest" && stat->aggregation != "max") {
        return UdsRuleSupport::Aggregation;
    }
    const auto& type = stat->data_type;
    if (type != "uint32" && type != "uint64" && type != "int64") {
        return UdsRuleSupport::DataType;
    }
    if (type == "int64" && (stat->aggregation != "max" || stat->min_value.empty())) {
        return UdsRuleSupport::Int64Latest;
    }
    const u64 limit = type == "uint32"  ? std::numeric_limits<u32>::max()
                      : type == "int64" ? static_cast<u64>(std::numeric_limits<s64>::max())
                                        : std::numeric_limits<u64>::max();
    if ((!stat->min_value.empty() && Unsigned(stat->min_value) != u64{0}) ||
        (!stat->max_value.empty() && Unsigned(stat->max_value) != limit)) {
        return UdsRuleSupport::Range;
    }
    if (!stat->initial_value.empty()) {
        const auto initial = Unsigned(stat->initial_value);
        if (!initial || *initial > limit) {
            return UdsRuleSupport::InitialValue;
        }
    }
    return UdsRuleSupport::Used;
}
