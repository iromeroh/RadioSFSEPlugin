#include "papyrus_bridge.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <unordered_set>

#include <windows.h>

PapyrusBridge* PapyrusBridge::g_instance_ = nullptr;

namespace
{
std::string hexAddress(std::uintptr_t value)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << value;
    return out.str();
}

template <class T>
void pushUnique(std::vector<T>& values, const T& value)
{
    if (std::find(values.begin(), values.end(), value) == values.end()) {
        values.push_back(value);
    }
}

struct RTTICompleteObjectLocator64
{
    std::uint32_t signature;
    std::uint32_t offset;
    std::uint32_t cdOffset;
    std::int32_t pTypeDescriptor;
    std::int32_t pClassDescriptor;
    std::int32_t pSelf;
};

std::optional<std::uintptr_t> allocateNearExecutablePage(std::uintptr_t target)
{
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uintptr_t granularity = static_cast<std::uintptr_t>(info.dwAllocationGranularity);
    const std::uintptr_t maxRelDistance = 0x7FFF0000ULL;

    auto alignDown = [&](std::uintptr_t value) {
        return value & ~(granularity - 1);
    };

    const std::uintptr_t minAddress = (target > maxRelDistance) ? (target - maxRelDistance) : 0;
    const std::uintptr_t maxAddress = target + maxRelDistance;
    const std::uintptr_t start = alignDown(target);

    for (std::uintptr_t delta = 0; delta <= maxRelDistance; delta += granularity) {
        const std::uintptr_t up = start + delta;
        if (up >= minAddress && up <= maxAddress) {
            if (void* page = VirtualAlloc(reinterpret_cast<void*>(up), granularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                return reinterpret_cast<std::uintptr_t>(page);
            }
        }

        if (delta == 0) {
            continue;
        }

        const std::uintptr_t down = (start > delta) ? (start - delta) : 0;
        if (down >= minAddress && down <= maxAddress) {
            if (void* page = VirtualAlloc(reinterpret_cast<void*>(down), granularity, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE)) {
                return reinterpret_cast<std::uintptr_t>(page);
            }
        }
    }

    return std::nullopt;
}

bool writeAbsoluteJumpStub(std::uintptr_t stubAddress, std::uintptr_t destination)
{
    // mov rax, imm64; jmp rax
    std::array<std::uint8_t, 12> stub{};
    stub[0] = 0x48;
    stub[1] = 0xB8;
    std::memcpy(&stub[2], &destination, sizeof(destination));
    stub[10] = 0xFF;
    stub[11] = 0xE0;

    std::memcpy(reinterpret_cast<void*>(stubAddress), stub.data(), stub.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(stubAddress), stub.size());
    return true;
}
}

PapyrusBridge::PapyrusBridge(Logger& logger) :
    logger_(logger)
{
}

bool PapyrusBridge::initialize()
{
    imageBase_ = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (imageBase_ == 0) {
        logger_.warn("[M4] PapyrusBridge failed to read image base.");
        return false;
    }

    if (!loadConfig()) {
        logger_.info("[M4] PapyrusBridge config not enabled; skipping direct hook.");
        return false;
    }

    if (!config_.enabled) {
        logger_.info("[M4] PapyrusBridge disabled in config.");
        return false;
    }

    const bool ok = installHook();
    if (!ok) {
        logger_.warn("[M4] PapyrusBridge install failed.");
        return false;
    }

    logger_.info("[M4] PapyrusBridge hook installed.");
    return true;
}

void PapyrusBridge::shutdown()
{
    if (g_instance_ == this) {
        g_instance_ = nullptr;
    }
}

bool PapyrusBridge::isInstalled() const
{
    return installed_;
}

bool PapyrusBridge::hasCapturedRegistry() const
{
    return registryCaptured_;
}

std::uintptr_t PapyrusBridge::capturedRegistryAddress() const
{
    return registryAddress_;
}

