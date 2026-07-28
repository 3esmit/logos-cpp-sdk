#pragma once
// Fixture: records DERIVED from an impl header.
//
// Blob and Wrapper are part of the API (they appear in signatures). Internal
// and Helper are not — they are the kind of private helper struct a real
// module carries (openmetrics has `struct ModuleSource`, the package manager
// has `struct PendingAction` inside the class), and publishing them as
// contract types would change a module's interface as a side effect of an
// internal refactor.
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <logos_module_context.h>

struct Blob {
    std::string          id;
    uint64_t             n;
    std::vector<uint8_t> payload;   // trailing comment: must NOT drop the field
};

struct Wrapper {
    Blob              inner;
    std::vector<Blob> blobs;
};

// Never named by any signature.
struct Internal {
    std::string secret;
};

class RecordsImpl : public LogosModuleContext {
public:
    Blob    echoBlob(const Blob& v);
    Wrapper echoWrapper(const Wrapper& v);
    std::map<std::string, int64_t> echoIntMap(const std::map<std::string, int64_t>& v);

private:
    // A private helper INSIDE the class — also never published.
    struct Helper {
        int64_t count;
    };
};
