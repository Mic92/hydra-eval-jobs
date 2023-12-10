#include <string>
#include <map>
#include <set>
#include <string>
#include <optional>

#include <nix/get-drvs.hh>
#include <nix/eval.hh>

#include <nlohmann/json.hpp>

#include "eval-args.hh"

/* The fields of a derivation that are printed in json form */
struct Drv {
    std::string name;
    std::string system;
    std::string drvPath;

    enum class CacheStatus { Cached, Uncached, Unknown } cacheStatus;
    std::map<std::string, std::string> outputs;
    std::map<std::string, std::set<std::string>> inputDrvs;
    std::optional<nlohmann::json> meta;

    Drv(std::string &attrPath, nix::EvalState &state, nix::DrvInfo &drvInfo, MyArgs &args);
};
void to_json(nlohmann::json &json, const Drv &drv);
