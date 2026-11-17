// JSON type used for every file on the card (nlohmann/json, keys kept in insertion order),
// plus readers that tolerate missing keys and wrong types in hand-edited files.
#pragma once

#include <QString>

#include <nlohmann/json.hpp>

using Json = nlohmann::ordered_json;

inline const Json* jsonFind(const Json& o, const char* key) {
    if (!o.is_object()) return nullptr;
    const auto it = o.find(key);
    return it == o.end() ? nullptr : &*it;
}
inline QString jsonString(const Json& o, const char* key) {
    const Json* v = jsonFind(o, key);
    return v && v->is_string() ? QString::fromStdString(v->get<std::string>()) : QString();
}
inline int jsonInt(const Json& o, const char* key, int fallback = 0) {
    const Json* v = jsonFind(o, key);
    return v && v->is_number() ? v->get<int>() : fallback;
}
inline bool jsonBool(const Json& o, const char* key) {
    const Json* v = jsonFind(o, key);
    return v && v->is_boolean() && v->get<bool>();
}
inline Json jsonArray(const Json& o, const char* key) {
    const Json* v = jsonFind(o, key);
    return v && v->is_array() ? *v : Json::array();
}
