# CLI-level assertions about logos-cpp-generator's ARGUMENT SURFACE.
#
# The gtest suite in nix/tests.nix links the generator's internals; it never
# runs the binary, so a flag that was removed from the CLI cannot be asserted
# there. This check runs the real `logos-cpp-generator` and looks at EXIT CODES.
#
# Why exit codes and not output diffing: `--module-dir` had no caller left in
# any nix build, so removing it changes no build output anywhere — a
# store-path diff of every module in the tree would be empty either way and
# would prove nothing. The only observable difference is what the binary does
# when handed the flag, and that is exactly what this asserts.
{ pkgs, common, generator }:

pkgs.runCommand "${common.pname}-generator-cli-tests"
  {
    nativeBuildInputs = [ generator ];
    meta = common.meta;
  }
  ''
    set -u
    fail() { echo "FAIL: $*" >&2; exit 1; }

    mkdir -p work/modules && cd work
    cat > metadata.json <<'EOF'
    {
      "name": "cli_probe_module",
      "version": "1.0.0",
      "type": "core",
      "dependencies": ["dep_one", "dep_two"]
    }
    EOF

    # ── Positive control ──────────────────────────────────────────────────
    # Same binary, same metadata, no `--module-dir`: must exit 0 and list the
    # dependencies. Without this, a non-zero exit below could just as well mean
    # "the binary is broken" or "the metadata is unreadable".
    # NB: never assign to `out` here — that is the derivation's output path.
    set +e
    listing=$(logos-cpp-generator --metadata ./metadata.json 2>err.txt)
    control_status=$?
    set -e
    if [ "$control_status" -ne 0 ]; then
      echo "--- stderr ---" >&2; cat err.txt >&2
      fail "control: the generator refused a plain --metadata run (exit $control_status)"
    fi
    echo "$listing" | grep -qx 'dep_one' || fail "control: dep_one missing from the dependency listing"
    echo "$listing" | grep -qx 'dep_two' || fail "control: dep_two missing from the dependency listing"
    echo "OK: control — --metadata alone exits 0 and lists dependencies"

    # ── The assertion ─────────────────────────────────────────────────────
    # `--module-dir <dir>` was the multi-dependency plugin-introspection mode.
    # It must now be REFUSED, not ignored: a silent fall-through to the listing
    # above would exit 0 having generated nothing.
    set +e
    logos-cpp-generator --metadata ./metadata.json --module-dir ./modules \
      >moddir.out 2>moddir.err
    status=$?
    set -e

    if [ "$status" -eq 0 ]; then
      echo "--- stdout ---" >&2; cat moddir.out >&2
      fail "--module-dir exited 0; the removed flag is still accepted"
    fi
    echo "OK: --module-dir exits non-zero (status=$status)"

    grep -q -- '--module-dir was removed' moddir.err \
      || { echo "--- stderr ---" >&2; cat moddir.err >&2
           fail "--module-dir failed, but not with the removal diagnostic"; }
    echo "OK: --module-dir fails with the removal diagnostic"

    # An existing directory must not change the answer — the old code only
    # errored when the directory was MISSING (exit 2 from the QDir::exists
    # check), so a passing test against a nonexistent path would prove nothing.
    if [ ! -d ./modules ]; then fail "fixture: ./modules should exist"; fi

    # `--general-only` is the supported replacement and must still work, so the
    # refusal above is a removal of one mode rather than of the metadata path.
    logos-cpp-generator --metadata ./metadata.json --general-only \
      --output-dir ./gen >/dev/null 2>generalonly.err \
      || { cat generalonly.err >&2; fail "--general-only regressed"; }
    [ -s ./gen/logos_sdk.h ] || fail "--general-only emitted no logos_sdk.h"
    echo "OK: --general-only still emits the umbrella"

    # ── `--binding origin`: the umbrella a module with no LogosAPI needs ──
    #
    # Emitter-level assertions live in the gtest suite; these are the ones only
    # the BINARY can answer — that the flag is wired to the mode at all, that an
    # unrecognised value is refused rather than defaulted, and that a module
    # with no name of its own is refused rather than given a blank identity.
    cat > origin_metadata.json <<'EOF'
    {
      "name": "cli_origin_module",
      "version": "1.0.0",
      "type": "core",
      "dependencies": ["dep_one", "dep_two"]
    }
    EOF

    logos-cpp-generator --metadata ./origin_metadata.json --general-only       --api-style qt --binding origin --output-dir ./gen-origin       >/dev/null 2>origin.err       || { cat origin.err >&2; fail "--binding origin was refused"; }
    [ -s ./gen-origin/logos_sdk.h ] || fail "--binding origin emitted no logos_sdk.h"

    # Default-constructible, so the cdylib glue's `new LogosModules()` compiles.
    grep -q 'LogosModules() : dep_one(QStringLiteral("cli_origin_module"))'       ./gen-origin/logos_sdk.h       || { cat ./gen-origin/logos_sdk.h >&2
           fail "the origin-bound umbrella is not default-constructible"; }

    # THE property: the origin is this module's OWN name, never an api object's.
    # `forTarget` derives an origin from `api->moduleName()`, and a wrapper
    # built on a borrowed api calls out under the lender's identity — so the
    # umbrella must hand every wrapper a stated name and hold no LogosAPI at all.
    if grep -q 'LogosAPI' ./gen-origin/logos_sdk.h; then
      cat ./gen-origin/logos_sdk.h >&2
      fail "the origin-bound umbrella still mentions LogosAPI"
    fi
    grep -q 'dep_two(QStringLiteral("cli_origin_module"))' ./gen-origin/logos_sdk.h       || fail "a dependency was not handed the consuming module's own name"
    echo "OK: --binding origin emits a default-constructible, LogosAPI-free umbrella"

    # The default is unchanged — same metadata, no flag, the historical shape.
    logos-cpp-generator --metadata ./origin_metadata.json --general-only       --api-style qt --output-dir ./gen-api >/dev/null 2>&1       || fail "the default (LogosAPI) umbrella regressed"
    grep -q 'explicit LogosModules(LogosAPI\* api)' ./gen-api/logos_sdk.h       || { cat ./gen-api/logos_sdk.h >&2
           fail "the default umbrella is no longer the LogosAPI-taking one"; }
    echo "OK: the default binding still emits the LogosAPI umbrella"

    # A misspelt value is refused. Defaulting it back to the LogosAPI form would
    # emit `LogosModules(LogosAPI*)` into a module that has none, and the
    # diagnostic would land as a constructor mismatch in generated code.
    set +e
    logos-cpp-generator --metadata ./origin_metadata.json --general-only       --api-style qt --binding orgin --output-dir ./gen-bad >badbinding.out 2>badbinding.err
    status=$?
    set -e
    [ "$status" -ne 0 ] || fail "--binding orgin (misspelt) exited 0"
    grep -q -- 'Unknown --binding value' badbinding.err       || { cat badbinding.err >&2; fail "a bad --binding failed without saying why"; }
    echo "OK: an unrecognised --binding is refused"

    # A module with no name cannot state an origin, and must not be given a
    # blank one. Refused at the CLI, where the metadata file can be named.
    cat > anonymous_metadata.json <<'EOF'
    {
      "version": "1.0.0",
      "type": "core",
      "dependencies": ["dep_one"]
    }
    EOF
    set +e
    logos-cpp-generator --metadata ./anonymous_metadata.json --general-only       --api-style qt --binding origin --output-dir ./gen-anon >anon.out 2>anon.err
    status=$?
    set -e
    [ "$status" -ne 0 ] || fail "--binding origin accepted metadata with no name"
    grep -q "asserted" anon.err       || { cat anon.err >&2; fail "the anonymous-origin refusal does not explain itself"; }
    echo "OK: --binding origin refuses a module that cannot name itself"

    mkdir -p "$out"
    echo "logos-cpp-generator CLI argument-surface tests passed" > "$out/result.txt"
  ''
