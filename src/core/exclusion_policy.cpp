#include "core/exclusion_policy.h"
#include "core/path_utils.h"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace everything_lite {
namespace {
bool asciiSpace(char c) { return c==' ' || (c>='\t' && c<='\r'); }
std::string trim(std::string s) {
    const auto begin=s.find_first_not_of(" \t\r\n\v\f");
    return begin==std::string::npos ? std::string{} : s.substr(begin,s.find_last_not_of(" \t\r\n\v\f")-begin+1);
}
// Validate Unicode scalar encodings; wildcard ? must never eat a continuation
// byte alone, nor silently accept overlong/surrogate/out-of-range encodings.
std::vector<std::string> units(const std::string& text,bool allow_opaque=false,
                               std::vector<unsigned char>* validity=nullptr) {
    std::vector<std::string> out;
    for(std::size_t i=0;i<text.size();) {
        const auto byte=static_cast<unsigned char>(text[i]);
        std::size_t n=1; unsigned value=byte; bool valid=byte>0 && byte<0x80;
        if(byte>=0xc2 && byte<=0xdf) { n=2; value=byte&0x1f; valid=true; }
        else if(byte>=0xe0 && byte<=0xef) { n=3; value=byte&0x0f; valid=true; }
        else if(byte>=0xf0 && byte<=0xf4) { n=4; value=byte&0x07; valid=true; }
        if(i+n>text.size()) valid=false;
        for(std::size_t j=1;valid && j<n;++j) {
            const auto continuation=static_cast<unsigned char>(text[i+j]);
            if((continuation&0xc0)!=0x80) valid=false;
            else value=(value<<6)|(continuation&0x3f);
        }
        if((n==2 && value<0x80) || (n==3 && value<0x800) || (n==4 && value<0x10000) ||
           (value>=0xd800 && value<=0xdfff) || value>0x10ffff) valid=false;
        if(!valid) {
            if(!allow_opaque) throw std::invalid_argument("exclusion rules require valid UTF-8 without NUL");
            n=1; // Filesystems can expose opaque bytes. * can consume them, ? cannot.
        }
        if(validity) validity->push_back(valid ? 1 : 0);
        out.push_back(asciiFold(text.substr(i,n))); i+=n;
    }
    return out;
}
void rejectPrefix(const std::string& term) {
    const auto colon=term.find(':');
    if(colon==std::string::npos) return;
    // Preserve native Windows drive paths; reject unsupported query/attribute
    // operators rather than interpreting them as innocent filename literals.
    if(colon==1 && term.size()>2 && (term[2]=='/' || term[2]=='\\') &&
       ((term[0]>='A' && term[0]<='Z') || (term[0]>='a' && term[0]<='z'))) return;
    if(colon==0) return;
    for(std::size_t i=0;i<colon;++i) {
        const auto c=term[i];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_' || c=='-')) return;
    }
    throw std::invalid_argument("unsupported exclusion prefix: only exact paths and glob filters (*, **, ?) are supported");
}
bool withinFolded(const std::string& path,const std::string& root) {
    return path==root || (root=="/" && !path.empty() && path.front()=='/') ||
        (path.size()>root.size() && path.compare(0,root.size(),root)==0 && path[root.size()]=='/');
}
}

std::vector<ExclusionPolicy::Pattern> ExclusionPolicy::parseFilters(const std::string& text) {
    std::vector<Pattern> out;
    std::size_t i=0;
    while(i<text.size()) {
        while(i<text.size() && (asciiSpace(text[i]) || text[i]==';')) ++i;
        if(i==text.size()) break;
        std::string term;
        if(text[i]=='"') {
            ++i; bool closed=false;
            while(i<text.size()) {
                if(text[i]=='"') {
                    if(i+1<text.size() && text[i+1]=='"') { term+='"'; i+=2; }
                    else { ++i; closed=true; break; }
                } else term+=text[i++];
            }
            if(!closed) throw std::invalid_argument("unclosed quote in exclusion filter");
            while(i<text.size() && asciiSpace(text[i])) ++i;
            if(i<text.size() && text[i]!=';') throw std::invalid_argument("unexpected text after quoted exclusion filter");
        } else {
            while(i<text.size() && text[i]!=';') {
                if(text[i]=='"') throw std::invalid_argument("quotes must wrap an entire exclusion filter");
                term+=text[i++];
            }
            term=trim(std::move(term));
        }
        if(i<text.size()) ++i;
        if(term.empty()) continue;
        rejectPrefix(term);
        std::replace(term.begin(),term.end(),'\\','/');
        auto parsed=units(term);
        // Collapse runs of two or more stars to the cross-directory token.
        std::vector<std::string> compiled;
        for(std::size_t j=0;j<parsed.size();++j) {
            if(parsed[j]=="*" && j+1<parsed.size() && parsed[j+1]=="*") {
                compiled.push_back("**"); while(j+1<parsed.size() && parsed[j+1]=="*") ++j;
            } else compiled.push_back(parsed[j]);
        }
        out.push_back({std::move(compiled),term.find('/')!=std::string::npos});
    }
    return out;
}
ExclusionPolicy::ExclusionPolicy(ExclusionRules rules,std::int64_t captured_revision)
    : revision(captured_revision), rules_(std::move(rules)),
      folder_patterns_(parseFilters(rules_.folder_filter)), file_patterns_(parseFilters(rules_.file_filter)) {
    for(auto* paths: {&rules_.directories,&rules_.files}) {
        for(auto& path:*paths) {
            rejectPrefix(path); units(path);
            if(path.empty() || !utf8ToPath(path).is_absolute()) throw std::invalid_argument("exact exclusions require absolute paths");
            path=normalizePath(path);
        }
    }
}
bool ExclusionPolicy::matches(const Pattern& pattern,const std::string& path) {
    const auto candidate=pattern.full_path ? path : path.substr(path.find_last_of('/')+1);
    std::vector<unsigned char> valid;
    const auto text=units(candidate,true,&valid);
    std::vector<unsigned char> previous(text.size()+1,0),current(text.size()+1,0); previous[0]=1;
    for(const auto& token:pattern.units) {
        std::fill(current.begin(),current.end(),0);
        const bool star=token=="*" || token=="**";
        if(star) current[0]=previous[0];
        for(std::size_t j=1;j<=text.size();++j) {
            if(star) current[j]=previous[j] || (current[j-1] && (token=="**" || text[j-1]!="/"));
            else current[j]=previous[j-1] && ((token=="?" && valid[j-1] && text[j-1]!="/") || token==text[j-1]);
        }
        previous.swap(current);
    }
    return previous.back()!=0;
}
bool ExclusionPolicy::excludes(const std::string& path,bool is_directory) const {
    const auto normalized=normalizePath(path), folded=asciiFold(normalized);
    for(const auto& directory:rules_.directories) if(withinFolded(folded,asciiFold(directory))) return true;
    if(!is_directory) {
        for(const auto& file:rules_.files) if(folded==asciiFold(file)) return true;
        for(const auto& pattern:file_patterns_) if(matches(pattern,normalized)) return true;
    }
    auto directory=is_directory ? normalized : normalizePath(pathToUtf8(utf8ToPath(normalized).parent_path()));
    while(!directory.empty()) {
        for(const auto& pattern:folder_patterns_) if(matches(pattern,directory)) return true;
        const auto parent=normalizePath(pathToUtf8(utf8ToPath(directory).parent_path()));
        if(parent==directory) break;
        directory=parent;
    }
    return false;
}
} // namespace everything_lite