std::uintptr_t PapyrusBridge::Hook_Registration(void* a0, void* a1, void* a2, void* a3)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return 0;
    }

    std::uintptr_t result = 0;
    if (self->originalRegistrationTarget_ != nullptr) {
        result = self->originalRegistrationTarget_(a0, a1, a2, a3);
    }

    if (a0 != nullptr && !self->registryCaptured_) {
        self->registryCaptured_ = true;
        self->registryAddress_ = reinterpret_cast<std::uintptr_t>(a0);
        self->logger_.info("[M5] Papyrus VM/registry captured at " + hexAddress(self->registryAddress_));
    } else if (self->config_.logEveryInvocation && a0 != nullptr) {
        self->logger_.info("[M5] Papyrus registration hook tick.");
    }

    if (!self->registeredAttempted_ && self->registryCaptured_) {
        self->registeredAttempted_ = true;
        (void)self->attemptNativeRegistration();
    }

    return result;
}

std::uint32_t PapyrusBridge::Hook_Invoke(void* thisPtr, std::uint64_t unk0, std::uint64_t unk1, void* registry, void* state)
{
    PapyrusBridge* self = g_instance_;
    if (self == nullptr) {
        return 0;
    }

    ++self->invokeCallCount_;
    if (!self->invokeObserved_) {
        self->invokeObserved_ = true;
        self->logger_.info("[M5] Hook_Invoke reached first time. this=" +
                           hexAddress(reinterpret_cast<std::uintptr_t>(thisPtr)) +
                           ", registry=" + hexAddress(reinterpret_cast<std::uintptr_t>(registry)));
    } else if (self->config_.logEveryInvocation) {
        self->logger_.info("[M5] Hook_Invoke call #" + std::to_string(self->invokeCallCount_));
    }

    if (registry != nullptr && !self->registryCaptured_) {
        self->registryCaptured_ = true;
        self->registryAddress_ = reinterpret_cast<std::uintptr_t>(registry);
        self->logger_.info("[M5] Papyrus VM/registry captured at " + hexAddress(self->registryAddress_));
    } else if (self->config_.logEveryInvocation && registry != nullptr) {
        self->logger_.info("[M5] Papyrus invoke hook tick.");
    }

    std::uint32_t result = 0;
    if (self->originalInvokeTarget_ != nullptr) {
        result = self->originalInvokeTarget_(thisPtr, unk0, unk1, registry, state);
    }

    if (!self->registeredAttempted_ && self->registryCaptured_) {
        self->registeredAttempted_ = true;
        (void)self->attemptNativeRegistration();
    }

    return result;
}

bool PapyrusBridge::loadConfig()
{
    const std::filesystem::path path = std::filesystem::path("Data") / "SFSE" / "Plugins" / "RadioSFSE.ini";
    if (!std::filesystem::exists(path)) {
        return false;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(in, line)) {
        const auto commentPos = line.find_first_of("#;");
        if (commentPos != std::string::npos) {
            line = line.substr(0, commentPos);
        }

        line = trim(line);
        if (line.empty()) {
            continue;
        }

        const auto eqPos = line.find('=');
        if (eqPos == std::string::npos) {
            continue;
        }

        const std::string key = toLower(trim(line.substr(0, eqPos)));
        const std::string value = trim(line.substr(eqPos + 1));

        if (key == "experimental_papyrus_hook") {
            const std::string v = toLower(value);
            config_.enabled = (v == "1" || v == "true" || v == "yes");
        } else if (key == "papyrus_hook_mode") {
            config_.hookMode = toLower(value);
        } else if (key == "papyrus_registration_call_address") {
            config_.registrationCallAddress = parseU64(value);
        } else if (key == "papyrus_registration_call_rva") {
            config_.registrationCallRva = parseU64(value);
        } else if (key == "papyrus_invoke_callsite_rva") {
            config_.invokeCallsiteRva = parseU64(value);
        } else if (key == "papyrus_invoke_target_rva") {
            const auto parsed = parseU64(value);
            if (parsed.has_value()) {
                config_.invokeTargetRva = *parsed;
            }
        } else if (key == "papyrus_invoke_autopatch_count") {
            const auto parsed = parseU64(value);
            if (parsed.has_value() && *parsed > 0) {
                config_.invokeAutoPatchCount = static_cast<std::size_t>(*parsed);
            }
        } else if (key == "papyrus_hook_verbose") {
            const std::string v = toLower(value);
            config_.logEveryInvocation = (v == "1" || v == "true" || v == "yes");
        }
    }

    return true;
}

