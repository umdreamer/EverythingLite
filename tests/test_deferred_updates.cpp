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
        second.enqueue("/synthetic/root","/synthetic/root/.cache",1500);
        const auto merged=first.due(1000);assert(merged.size()==1 && merged.front().path=="/synthetic/root/.cache");
        assert(!first.acknowledge(changed.front()));
        first.postpone(changed.front().root,changed.front().path,2000);assert(first.due(1999).empty());
    }
#if !defined(_WIN32)
    fs::create_symlink(path,dir/"linked.sqlite");
    bool link_rejected=false;
    try {DeferredUpdates linked((dir/"linked.sqlite").string());} catch(const std::exception&) {link_rejected=true;}
    assert(link_rejected);
#endif
    {
        DeferredUpdates reopened(path);
        const auto pending=reopened.due(2000);assert(pending.size()==1);
        assert(reopened.acknowledge(pending.front()));assert(reopened.due(9999).empty());
        reopened.enqueue(pending.front().root,pending.front().path,3000);
        assert(!reopened.acknowledge(pending.front()));
        const auto again=reopened.due(3000);assert(again.size()==1 && again.front().generation>pending.front().generation);
        assert(reopened.acknowledge(again.front()));
        for(int i=0;i<401;++i)reopened.enqueue("/synthetic/root","/synthetic/root/item-"+std::to_string(i),4000);
        const auto overflow=reopened.due(4000);assert(overflow.size()==1 && overflow.front().path=="/synthetic/root");
        assert(reopened.acknowledge(overflow.front()));
    }
    fs::remove_all(dir);std::cout<<"deferred updates passed\n";
}
