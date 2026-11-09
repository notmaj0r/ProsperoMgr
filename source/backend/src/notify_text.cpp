#include "notify_text.hpp"

#include <cstddef>
#include <iterator>

#include "ps5_services.hpp"

namespace prospero {
namespace {

enum Lang { kEn, kPt, kEs, kFr, kDe, kIt, kPl, kRu, kTr, kJa, kKo, kZh, kAr, kHu, kTh, kUk, kLangCount };

const char *const kCatalog[kLangCount][static_cast<size_t>(Msg::kCount)] = {
#include "notify_catalog.inc"
};

// System language ids (sceSystemServiceParamGetInt, id 1) to the closest language in the catalog.
Lang LangForSystem(int32_t id) {
    static constexpr Lang kBySystemId[] = {
        kJa, kEn, kFr, kEs, kDe, kIt, kEn, kPt, kRu, kKo,  // 0-9: ja, en-US, fr, es, de, it, nl, pt-PT, ru, ko
        kZh, kZh, kEn, kEn, kEn, kEn, kPl, kPt, kEn, kTr,  // 10-19: zh-TW, zh-CN, fi, sv, da, no, pl, pt-BR, en-GB, tr
        kEs, kAr, kFr, kEn, kHu, kEn, kEn, kTh, kEn, kEn,  // 20-29: es-MX, ar, fr-CA, cs, hu, el, ro, th, vi, id
        kUk,                                               // 30: uk
    };
    if (id < 0 || static_cast<size_t>(id) >= std::size(kBySystemId)) return kEn;
    return kBySystemId[id];
}

} // namespace

std::string NotifyText(Msg msg, TextArgs args) {
    int32_t system_language = 1;
    if (sceSystemServiceParamGetInt(1, &system_language) != 0) system_language = 1;
    std::string text = kCatalog[LangForSystem(system_language)][static_cast<size_t>(msg)];
    for (const auto &[name, value] : args) {
        std::string token = std::string("{") + name + "}";
        for (size_t at = text.find(token); at != std::string::npos; at = text.find(token, at + value.size())) {
            text.replace(at, token.size(), value);
        }
    }
    return text;
}

} // namespace prospero
