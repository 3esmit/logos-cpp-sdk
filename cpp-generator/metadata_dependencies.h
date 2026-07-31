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

#endif // METADATA_DEPENDENCIES_H
