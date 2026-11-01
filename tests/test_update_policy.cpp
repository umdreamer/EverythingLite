#include <cassert>
#include <iostream>
#include "core/update_policy.h"
using namespace everything_lite;
int main() {
    UpdatePolicy policy("/synthetic/home",{{"/synthetic/home/.cache/important",UpdateCadence::Realtime},
        {"/synthetic/home/Documents/generated",UpdateCadence::Periodic}},
        {"/synthetic/home/project/build-output"});
    assert(policy.classify("/synthetic/home/Documents/note.txt")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/.cache/tool/file")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/.cache/important/file")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/Library/data/file")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/LibraryOther/file")==UpdateCadence::Realtime);
    assert(policy.classify("/synthetic/home/Documents/generated/x")==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/Documents/item",true)==UpdateCadence::Periodic);
    assert(policy.classify("/synthetic/home/project/build-output/x")==UpdateCadence::Ignore);
    assert(policy.classify("/synthetic/home/project/build-output-old/x")==UpdateCadence::Realtime);
    bool rejected=false;try {UpdatePolicy invalid("/synthetic/home",{{"relative",UpdateCadence::Realtime}},{});} catch(const std::invalid_argument&) {rejected=true;}
    assert(rejected);std::cout<<"update policy passed\n";
}
