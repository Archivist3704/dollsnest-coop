#include "dsh/game/module_component.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace dsh::game {
namespace {

std::string hex_ptr(std::uintptr_t v) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llX", static_cast<unsigned long long>(v));
    return buf;
}

bool read_u8(ModuleComponentReader read, std::uintptr_t address, std::uintptr_t offset, std::uint8_t& out,
             std::uint64_t* bytesRead = nullptr) {
    if (read == nullptr) return false;
    if (bytesRead != nullptr) *bytesRead += sizeof(out);
    return read(address + offset, &out, sizeof(out));
}

bool read_u64(ModuleComponentReader read, std::uintptr_t address, std::uintptr_t offset, std::uint64_t& out,
              std::uint64_t* bytesRead = nullptr) {
    if (read == nullptr) return false;
    if (bytesRead != nullptr) *bytesRead += sizeof(out);
    return read(address + offset, &out, sizeof(out));
}

constexpr std::uintptr_t kUObjectClassOffset = 0x10;
constexpr std::uintptr_t kUObjectNameOffset = 0x18;

void read_module_fields(ModuleComponentReport& out, ModuleComponentReader read,
                        std::uint64_t* bytesRead);

struct WalkState {
    std::uintptr_t          module = 0;
    ModuleComponentReader   read = nullptr;
    ModuleNameResolver      namer = nullptr;
    ModuleClassChainCheck   chainReaches = nullptr;
    std::uintptr_t          niagaraClass = 0;
    ModuleComponentReport*  out = nullptr;
    std::uint32_t           matched = 0;
};

void on_object(void* user, std::uintptr_t object, std::uintptr_t outer) {
    auto* state = static_cast<WalkState*>(user);
    if (state == nullptr || state->out == nullptr) return;
    if (outer != state->module) return;
    ++state->matched;

    ModuleOwnedObject entry;
    entry.address = object;

    std::uint64_t classValue = 0;
    if (!read_u64(state->read, object, kUObjectClassOffset, classValue) || classValue < 0x10000) {
        ++state->out->unnameable;
        return;
    }
    entry.classAddress = static_cast<std::uintptr_t>(classValue);
    if (!state->namer(entry.classAddress + kUObjectNameOffset, entry.className) ||
        entry.className.empty()) {
        ++state->out->unnameable;
        return;
    }

    if (!state->namer(object + kUObjectNameOffset, entry.objectName)) {
        entry.objectName.clear();
        ++state->out->unnamed;
    }

    read_component_state(entry, state->niagaraClass, state->read, state->chainReaches,
                         &state->out->walkBytesRead);
    state->out->components.push_back(std::move(entry));
}

void read_module_fields(ModuleComponentReport& out, ModuleComponentReader read,
                        std::uint64_t* bytesRead) {
    if (read == nullptr) return;
    std::uint8_t purged = 0;
    if (read_u8(read, out.module, kThrusterModulePurgedOffset, purged, bytesRead)) {
        out.purged = purged != 0;
        out.purgedRead = true;
    }
    std::uint32_t level = 0;
    if (read != nullptr) {
        if (bytesRead != nullptr) *bytesRead += sizeof(level);
        if (read(out.module + kThrusterModuleLevelOffset, &level, sizeof(level))) {
            out.level = static_cast<std::int32_t>(level);
            out.levelRead = true;
        }
    }

    if (bytesRead != nullptr) *bytesRead += kThrusterModuleStatusBytes;
    out.statusRead = read(out.module + kThrusterModuleStatusOffset, out.status,
                          kThrusterModuleStatusBytes);
    std::uint64_t mesh = 0;
    if (read_u64(read, out.module, kThrusterModuleMeshOffset, mesh, bytesRead)) {
        out.mesh = static_cast<std::uintptr_t>(mesh);
        out.meshRead = true;
    }
}

std::string name_or_unresolved(const std::string& name) {
    return name.empty() ? std::string("<name not resolved>") : name;
}

std::string activation_text(const ModuleOwnedObject& c) {
    if (!c.activationRead) {
        return c.activationRefusal.empty()
                   ? std::string("activation NOT read (/ refused)")
                   : ("activation NOT read (" + c.activationRefusal + ")");
    }
    return std::string("bAutoActivate=") + (c.autoActivate ? "1" : "0") + " bIsActive=" +
           (c.isActive ? "1" : "0");
}

std::string niagara_text(const ModuleOwnedObject& c) {
    if (!c.niagara) return std::string();
    std::string out = " Asset=";
    if (!c.assetRead) {
        out += "NOT read";
    } else if (c.asset == 0) {

        out += "NULL (no system to render)";
    } else {
        out += hex_ptr(c.asset);
    }
    out += " niagara flags=";
    if (!c.niagaraFlagsRead) {
        out += "NOT read";
    } else {
        out += std::string("bAutoDestroy=") + (c.autoDestroy ? "1" : "0") + " bRenderingEnabled=" +
               (c.renderingEnabled ? "1" : "0");
    }
    return out;
}

}

