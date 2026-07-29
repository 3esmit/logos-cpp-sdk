#pragma once
#include <nlohmann/json.hpp>

// Semantic aliases for nlohmann::json used in universal module impl classes.
// The code generator recognizes these names and emits QVariantMap / QVariantList
// conversions in the Qt glue layer, so impl classes remain Qt-free.
//
// ALIASES ONLY, on purpose. This header also used to define b64UrlEncode,
// b64UrlDecode, bytesToJson and jsonToBytes — second copies of functions
// logos-protocol's logos_codec.h already owned. That was not merely duplication:
// the two sets had the same mangled names with weak linkage and DIFFERENT bodies,
// and both reached one program, because module TUs compiled these while
// liblogos_protocol.a carries TUs that included logos_codec.h. Which body won was
// down to link order.
//
// It also made logos_codec.h unincludable from any TU that wanted LogosMap — a
// redefinition error, since `inline` allows one definition per translation unit,
// not two. That is why the cdylib generator had to emit its own copy of the
// entire codec, and why every codec fix had to be written twice.
//
// The byte helpers now live where they belong: the canonical ones in
// logos-protocol's logos_codec.h, and the lenient `lp`-path jsonToBytes beside
// its sibling jsonToStringVec in logos_lp_client.h.
//
// Keeping this header dependency-free (nlohmann only) is deliberate. Some thirty
// alias-only include sites across the module repos would otherwise inherit an
// include path they have no use for, and logos-cpp-sdkConfig.cmake's "its only
// dependency is nlohmann_json" would stop being true.
using LogosMap  = nlohmann::json;
using LogosList = nlohmann::json;
