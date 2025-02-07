#include <nix/get-drvs.hh>
#include <nix/eval.hh>
#include <nlohmann/json_fwd.hpp>
// we need this include or otherwise we cannot instantiate std::optional
#include <nlohmann/json.hpp> //NOLINT(misc-include-cleaner)
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "eval-args.hh"

namespace nix {
class EvalState;
struct PackageInfo;
} // namespace nix

struct Constituents {
    std::vector<std::string> constituents;
    std::vector<std::string> namedConstituents;
    bool globConstituents;
    Constituents(std::vector<std::string> constituents,
                 std::vector<std::string> namedConstituents,
                 bool globConstituents)
        : constituents(std::move(constituents)),
          namedConstituents(std::move(namedConstituents)),
          globConstituents(globConstituents) {};
};

/* The fields of a derivation that are printed in json form */
struct Drv {
    Drv(std::string &attrPath, nix::EvalState &state,
        nix::PackageInfo &packageInfo, MyArgs &args,
        std::optional<Constituents> constituents);
    std::string name;
    std::string system;
    std::string drvPath;

    std::map<std::string, std::optional<std::string>> outputs;

    std::optional<std::map<std::string, std::set<std::string>>> inputDrvs =
        std::nullopt;

    // TODO: can we lazily allocate these?
    std::vector<std::string> neededBuilds;
    std::vector<std::string> neededSubstitutes;
    std::vector<std::string> unknownPaths;

    // TODO: we might not need to store this as it can be computed from the
    // above
    enum class CacheStatus : uint8_t {
        Local,
        Cached,
        NotBuilt,
        Unknown
    } cacheStatus;

    std::optional<nlohmann::json> meta;
    std::optional<Constituents> constituents;
};
void to_json(nlohmann::json &json, const Drv &drv);