void read_component_state(ModuleOwnedObject& out, std::uintptr_t niagaraClass, ModuleComponentReader read,
                          ModuleClassChainCheck chainReaches, std::uint64_t* bytesRead) {
    out.activationRead = false;
    out.autoActivate = false;
    out.isActive = false;
    out.activationRefusal.clear();
    out.niagara = false;
    out.assetRead = false;
    out.asset = 0;
    out.niagaraFlagsRead = false;
    out.autoDestroy = false;
    out.renderingEnabled = false;
    if (out.address < 0x10000 || read == nullptr) return;

    std::uint8_t autoByte = 0;
    std::uint8_t activeByte = 0;
    const bool haveAuto = read_u8(read, out.address, kActorComponentAutoActivateOffset, autoByte, bytesRead);
    const bool haveActive =
        read_u8(read, out.address, kActorComponentIsActiveOffset, activeByte, bytesRead);
    if (haveAuto && haveActive) {

        out.autoActivate = (autoByte & 0x80u) != 0;
        out.isActive = (activeByte & 0x01u) != 0;
        out.activationRead = true;
    } else if (!haveAuto && !haveActive) {

        out.activationRefusal = "/ refused: neither byte could be read";
    } else if (!haveAuto) {

        out.activationRefusal = " refused (bAutoActivate, bit 7) while read";
    } else {
        out.activationRefusal = " refused (bIsActive, bit 0) while read";
    }

    if (niagaraClass == 0 || chainReaches == nullptr) return;

    if (!chainReaches(out.address, niagaraClass, read)) return;
    out.niagara = true;

    std::uint64_t asset = 0;
    if (read_u64(read, out.address, kNiagaraComponentAssetOffset, asset, bytesRead)) {
        out.asset = static_cast<std::uintptr_t>(asset);
        out.assetRead = true;
    }
    std::uint8_t flags = 0;
    if (read_u8(read, out.address, kNiagaraComponentFlagsOffset, flags, bytesRead)) {
        out.autoDestroy = (flags & 0x01u) != 0;
        out.renderingEnabled = (flags & 0x02u) != 0;
        out.niagaraFlagsRead = true;
    }
}

bool module_component_dump(std::uintptr_t machine, const std::string& label,
                           std::uintptr_t thrusterClass, std::uintptr_t niagaraClass,
                           std::uint32_t maxObjects, ModuleObjectIterator iterate,
                           ModuleComponentReader read, ModuleNameResolver namer,
                           ModuleClassChainCheck chainReaches, ModuleComponentReport& out) {

    out = ModuleComponentReport{};
    out.label = label;
    out.machine = machine;
    out.listFresh = true;
    out.walkBytesRead = 0;
    out.refreshBytesRead = 0;
    out.niagaraClass = niagaraClass;

    out.niagaraChecked = (niagaraClass != 0 && chainReaches != nullptr);

    if (read == nullptr) {
        out.note = "no memory reader was supplied";
        return false;
    }
    if (namer == nullptr) {
        out.note = "no FName resolver was supplied, so a component list could not be named";
        return false;
    }
    if (iterate == nullptr) {
        out.note = "no object iterator was supplied";
        return false;
    }
    if (machine < 0x10000) {
        out.note = "there is no machine actor to read a thruster module from";
        return false;
    }

    out.machineClass = std::string();
    std::uint64_t machineClass = 0;
    if (read_u64(read, machine, kUObjectClassOffset, machineClass, &out.walkBytesRead) &&
        machineClass >= 0x10000) {
        namer(static_cast<std::uintptr_t>(machineClass) + kUObjectNameOffset, out.machineClass);
    }

    std::uint64_t moduleValue = 0;
    if (!read_u64(read, machine, kMachineThrusterOffset, moduleValue, &out.walkBytesRead)) {
        out.note = "the thruster slot at the machine's could not be read";
        return false;
    }
    out.moduleRead = true;
    out.module = static_cast<std::uintptr_t>(moduleValue);
    if (out.module < 0x10000) {

        out.note = "the thruster slot at the machine's is NULL -- this machine has no thruster "
                   "module, so there is nothing whose components could be listed";
        return false;
    }

    std::uint64_t moduleClass = 0;
    if (read_u64(read, out.module, kUObjectClassOffset, moduleClass, &out.walkBytesRead) &&
        moduleClass >= 0x10000) {
        namer(static_cast<std::uintptr_t>(moduleClass) + kUObjectNameOffset, out.moduleClass);
    }

    if (thrusterClass == 0) {
        out.note = std::string("the '") + kThrusterModuleApiClass +
                   "' class could not be resolved, so the module's own offsets cannot be checked and "
                   "NOTHING is read from it (the component walk below is still a walk of its Outer)";
    } else if (chainReaches == nullptr) {
        out.note = "no class-chain check was supplied, so the module's own offsets are NOT read";
    } else if (!chainReaches(out.module, thrusterClass, read)) {
        out.note = std::string("the actor at the thruster slot is a '") +
                   name_or_unresolved(out.moduleClass) + "' whose class chain does not reach '" +
                   kThrusterModuleApiClass + "', so its own offsets are NOT read as this module's fields";
    } else {
        out.moduleUsable = true;

        read_module_fields(out, read, &out.walkBytesRead);

        if (out.meshRead && out.mesh >= 0x10000) {
            std::uint64_t meshClassValue = 0;
            if (read_u64(read, out.mesh, kUObjectClassOffset, meshClassValue, &out.walkBytesRead) &&
                meshClassValue >= 0x10000) {
                namer(static_cast<std::uintptr_t>(meshClassValue) + kUObjectNameOffset, out.meshClass);
            }
        }
    }

    WalkState state;
    state.module = out.module;
    state.read = read;
    state.namer = namer;
    state.chainReaches = chainReaches;
    state.niagaraClass = niagaraClass;
    state.out = &out;
    std::uint32_t visited = 0;
    iterate(maxObjects, &on_object, &state, visited);

    if (visited == 0) {

        if (!out.note.empty()) out.note += "; ";
        out.note += "the object table was not walked (0 objects visited), so the component list is EMPTY "
                    "because nothing was looked at, not because the module owns nothing";
    }
    return true;
}

