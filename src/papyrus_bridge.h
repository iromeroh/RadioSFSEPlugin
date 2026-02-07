#pragma once

#include "logger.h"

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class PapyrusBridge
{
public:
    explicit PapyrusBridge(Logger& logger);

    bool initialize();
    void shutdown();

    bool isInstalled() const;
    bool hasCapturedRegistry() const;
    std::uintptr_t capturedRegistryAddress() const;

private:
    struct Config
    {
        bool enabled{ false };
        std::string hookMode{ "invoke_callsite" };
        std::optional<std::uintptr_t> registrationCallAddress;
        std::optional<std::uintptr_t> registrationCallRva;
        std::optional<std::uintptr_t> invokeCallsiteRva;
        std::uintptr_t invokeTargetRva{ 0 };
        std::size_t invokeAutoPatchCount{ 64 };
        bool logEveryInvocation{ false };
    };

    using RegistrationHookTarget = std::uintptr_t (*)(void*, void*, void*, void*);
    using InvokeHookTarget = std::uint32_t (*)(void*, std::uint64_t, std::uint64_t, void*, void*);

    static PapyrusBridge* g_instance_;
    static std::uintptr_t Hook_Registration(void* a0, void* a1, void* a2, void* a3);
    static std::uint32_t Hook_Invoke(void* thisPtr, std::uint64_t unk0, std::uint64_t unk1, void* registry, void* state);

    bool loadConfig();
    bool installHook();
    bool installRegistrationCallsiteHook();
    bool installInvokeCallsiteHook();
    bool attemptNativeRegistration();
    std::optional<std::uintptr_t> resolveInvokeTarget() const;
    bool validateCallsiteToTarget(std::uintptr_t callsite, std::uintptr_t target) const;
    std::vector<std::uintptr_t> findCallsitesToTarget(std::uintptr_t target) const;
    std::pair<std::uintptr_t, std::size_t> textRange() const;
    std::pair<std::uintptr_t, std::size_t> sectionRange(const char* sectionName) const;

    static std::optional<std::uintptr_t> parseU64(const std::string& value);
    static std::string trim(const std::string& text);
    static std::string toLower(std::string text);

    static bool writeRelativeCall(std::uintptr_t src, std::uintptr_t dst);

    Logger& logger_;
    Config config_{};
    std::uintptr_t imageBase_{ 0 };

    std::uintptr_t callSite_{ 0 };
    std::uintptr_t originalTargetAddress_{ 0 };
    RegistrationHookTarget originalRegistrationTarget_{ nullptr };
    InvokeHookTarget originalInvokeTarget_{ nullptr };
    bool installed_{ false };
    bool registeredAttempted_{ false };
    bool invokeObserved_{ false };
    std::uint64_t invokeCallCount_{ 0 };
    bool registryCaptured_{ false };
    std::uintptr_t registryAddress_{ 0 };
};
