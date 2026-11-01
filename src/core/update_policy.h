#pragma once
#include <stdexcept>
#include <string>
#include <vector>
namespace everything_lite {
enum class UpdateCadence { Realtime, Periodic, Ignore };
struct UpdateRule {std::string path;UpdateCadence cadence;};
class UpdatePolicy {
public:
    UpdatePolicy(std::string home,std::vector<UpdateRule> overrides,std::vector<std::string> own_directories);
    UpdateCadence classify(const std::string& path,bool finder_hidden=false) const;
private:
    std::string home_;
    std::vector<UpdateRule> overrides_;
    std::vector<std::string> own_directories_;
};
}
