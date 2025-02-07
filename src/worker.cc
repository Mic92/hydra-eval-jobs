#include <nix/config.h> // IWYU pragma: keep

// doesn't exist on macOS
// IWYU pragma: no_include <bits/types/struct_rusage.h>

#include <nix/eval-error.hh>
#include <nix/pos-idx.hh>
#include <nix/terminal.hh>
#include <nix/attr-path.hh>
#include <nix/local-fs-store.hh>
#include <nix/installable-flake.hh>
#include <nix/value-to-json.hh>
#include <sys/resource.h>
#include <nlohmann/json.hpp>
#include <cstdio>
#include <iostream>
// NOLINTBEGIN(modernize-deprecated-headers)
// misc-include-cleaner wants this header rather than the C++ version
#include <stdlib.h>
// NOLINTEND(modernize-deprecated-headers)
#include <exception>
#include <filesystem>
#include <nix/attr-set.hh>
#include <nix/common-eval-args.hh>
#include <nix/error.hh>
#include <nix/eval.hh>
#include <nix/file-system.hh>
#include <nix/flake/flakeref.hh>
#include <nix/get-drvs.hh>
#include <nix/logging.hh>
#include <nix/outputs-spec.hh>
#include <nix/ref.hh>
#include <nix/store-api.hh>
#include <nix/symbol-table.hh>
#include <nix/types.hh>
#include <nix/util.hh>
#include <nix/value.hh>
#include <nix/value/context.hh>
#include <nlohmann/json_fwd.hpp>
#include <numeric>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "worker.hh"
#include "drv.hh"
#include "buffered-io.hh"
#include "eval-args.hh"

namespace nix {
struct Expr;
} // namespace nix

namespace {
auto releaseExprTopLevelValue(nix::EvalState &state, nix::Bindings &autoArgs,
                              MyArgs &args) -> nix::Value * {
    nix::Value vTop;

    if (args.fromArgs) {
        nix::Expr *e =
            state.parseExprFromString(args.releaseExpr, state.rootPath("."));
        state.eval(e, vTop);
    } else {
        state.evalFile(lookupFileArg(state, args.releaseExpr), vTop);
    }

    auto *vRoot = state.allocValue();

    state.autoCallFunction(autoArgs, vTop, *vRoot);

    return vRoot;
}

auto attrPathJoin(nlohmann::json input) -> std::string {
    return std::accumulate(input.begin(), input.end(), std::string(),
                           [](const std::string &ss, std::string s) {
                               // Escape token if containing dots
                               if (s.find('.') != std::string::npos) {
                                   s = "\"" + s + "\"";
                               }
                               return ss.empty() ? s : ss + "." + s;
                           });
}
} // namespace

