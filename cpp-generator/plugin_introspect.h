#ifndef PLUGIN_INTROSPECT_H
#define PLUGIN_INTROSPECT_H

// The QPluginLoader-introspection mode: given a BUILT plugin, walk its
// QMetaObject and emit a consumer wrapper for it. main() falls through to this
// when its own modes do not claim the arguments.
//
// This lived in `legacy/main.cpp` behind `legacy_main()`. The directory was not
// a legacy library — it held exactly one exported symbol and one reachable
// mode — so it was merged here rather than kept as a parallel implementation.
int runPluginIntrospectMode(int argc, char* argv[]);

#endif // PLUGIN_INTROSPECT_H
