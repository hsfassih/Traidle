#include "lint.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

namespace fs = std::filesystem;

namespace {

optional<json> readJson(const fs::path& p, Diagnostics& d) {
    ifstream in(p);
    if (!in) {
        d.error(p.string(), "cannot open");
        return nullopt;
    }
    try {
        return json::parse(in);
    } catch (const exception& e) {
        d.error(p.filename().string(), string("JSON parse error: ") + e.what());
        return nullopt;
    }
}

Catalog loadCatalog(const json& c) {
    Catalog cat;
    cat.raw = c;
    auto& v = cat.vocab;
    for (auto& [k, val] : c["enums"].items()) {
        for (auto& x : val) {
            if (x.is_string()) cat.enums[k].insert(x.get<string>());
        }
    }
    for (auto& l : c["timeframes"]["labels"]) cat.tfLabels.insert(l.get<string>());
    for (auto& r : c["timeframes"]["roles"]) cat.roles.insert(r.get<string>());
    v.scopes = cat.tfLabels;
    v.scopes.insert(cat.roles.begin(), cat.roles.end());
    for (auto& [path, s] : c["signals"].items()) {
        cat.signals[path] = s;
        v.paths.insert(path);
        if (s.contains("expression_alias")) v.bareNames.insert(s["expression_alias"].get<string>());
        if (path.find('.') == string::npos) v.bareNames.insert(path);
    }
    for (auto& [name, col] : c["collections"].items()) {
        v.collections.insert(name);
        for (auto& f : col["fields"]) v.collectionFields[name].insert(f.get<string>());
    }
    for (auto& [name, view] : c["views"].items()) {
        v.collections.insert(name);
        for (auto& f : view["fields"]) v.collectionFields[name].insert(f.get<string>());
    }
    for (auto& p : c["price_references"]) cat.priceRefs.insert(p.get<string>());
    for (auto& [k, _] : c["detections"].items()) cat.detections.insert(k);
    for (const char* e : {"structural_event_kind", "bias", "kill_zone", "order_block_subtype", "fvg_state"}) {
        v.enumWords.insert(cat.enums[e].begin(), cat.enums[e].end());
    }
    v.functions = {{"abs", {1, 1}}, {"min", {2, -1}}, {"max", {2, -1}}, {"avg", {2, 2}}, {"highest", {2, 2}},
                   {"lowest", {2, 2}}, {"count", {2, 2}}, {"any", {2, 2}}, {"all", {2, 2}}};
    return cat;
}

void extract(const json& doc, const string& file, Corpus& corpus, Diagnostics& d) {
    if (!doc.is_object()) {
        d.error(file, "envelope must be an object {strategies, examples}");
        return;
    }
    for (auto& [k, _] : doc.items()) {
        if (k != "strategies" && k != "examples") d.error(file, "unexpected top-level key '" + k + "'");
    }
    if (!doc.contains("strategies") || !doc["strategies"].is_array()) d.error(file, "missing array 'strategies'");
    if (!doc.contains("examples") || !doc["examples"].is_array()) d.error(file, "missing array 'examples'");
    if (doc.contains("strategies") && doc["strategies"].is_array()) {
        for (auto& s : doc["strategies"]) {
            if (!s.is_object() || !s.contains("strategy_id") || !s["strategy_id"].is_string()) {
                d.error(file, "strategy without string strategy_id");
                continue;
            }
            StrategyInfo info{file, s["strategy_id"].get<string>(), s, {}, {}};
            if (corpus.strategyFile.count(info.id)) d.error(file, "duplicate strategy_id " + info.id);
            corpus.strategyFile[info.id] = file;
            corpus.strategies.push_back(info);
        }
    }
    if (doc.contains("examples") && doc["examples"].is_array()) {
        for (auto& e : doc["examples"]) {
            if (!e.is_object() || !e.contains("example_id") || !e["example_id"].is_string()) {
                d.error(file, "example without string example_id");
                continue;
            }
            const string id = e["example_id"].get<string>();
            if (corpus.exampleOwner.count(id)) d.error(file, "duplicate example_id " + id);
            corpus.exampleOwner[id] = e.value("strategy_id", json()).is_string() ? e["strategy_id"].get<string>() : "";
            corpus.examples.push_back({file, e});
        }
    }
}

string joinSet(const set<string>& s) {
    string out;
    for (const auto& x : s) out += (out.empty() ? "" : ", ") + x;
    return out.empty() ? "-" : out;
}

void writeReport(const string& path, const Corpus& corpus, const Catalog& cat, const Registry& reg,
                 const Diagnostics& d) {
    ofstream out(path);
    map<char, int> counts;
    ostringstream rows;
    for (const auto& s : corpus.strategies) {
        const TierInfo t = computeTier(s, cat, reg);
        counts[t.tier]++;
        rows << "| " << s.id << " | " << s.file << " | " << t.tier << " | " << joinSet(t.timeframes) << " | "
             << joinSet(t.engineSignals) << " | " << joinSet(t.customSignals) << " | " << t.nonComputableSteps << " |\n";
    }
    out << "# strategy_lint report\n\n";
    out << "Errors: " << d.errors.size() << "  Warnings: " << d.warnings.size() << "\n\n";
    out << "Tier counts: A=" << counts['A'] << " B=" << counts['B'] << " C=" << counts['C'] << "\n\n";
    out << "| strategy | file | tier | timeframes | engine signals | custom signals | non-computable steps |\n";
    out << "|---|---|---|---|---|---|---|\n" << rows.str() << "\n";
    out << "## Errors\n\n";
    for (const auto& e : d.errors) out << "- " << e << "\n";
    out << "\n## Warnings\n\n";
    for (const auto& w : d.warnings) out << "- " << w << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        cerr << "usage: strategy_lint <strategies_dir> [--catalog <dir>] [--report <out.md>]\n";
        return 2;
    }
    fs::path dir = argv[1];
    fs::path catalogDir = dir / "catalog";
    string reportPath;
    for (int i = 2; i < argc; ++i) {
        const string a = argv[i];
        if (a == "--catalog" && i + 1 < argc) catalogDir = argv[++i];
        else if (a == "--report" && i + 1 < argc) reportPath = argv[++i];
        else {
            cerr << "unknown argument " << a << "\n";
            return 2;
        }
    }