void refresh_module_component_state(ModuleComponentReport& out, std::uintptr_t thrusterClass,
                                    ModuleComponentReader read, ModuleClassChainCheck chainReaches) {

    out.listFresh = false;
    out.refreshBytesRead = 0;
    if (read == nullptr) return;

    if (out.moduleUsable && thrusterClass != 0 && chainReaches != nullptr &&
        chainReaches(out.module, thrusterClass, read)) {
        read_module_fields(out, read, &out.refreshBytesRead);

    } else if (out.moduleUsable) {
        out.moduleUsable = false;
        out.note = "the module's class chain no longer reaches '" + std::string(kThrusterModuleApiClass) +
                   "', so its own fields are NOT re-read on this line -- the values above are the last ones "
                   "a walk established, not a measurement taken now";
    }

    for (auto& c : out.components) {
        read_component_state(c, out.niagaraClass, read, chainReaches, &out.refreshBytesRead);
    }
}

std::vector<std::string> ModuleComponentReport::lines() const {
    std::vector<std::string> out;
    std::string head = "thruster module walk[" + label + "]: machine " + hex_ptr(machine) + " '" +
                       name_or_unresolved(machineClass) + "' -> module ";
    if (module < 0x10000) {
        out.push_back(head + "NULL");
        if (!note.empty()) out.push_back("thruster module walk[" + label + "]: " + note);
        return out;
    }
    head += hex_ptr(module) + " '" + name_or_unresolved(moduleClass) + "'";

    if (moduleUsable) {
        head += " bPurged=";
        head += purgedRead ? (purged ? "1" : "0") : "NOT read";
        head += " ModuleLevel=";
        head += levelRead ? std::to_string(level) : "NOT read";
        head += " Mesh=";
        if (!meshRead) {
            head += "NOT read";
        } else if (mesh == 0) {
            head += "NULL";
        } else {
            head += hex_ptr(mesh) + " '" + name_or_unresolved(meshClass) + "'";
        }
    }
    out.push_back(head);
    if (!note.empty()) out.push_back("thruster module walk[" + label + "]: " + note);

    const int niagaras = niagaraCount();

    const std::string niagaraClause =
        niagaraChecked
            ? ("of which " + std::to_string(niagaras) + " is/are a NiagaraComponent")
            : ("of which the NiagaraComponent count was NOT measured: no component's class chain was "
               "checked (the '" + std::string(kNiagaraApiClass) + "' UClass was not resolved, or no "
               "class-chain check was supplied), so the classes below are the objects' own class names "
               "and nothing here classified them");
    out.push_back("thruster module walk[" + label + "]: owns " + std::to_string(components.size()) +
                  " object(s) whose UObject::Outer is that module, " + niagaraClause +
                  (unnameable > 0 ? ("; " + std::to_string(unnameable) +
                                     " object(s) matched but could not be named at all and are NOT in the "
                                     "list below")
                                  : std::string()) +
                  (unnamed > 0 ? ("; " + std::to_string(unnamed) +
                                  " of the listed ones has a class but no readable OWN name")
                               : std::string()) +
                  "; component list " + (listFresh ? "fresh (walked this line)" : "cached") +
                  ", this line read " + std::to_string(refreshBytesRead) + " B of component state, the walk "
                  "that produced the list read " + std::to_string(walkBytesRead) + " B");

    for (const auto& c : components) {
        std::string line = "thruster module walk[" + label + "]: " + name_or_unresolved(c.className) +
                           " '" + name_or_unresolved(c.objectName) + "' at " + hex_ptr(c.address) +
                           " " + activation_text(c);
        line += niagara_text(c);
        out.push_back(std::move(line));
    }

    if (statusRead) {
        constexpr std::size_t kPerLine = 0x44;
        for (std::size_t start = 0; start < kThrusterModuleStatusBytes; start += kPerLine) {
            std::string line = "thruster module walk[" + label + "]: ModuleStatus bytes " +
                               hex_ptr(static_cast<std::uintptr_t>(start)) + "..";
            const std::size_t end =
                (start + kPerLine < kThrusterModuleStatusBytes) ? start + kPerLine
                                                                : kThrusterModuleStatusBytes;
            char buf[4];
            for (std::size_t i = start; i < end; ++i) {
                std::snprintf(buf, sizeof(buf), "%02X ", status[i]);
                line += buf;
            }
            out.push_back(std::move(line));
        }
    } else if (moduleUsable) {
        out.push_back("thruster module walk[" + label +
                      "]: ModuleStatus NOT read (the 0x88-byte block was refused)");
    }
    return out;
}

}

