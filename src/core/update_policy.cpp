#include "core/update_policy.h"
#include "core/path_utils.h"
#include <algorithm>
#include <unordered_set>
#include <utility>
namespace everything_lite {
namespace {
void validate(const std::string& path) {
    if(path.empty() || path.find('\0')!=std::string::npos || !utf8ToPath(path).is_absolute() || normalizePath(path)!=path)
        throw std::invalid_argument("update policy path must be absolute and normalized");
}
}
UpdatePolicy::UpdatePolicy(std::string home,std::vector<UpdateRule> overrides,std::vector<std::string> own_directories)
    :home_(std::move(home)),overrides_(std::move(overrides)),own_directories_(std::move(own_directories)) {
    validate(home_);std::unordered_set<std::string> seen;
    for(const auto& rule:overrides_) {
        validate(rule.path);
        if(rule.cadence==UpdateCadence::Ignore || !seen.insert(rule.path).second)
            throw std::invalid_argument("invalid or duplicate update override");
    }
    for(const auto& path:own_directories_)validate(path);
    std::stable_sort(overrides_.begin(),overrides_.end(),[](const auto& a,const auto& b){return a.path.size()>b.path.size();});
}
UpdateCadence UpdatePolicy::classify(const std::string& path,bool finder_hidden) const {
    validate(path);
    for(const auto& own:own_directories_)if(pathIsWithin(path,own))return UpdateCadence::Ignore;
    for(const auto& rule:overrides_)if(pathIsWithin(path,rule.path))return rule.cadence;
    if(finder_hidden || pathIsWithin(path,normalizePath(home_+"/Library")))return UpdateCadence::Periodic;
    for(const auto& part:utf8ToPath(path)) {
        const auto name=pathToUtf8(part);
        if(name.size()>1 && name.front()=='.')return UpdateCadence::Periodic;
    }
    return UpdateCadence::Realtime;
}
}