bool PapyrusBridge::installHook()
{
    if (config_.hookMode == "manual_callsite") {
        return installRegistrationCallsiteHook();
    }

    return installInvokeCallsiteHook();
}

bool PapyrusBridge::installRegistrationCallsiteHook()
{
    if (config_.registrationCallAddress.has_value()) {
        callSite_ = *config_.registrationCallAddress;
    } else if (config_.registrationCallRva.has_value()) {
        callSite_ = imageBase_ + *config_.registrationCallRva;
    } else {
        logger_.warn("[M4] manual_callsite requires papyrus_registration_call_rva/address.");
        return false;
    }

    if (callSite_ == 0) {
        return false;
    }

    const auto opcode = *reinterpret_cast<std::uint8_t*>(callSite_);
    if (opcode != 0xE8) {
        logger_.warn("[M4] manual_callsite target is not CALL (E8).");
        return false;
    }

    const auto displacement = *reinterpret_cast<std::int32_t*>(callSite_ + 1);
    originalTargetAddress_ = callSite_ + 5 + static_cast<std::intptr_t>(displacement);
    originalRegistrationTarget_ = reinterpret_cast<RegistrationHookTarget>(originalTargetAddress_);

    g_instance_ = this;
    if (!writeRelativeCall(callSite_, reinterpret_cast<std::uintptr_t>(&PapyrusBridge::Hook_Registration))) {
        g_instance_ = nullptr;
        return false;
    }

    installed_ = true;
    logger_.info("[M4] Hooked Papyrus callsite at " + hexAddress(callSite_) +
                 ", original target " + hexAddress(originalTargetAddress_));
    return true;
}