void worker(
    MyArgs &args,
    nix::AutoCloseFD &to, // NOLINT(bugprone-easily-swappable-parameters)
    nix::AutoCloseFD &from) {

    auto evalStore = args.evalStoreUrl ? nix::openStore(*args.evalStoreUrl)
                                       : nix::openStore();
    auto state = nix::make_ref<nix::EvalState>(
        args.lookupPath, evalStore, nix::fetchSettings, nix::evalSettings);
    nix::Bindings &autoArgs = *args.getAutoArgs(*state);

    nix::Value *vRoot = [&]() {
        if (args.flake) {
            auto [flakeRef, fragment, outputSpec] =
                nix::parseFlakeRefWithFragmentAndExtendedOutputsSpec(
                    nix::fetchSettings, args.releaseExpr,
                    nix::absPath(std::filesystem::path(".")));
            nix::InstallableFlake flake{
                {}, state, std::move(flakeRef), fragment, outputSpec,
                {}, {},    args.lockFlags};

            return flake.toValue(*state).first;
        }

        return releaseExprTopLevelValue(*state, autoArgs, args);
    }();

    LineReader fromReader(from.release());

    while (true) {
        /* Wait for the collector to send us a job name. */
        if (tryWriteLine(to.get(), "next") < 0) {
            return; // main process died
        }

        auto s = fromReader.readLine();
        if (s == "exit") {
            break;
        }
        if (!nix::hasPrefix(s, "do ")) {
            std::cerr << "worker error: received invalid command '" << s
                      << "'\n";
            abort();
        }
        auto path = nlohmann::json::parse(s.substr(3));
        auto attrPathS = attrPathJoin(path);

        /* Evaluate it and send info back to the collector. */
        nlohmann::json reply =
            nlohmann::json{{"attr", attrPathS}, {"attrPath", path}};
        try {
            auto *vTmp =
                nix::findAlongAttrPath(*state, attrPathS, autoArgs, *vRoot)
                    .first;

            auto *v = state->allocValue();
            state->autoCallFunction(autoArgs, *vTmp, *v);

            if (v->type() == nix::nAttrs) {
                if (auto packageInfo = nix::getDerivation(*state, *v, false)) {

                    std::optional<Constituents> maybeConstituents;
                    if (args.constituents) {
                        std::vector<std::string> constituents;
                        std::vector<std::string> namedConstituents;
                        bool globConstituents = false;
                        const auto *a = v->attrs()->get(
                            state->symbols.create("_hydraAggregate"));
                        if (a != nullptr &&
                            state->forceBool(*a->value, a->pos,
                                             "while evaluating the "
                                             "`_hydraAggregate` attribute")) {
                            const auto *a = v->attrs()->get(
                                state->symbols.create("constituents"));
                            if (a == nullptr) {
                                state
                                    ->error<nix::EvalError>(
                                        "derivation must have a ‘constituents’ "
                                        "attribute")
                                    .debugThrow();
                            }

                            nix::NixStringContext context;
                            state->coerceToString(
                                a->pos, *a->value, context,
                                "while evaluating the `constituents` attribute",
                                true, false);
                            for (const auto &c : context) {
                                std::visit(
                                    nix::overloaded{
                                        [&](const nix::NixStringContextElem::
                                                Built &b) {
                                            constituents.push_back(
                                                b.drvPath->to_string(
                                                    *state->store));
                                        },
                                        [&](const nix::NixStringContextElem::
                                                Opaque &o) {},
                                        [&](const nix::NixStringContextElem::
                                                DrvDeep &d) {},
                                    },
                                    c.raw);
                            }

                            state->forceList(*a->value, a->pos,
                                             "while evaluating the "
                                             "`constituents` attribute");
                            auto constituents = std::span(a->value->listElems(),
                                                          a->value->listSize());
                            for (const auto &v : constituents) {
                                state->forceValue(*v, nix::noPos);
                                if (v->type() == nix::nString) {
                                    namedConstituents.emplace_back(v->c_str());
                                }
                            }

                            const auto *glob =
                                v->attrs()->get(state->symbols.create(
                                    "_hydraGlobConstituents"));
                            globConstituents =
                                glob != nullptr &&
                                state->forceBool(
                                    *glob->value, glob->pos,
                                    "while evaluating the "
                                    "`_hydraGlobConstituents` attribute");
                        }
                        maybeConstituents = Constituents(
                            constituents, namedConstituents, globConstituents);
                    }

                    if (args.applyExpr != "") {
                        auto applyExpr = state->parseExprFromString(
                            args.applyExpr, state->rootPath("."));

                        nix::Value vApply;
                        nix::Value vRes;

                        state->eval(applyExpr, vApply);

                        state->callFunction(vApply, *v, vRes, nix::noPos);
                        state->forceAttrs(
                            vRes, nix::noPos,
                            "apply needs to evaluate to an attrset");

                        nix::NixStringContext context;
                        std::stringstream ss;
                        nix::printValueAsJSON(*state, true, vRes, nix::noPos,
                                              ss, context);

                        reply.update(nlohmann::json::parse(ss.str()));
                    }

                    auto drv = Drv(attrPathS, *state, *packageInfo, args,
                                   maybeConstituents);
                    reply.update(drv);

                    /* Register the derivation as a GC root.  !!! This
                       registers roots for jobs that we may have already
                       done. */
                    if (!args.gcRootsDir.empty()) {
                        const nix::Path root =
                            args.gcRootsDir + "/" +
                            std::string(nix::baseNameOf(drv.drvPath));
                        if (!nix::pathExists(root)) {
                            auto localStore =
                                state->store
                                    .dynamic_pointer_cast<nix::LocalFSStore>();
                            auto storePath =
                                localStore->parseStorePath(drv.drvPath);
                            localStore->addPermRoot(storePath, root);
                        }
                    }
                } else {
                    auto attrs = nlohmann::json::array();
                    bool recurse =
                        args.forceRecurse ||
                        path.empty(); // Dont require `recurseForDerivations
                                      // = true;` for top-level attrset

                    for (auto &i :
                         v->attrs()->lexicographicOrder(state->symbols)) {
                        const std::string_view &name = state->symbols[i->name];
                        attrs.push_back(name);

                        if (name == "recurseForDerivations" &&
                            !args.forceRecurse) {
                            const auto *attrv =
                                v->attrs()->get(state->sRecurseForDerivations);
                            recurse = state->forceBool(
                                *attrv->value, attrv->pos,
                                "while evaluating recurseForDerivations");
                        }
                    }
                    if (recurse) {
                        reply["attrs"] = std::move(attrs);
                    } else {
                        reply["attrs"] = nlohmann::json::array();
                    }
                }
            } else {
                // We ignore everything that cannot be build
                reply["attrs"] = nlohmann::json::array();
            }
        } catch (nix::EvalError &e) {
            const auto &err = e.info();
            std::ostringstream oss;
            nix::showErrorInfo(oss, err, nix::loggerSettings.showTrace.get());
            auto msg = oss.str();

            // Transmits the error we got from the previous evaluation
            // in the JSON output.
            reply["error"] = nix::filterANSIEscapes(msg, true);
            // Don't forget to print it into the STDERR log, this is
            // what's shown in the Hydra UI.
            std::cerr << msg << "\n";
        } catch (
            const std::exception &e) { // FIXME: for some reason the catch block
                                       // above, doesn't trigger on macOS (?)
            const auto *msg = e.what();
            reply["error"] = nix::filterANSIEscapes(msg, true);
            std::cerr << msg << '\n';
        }

        if (tryWriteLine(to.get(), reply.dump()) < 0) {
            return; // main process died
        }

        /* If our RSS exceeds the maximum, exit. The collector will
           start a new process. */
        struct rusage r = {}; // NOLINT(misc-include-cleaner)
        getrusage(RUSAGE_SELF, &r);
        const size_t maxrss =
            r.ru_maxrss; // NOLINT(cppcoreguidelines-pro-type-union-access)
        if (maxrss > args.maxMemorySize * 1024) {
            break;
        }
    }

    if (tryWriteLine(to.get(), "restart") < 0) {
        return; // main process died
    };
}
