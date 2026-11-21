#include "Json.h"
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    Json::Value v; Json::Error e;
    if (Json::parse((const char*)d, n, &v, &e)) { std::string s = Json::write(v); Json::Value w; Json::parse(s.data(), s.size(), &w, &e); }
    return 0;
}
