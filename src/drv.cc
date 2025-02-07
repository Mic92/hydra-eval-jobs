#include <nix/config.h> // IWYU pragma: keep
#include <nix/path-with-outputs.hh>
#include <nix/store-api.hh>
#include <nix/local-fs-store.hh>
#include <nix/value-to-json.hh>
#include <nix/config.hh>
#include <nix/derivations.hh>
#include <nix/get-drvs.hh>
#include <nix/derived-path-map.hh>
#include <nix/eval.hh>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <nix/path.hh>
#include <nix/ref.hh>
#include <nix/value/context.hh>
#include <nix/error.hh>
#include <nix/eval-error.hh>
#include <nix/experimental-features.hh>
// required for std::optional
#include <nix/json-utils.hh> //NOLINT(misc-include-cleaner)
#include <nix/pos-idx.hh>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>
#include <algorithm>

#include "drv.hh"
#include "eval-args.hh"

namespace {

auto queryCacheStatus(
    nix::Store &store,
    std::map<std::string, std::optional<std::string>> &outputs,
    std::vector<std::string> &neededBuilds,
    std::vector<std::string> &neededSubstitutes,
    std::vector<std::string> &unknownPaths) -> Drv::CacheStatus {
    uint64_t downloadSize = 0;
    uint64_t narSize = 0;

    std::vector<nix::StorePathWithOutputs> paths;
    for (auto const &[key, val] : outputs) {
        if (val) {
            paths.push_back(followLinksToStorePathWithOutputs(store, *val));
        }
    }
    nix::StorePathSet willBuild;
    nix::StorePathSet willSubstitute;
    nix::StorePathSet unknown;

    store.queryMissing(toDerivedPaths(paths), willBuild, willSubstitute,
                       unknown, downloadSize, narSize);

    if (!willBuild.empty()) {
        // TODO: can we expose the topological sort order as a graph?
        auto sorted = store.topoSortPaths(willBuild);
        std::ranges::reverse(sorted.begin(), sorted.end());
        for (auto &i : sorted) {
            neededBuilds.push_back(store.printStorePath(i));
        }
    }
    if (!willSubstitute.empty()) {
        std::vector<const nix::StorePath *> willSubstituteSorted = {};
        std::ranges::for_each(willSubstitute.begin(), willSubstitute.end(),
                              [&](const nix::StorePath &p) {
                                  willSubstituteSorted.push_back(&p);
                              });
        std::ranges::sort(
            willSubstituteSorted.begin(), willSubstituteSorted.end(),
            [](const nix::StorePath *lhs, const nix::StorePath *rhs) {
                if (lhs->name() == rhs->name()) {
                    return lhs->to_string() < rhs->to_string();
                }
                return lhs->name() < rhs->name();
            });
        for (const auto *p : willSubstituteSorted) {
            neededSubstitutes.push_back(store.printStorePath(*p));
        }
    }

    if (!unknown.empty()) {
        for (const auto &i : unknown) {
            unknownPaths.push_back(store.printStorePath(i));
        }
    }

    if (willBuild.empty() && unknown.empty()) {
        if (willSubstitute.empty()) {
            // cacheStatus is Local if:
            //  - there's nothing to build
            //  - there's nothing to substitute
            return Drv::CacheStatus::Local;
        }
        // cacheStatus is Cached if:
        //  - there's nothing to build
        //  - there are paths to substitute
        return Drv::CacheStatus::Cached;
    }
    return Drv::CacheStatus::NotBuilt;
};

} // namespace