bool PapyrusBridge::installInvokeCallsiteHook()
{
    std::uintptr_t invokeTarget = 0;
    if (config_.invokeTargetRva != 0) {
        invokeTarget = imageBase_ + config_.invokeTargetRva;
    } else {
        const auto resolved = resolveInvokeTarget();
        if (!resolved.has_value()) {
            logger_.warn("[M4] Failed to resolve invoke target automatically.");
            return false;
        }
        invokeTarget = *resolved;
    }

    if (invokeTarget == 0) {
        return false;
    }

    logger_.info("[M4] Using invoke target " + hexAddress(invokeTarget) +
                 " (rva " + hexAddress(invokeTarget - imageBase_) + ")");

    if (config_.invokeCallsiteRva.has_value()) {
        callSite_ = imageBase_ + *config_.invokeCallsiteRva;
        if (!validateCallsiteToTarget(callSite_, invokeTarget)) {
            logger_.warn("[M4] papyrus_invoke_callsite_rva does not call invoke target.");
            return false;
        }

        originalTargetAddress_ = invokeTarget;
        originalInvokeTarget_ = reinterpret_cast<InvokeHookTarget>(invokeTarget);
        g_instance_ = this;
        if (!writeRelativeCall(callSite_, reinterpret_cast<std::uintptr_t>(&PapyrusBridge::Hook_Invoke))) {
            g_instance_ = nullptr;
            return false;
        }

        installed_ = true;
        logger_.info("[M4] Hooked invoke callsite at " + hexAddress(callSite_) +
                     ", invoke target " + hexAddress(invokeTarget));
        return true;
    } else {
        auto callsites = findCallsitesToTarget(invokeTarget);
        if (callsites.empty() && config_.invokeTargetRva != 0) {
            logger_.warn("[M4] No callsites found for configured invoke target, trying RTTI auto-resolve.");
            const auto resolved = resolveInvokeTarget();
            if (resolved.has_value() && *resolved != invokeTarget) {
                invokeTarget = *resolved;
                callsites = findCallsitesToTarget(invokeTarget);
                logger_.info("[M4] Fallback invoke target " + hexAddress(invokeTarget) +
                             " (rva " + hexAddress(invokeTarget - imageBase_) + ")");
            }
        }

        if (callsites.empty()) {
            logger_.warn("[M4] Could not find callsites to invoke target " + hexAddress(invokeTarget));
            return false;
        }

        const std::size_t patchCount = std::min(config_.invokeAutoPatchCount, callsites.size());
        callSite_ = callsites.front();
        logger_.info("[M4] invoke_callsite auto-discovered " + std::to_string(callsites.size()) +
                     " callsite(s), patching " + std::to_string(patchCount) +
                     ", first " + hexAddress(callSite_) +
                     " (rva " + hexAddress(callSite_ - imageBase_) + ")");

        originalTargetAddress_ = invokeTarget;
        originalInvokeTarget_ = reinterpret_cast<InvokeHookTarget>(invokeTarget);
        g_instance_ = this;

        std::vector<std::size_t> selectedIndices;
        selectedIndices.reserve(patchCount);
        if (patchCount == callsites.size() || patchCount <= 1) {
            for (std::size_t i = 0; i < patchCount; ++i) {
                selectedIndices.push_back(i);
            }
        } else {
            for (std::size_t i = 0; i < patchCount; ++i) {
                const std::size_t idx = (i * (callsites.size() - 1)) / (patchCount - 1);
                if (selectedIndices.empty() || selectedIndices.back() != idx) {
                    selectedIndices.push_back(idx);
                }
            }
            while (selectedIndices.size() < patchCount && selectedIndices.back() + 1 < callsites.size()) {
                selectedIndices.push_back(selectedIndices.back() + 1);
            }
        }

        std::size_t patched = 0;
        for (const std::size_t idx : selectedIndices) {
            if (writeRelativeCall(callsites[idx], reinterpret_cast<std::uintptr_t>(&PapyrusBridge::Hook_Invoke))) {
                ++patched;
            }
        }

        if (patched == 0) {
            g_instance_ = nullptr;
            return false;
        }

        installed_ = true;
        const std::uintptr_t lastCallsite = callsites[selectedIndices.back()];
        logger_.info("[M4] Hooked invoke callsites: " + std::to_string(patched) +
                     "/" + std::to_string(selectedIndices.size()) +
                     ", span " + hexAddress(callSite_) + " .. " + hexAddress(lastCallsite));
        return true;
    }
}

bool PapyrusBridge::validateCallsiteToTarget(std::uintptr_t callsite, std::uintptr_t target) const
{
    if (callsite == 0) {
        return false;
    }

    const auto opcode = *reinterpret_cast<const std::uint8_t*>(callsite);
    if (opcode != 0xE8) {
        return false;
    }

    const auto displacement = *reinterpret_cast<const std::int32_t*>(callsite + 1);
    const std::uintptr_t resolved = callsite + 5 + static_cast<std::intptr_t>(displacement);
    return resolved == target;
}

