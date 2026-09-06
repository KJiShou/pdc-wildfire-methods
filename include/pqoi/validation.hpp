#pragma once

#include "pqoi/core/image.hpp"

#include <string>

namespace pqoi {

struct ValidationDetails {
    bool decoder_accepted{false};
    bool dimensions_match{false};
    bool channels_match{false};
    bool pixel_match{false};

    [[nodiscard]] bool passed() const noexcept {
        return decoder_accepted && dimensions_match && channels_match && pixel_match;
    }
};

ValidationDetails validate_qoi_detailed(const std::string& qoi_path, const Image& expected);
bool validate_qoi(const std::string& qoi_path, const Image& expected);
Image decode_qoi(const std::string& qoi_path);

}  // namespace pqoi