/* The fields of a derivation that are printed in json form */
Drv::Drv(std::string &attrPath, nix::EvalState &state,
         nix::PackageInfo &packageInfo, MyArgs &args,
         std::optional<Constituents> constituents)
    : constituents(std::move(constituents)) {

    auto localStore = state.store.dynamic_pointer_cast<nix::LocalFSStore>();

    try {
        nix::PackageInfo::Outputs outputsQueried;

        // CA derivations do not have static output paths, so we have to
        // fallback if we encounter an error
        try {
            outputsQueried = packageInfo.queryOutputs(true);
        } catch (const nix::Error &e) {
            // We could be hitting `nix::UnimplementedError`:
            // https://github.com/NixOS/nix/blob/39da9462e9c677026a805c5ee7ba6bb306f49c59/src/libexpr/get-drvs.cc#L106
            //
            // Or we could be hitting:
            // ```
            // error: derivation 'caDependingOnCA' does not have valid outputs:
            // error: while evaluating the output path of a derivation at
            // <nix/derivation-internal.nix>:19:9:
            //
            //    18|       value = commonAttrs // {
            //    19|         outPath = builtins.getAttr outputName strict;\n
            //      |         ^
            //    20|         drvPath = strict.drvPath;
            //
            // error: path
            // '/0rmq7bvk2raajd310spvd416f2jajrabcg6ar706gjbd6b8nmvks' is not in
            // the Nix store
            // ```
            // i.e. the placeholders were confusing it.
            //
            // FIXME: a better fix would be in Nix to first check if
            // `outPath` is equal to the placeholder. See
            // https://github.com/NixOS/nix/issues/11885.
            if (!nix::experimentalFeatureSettings.isEnabled(
                    nix::Xp::CaDerivations)) {
                // If we do have CA derivations enabled, we should not encounter
                // these errors.
                throw;
            }
            outputsQueried = packageInfo.queryOutputs(false);
        }
        for (auto &[outputName, optOutputPath] : outputsQueried) {
            if (optOutputPath) {
                outputs[outputName] =
                    localStore->printStorePath(*optOutputPath);
            } else {
                outputs[outputName] = std::nullopt;
            }
        }
    } catch (const std::exception &e) {
        state
            .error<nix::EvalError>(
                "derivation '%s' does not have valid outputs: %s", attrPath,
                e.what())
            .debugThrow();
    }

    if (args.checkCacheStatus) {
        // TODO: is this a bottleneck, where we should batch these queries?
        cacheStatus = queryCacheStatus(*localStore, outputs, neededBuilds,
                                       neededSubstitutes, unknownPaths);
    } else {
        cacheStatus = Drv::CacheStatus::Unknown;
    }

    if (args.meta) {
        nlohmann::json meta_;
        for (const auto &metaName : packageInfo.queryMetaNames()) {
            nix::NixStringContext context;
            std::stringstream ss;

            auto *metaValue = packageInfo.queryMeta(metaName);
            // Skip non-serialisable types
            // TODO: Fix serialisation of derivations to store paths
            if (metaValue == nullptr) {
                continue;
            }

            nix::printValueAsJSON(state, true, *metaValue, nix::noPos, ss,
                                  context);

            meta_[metaName] = nlohmann::json::parse(ss.str());
        }
        meta = meta_;
    }

    drvPath = localStore->printStorePath(packageInfo.requireDrvPath());

    name = packageInfo.queryName();

    // TODO: Ideally we wouldn't have to parse the derivation to get the system
    auto drv = localStore->readDerivation(packageInfo.requireDrvPath());
    system = drv.platform;
    if (args.showInputDrvs) {
        std::map<std::string, std::set<std::string>> drvs;
        for (const auto &[inputDrvPath, inputNode] : drv.inputDrvs.map) {
            std::set<std::string> inputDrvOutputs;
            for (const auto &outputName : inputNode.value) {
                inputDrvOutputs.insert(outputName);
            }
            drvs[localStore->printStorePath(inputDrvPath)] = inputDrvOutputs;
        }
        inputDrvs = drvs;
    }
}

void to_json(nlohmann::json &json, const Drv &drv) {
    json = nlohmann::json{{"name", drv.name},
                          {"system", drv.system},
                          {"drvPath", drv.drvPath},
                          {"outputs", drv.outputs}};

    if (drv.meta.has_value()) {
        json["meta"] = drv.meta.value();
    }
    if (drv.inputDrvs) {
        json["inputDrvs"] = drv.inputDrvs.value();
    }

    if (auto constituents = drv.constituents) {
        json["constituents"] = constituents->constituents;
        json["namedConstituents"] = constituents->namedConstituents;
        json["globConstituents"] = constituents->globConstituents;
    }

    if (drv.cacheStatus != Drv::CacheStatus::Unknown) {
        // Deprecated field
        json["isCached"] = drv.cacheStatus == Drv::CacheStatus::Cached ||
                           drv.cacheStatus == Drv::CacheStatus::Local;

        switch (drv.cacheStatus) {
        case Drv::CacheStatus::Cached:
            json["cacheStatus"] = "cached";
            break;
        case Drv::CacheStatus::Local:
            json["cacheStatus"] = "local";
            break;
        default:
            json["cacheStatus"] = "notBuilt";
            break;
        }
        json["neededBuilds"] = drv.neededBuilds;
        json["neededSubstitutes"] = drv.neededSubstitutes;
        // TODO: is it useful to include "unknown" paths at all?
        // json["unknown"] = drv.unknownPaths;
    }
}
