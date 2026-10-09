#include "ui/directory_sync_permissions.h"
#include <iostream>
#include <memory>
#include <stdexcept>
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#include <CoreServices/CoreServices.h>
#endif
namespace everything_lite {
DirectorySyncPermissions queryDirectorySyncPermissions(bool request) {
    DirectorySyncPermissions result;
#ifdef __APPLE__
    // A null options dictionary is a read-only trust check and cannot prompt.
    if (request) {
        const void* keys[] = {kAXTrustedCheckOptionPrompt};
        const void* values[] = {kCFBooleanTrue};
        using Dictionary = std::unique_ptr<const __CFDictionary, decltype(&CFRelease)>;
        Dictionary options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                           &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks), CFRelease);
        if (!options) throw std::runtime_error("permission initialization");
        result.accessibility = AXIsProcessTrustedWithOptions(options.get());
    } else {
        result.accessibility = AXIsProcessTrustedWithOptions(nullptr);
    }
    struct Target {
        AEDesc descriptor{typeNull, nullptr};
        ~Target() { AEDisposeDesc(&descriptor); }
    } target;
    constexpr char bundleId[] = "com.apple.finder";
    if (AECreateDesc(typeApplicationBundleID, bundleId, sizeof(bundleId) - 1,
                     &target.descriptor) != noErr)
        throw std::runtime_error("permission initialization");
    // Check consent for future directory changes; send no actual Apple event.
    const OSStatus error = AEDeterminePermissionToAutomateTarget(
        &target.descriptor, kAECoreSuite, kAESetData, request);
    result.finder_error = static_cast<std::int32_t>(error);
    switch (error) {
    case noErr: result.finder_status = "granted"; break;
    case -1744: result.finder_status = "consent-required"; break;
    case -1743: result.finder_status = "denied"; break;
    case procNotFound: result.finder_status = "unavailable"; break;
    default: result.finder_status = "error"; break;
    }
#else
    (void)request;
#endif
    return result;
}
int runDirectorySyncPermissionHelper(bool request) {
    try {
        const auto value = queryDirectorySyncPermissions(request);
        // Only fixed status values and scalar permission state cross this boundary.
        std::cout << "{\"protocol\":1,\"accessibility\":" << (value.accessibility ? "true" : "false")
                  << ",\"finder_status\":\"" << value.finder_status << "\",\"finder_error\":"
                  << value.finder_error << ",\"request\":" << (request ? "true" : "false") << "}\n";
        return std::cout.good() ? 0 : 1;
    } catch (...) {
        std::cerr << "Permission check failed.\n";
        return 1;
    }
}
} // namespace everything_lite
