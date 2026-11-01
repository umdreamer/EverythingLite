#include <cassert>
#include <filesystem>
#include <iostream>
#include "core/deferred_updates.h"
using namespace everything_lite;
int main() {
    namespace fs=std::filesystem;
    const auto dir=fs::temp_directory_path()/"everything-lite-deferred-test";
    fs::remove_all(dir);fs::create_directories(dir);
    const auto path=(dir/"queue.sqlite").string();
    {
        DeferredUpdates first(path),second(path);
        bool rejected=false;
        try {first.enqueue("/synthetic/root","/synthetic/root-other/item",1000);} catch(const std::invalid_argument&) {rejected=true;}
        assert(rejected);
        first.enqueue("/synthetic/root","/synthetic/root/.cache/item",1000);
        assert(first.due(999).empty());
        const auto initial=first.due(1000);assert(initial.size()==1);
        second.enqueue("/synthetic/root","/synthetic/root/.cache/item",2000);
        assert(!first.acknowledge(initial.front()));
        const auto changed=first.due(1000);assert(changed.size()==1 && changed.front().generation>initial.front().generation);
    }
#if !defined(_WIN32)
    fs::create_symlink(path,dir/"linked.sqlite");
    bool link_rejected=false;
    try {DeferredUpdates linked((dir/"linked.sqlite").string());} catch(const std::exception&) {link_rejected=true;}
    assert(link_rejected);
#endif
    {
        DeferredUpdates reopened(path);
        const auto pending=reopened.due(1000);assert(pending.size()==1);
        assert(reopened.acknowledge(pending.front()));assert(reopened.due(9999).empty());
    }
    fs::remove_all(dir);std::cout<<"deferred updates passed\n";
}
