#include "mappings.h"

#include "core/embedded_mappings.h"
#include "core/verify.h"

#include <memory>

namespace tome {
namespace {

struct Loaded {
    mappings::Build data;
    BuildMappings bound;
};

struct Database {
    std::vector<std::unique_ptr<Loaded>> builds;
    std::vector<std::string> errors;
};

// Parsed on first use, which is in DllMain: only memory work, no I/O.
const Database& database() {
    static const Database db = [] {
        Database out;
        for (size_t i = 0; i < mappings::kEmbeddedFileCount; ++i) {
            const mappings::EmbeddedFile& file = mappings::kEmbeddedFiles[i];
            std::vector<std::string> errors;
            json::Value root;
            json::ParseResult json = json::parse(file.text, root);
            auto loaded = std::make_unique<Loaded>();
            if (!json.ok) {
                errors.push_back(json.error);
            } else {
                mappings::ParseResult parsed = mappings::parse_build(root, loaded->data);
                errors = std::move(parsed.errors);
                if (parsed.ok) bind_build(loaded->data, loaded->bound, errors);
            }
            if (errors.empty()) {
                out.builds.push_back(std::move(loaded));
            } else {
                for (const std::string& e : errors) out.errors.push_back(std::string(file.name) + ": " + e);
            }
        }
        return out;
    }();
    return db;
}

const IMAGE_NT_HEADERS32* nt_headers(HMODULE module) {
    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : nullptr;
}

} // namespace

const BuildMappings* find_build_by_header(HMODULE game) {
    const IMAGE_NT_HEADERS32* nt = nt_headers(game);
    if (!nt) return nullptr;
    for (const auto& build : database().builds) {
        if (nt->FileHeader.TimeDateStamp == build->data.pe_timestamp &&
            nt->OptionalHeader.SizeOfImage == build->data.size_of_image) {
            return &build->bound;
        }
    }
    return nullptr;
}

const BuildMappings* find_build_by_sha256(const char* sha256) {
    for (const auto& build : database().builds) {
        if (build->data.sha256 == sha256) return &build->bound;
    }
    return nullptr;
}

bool verify_loaded_build(HMODULE game, const BuildMappings& build, std::vector<std::string>& errors) {
    const IMAGE_NT_HEADERS32* nt = nt_headers(game);
    if (!nt) {
        errors.push_back("game.dll has no PE header");
        return false;
    }
    std::vector<std::string> found = mappings::verify_code(*build.data, reinterpret_cast<const uint8_t*>(game),
                                                           nt->OptionalHeader.SizeOfImage);
    errors.insert(errors.end(), found.begin(), found.end());
    return found.empty();
}

const std::vector<std::string>& mapping_errors() {
    return database().errors;
}

} // namespace tome
