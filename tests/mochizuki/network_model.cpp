// Both compiled model graphs, without a GPU or user weights.
#include <nr_native_plan.hpp>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void topology(const nr::NativePlan& plan, int expectedRows, int finalBlock, int firstVit, int lastVit) {
    std::istringstream input(plan.text);
    std::string line;
    int rows = 0, last = -1, minVit = 100, maxVit = -1;
    while (std::getline(input, line)) {
        std::istringstream row(line);
        std::vector<std::string> fields;
        for (std::string field; row >> field;) fields.push_back(field);
        require(fields.size() == 21, "invalid plan row");
        last = std::stoi(fields[0]);
        if (fields[17].rfind("CCVit1D", 0) == 0) {
            minVit = std::min(minVit, last); maxVit = std::max(maxVit, last);
            require(std::stoull(fields[5]) == std::stoull(fields[3]) * std::stoull(fields[4]),
                    "ViT token count was aligned as a Swin layer");
        }
        ++rows;
    }
    require(rows == expectedRows && last == finalBlock, "wrong model topology");
    require(minVit == firstVit && maxVit == lastVit, "wrong ViT boundary");
}

int main() try {
    const auto original = nr::make_native_plan(1920, 1080, nr::NetworkModel::Original);
    const auto small = nr::make_native_plan(1920, 1080, nr::NetworkModel::Small);
    require(original.width == 1920 && original.height == 1088, "310.8.0 dimensions changed");
    require(small.width == 1920 && small.height == 1152, "310.9.16 DLL extent rule missing");
    require(!original.model.empty() && !small.model.empty() && original.model != small.model,
            "model fingerprint did not change");
    topology(original, 152, 70, 31, 38);
    topology(small, 76, 30, 11, 18);
    for (auto model : {nr::NetworkModel::Original, nr::NetworkModel::Small}) {
        for (auto size : {std::pair{1129u, 635u}, std::pair{2560u, 1440u}, std::pair{3840u, 2160u}}) {
            const auto plan = nr::make_native_plan(size.first, size.second, model);
            require(plan.width >= size.first && plan.height >= size.second, "working extent cropped input");
            topology(plan, model == nr::NetworkModel::Small ? 76 : 152,
                     model == nr::NetworkModel::Small ? 30 : 70,
                     model == nr::NetworkModel::Small ? 11 : 31,
                     model == nr::NetworkModel::Small ? 18 : 38);
        }
        bool rejected = false;
        try { nr::make_native_plan(0, 1080, model); } catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "empty input accepted");
    }
    std::cout << "MOCHIZUKI_NETWORK_MODELS_OK\n";
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