namespace dsh::game {

std::vector<std::string> class_field_lines(std::uintptr_t classAddress, ModuleComponentReader read,
                                           ModuleNameResolver namer, std::uint64_t* bytesRead) {
    constexpr std::uintptr_t kClass    = 0x0010;
    constexpr std::uintptr_t kName     = 0x0018;
    constexpr std::uintptr_t kNext     = 0x0028;
    constexpr std::uintptr_t kSuper    = 0x0040;
    constexpr std::uintptr_t kChildren = 0x0048;
    constexpr int            kMaxClasses = 32;
    constexpr int            kMaxFields  = 512;

    std::vector<std::string> out;
    if (!read || !namer || classAddress == 0) {
        out.push_back("class fields: NOT listed -- no reader, no namer, or no class address");
        return out;
    }

    int fields = 0;
    int unnamed = 0;
    int classes = 0;
    bool stopped = false;
    std::string stopReason;

    std::uintptr_t uclass = classAddress;
    for (; uclass != 0 && classes < kMaxClasses && !stopped; ++classes) {
        std::string className;
        if (!namer(uclass + kName, className)) className.clear();

        std::uintptr_t field = 0;
        if (!read(uclass + kChildren, &field, sizeof(field))) {
            stopped = true;
            stopReason = "the field chain at could not be read";
            break;
        }
        if (bytesRead) *bytesRead += sizeof(field);

        out.push_back("class fields: -- " + (className.empty() ? std::string("<unnamed class>") : className) +
                      " 0x" + [&] { char b[32]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(uclass)); return std::string(b); }());

        for (int n = 0; field != 0 && n < kMaxFields && !stopped; ++n) {
            std::string name;
            if (!namer(field + kName, name)) { name.clear(); ++unnamed; }

            std::uintptr_t fieldClass = 0;
            std::string fieldClassName;
            if (read(field + kClass, &fieldClass, sizeof(fieldClass))) {
                if (bytesRead) *bytesRead += sizeof(fieldClass);
                if (fieldClass != 0) (void)namer(fieldClass + kName, fieldClassName);
            }

            out.push_back("class fields: " + fieldClassName + " " +
                          (name.empty() ? std::string("<name not resolved>") : name) +
                          " (field 0x" + [&] { char b[32]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(field)); return std::string(b); }() + ")");
            ++fields;

            std::uintptr_t next = 0;
            if (!read(field + kNext, &next, sizeof(next))) {
                stopped = true;
                stopReason = "the field chain broke at";
                break;
            }
            if (bytesRead) *bytesRead += sizeof(next);
            field = next;
        }

        std::uintptr_t super = 0;
        if (!stopped && !read(uclass + kSuper, &super, sizeof(super))) {
            stopped = true;
            stopReason = "UStruct::SuperStruct at could not be read";
            break;
        }
        if (bytesRead) *bytesRead += sizeof(super);
        uclass = super;
    }

    const std::string head =
        "class fields: " + std::to_string(fields) + " field(s) over " + std::to_string(classes) +
        " class(es) starting at 0x" +
        [&] { char b[32]; std::snprintf(b, sizeof(b), "%llX", static_cast<unsigned long long>(classAddress)); return std::string(b); }() +
        (unnamed != 0 ? (", " + std::to_string(unnamed) + " name(s) NOT resolved") : std::string()) +
        (stopped ? (", STOPPED: " + stopReason + " -- the list below is SHORT") : std::string());
    out.insert(out.begin(), head);
    return out;
}

}