std::vector<std::uintptr_t> PapyrusBridge::findCallsitesToTarget(std::uintptr_t target) const
{
    std::vector<std::uintptr_t> callsites;
    const auto [start, size] = textRange();
    if (start == 0 || size < 5) {
        return callsites;
    }

    const std::uintptr_t end = start + size - 5;
    for (std::uintptr_t addr = start; addr <= end; ++addr) {
        const auto opcode = *reinterpret_cast<const std::uint8_t*>(addr);
        if (opcode != 0xE8) {
            continue;
        }

        const auto displacement = *reinterpret_cast<const std::int32_t*>(addr + 1);
        const std::uintptr_t resolved = addr + 5 + static_cast<std::intptr_t>(displacement);
        if (resolved == target) {
            callsites.push_back(addr);
        }
    }

    return callsites;
}

std::pair<std::uintptr_t, std::size_t> PapyrusBridge::textRange() const
{
    return sectionRange(".text");
}

std::pair<std::uintptr_t, std::size_t> PapyrusBridge::sectionRange(const char* sectionName) const
{
    if (sectionName == nullptr || sectionName[0] == '\0') {
        return { 0, 0 };
    }

    if (imageBase_ == 0) {
        return { 0, 0 };
    }

    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return { 0, 0 };
    }

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase_ + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return { 0, 0 };
    }

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        const char* name = reinterpret_cast<const char*>(section->Name);
        if (std::strncmp(name, sectionName, std::strlen(sectionName)) == 0) {
            const std::uintptr_t start = imageBase_ + section->VirtualAddress;
            std::size_t size = static_cast<std::size_t>(section->Misc.VirtualSize);
            if (size == 0) {
                size = static_cast<std::size_t>(section->SizeOfRawData);
            }
            return { start, size };
        }
    }

    return { 0, 0 };
}

