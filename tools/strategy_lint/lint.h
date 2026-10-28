#pragma once

#include "expr.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std;
using json = nlohmann::json;

struct Catalog {
    json raw;
    Vocabulary vocab;
    map<string, json> signals;
    set<string> priceRefs;
    set<string> detections;
    set<string> tfLabels;
    set<string> roles;
    map<string, set<string>> enums;
};

struct Registry {
    map<string, json> entries;
    map<string, json> constants;
};

struct StrategyInfo {
    string file;
    string id;
    json obj;
    set<string> customNames;
    set<int> steps;
};

struct Diagnostics {
    vector<string> errors;
    vector<string> warnings;
    void error(const string& where, const string& msg) { errors.push_back(where + ": " + msg); }
    void warn(const string& where, const string& msg) { warnings.push_back(where + ": " + msg); }
};

struct Corpus {
    vector<StrategyInfo> strategies;
    vector<pair<string, json>> examples;
    map<string, string> strategyFile;
    map<string, string> exampleOwner;
    set<string> usedConstants;
};

struct TierInfo {
    char tier = 'A';
    set<string> timeframes;
    set<string> engineSignals;
    set<string> customSignals;
    int nonComputableSteps = 0;
};

bool isSnake(const string& s);
bool isTs(const json& v);
bool isTfField(const json& v, const Catalog& cat, bool allowRole);
string enumList(const set<string>& s);

void checkGlobalStrings(const json& node, const string& where, Diagnostics& d);
void checkStrategy(const StrategyInfo& s, const Catalog& cat, const Registry& reg, Corpus& corpus, Diagnostics& d);
void checkExample(const string& file, const json& e, const Catalog& cat, const Registry& reg, Corpus& corpus,
                  Diagnostics& d);
TierInfo computeTier(const StrategyInfo& s, const Catalog& cat, const Registry& reg);
void checkRegistry(const Catalog& cat, Registry& reg, Corpus& corpus, Diagnostics& d);
