#include "impl_header_parser.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStringList>

// ---------------------------------------------------------------------------
// Strip leading declaration specifiers / attributes from a return-type string.
// ---------------------------------------------------------------------------

static QString stripDeclarationSpecifiers(QString string)
{
    static const QRegularExpression attributeRe("\\[\\[[^\\]]*\\]\\]");
    static const QRegularExpression specifierRe(
        "^(static|virtual|inline|explicit|constexpr|consteval|friend)\\s+");

    string.remove(attributeRe);
    string = string.trimmed();

    QRegularExpressionMatch specifierMatch = specifierRe.match(string);
    while (specifierMatch.hasMatch()) {
        string = string.mid(specifierMatch.capturedLength()).trimmed();
        specifierMatch = specifierRe.match(string);
    }

    return string;
}

// ---------------------------------------------------------------------------
// C++ type string → LIDL TypeExpr
// ---------------------------------------------------------------------------

static TypeExpr cppTypeToLidl(const QString& raw)
{
    // Normalize: strip const, &, leading/trailing whitespace
    QString t = raw.trimmed();
    t.remove(QRegularExpression("^const\\s+"));
    t.remove(QRegularExpression("\\s*&$"));
    t = t.trimmed();

    // Primitives
    if (t == "bool")     return { TypeExpr::Primitive, "bool", {} };
    if (t == "void")     return { TypeExpr::Primitive, "void", {} };

    // Numbers are 64-bit ONLY: int64_t, uint64_t, double. Narrower spellings are
    // NOT auto-widened — a uint32_t parameter is a build error telling the author
    // to write uint64_t, rather than a silent widening that makes the declared
    // C++ type and the published LIDL contract disagree about range. uint8_t has
    // exactly one meaning in this contract, and it is std::vector<uint8_t> =
    // bstr, handled below.
    if (t == "int64_t")  return { TypeExpr::Primitive, "int", {} };
    if (t == "uint64_t") return { TypeExpr::Primitive, "uint", {} };
    if (t == "double")   return { TypeExpr::Primitive, "float64", {} };

    // std::string
    if (t == "std::string")
        return { TypeExpr::Primitive, "tstr", {} };

    // std::vector<T> — recursive: the element is parsed by the same function, so
    // [T] composes to any depth ([[bstr]], [{tstr: [int]}], …) without this
    // table having to enumerate the combinations. std::vector<uint8_t> is the
    // one exception: it IS `bstr`, not an array of uint.
    static QRegularExpression vecRe("^std::vector\\s*<\\s*(.+)\\s*>$");
    QRegularExpressionMatch m = vecRe.match(t);
    if (m.hasMatch()) {
        const QString inner = m.captured(1).trimmed();
        if (inner == "uint8_t")
            return { TypeExpr::Primitive, "bstr", {} };
        return { TypeExpr::Array, "", { cppTypeToLidl(inner) } };
    }

    // std::map / std::unordered_map<std::string, T> — recursive on the value.
    // Only string-keyed maps are representable ({tstr: T}); any other key type
    // falls through to the unsupported marker below.
    static QRegularExpression mapRe(
        "^std::(?:unordered_)?map\\s*<\\s*std::string\\s*,\\s*(.+)\\s*>$");
    QRegularExpressionMatch mm = mapRe.match(t);
    if (mm.hasMatch()) {
        return { TypeExpr::Map, "",
                 { { TypeExpr::Primitive, "tstr", {} }, cppTypeToLidl(mm.captured(1).trimmed()) } };
    }

    // Qt collection types — pass through directly (non-std-convertible)
    if (t == "QVariantMap")
        return { TypeExpr::Map, "", { {TypeExpr::Primitive, "tstr", {}}, {TypeExpr::Primitive, "any", {}} } };
    if (t == "QVariantList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "any", {}} } };
    if (t == "QStringList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "tstr", {}} } };

    // LogosMap / LogosList — nlohmann::json aliases; same LIDL shape as the Qt types
    // but flagged so the generator emits an nlohmann→Qt conversion in the glue.
    if (t == "LogosMap")
        return { TypeExpr::Map, "", { {TypeExpr::Primitive, "tstr", {}}, {TypeExpr::Primitive, "any", {}} } };
    if (t == "LogosList")
        return { TypeExpr::Array, "", { {TypeExpr::Primitive, "any", {}} } };

    // The alias spelled out. LogosMap/LogosList ARE nlohmann::json, and modules
    // do write the underlying name (test_fullapi_cpp's echoAny, the full_api
    // interface headers). It only survived via the opaque fallback below, so it
    // has to be named explicitly now that the fallback is an error.
    if (t == "nlohmann::json" || t == "json")
        return { TypeExpr::Primitive, "any", {} };

    // StdLogosResult — pure C++ result type for universal impls. The generator
    // emits a StdLogosResult→Qt LogosResult conversion in the glue layer.
    if (t == "StdLogosResult")
        return { TypeExpr::Primitive, "result", {} };

    // Anything else is UNSUPPORTED, and says so by name.
    //
    // This used to return the opaque primitive `any`, which the cdylib gate
    // admits — so an unrecognised spelling was silently accepted and then either
    // worked by luck through nlohmann's implicit conversions, threw at call time,
    // or (worst) emitted a non-canonical wire value: a
    // std::vector<std::vector<std::vector<uint8_t>>> return went out as untagged
    // nested number arrays that no consumer decodes as bytes.
    //
    // `Named` carries the offending C++ spelling, and no backend accepts a Named
    // type, so the module fails to BUILD with the parameter and the type in the
    // message. Compatible spellings are enumerated above; the composition rule
    // (vector<T>, map<string,T>) is recursive, so this fires only for types that
    // genuinely have no canonical JSON form (std::pair, std::set, std::optional,
    // custom structs, pointers, Qt types in a Qt-free module).
    return { TypeExpr::Named, t.toStdString(), {} };
}

// ---------------------------------------------------------------------------
// Parse a single method declaration line
// ---------------------------------------------------------------------------

static bool parseMethodLine(const QString& line, MethodDecl& out)
{
    // Find the parameter list: everything between the last '(' and ')'
    int parenOpen = -1;
    int parenClose = -1;
    int depth = 0;
    for (int i = line.size() - 1; i >= 0; --i) {
        if (line[i] == ')') {
            if (parenClose < 0) parenClose = i;
            depth++;
        } else if (line[i] == '(') {
            depth--;
            if (depth == 0) {
                parenOpen = i;
                break;
            }
        }
    }
    if (parenOpen < 0 || parenClose < 0)
        return false;

    QString paramStr = line.mid(parenOpen + 1, parenClose - parenOpen - 1).trimmed();

    // Everything before '(' is "returnType methodName"
    QString prefix = line.left(parenOpen).trimmed();

    // The method name is the last identifier token in prefix
    int nameEnd = prefix.size();
    while (nameEnd > 0 && prefix[nameEnd - 1].isSpace())
        nameEnd--;
    int nameStart = nameEnd;
    while (nameStart > 0 && (prefix[nameStart - 1].isLetterOrNumber() || prefix[nameStart - 1] == '_'))
        nameStart--;

    if (nameStart >= nameEnd)
        return false;

    const QString methodName = prefix.mid(nameStart, nameEnd - nameStart);

    // Reject if the extracted name is a C++ keyword — this filters out
    // member variable declarations like "std::function<void(...)> onEvent"
    // where the parser would mistakenly extract "void" as the method name.
    static const QSet<QString> cppKeywords = {
        "void", "int", "bool", "char", "short", "long", "double", "float",
        "auto", "return", "if", "else", "for", "while", "do", "switch",
        "case", "break", "continue", "const", "static", "inline", "virtual"
    };
    if (cppKeywords.contains(methodName))
        return false;
    out.name = methodName.toStdString();
    QString retTypeStr = stripDeclarationSpecifiers(prefix.left(nameStart).trimmed());
    out.returnType = cppTypeToLidl(retTypeStr);
    // Flag methods whose impl returns LogosMap / LogosList so the generator
    // can emit nlohmann→Qt conversion code in the glue layer.
    out.jsonReturn = (retTypeStr == "LogosMap" || retTypeStr == "LogosList");
    // Flag methods whose impl returns StdLogosResult so the generator can
    // emit a StdLogosResult→Qt LogosResult conversion in the glue layer.
    out.resultReturn = (retTypeStr == "StdLogosResult");

    // Parse parameters
    out.params.clear();
    if (!paramStr.isEmpty()) {
        // Split by comma, respecting template depth
        QStringList parts;
        int start = 0;
        int tdepth = 0;
        for (int i = 0; i < paramStr.size(); ++i) {
            if (paramStr[i] == '<') tdepth++;
            else if (paramStr[i] == '>') tdepth--;
            else if (paramStr[i] == ',' && tdepth == 0) {
                parts.append(paramStr.mid(start, i - start).trimmed());
                start = i + 1;
            }
        }
        parts.append(paramStr.mid(start).trimmed());

        for (const QString& part : parts) {
            if (part.isEmpty()) continue;
            QString p = part.trimmed();
            int pNameEnd = p.size();
            while (pNameEnd > 0 && p[pNameEnd - 1].isSpace())
                pNameEnd--;
            int pNameStart = pNameEnd;
            while (pNameStart > 0 && (p[pNameStart - 1].isLetterOrNumber() || p[pNameStart - 1] == '_'))
                pNameStart--;

            if (pNameStart >= pNameEnd) continue;

            ParamDecl pd;
            pd.name = p.mid(pNameStart, pNameEnd - pNameStart).toStdString();
            pd.type = cppTypeToLidl(p.left(pNameStart));
            out.params.push_back(pd);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Main entry point
// ---------------------------------------------------------------------------

// Join doc-comment lines preserving line breaks (drop leading/trailing blanks).
static QString joinDocLines(QStringList lines)
{
    while (!lines.isEmpty() && lines.first().trimmed().isEmpty()) lines.removeFirst();
    while (!lines.isEmpty() && lines.last().trimmed().isEmpty()) lines.removeLast();
    return lines.join('\n');
}

ImplParseResult parseImplHeader(const QString& headerPath,
                                const QString& className,
                                const QString& metadataPath,
                                QTextStream& err)
{
    ImplParseResult result;

    // --- Read metadata.json ---
    {
        QFile mf(metadataPath);
        if (!mf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            result.error = "Failed to open metadata file: " + metadataPath;
            return result;
        }
        QJsonParseError pe;
        QJsonDocument doc = QJsonDocument::fromJson(mf.readAll(), &pe);
        if (pe.error != QJsonParseError::NoError) {
            result.error = "Failed to parse metadata JSON: " + pe.errorString();
            return result;
        }
        QJsonObject obj = doc.object();
        result.module.name = obj.value("name").toString().toStdString();
        result.module.version = obj.value("version").toString().toStdString();
        result.module.description = obj.value("description").toString().toStdString();
        result.module.category = obj.value("category").toString().toStdString();
        QJsonArray deps = obj.value("dependencies").toArray();
        for (const QJsonValue& v : deps)
            result.module.depends.push_back(v.toString().toStdString());

        // Read events declared in metadata.json
        QJsonArray events = obj.value("events").toArray();
        for (const QJsonValue& ev : events) {
            QJsonObject evObj = ev.toObject();
            EventDecl ed;
            ed.name = evObj.value("name").toString().toStdString();
            ed.description = evObj.value("description").toString().toStdString();
            QJsonArray params = evObj.value("params").toArray();
            for (const QJsonValue& pv : params) {
                QJsonObject po = pv.toObject();
                ParamDecl pd;
                pd.name = po.value("name").toString().toStdString();
                pd.type = cppTypeToLidl(po.value("type").toString());
                ed.params.push_back(pd);
            }
            if (!ed.name.empty())
                result.module.events.push_back(ed);
        }
    }

    // --- Read and parse header ---
    QFile hf(headerPath);
    if (!hf.open(QIODevice::ReadOnly | QIODevice::Text)) {
        result.error = "Failed to open header file: " + headerPath;
        return result;
    }
    QString source = QString::fromUtf8(hf.readAll());
    hf.close();

    // Split into physical lines, then merge any whose parentheses are still
    // open into one logical line. The scanner below is line-based — it only
    // accepts a method when a single trimmed line ends in ';' and
    // parseMethodLine finds a balanced '(...)' on it — so without this a method
    // signature wrapped across several physical lines is silently dropped.
    // Parens inside comments / string / char literals are ignored.
    QStringList lines;
    {
        const QStringList physical = source.split('\n');
        QString acc;
        int parenDepth = 0;
        bool inBlockComment = false;
        for (const QString& phys : physical) {
            bool inStr = false;
            bool inChr = false;
            for (int i = 0; i < phys.size(); ++i) {
                const QChar c = phys[i];
                const QChar n = (i + 1 < phys.size()) ? phys[i + 1] : QChar();
                if (inBlockComment) {
                    if (c == '*' && n == '/') { inBlockComment = false; ++i; }
                } else if (inStr) {
                    if (c == '\\') ++i; else if (c == '"') inStr = false;
                } else if (inChr) {
                    if (c == '\\') ++i; else if (c == '\'') inChr = false;
                } else if (c == '/' && n == '*') {
                    inBlockComment = true; ++i;
                } else if (c == '/' && n == '/') {
                    break;
                } else if (c == '"') {
                    inStr = true;
                } else if (c == '\'') {
                    inChr = true;
                } else if (c == '(') {
                    ++parenDepth;
                } else if (c == ')') {
                    if (parenDepth > 0) --parenDepth;
                }
            }
            if (acc.isEmpty())
                acc = phys;
            else
                acc += ' ' + phys.trimmed();
            if (parenDepth <= 0) {
                lines.append(acc);
                acc.clear();
            }
        }
        if (!acc.isEmpty())
            lines.append(acc);
    }

    // State machine: find "class <className>", then collect declarations.
    // `InLogosEvents` is entered by the literal `logos_events:` token
    // (mirrors Qt's `signals:`) — methods declared there are parsed as
    // EventDecls and appended to ModuleDecl.events instead of .methods.
    enum State { LookingForClass, InClass, InPublic, InPrivate, InLogosEvents };
    State state = LookingForClass;
    int braceDepth = 0;

    // Accumulates doc-comment lines adjacent to a method so the doc comment
    // becomes the method's description. Reset on any blank / non-comment line.
    QStringList pendingDoc;
    bool inBlockComment = false;

    // Record being accumulated (see LookingForClass below).
    TypeDecl record;
    bool inRecord = false;

    QRegularExpression classRe("\\bclass\\s+" + QRegularExpression::escape(className) + "\\b");
    QRegularExpression accessRe("^\\s*(public|private|protected)\\s*:");
    QRegularExpression eventsRe("^\\s*logos_events\\s*:");
    QRegularExpression ctorDtorRe("^\\s*~?" + QRegularExpression::escape(className) + "\\s*\\(");

    for (const QString& rawLine : lines) {
        QString line = rawLine.trimmed();

        switch (state) {
        case LookingForClass:
            // A plain `struct Foo { T a; U b; };` ahead of the impl class is a
            // RECORD: a named wire shape the module's methods can take and
            // return. LIDL has carried TypeDecl/FieldDecl all along (chat_module
            // declares records in a hand-written .lidl) — this is the
            // impl-header path finally producing them, so an author writes a
            // struct instead of an untyped LogosMap.
            if (inRecord) {
                if (line.startsWith("}")) {
                    if (!record.fields.empty())
                        result.module.types.push_back(record);
                    inRecord = false;
                } else {
                    static QRegularExpression fieldRe(
                        "^([A-Za-z_][A-Za-z0-9_:<>,\\s\\*&]*?)\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*;$");
                    const QRegularExpressionMatch fm = fieldRe.match(line);
                    if (fm.hasMatch()) {
                        FieldDecl f;
                        f.name = fm.captured(2).toStdString();
                        f.type = cppTypeToLidl(fm.captured(1).trimmed());
                        record.fields.push_back(f);
                    }
                }
                break;
            }
            {
                static QRegularExpression recordRe(
                    "^struct\\s+([A-Za-z_][A-Za-z0-9_]*)\\s*\\{\\s*$");
                const QRegularExpressionMatch rm = recordRe.match(line);
                if (rm.hasMatch()) {
                    record = TypeDecl();
                    record.name = rm.captured(1).toStdString();
                    inRecord = true;
                    break;
                }
            }
            if (classRe.match(line).hasMatch()) {
                state = InClass;
                for (QChar c : line) {
                    if (c == '{') braceDepth++;
                    else if (c == '}') braceDepth--;
                }
            }
            break;

        case InClass:
        case InPublic:
        case InPrivate:
        case InLogosEvents:
            // Inside a multi-line /** ... */ doc-comment block: capture its
            // text (skip brace counting — comments don't affect scope).
            if (inBlockComment) {
                QString t = line;
                int end = t.indexOf("*/");
                if (end >= 0) { t = t.left(end); inBlockComment = false; }
                t.remove(QRegularExpression(R"(^\*+\s?)"));
                t = t.trimmed();
                pendingDoc.append(t);
                break;
            }

            // Count braces only on real code lines. Braces inside a doc/line
            // comment (e.g. `/// returns { "k": v }`) must not affect scope
            // tracking, or an unbalanced brace in a comment would make the
            // parser think the class ended early and drop later declarations.
            if (!(line.startsWith("//") || line.startsWith("/*") || line.startsWith("*"))) {
                for (QChar c : line) {
                    if (c == '{') braceDepth++;
                    else if (c == '}') braceDepth--;
                }

                if (braceDepth <= 0) {
                    state = LookingForClass;
                    goto done;
                }
            }

            // A section specifier may be followed by a declaration on the
            // *same* physical line — e.g. clang-format / prettier collapse
            //     logos_events:
            //         void versionReady(const std::string& version);
            // into `logos_events : void versionReady(const std::string& version);`.
            // Strip any leading specifiers, updating the section state, and
            // let whatever remains fall through to the declaration parser
            // below — otherwise everything after the colon is discarded and
            // the same valid C++ is parsed differently based on formatting.
            //
            // `logos_events:` takes precedence over the standard access
            // specifiers: it's a separate section that the codegen pulls
            // event prototypes from. (At preprocess time, `logos_events`
            // expands to `public`, but the raw source still carries the
            // token we recognise here.)
            bool specifierStripped = false;
            while (true) {
                QRegularExpressionMatch em = eventsRe.match(line);
                if (em.hasMatch()) {
                    state = InLogosEvents;
                    line = line.mid(em.capturedEnd()).trimmed();
                    specifierStripped = true;
                    continue;
                }
                QRegularExpressionMatch am = accessRe.match(line);
                if (am.hasMatch()) {
                    QString spec = am.captured(1);
                    if (spec == "public") state = InPublic;
                    else state = InPrivate;
                    line = line.mid(am.capturedEnd()).trimmed();
                    specifierStripped = true;
                    continue;
                }
                break;
            }
            // A *bare* specifier (nothing after the colon) is a section
            // boundary and resets any pending doc-comment, mirroring Qt's
            // `signals:`. But when a declaration shares the line, the doc
            // comment preceding the whole line must still attach to that
            // declaration — otherwise documentation, like the declaration
            // itself (#76), would become formatting-dependent. So only clear
            // here for the bare form; the same-line form keeps pendingDoc and
            // attaches it in the declaration parser below.
            if (specifierStripped && line.isEmpty())
                pendingDoc.clear();

            // Only doc comments (/// or /** ... */ / /*! ... */) accumulate as
            // the pending description for the next method. Plain // and /*
            // comments are ignored but leave pending doc intact; blank /
            // preprocessor lines reset it so only *adjacent* comments attach.
            if (line.startsWith("///")) {
                QString text = line.mid(3);
                if (text.startsWith('<')) text = text.mid(1); // ///< trailing form
                text = text.trimmed();
                pendingDoc.append(text);
                break;
            }
            if (line.startsWith("/**") || line.startsWith("/*!")) {
                QString text = line.mid(3);
                int end = text.indexOf("*/");
                if (end >= 0) text = text.left(end);
                else inBlockComment = true;
                text.remove(QRegularExpression(R"(^\*+\s?)"));
                text = text.trimmed();
                pendingDoc.append(text);
                break;
            }
            if (line.startsWith("//") || line.startsWith("/*") || line.startsWith("*")) {
                break; // non-doc comment: ignore, keep pending doc
            }
            if (line.isEmpty() || line.startsWith("#")) {
                pendingDoc.clear();
                break;
            }

            if (ctorDtorRe.match(line).hasMatch()) {
                pendingDoc.clear();
                break;
            }

            if (line.startsWith("typedef") || line.startsWith("using")
                || line.startsWith("friend") || line.startsWith("enum")
                || line.startsWith("struct")) {
                pendingDoc.clear();
                break;
            }

            if (state == InLogosEvents) {
                // Inside `logos_events:` — every bare prototype is an event.
                // Events are always void-returning by definition, so we
                // re-use parseMethodLine to extract name + params and
                // discard the return type.
                if (line.endsWith(';')) {
                    QString decl = line.left(line.size() - 1).trimmed();
                    MethodDecl md;
                    if (parseMethodLine(decl, md)) {
                        EventDecl ed;
                        ed.name = md.name;
                        ed.params = md.params;
                        ed.description = joinDocLines(pendingDoc).toStdString();
                        result.module.events.push_back(ed);
                    }
                }
                pendingDoc.clear();
                break;
            }

            if (state != InPublic) { pendingDoc.clear(); break; }

            if (line.contains("std::function<")) {
                // A std::function member is not a method — skip it so the
                // `parseMethodLine` path below doesn't choke on the nested
                // parens in its type. (Events are declared in a typed
                // `logos_events:` section, parsed above — there is no longer
                // any special `std::function emitEvent` member to detect.)
                pendingDoc.clear();
                break;
            }

            if (line.endsWith(';')) {
                QString decl = line.left(line.size() - 1).trimmed();
                MethodDecl md;
                if (parseMethodLine(decl, md)) {
                    // LogosModuleContext lifecycle hooks / context accessors are
                    // framework plumbing, not part of the module's API contract.
                    // An impl commonly overrides `onContextReady()` (and could
                    // re-declare an accessor) in its own public section, so the
                    // header parser would otherwise emit them into the derived
                    // LIDL — breaking cdylib eligibility (e.g. the inherited
                    // accessors' Qt-free-subset check) and exposing non-API
                    // methods. Skip the reserved names regardless of access.
                    static const QSet<QString> reserved = {
                        "onContextReady", "modules", "modulePath",
                        "instanceId", "instancePersistencePath"
                    };
                    if (!reserved.contains(qs(md.name))) {
                        md.description = joinDocLines(pendingDoc).toStdString();
                        result.module.methods.push_back(md);
                    }
                }
            }
            pendingDoc.clear();
            break;
        }
    }

done:
    if (result.module.methods.empty()) {
        err << "Warning: no public methods found in class " << className
            << " in " << headerPath << "\n";
    }

    return result;
}