    Diagnostics d;
    const auto catJson = readJson(catalogDir / "signal_catalog.json", d);
    const auto regJson = readJson(catalogDir / "custom_signals.json", d);
    const auto constJson = readJson(catalogDir / "constants.json", d);
    if (!catJson || !regJson || !constJson) {
        for (const auto& e : d.errors) cerr << "ERROR " << e << "\n";
        return 1;
    }
    Catalog cat = loadCatalog(*catJson);
    Registry reg;
    for (auto& c : (*constJson)["constants"]) {
        reg.constants[c["name"].get<string>()] = c;
        cat.vocab.constants.insert(c["name"].get<string>());
    }
    for (auto& s : (*regJson)["signals"]) reg.entries[s["name"].get<string>()] = s;

    Corpus corpus;
    vector<fs::path> files;
    for (auto& entry : fs::directory_iterator(dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") files.push_back(entry.path());
    }
    sort(files.begin(), files.end());
    for (const auto& f : files) {
        const auto doc = readJson(f, d);
        if (!doc) continue;
        checkGlobalStrings(*doc, f.filename().string(), d);
        extract(*doc, f.filename().string(), corpus, d);
    }

    checkRegistry(cat, reg, corpus, d);
    for (auto& s : corpus.strategies) {
        for (auto& cs : s.obj.value("custom_signals", json::array())) {
            if (cs.contains("name") && cs["name"].is_string()) s.customNames.insert(cs["name"].get<string>());
        }
        for (auto& st : s.obj.value("signals_required", json::array())) {
            if (st.contains("step") && st["step"].is_number_integer()) s.steps.insert(st["step"].get<int>());
        }
    }
    for (const auto& s : corpus.strategies) checkStrategy(s, cat, reg, corpus, d);
    for (const auto& [file, e] : corpus.examples) checkExample(file, e, cat, reg, corpus, d);
    for (const auto& [name, _] : reg.constants) {
        if (!corpus.usedConstants.count(name)) d.warn("constants.json", "unused constant " + name);
    }

    for (const auto& w : d.warnings) cout << "WARN  " << w << "\n";
    for (const auto& e : d.errors) cout << "ERROR " << e << "\n";
    cout << "strategies=" << corpus.strategies.size() << " examples=" << corpus.examples.size()
         << " errors=" << d.errors.size() << " warnings=" << d.warnings.size() << "\n";
    if (!reportPath.empty()) writeReport(reportPath, corpus, cat, reg, d);
    return d.errors.empty() ? 0 : 1;
}
