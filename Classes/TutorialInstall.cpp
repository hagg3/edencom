// UX.10-2: see TutorialInstall.h.
#include "TutorialInstall.h"
#include "FileManager.h"
#include "Json.h"
#include "port/src/shim/foundation/eden_fs.h"
#include "port/src/shim/foundation/eden_prefs.h"
#include <cstdio>
#include <cstring>

namespace TutorialInstall {

bool autoInstall = true;
int  testMutation = 0;

const char* prefsKey() { return "tutorial.installed"; }
const char* markerFile() { return "tutorial.json"; }

static bool readAll(const std::string& path, std::string* out, size_t cap) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out->resize(cap + 1);
    const size_t n = fread(&(*out)[0], 1, out->size(), f);
    fclose(f);
    out->resize(n);
    return n > 0 && n <= cap;
}

bool readMarker(const std::string& dir, Marker* out) {
    std::string text;
    if (!readAll(dir + "/" + markerFile(), &text, 64 * 1024)) return false;
    Json::Value v;
    Json::Error err;
    if (!Json::parse(text.data(), text.size(), &v, &err) || !v.isObject()) return false;
    int64_t ver = 0;
    const Json::Value* jv = v.get("v");
    if (!jv || !jv->asInt(&ver) || ver != 1) return false;
    Marker m;
    if (const Json::Value* d = v.get("done")) {
        if (!d->isArray()) return false;
        for (size_t i = 0; i < d->arr.size() && i < 256; i++) {
            if (!d->arr[i].isString()) return false;
            m.done.push_back(d->arr[i].str);
        }
    }
    if (const Json::Value* h = v.get("hidden")) {
        if (!h->isBool()) return false;
        m.hidden = h->b;
    }
    if (out) *out = m;
    return true;
}

bool writeMarker(const std::string& dir, const Marker& m) {
    Json::Value v;
    v.type = Json::Object;
    Json::Value one;
    one.type = Json::Number;
    one.num = 1;
    v.obj.push_back(Json::Member("v", one));
    Json::Value done;
    done.type = Json::Array;
    for (const std::string& s : m.done) {
        Json::Value e;
        e.type = Json::String;
        e.str = s;
        done.arr.push_back(e);
    }
    v.obj.push_back(Json::Member("done", done));
    if (m.hidden) {
        Json::Value h;
        h.type = Json::Bool;
        h.b = true;
        v.obj.push_back(Json::Member("hidden", h));
    }
    const std::string text = Json::write(v);
    return eden_fs_write_atomic(dir + "/" + markerFile(), text.data(), text.size());
}

bool isTutorialWorld(const std::string& documents, const std::string& key) {
    const std::string rel = FileManager::worldDirOf(key);
    return !rel.empty() && readMarker(documents + "/" + rel, NULL);
}

std::vector<std::string> markedWorlds(const std::string& documents) {
    std::vector<std::string> out;
    const std::string wroot = documents + "/Worlds";
    const std::vector<std::string> ids = eden_fs_list(wroot);
    for (size_t i = 0; i < ids.size(); i++) {
        if (!FileManager::validWorldId(ids[i])) continue;
        const std::string key = FileManager::worldKeyOfId(ids[i]);
        if (eden_fs_exists(documents + "/" + key) && isTutorialWorld(documents, key)) out.push_back(key);
    }
    return out;
}

std::string install(const std::string& documents) {
    const std::string src = eden_bundle_path("Tutorial.emod");
    std::string bytes;
    if (src.empty() || !readAll(src, &bytes, 8u << 20)) {
        fprintf(stderr, "tutorial: the bundled Tutorial.emod is missing or unreadable\n");
        return std::string();
    }
    const std::string id = FileManager::newWorldId(documents);
    const std::string stage = FileManager::beginStaging(documents, id);
    if (stage.empty()) {
        fprintf(stderr, "tutorial: cannot make the staging folder\n");
        return std::string();
    }
    if (!eden_fs_write_atomic(stage + "/world.emod", bytes.data(), bytes.size())) {
        fprintf(stderr, "tutorial: cannot write the world file\n");
        FileManager::discardStaging(documents, id);
        return std::string();
    }
    FileManager::stageStep("tutorial world written");
    if (testMutation != 1 && !writeMarker(stage, Marker())) {
        fprintf(stderr, "tutorial: cannot write the marker\n");
        FileManager::discardStaging(documents, id);
        return std::string();
    }
    FileManager::stageStep("tutorial marker written");
    eden_fs_sync_dir(stage);
    const std::string key = FileManager::commitStaging(documents, id);   // the commit point
    if (key.empty()) {
        fprintf(stderr, "tutorial: cannot move the world into place\n");
        FileManager::discardStaging(documents, id);
        return std::string();
    }
    return key;
}

std::string restore(const std::string& documents) {
    const std::vector<std::string> old = markedWorlds(documents);
    const std::string key = install(documents);
    if (key.empty()) return key;
    for (size_t i = 0; i < old.size(); i++) {
        const std::string rel = FileManager::worldDirOf(old[i]);
        if (rel.empty()) continue;
        eden_fs_remove_tree(documents + "/" + rel);   // FileManager::removeWorldDir's rule: Worlds/<valid id> only
    }
    return key;
}

bool installOnce(const std::string& documents) {
    if (!autoInstall || eden_prefs_get_int(prefsKey(), 0) != 0) return false;
    if (FileManager::worldJobBusy()) return false;   // a conversion/download is live: leave the key unset, try again
    if (testMutation == 2) eden_prefs_set_int(prefsKey(), 1);
    if (!markedWorlds(documents).empty()) {   // a kill between the commit rename and the pref: the world is there, only the key is owed
        eden_prefs_set_int(prefsKey(), 1);
        return false;
    }
    const std::string key = install(documents);
    if (key.empty()) {
        fprintf(stderr, "tutorial: install failed; it is retried at the next launch\n");
        return false;
    }
    eden_prefs_set_int(prefsKey(), 1);
    printf("tutorial: installed %s\n", key.c_str());
    return true;
}

}  // namespace TutorialInstall
