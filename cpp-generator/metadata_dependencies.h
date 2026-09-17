#ifndef METADATA_DEPENDENCIES_H
#define METADATA_DEPENDENCIES_H

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

/// The module named by one `metadata.json` `dependencies[]` element.
///
/// An element is either a bare name or an object holding that name alongside
/// the constraints an installer resolves it by (version range, signer DID);
/// generation needs the name only. Empty for an element that names nothing.
inline QString dependencyName(const QJsonValue& entry)
{
    if (entry.isString()) {
        return entry.toString();
    }
    if (entry.isObject()) {
        return entry.toObject().value("name").toString();
    }
    return QString();
}

/// Every module named by a `metadata.json` `dependencies[]` array, in order.
///
/// Read the array through this rather than iterating it: an emitter that walks
/// `deps` itself decides on its own what an element names, and one that decides
/// differently from its neighbours emits an aggregate whose members and includes
/// disagree — which does not fail until the generated code is compiled.
/// Elements that name nothing are dropped.
inline QStringList dependencyNames(const QJsonArray& entries)
{
    QStringList names;
    for (const QJsonValue& entry : entries) {
        const QString name = dependencyName(entry);
        if (!name.isEmpty()) {
            names.append(name);
        }
    }
    return names;
}

/// Every module a consumer gets a typed `modules().<name>` member for: the
/// union of `dependencies` and `optional_dependencies`, in that order.
///
/// The two kinds differ only in LIFETIME — an optional dependency is not
/// auto-loaded and its absence is not a load error — and lifetime is not
/// something the generated wrapper can express. So a consumer surface that
/// split them would carry two spellings for one call.
inline QJsonArray consumerDependencyEntries(const QJsonObject& metadata)
{
    QJsonArray all = metadata.value("dependencies").toArray();
    for (const QJsonValue& entry : metadata.value("optional_dependencies").toArray()) {
        all.append(entry);
    }
    return all;
}

/// The umbrella's member list, preferring the `--dep` flags to metadata.json.
///
/// Both name the same modules, but the flags are RESOLVED: nix expands them per
/// platform and per dependency kind, so a member list built from them needs no
/// second opinion about what a dependency is. Re-deriving it here is how the
/// two came to disagree — a new dependency kind reached `--dep` through the
/// builder while this binary still read `dependencies` alone, and the consuming
/// module compiled with a wrapper it had no member for.
///
/// metadata.json stays the source when no flag is passed, which is the raw
/// dev-shell path: LogosModule.cmake invokes with `--metadata` alone under
/// `if(LOGOS_CPP_SDK_IS_SOURCE)`.
inline QJsonArray umbrellaDependencyEntries(const QStringList& depFlagNames,
                                            const QJsonObject& metadata)
{
    if (depFlagNames.isEmpty()) {
        return consumerDependencyEntries(metadata);
    }
    QJsonArray all;
    for (const QString& name : depFlagNames) {
        all.append(name);
    }
    return all;
}

#endif // METADATA_DEPENDENCIES_H