std::optional<std::uintptr_t> PapyrusBridge::resolveInvokeTarget() const
{
    const auto [textStart, textSize] = textRange();
    const auto [rdataStart, rdataSize] = sectionRange(".rdata");
    if (textStart == 0 || textSize == 0 || rdataStart == 0 || rdataSize == 0) {
        logger_.warn("[M4] RTTI resolve failed: missing .text/.rdata range.");
        return std::nullopt;
    }

    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(imageBase_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        logger_.warn("[M4] RTTI resolve failed: invalid DOS header.");
        return std::nullopt;
    }

    const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(imageBase_ + static_cast<std::uintptr_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        logger_.warn("[M4] RTTI resolve failed: invalid NT header.");
        return std::nullopt;
    }

    const std::uintptr_t imageEnd = imageBase_ + static_cast<std::uintptr_t>(nt->OptionalHeader.SizeOfImage);
    const std::uintptr_t textEnd = textStart + textSize;
    const std::uintptr_t rdataEnd = rdataStart + rdataSize;

    auto isInText = [&](std::uintptr_t address) {
        return address >= textStart && address < textEnd;
    };
    auto isInImage = [&](std::uintptr_t address) {
        return address >= imageBase_ && address < imageEnd;
    };

    static constexpr const char* kTypeNames[] = {
        ".?AVNativeFunctionBase@NF_util@BSScript@@",
        ".?AVNativeFunctionBase@BSScript@@"
    };

    std::vector<std::uintptr_t> typeDescriptors;
    const auto* imageBytes = reinterpret_cast<const std::uint8_t*>(imageBase_);
    const std::size_t imageSize = static_cast<std::size_t>(nt->OptionalHeader.SizeOfImage);
    std::size_t typeNameHits = 0;
    for (const char* typeName : kTypeNames) {
        const std::size_t len = std::strlen(typeName);
        if (len == 0 || len >= imageSize) {
            continue;
        }

        for (std::size_t offset = 0; offset + len < imageSize; ++offset) {
            const auto* cursor = imageBytes + offset;
            if (std::memcmp(cursor, typeName, len) != 0) {
                continue;
            }

            ++typeNameHits;

            const std::uintptr_t nameAddress = imageBase_ + offset;
            if (nameAddress < imageBase_ + 16) {
                continue;
            }

            const std::uintptr_t typeDescriptor = nameAddress - 16;
            if (!isInImage(typeDescriptor) || !isInImage(typeDescriptor + 15)) {
                continue;
            }

            pushUnique(typeDescriptors, typeDescriptor);
        }
    }

    if (typeDescriptors.empty()) {
        logger_.warn("[M4] RTTI resolve failed: NativeFunctionBase type descriptor not found.");
        return std::nullopt;
    }

    logger_.info("[M4] RTTI type-name hits: " + std::to_string(typeNameHits) +
                 ", candidate type descriptors: " + std::to_string(typeDescriptors.size()));

    std::vector<std::uintptr_t> colAddresses;
    for (std::uintptr_t scan = rdataStart; scan + sizeof(RTTICompleteObjectLocator64) <= rdataEnd; scan += 4) {
        RTTICompleteObjectLocator64 col{};
        std::memcpy(&col, reinterpret_cast<const void*>(scan), sizeof(col));
        if (col.signature > 1) {
            continue;
        }

        const std::uintptr_t colRva = scan - imageBase_;
        if (col.signature == 1 && static_cast<std::uint32_t>(col.pSelf) != static_cast<std::uint32_t>(colRva)) {
            continue;
        }

        if (col.pTypeDescriptor <= 0 || col.pClassDescriptor <= 0) {
            continue;
        }

        const std::uintptr_t typeDescriptorAddress = imageBase_ + static_cast<std::uint32_t>(col.pTypeDescriptor);
        const std::uintptr_t classDescriptorAddress = imageBase_ + static_cast<std::uint32_t>(col.pClassDescriptor);
        if (!isInImage(typeDescriptorAddress) || !isInImage(classDescriptorAddress)) {
            continue;
        }

        if (std::find(typeDescriptors.begin(), typeDescriptors.end(), typeDescriptorAddress) == typeDescriptors.end()) {
            continue;
        }

        pushUnique(colAddresses, scan);
    }

    if (colAddresses.empty()) {
        logger_.warn("[M4] RTTI resolve failed: no CompleteObjectLocator for NativeFunctionBase.");
        return std::nullopt;
    }

    std::unordered_set<std::uintptr_t> colSet(colAddresses.begin(), colAddresses.end());
    std::vector<std::uintptr_t> vtables;
    for (std::uintptr_t slot = rdataStart + sizeof(std::uintptr_t); slot + sizeof(std::uintptr_t) <= rdataEnd;
         slot += sizeof(std::uintptr_t)) {
        const std::uintptr_t marker = *reinterpret_cast<const std::uintptr_t*>(slot - sizeof(std::uintptr_t));
        if (!colSet.contains(marker)) {
            continue;
        }

        const std::uintptr_t firstFunction = *reinterpret_cast<const std::uintptr_t*>(slot);
        if (!isInText(firstFunction)) {
            continue;
        }

        pushUnique(vtables, slot);
    }

    if (vtables.empty()) {
        logger_.warn("[M4] RTTI resolve failed: no vtable found for NativeFunctionBase.");
        return std::nullopt;
    }

    logger_.info("[M4] RTTI found " + std::to_string(typeDescriptors.size()) +
                 " type descriptor(s), " + std::to_string(colAddresses.size()) +
                 " COL(s), " + std::to_string(vtables.size()) + " vtable(s).");

    static constexpr std::size_t kExpectedInvokeIndex = 14;
    for (std::uintptr_t vtable : vtables) {
        const std::uintptr_t slot = vtable + (kExpectedInvokeIndex * sizeof(std::uintptr_t));
        if (slot + sizeof(std::uintptr_t) > rdataEnd) {
            continue;
        }

        const std::uintptr_t candidate = *reinterpret_cast<const std::uintptr_t*>(slot);
        if (!isInText(candidate)) {
            continue;
        }

        logger_.info("[M4] RTTI selected invoke target at vtable index " + std::to_string(kExpectedInvokeIndex) +
                     ": " + hexAddress(candidate));
        return candidate;
    }

    std::uintptr_t bestTarget = 0;
    std::size_t bestIndex = 0;
    std::size_t bestScore = 0;

    for (std::uintptr_t vtable : vtables) {
        for (std::size_t idx = 8; idx <= 28; ++idx) {
            const std::uintptr_t slot = vtable + (idx * sizeof(std::uintptr_t));
            if (slot + sizeof(std::uintptr_t) > rdataEnd) {
                break;
            }

            const std::uintptr_t candidate = *reinterpret_cast<const std::uintptr_t*>(slot);
            if (!isInText(candidate)) {
                continue;
            }

            const std::size_t callsiteCount = findCallsitesToTarget(candidate).size();
            if (callsiteCount > bestScore) {
                bestScore = callsiteCount;
                bestIndex = idx;
                bestTarget = candidate;
            }
        }
    }

    if (bestTarget != 0 && bestScore > 0) {
        logger_.warn("[M4] RTTI fallback selected invoke-like target index " + std::to_string(bestIndex) +
                     " with " + std::to_string(bestScore) + " callsite(s): " + hexAddress(bestTarget));
        return bestTarget;
    }

    logger_.warn("[M4] RTTI resolve failed: invoke slot unresolved.");
    return std::nullopt;
}

