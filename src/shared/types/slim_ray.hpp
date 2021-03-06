#pragma once

#include "glm/glm.hpp"

#include "utils/tostring.hpp"

namespace fr {

/// Avoid self-intersection by only recognizing intersections that occur
/// at this minimum t-value along the ray.
extern const float SELF_INTERSECT_EPSILON;

/// A light ray must hit within this distance of its target to say it has hit
/// the target.
extern const float TARGET_INTERSECT_EPSILON;

struct SlimRay {
    explicit SlimRay(glm::vec3 origin, glm::vec3 direction);

    explicit SlimRay();

    /// The origin position of the ray.
    glm::vec3 origin;

    /// The normalized direction of the ray. Unit length is not enforced.
    glm::vec3 direction;

    /// Returns a new ray that is this ray transformed by the given
    /// transformation matrix.
    SlimRay TransformTo(const glm::mat4& transform) const;

    /// Evaluate a point along the ray at a specific t value.
    inline glm::vec3 EvaluateAt(float t) const {
        return direction * t + origin;
    }

    TOSTRINGABLE(SlimRay);
};

std::string ToString(const SlimRay& ray, const std::string& indent = "");

} // namespace fr
