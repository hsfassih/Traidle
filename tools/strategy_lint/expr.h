#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

using namespace std;

struct Vocabulary {
    set<string> paths;
    set<string> collections;
    map<string, set<string>> collectionFields;
    set<string> bareNames;
    set<string> enumWords;
    set<string> scopes;
    map<string, pair<int, int>> functions;
    set<string> constants;
};

struct ExprResult {
    vector<string> errors;
    set<string> customRefs;
    set<string> constantRefs;
    set<string> catalogRefs;
    set<string> scopeLabels;
    bool external = false;
};

ExprResult checkExpression(const string& text, const Vocabulary& vocab, const set<string>& customNames);
