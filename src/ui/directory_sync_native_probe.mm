#include "ui/directory_sync_native_probe.h"
#include "ui/directory_sync_permissions.h"

#import <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <limits>
#include <memory>

namespace everything_lite {
namespace {
using Ref = std::unique_ptr<const void, decltype(&CFRelease)>;
Ref owned(CFTypeRef value) { return Ref(value, CFRelease); }
constexpr int nodeLimit = 128;
constexpr int depthLimit = 8;
constexpr int outputLimit = 180000;

QString safeName(CFTypeRef value, int limit = 100) {
    if (!value || CFGetTypeID(value) != CFStringGetTypeID()) return QStringLiteral("unavailable");
    const auto string = static_cast<CFStringRef>(value);
    const CFIndex count = CFStringGetLength(string);
    if (count < 1 || count > limit) return QStringLiteral("redacted");
    QString result;
    for (CFIndex i = 0; i < count; ++i) {
        const UniChar c = CFStringGetCharacterAtIndex(string, i);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return QStringLiteral("redacted");
        result.append(QChar(c));
    }
    return result;
}
QString dataType(CFTypeRef value) {
    if (!value) return QStringLiteral("unavailable");
    const auto type = CFGetTypeID(value);
    if (type == CFStringGetTypeID()) return QStringLiteral("string");
    if (type == CFURLGetTypeID()) return QStringLiteral("url");
    if (type == CFNumberGetTypeID()) return QStringLiteral("number");
    if (type == CFBooleanGetTypeID()) return QStringLiteral("boolean");
    if (type == CFArrayGetTypeID()) return QStringLiteral("array");
    if (type == AXUIElementGetTypeID()) return QStringLiteral("element");
    return QStringLiteral("other");
}
struct Probe {
    QJsonObject output{{"protocol", 1}, {"accessibility", false},
        {"finder_permission_only", true}, {"finder_status", "error"}, {"finder_error", 0},
        {"focused_window_read", false}, {"native_status", "unsupported"}, {"ax_error", 0}, {"kind", "other"},
        {"directory_known", false}, {"truncated", false},
        {"fixture_directory_match", false}, {"fixture_reason", "none"}};
    QJsonArray nodes;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    CFStringRef expected = nullptr;
    CFStringRef expectedUrl = nullptr;
    bool failed = false;
    int discovered = 1;
    bool ready() {
        if (std::chrono::steady_clock::now() < deadline) return true;
        output["truncated"] = true;
        fail(kAXErrorCannotComplete);
        return false;
    }
    void fail(AXError error) {
        if (error == kAXErrorSuccess) return;
        // Retain the first actual failure, except an explicit OS permission error wins.
        if (failed && error != kAXErrorAPIDisabled) return;
        failed = true;
        output["ax_error"] = static_cast<int>(error);
        output["native_status"] = error == kAXErrorAPIDisabled ? "permission-denied" :
            (error == kAXErrorAttributeUnsupported || error == kAXErrorNotImplemented ||
             error == kAXErrorNoValue ? "unsupported" : "error");
    }
    bool prepare(AXUIElementRef element) {
        if (!ready()) return false;
        const float remaining = std::chrono::duration<float>(deadline - std::chrono::steady_clock::now()).count();
        const auto error = AXUIElementSetMessagingTimeout(element, std::min(0.2f, std::max(0.001f, remaining)));
        fail(error);
        return error == kAXErrorSuccess;
    }
    Ref copy(AXUIElementRef element, CFStringRef name) {
        CFTypeRef value = nullptr;
        if (prepare(element) && ready()) {
            const auto error = AXUIElementCopyAttributeValue(element, name, &value);
            fail(error);
            if (error != kAXErrorSuccess) {
                if (value) CFRelease(value);
                value = nullptr;
            }
        }
        return owned(value);
    }
    bool matches(CFTypeRef value) {
        if (!expected || !value) return false;
        if (CFGetTypeID(value) == CFStringGetTypeID())
            return CFEqual(value, expected) || (expectedUrl && CFEqual(value, expectedUrl));
        if (CFGetTypeID(value) == CFURLGetTypeID()) {
            const auto url = static_cast<CFURLRef>(value);
            auto scheme = owned(CFURLCopyScheme(url));
            auto host = owned(CFURLCopyHostName(url));
            if (!scheme || !CFEqual(scheme.get(), CFSTR("file")) ||
                (host && CFStringGetLength(static_cast<CFStringRef>(host.get())) != 0 &&
                 !CFEqual(host.get(), CFSTR("localhost")))) return false;
            auto path = owned(CFURLCopyFileSystemPath(url, kCFURLPOSIXPathStyle));
            return path && CFEqual(path.get(), expected);
        }
        return false;
    }
    void walk(AXUIElementRef element, int depth) {
        if (!ready()) return;
        if (nodes.size() >= nodeLimit) { output["truncated"] = true; return; }
        auto role = copy(element, kAXRoleAttribute);
        auto identifier = copy(element, kAXIdentifierAttribute);
        CFArrayRef namesRaw = nullptr;
        if (prepare(element) && ready()) fail(AXUIElementCopyAttributeNames(element, &namesRaw));
        auto names = owned(namesRaw);
        QJsonArray attributes;
        if (names && CFGetTypeID(names.get()) == CFArrayGetTypeID()) {
            const auto array = static_cast<CFArrayRef>(names.get());
            const CFIndex count = CFArrayGetCount(array);
            for (CFIndex i = 0; i < count && i < 32; ++i)
                attributes.append(safeName(CFArrayGetValueAtIndex(array, i), 64));
            if (count > 32) output["truncated"] = true;
        }
        auto url = copy(element, kAXURLAttribute);
        auto value = copy(element, kAXValueAttribute);
        const bool urlMatch = matches(url.get());
        const bool valueMatch = matches(value.get());
        if (urlMatch || valueMatch) output["fixture_directory_match"] = true;
        QJsonObject node{{"role", safeName(role.get())}, {"identifier", safeName(identifier.get())},
            {"attributes", attributes}, {"url_type", dataType(url.get())}, {"value_type", dataType(value.get())},
            {"fixture_url_match", urlMatch}, {"fixture_value_match", valueMatch}};
        // Bound the serialized protocol as well as node/attribute counts.
        auto candidate = nodes;
        candidate.append(node);
        if (QJsonDocument(candidate).toJson(QJsonDocument::Compact).size() > outputLimit) {
            output["truncated"] = true;
            return;
        }
        nodes = candidate;
        if (!ready()) return;
        CFIndex count = 0;
        if (!prepare(element) || !ready()) return;
        const auto countError = AXUIElementGetAttributeValueCount(element, kAXChildrenAttribute, &count);
        fail(countError);
        if (countError != kAXErrorSuccess || count < 1) return;
        if (depth >= depthLimit) { output["truncated"] = true; return; }
        const CFIndex remaining = nodeLimit - discovered;
        if (remaining < 1) { output["truncated"] = true; return; }
        const CFIndex take = std::min(count, remaining);
        discovered += static_cast<int>(take);
        if (take < count) output["truncated"] = true;
        CFArrayRef childrenRaw = nullptr;
        if (!prepare(element) || !ready()) return;
        fail(AXUIElementCopyAttributeValues(element, kAXChildrenAttribute, 0, take, &childrenRaw));
        auto children = owned(childrenRaw);
        if (!children || CFGetTypeID(children.get()) != CFArrayGetTypeID()) return;
        const auto array = static_cast<CFArrayRef>(children.get());
        for (CFIndex i = 0; i < CFArrayGetCount(array) && i < take; ++i) {
            if (!ready()) return;
            if (nodes.size() >= nodeLimit) { output["truncated"] = true; return; }
            const auto child = CFArrayGetValueAtIndex(array, i);
            if (child && CFGetTypeID(child) == AXUIElementGetTypeID())
                walk(static_cast<AXUIElementRef>(child), depth + 1);
        }
    }
};
bool validDirectory(const std::string& path) {
    // Lexical validation only: no filesystem access, symlink resolution, or NAS stat.
    if (path.empty() || path.find('\0') != std::string::npos) return false;
    if (path.rfind("/private/tmp/", 0) != 0 && path.rfind("/tmp/", 0) != 0) return false;
    if (path.back() == '/' || path.find("//") != std::string::npos) return false;
    size_t start = 1;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto component = path.substr(start, end - start);
        if (component == "." || component == "..") return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
} // namespace

int runDirectorySyncNativeProbe(bool fixtureMode, const std::string& fixturePid, const std::string& fixtureDirectory) {
    @autoreleasepool {
        Probe probe;
        try {
            const auto permission = queryDirectorySyncPermissions(false);
            probe.output["accessibility"] = permission.accessibility;
            probe.output["finder_status"] = QString::fromStdString(permission.finder_status);
            probe.output["finder_error"] = permission.finder_error;
            probe.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            NSRunningApplication* target = nil;
            Ref expected = owned(nullptr), expectedUrl = owned(nullptr);
            if (fixtureMode) {
                if (!validDirectory(fixtureDirectory)) {
                    probe.output["fixture_reason"] = "invalid-path";
                } else {
                    bool validPid = !fixturePid.empty();
                    unsigned long long pidValue = 0;
                    for (const auto c : fixturePid) {
                        if (c < '0' || c > '9' || pidValue > (std::numeric_limits<int>::max() - (c - '0')) / 10ULL) {
                            validPid = false; break;
                        }
                        pidValue = pidValue * 10 + c - '0';
                    }
                    if (!validPid || pidValue < 1) probe.output["fixture_reason"] = "invalid-pid";
                    else {
                        target = [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(pidValue)];
                        if (!target || ![target.bundleIdentifier isEqualToString:@"org.everythinglite.directory-fixture"]) {
                            probe.output["fixture_reason"] = "target-rejected";
                            target = nil;
                        } else {
                            expected = owned(CFStringCreateWithBytes(kCFAllocatorDefault,
                                reinterpret_cast<const UInt8*>(fixtureDirectory.data()), fixtureDirectory.size(), kCFStringEncodingUTF8, false));
                            if (!expected) {
                                probe.output["fixture_reason"] = "invalid-path";
                                target = nil;
                            } else {
                                auto url = owned(CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                    static_cast<CFStringRef>(expected.get()), kCFURLPOSIXPathStyle, true));
                                if (url) expectedUrl = owned(CFRetain(CFURLGetString(static_cast<CFURLRef>(url.get()))));
                                probe.expected = static_cast<CFStringRef>(expected.get());
                                probe.expectedUrl = static_cast<CFStringRef>(expectedUrl.get());
                            }
                        }
                    }
                }
            } else target = NSWorkspace.sharedWorkspace.frontmostApplication;
            if (target) {
                // Always try the actual public read API; trust state alone does not gate it.
                auto application = owned(AXUIElementCreateApplication(target.processIdentifier));
                if (application) {
                    auto window = probe.copy(static_cast<AXUIElementRef>(application.get()), kAXFocusedWindowAttribute);
                    if (window && CFGetTypeID(window.get()) == AXUIElementGetTypeID()) {
                        probe.output["focused_window_read"] = true;
                        auto element = static_cast<AXUIElementRef>(window.get());
                        auto identifier = probe.copy(element, kAXIdentifierAttribute);
                        const auto kind = safeName(identifier.get());
                        if (kind == "open-panel" || kind == "save-panel") {
                            probe.output["kind"] = kind;
                            if (!probe.failed) probe.output["native_status"] = "success";
                            probe.walk(element, 0);
                        }
                    }
                } else probe.fail(kAXErrorFailure);
            }
        } catch (...) {
            probe.output["native_status"] = "error";
            probe.output["ax_error"] = static_cast<int>(kAXErrorFailure);
        }
        probe.output["nodes"] = probe.nodes;
        const auto json = QJsonDocument(probe.output).toJson(QJsonDocument::Compact);
        std::cout.write(json.constData(), json.size());
        std::cout << '\n';
        return std::cout.good() ? 0 : 1;
    }
}
} // namespace everything_lite
