#include <gtest/gtest.h>
#include <QJsonArray>
#include <QJsonObject>
#include "generator_lib.h"

static QJsonArray makeTestMethods()
{
    QJsonArray methods;

    // A simple invokable method: int add(int a, int b)
    {
        QJsonObject m;
        m["name"] = "add";
        m["returnType"] = "int";
        m["isInvokable"] = true;
        QJsonArray params;
        {
            QJsonObject p;
            p["type"] = "int";
            p["name"] = "a";
            params.append(p);
        }
        {
            QJsonObject p;
            p["type"] = "int";
            p["name"] = "b";
            params.append(p);
        }
        m["parameters"] = params;
        methods.append(m);
    }

    // A void method with no params
    {
        QJsonObject m;
        m["name"] = "reset";
        m["returnType"] = "void";
        m["isInvokable"] = true;
        m["parameters"] = QJsonArray();
        methods.append(m);
    }

    // A non-invokable method (should be skipped)
    {
        QJsonObject m;
        m["name"] = "internal";
        m["returnType"] = "void";
        m["isInvokable"] = false;
        m["parameters"] = QJsonArray();
        methods.append(m);
    }

    return methods;
}

TEST(MakeHeaderTest, ContainsPragmaOnce)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("#pragma once"));
}

TEST(MakeHeaderTest, ContainsClassName)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("class TestMod"));
}

TEST(MakeHeaderTest, ContainsConstructor)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("explicit TestMod(LogosAPI* api)"));
}

TEST(MakeHeaderTest, ContainsIncludes)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("#include \"logos_types.h\""));
    EXPECT_TRUE(h.contains("#include \"logos_api.h\""));
    EXPECT_TRUE(h.contains("#include \"logos_api_client.h\""));
    EXPECT_TRUE(h.contains("#include \"logos_object.h\""));
    EXPECT_TRUE(h.contains("#include <QVariantList>"));
    EXPECT_TRUE(h.contains("#include <QVariantMap>"));
}

TEST(MakeHeaderTest, ContainsEventCallbackTypedefs)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("RawEventCallback"));
    EXPECT_TRUE(h.contains("EventCallback"));
}

TEST(MakeHeaderTest, ContainsMethodDeclarations)
{
    QJsonArray methods = makeTestMethods();
    QString h = makeHeader("test_mod", "TestMod", methods);

    EXPECT_TRUE(h.contains("int add(int a, int b, logos::CallError* err = nullptr, Timeout timeout = Timeout())"));
    EXPECT_TRUE(h.contains("void reset(logos::CallError* err = nullptr, Timeout timeout = Timeout())"));
    // Non-invokable should not appear
    EXPECT_FALSE(h.contains("internal"));
}

TEST(MakeHeaderTest, ContainsAsyncOverloads)
{
    QJsonArray methods = makeTestMethods();
    QString h = makeHeader("test_mod", "TestMod", methods);

    EXPECT_TRUE(h.contains("addAsync("));
    EXPECT_TRUE(h.contains("resetAsync("));
    // Async callback types
    EXPECT_TRUE(h.contains("std::function<void(int)> callback"));
    EXPECT_TRUE(h.contains("std::function<void()> callback"));
}

TEST(MakeHeaderTest, ContainsPrivateMembers)
{
    QString h = makeHeader("test_mod", "TestMod", QJsonArray());
    EXPECT_TRUE(h.contains("m_api"));
    EXPECT_TRUE(h.contains("m_client"));
    EXPECT_TRUE(h.contains("m_moduleName"));
    // ensureReplica()/m_eventReplica are gone: the deferred subscription
    // channel owns the acquire, and a per-wrapper replica would bring back both
    // the blocking acquire and the permanent failure it caused.
    EXPECT_FALSE(h.contains("ensureReplica"));
    EXPECT_FALSE(h.contains("m_eventReplica"));
}

TEST(MakeHeaderTest, ConstRefForStringParams)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "greet";
        m["returnType"] = "QString";
        m["isInvokable"] = true;
        QJsonArray params;
        QJsonObject p;
        p["type"] = "QString";
        p["name"] = "name";
        params.append(p);
        m["parameters"] = params;
        methods.append(m);
    }

    QString h = makeHeader("mod", "Mod", methods);
    EXPECT_TRUE(h.contains("const QString& name"));
}

TEST(MakeHeaderTest, ConstRefForQVariantListParam)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "process";
        m["returnType"] = "void";
        m["isInvokable"] = true;
        QJsonArray params;
        QJsonObject p;
        p["type"] = "QVariantList";
        p["name"] = "items";
        params.append(p);
        m["parameters"] = params;
        methods.append(m);
    }

    QString h = makeHeader("mod", "Mod", methods);
    EXPECT_TRUE(h.contains("const QVariantList& items"));
}

TEST(MakeHeaderTest, ConstRefForQVariantMapParam)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "update";
        m["returnType"] = "void";
        m["isInvokable"] = true;
        QJsonArray params;
        QJsonObject p;
        p["type"] = "QVariantMap";
        p["name"] = "data";
        params.append(p);
        m["parameters"] = params;
        methods.append(m);
    }

    QString h = makeHeader("mod", "Mod", methods);
    EXPECT_TRUE(h.contains("const QVariantMap& data"));
}

TEST(MakeHeaderTest, QVariantListAsyncOverload)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "getItems";
        m["returnType"] = "QVariantList";
        m["isInvokable"] = true;
        m["parameters"] = QJsonArray();
        methods.append(m);
    }

    QString h = makeHeader("mod", "Mod", methods);
    EXPECT_TRUE(h.contains("QVariantList getItems(logos::CallError* err = nullptr, Timeout timeout = Timeout())"));
    EXPECT_TRUE(h.contains("getItemsAsync("));
    EXPECT_TRUE(h.contains("std::function<void(QVariantList)> callback"));
}

TEST(MakeHeaderTest, QVariantMapAsyncOverload)
{
    QJsonArray methods;
    {
        QJsonObject m;
        m["name"] = "getData";
        m["returnType"] = "QVariantMap";
        m["isInvokable"] = true;
        m["parameters"] = QJsonArray();
        methods.append(m);
    }

    QString h = makeHeader("mod", "Mod", methods);
    EXPECT_TRUE(h.contains("QVariantMap getData(logos::CallError* err = nullptr, Timeout timeout = Timeout())"));
    EXPECT_TRUE(h.contains("getDataAsync("));
    EXPECT_TRUE(h.contains("std::function<void(QVariantMap)> callback"));
}