bool PapyrusBridge::attemptNativeRegistration()
{
    // Direct native registration requires ABI details missing from the bundled SFSE SDK:
    // VMClassRegistry layout, RegisterFunction signature, and native function wrappers.
    // This is the milestone checkpoint where we confirm the hook and live registry pointer.
    logger_.warn("[M6] Registry captured, but native registration ABI is unresolved in this build.");
    return false;
}

std::optional<std::uintptr_t> PapyrusBridge::parseU64(const std::string& value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    try {
        std::size_t idx = 0;
        unsigned long long parsed = 0;
        if (value.size() > 2 && (value[0] == '0') && (value[1] == 'x' || value[1] == 'X')) {
            parsed = std::stoull(value, &idx, 16);
        } else {
            parsed = std::stoull(value, &idx, 10);
        }
        if (idx != value.size()) {
            return std::nullopt;
        }
        return static_cast<std::uintptr_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

std::string PapyrusBridge::trim(const std::string& text)
{
    std::size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])) != 0) {
        ++start;
    }

    std::size_t end = text.size();
    while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }

    return text.substr(start, end - start);
}

std::string PapyrusBridge::toLower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

bool PapyrusBridge::writeRelativeCall(std::uintptr_t src, std::uintptr_t dst)
{
    auto patchRelCall = [&](std::uintptr_t target) {
        const std::uintptr_t next = src + 5;
        const std::intptr_t disp64 = static_cast<std::intptr_t>(target) - static_cast<std::intptr_t>(next);
        if (disp64 < std::numeric_limits<std::int32_t>::min() || disp64 > std::numeric_limits<std::int32_t>::max()) {
            return false;
        }

        std::array<std::uint8_t, 5> patch{};
        patch[0] = 0xE8;
        const auto disp32 = static_cast<std::int32_t>(disp64);
        std::memcpy(&patch[1], &disp32, sizeof(disp32));

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(src), patch.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            return false;
        }

        std::memcpy(reinterpret_cast<void*>(src), patch.data(), patch.size());

        DWORD oldProtectUnused = 0;
        (void)VirtualProtect(reinterpret_cast<void*>(src), patch.size(), oldProtect, &oldProtectUnused);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(src), patch.size());
        return true;
    };

    if (patchRelCall(dst)) {
        return true;
    }

    const auto stubPage = allocateNearExecutablePage(src);
    if (!stubPage.has_value()) {
        return false;
    }

    const std::uintptr_t stub = *stubPage;
    if (!writeAbsoluteJumpStub(stub, dst)) {
        return false;
    }

    if (!patchRelCall(stub)) {
        return false;
    }

    return true;
}
