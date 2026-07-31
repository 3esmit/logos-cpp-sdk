#pragma once
// Fixture: OPTIONALITY derived from an impl header.
//
// The header-first C++ provider is the one authoring path where the contract is
// read out of the source rather than written by hand, so `std::optional<T>` has
// to be recognised — before this fixture existed it fell through to the opaque
// `any` fallback with no diagnostic, and a module that declared an optional
// published a contract that said something else.
//
// `required` is here to hold the other half down: only the optional slots may
// become `?T`. And `maybeBlob` covers the case that has to compose — an optional
// over a declared record, not just over a scalar.
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <logos_module_context.h>

struct Blob {
    std::string          id;
    std::vector<uint8_t> payload;
};

struct Profile {
    std::string                         required;
    std::optional<std::string>          nickname;
    std::optional<uint64_t>             age;
    std::optional<std::vector<uint8_t>> avatar;
    std::optional<Blob>                 blob;
};
// `std::optional<std::optional<T>>` is deliberately NOT here: it has no LIDL
// type (three C++ states over a two-state wire), so the generated codec is
// written for the collapsed `?T` and does not compile against such a member.
// That is the intended outcome, and it is pinned by
// ImplHeaderParser.NestedOptionalCollapsesAndIsReported — a fixture used to
// prove the emitted code COMPILES cannot also carry a case that must not.

class OptionalImpl : public LogosModuleContext {
public:
    Profile                    echoProfile(const Profile& v);
    std::optional<std::string> echoOptional(const std::optional<std::string>& v);
    std::optional<Blob>        maybeBlob(const std::string& id);
    std::vector<std::optional<std::string>> echoOptionalList(
        const std::vector<std::optional<std::string>>& v);
    std::string                required(const std::string& v);

logos_events:
    void profileChanged(const std::string& id, const std::optional<std::string>& nickname);
};
