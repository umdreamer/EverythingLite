#include "core/update_policy.h"
#include "core/path_utils.h"
#include <algorithm>
#include <unordered_set>
#include <utility>
namespace everything_lite {
namespace {
bool within(const std::string& path,const std::string& root) {
    return path==root || (root=="/" ? !path.empty() && path.front()=='/' :
        path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]=='/');
}
void validate(const std::string& path) {
#if defined(_WIN32)
    if(path.empty() || path.find('\0')!=std::string::npos || !utf8ToPath(path).is_absolute() || normalizePath(path)!=path)
        throw std::invalid_argument("update policy path must be absolute and normalized");
#else
    if(path.empty() || path.front()!='/' || path.find('\0')!=std::string::npos || (path.size()>1 && path.back()=='/'))
        throw std::invalid_argument("update policy path must be absolute and normalized");
    for(std::size_t begin=1;begin<path.size();) {
        const auto end=path.find('/',begin);const auto length=(end==std::string::npos?path.size():end)-begin;
        if(length==0 || (length==1 && path[begin]=='.') || (length==2 && path.compare(begin,2,"..")==0))
            throw std::invalid_argument("update policy path must be absolute and normalized");
        if(end==std::string::npos)break;begin=end+1;
    }
#endif
}
}
UpdatePolicy::UpdatePolicy(std::string home,std::vector<UpdateRule> overrides,std::vector<std::string> own_directories,bool include_periodic)
    :include_periodic_(include_periodic),home_(std::move(home)),overrides_(std::move(overrides)),own_directories_(std::move(own_directories)) {
    validate(home_);library_=normalizePath(home_+"/Library");
    std::unordered_set<std::string> seen;
    for(const auto& rule:overrides_) {
        validate(rule.path);
        if(rule.cadence==UpdateCadence::Ignore || !seen.insert(rule.path).second)
            throw std::invalid_argument("invalid or duplicate update override");
    }
    for(const auto& path:own_directories_)validate(path);
    std::stable_sort(overrides_.begin(),overrides_.end(),[](const auto& a,const auto& b){return a.path.size()>b.path.size();});
}
bool UpdatePolicy::allows(const std::string& path,bool finder_hidden) const {
    const auto cadence=classify(path,finder_hidden);
    return cadence==UpdateCadence::Realtime || (include_periodic_ && cadence==UpdateCadence::Periodic);
}
bool UpdatePolicy::shouldDescend(const std::string& path,bool finder_hidden) const {
    if(allows(path,finder_hidden))return true;
    if(classify(path,finder_hidden)==UpdateCadence::Ignore)return false;
    // A periodic ancestor may contain an explicitly realtime descendant.
    return std::any_of(overrides_.begin(),overrides_.end(),[&](const auto& rule) {
        return rule.cadence==UpdateCadence::Realtime && within(rule.path,path);
    });
}
UpdateCadence UpdatePolicy::classify(const std::string& path,bool finder_hidden) const {
    validate(path);
    for(const auto& own:own_directories_)if(within(path,own))return UpdateCadence::Ignore;
    for(const auto& rule:overrides_)if(within(path,rule.path))return rule.cadence;
    if(finder_hidden || within(path,library_))return UpdateCadence::Periodic;
    for(std::size_t begin=0;begin<path.size();) {
        const auto end=path.find('/',begin);const auto length=(end==std::string::npos?path.size():end)-begin;
        if(length>1 && path[begin]=='.')return UpdateCadence::Periodic;
        if(end==std::string::npos)break;begin=end+1;
    }
    return UpdateCadence::Realtime;
}
}
