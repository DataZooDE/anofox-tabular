#include "anofox_isolation_forest.hpp"
#include <cmath>
#include <numeric>

namespace duckdb {
namespace anofox {

// ============================================================================
// IsolationTree Implementation
// ============================================================================

void IsolationTree::BuildTree(
    const std::vector<std::vector<double>>& data,
    const std::vector<size_t>& sample_indices,
    size_t current_depth,
    size_t max_depth,
    std::mt19937& rng
) {
    size_t n_samples = sample_indices.size();
    size_t n_features = data[0].size();

    // Terminal conditions: leaf node
    if (n_samples <= 1 || current_depth >= max_depth) {
        nodes_.push_back({
            true,                           // is_leaf
            0,                              // feature_idx (unused for leaf)
            0.0,                            // split_value (unused for leaf)
            -1,                             // left_child
            -1,                             // right_child
            static_cast<uint16_t>(current_depth)
        });
        return;
    }

    // Randomly select feature to split on
    std::uniform_int_distribution<size_t> feat_dist(0, n_features - 1);
    size_t split_feature = feat_dist(rng);

    // Find min/max values of selected feature in sample
    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();

    for (size_t idx : sample_indices) {
        double val = data[idx][split_feature];
        if (!std::isnan(val)) {
            min_val = std::min(min_val, val);
            max_val = std::max(max_val, val);
        }
    }

    // If all values identical (or all NaN), create leaf
    if (min_val >= max_val) {
        nodes_.push_back({
            true,
            0,
            0.0,
            -1,
            -1,
            static_cast<uint16_t>(current_depth)
        });
        return;
    }

    // Random split value between min and max
    std::uniform_real_distribution<double> split_dist(min_val, max_val);
    double split_value = split_dist(rng);

    // Partition samples into left and right
    std::vector<size_t> left_indices, right_indices;
    for (size_t idx : sample_indices) {
        double val = data[idx][split_feature];
        if (std::isnan(val)) {
            // Treat NaN as going left (arbitrary choice)
            left_indices.push_back(idx);
        } else if (val < split_value) {
            left_indices.push_back(idx);
        } else {
            right_indices.push_back(idx);
        }
    }

    // Create internal node
    size_t node_idx = nodes_.size();
    nodes_.push_back({
        false,                          // is_leaf
        static_cast<uint16_t>(split_feature),
        split_value,
        -1,                             // left_child (to be updated)
        -1,                             // right_child (to be updated)
        static_cast<uint16_t>(current_depth)
    });

    // Recursively build left subtree
    nodes_[node_idx].left_child = static_cast<int32_t>(nodes_.size());
    BuildTree(data, left_indices, current_depth + 1, max_depth, rng);

    // Recursively build right subtree
    nodes_[node_idx].right_child = static_cast<int32_t>(nodes_.size());
    BuildTree(data, right_indices, current_depth + 1, max_depth, rng);
}

double IsolationTree::ComputePathLength(const std::vector<double>& point) const {
    if (nodes_.empty()) {
        return 0.0;
    }
    return ComputePathLengthRecursive(0, point, 0);
}

double IsolationTree::ComputePathLengthRecursive(
    size_t node_idx,
    const std::vector<double>& point,
    size_t current_depth
) const {
    const IsoNode& node = nodes_[node_idx];

    // Terminal node: return current depth
    if (node.is_leaf) {
        return static_cast<double>(current_depth);
    }

    // Internal node: traverse based on feature value
    double val = point[node.feature_idx];

    // Treat NaN as going left (arbitrary but consistent)
    if (std::isnan(val)) {
        return ComputePathLengthRecursive(node.left_child, point, current_depth + 1);
    }

    if (val < node.split_value) {
        return ComputePathLengthRecursive(node.left_child, point, current_depth + 1);
    } else {
        return ComputePathLengthRecursive(node.right_child, point, current_depth + 1);
    }
}

// ============================================================================
// IsolationForest Implementation
// ============================================================================

double IsolationForest::AveragePathLength(size_t n) {
    if (n <= 1) return 0.0;

    // Formula: 2 * H(n-1) - (2 * (n-1) / n)
    // Where H(n) is the harmonic number
    double harmonic = 0.0;
    for (size_t i = 1; i < n; i++) {
        harmonic += 1.0 / static_cast<double>(i);
    }

    double log_approx = std::log(static_cast<double>(n - 1)) + 0.5772156649; // Euler-Mascheroni
    return 2.0 * log_approx - (2.0 * static_cast<double>(n - 1) / static_cast<double>(n));
}

void IsolationForest::Fit(const std::vector<std::vector<double>>& data) {
    if (data.empty()) {
        return;
    }

    n_features_ = data[0].size();

    // Ensure sample_size doesn't exceed data size
    size_t actual_sample_size = std::min(sample_size_, data.size());

    // Compute max depth for trees
    // Default: ceil(log2(sample_size)) + 1
    size_t max_depth = static_cast<size_t>(std::ceil(std::log2(actual_sample_size))) + 1;

    // Build all trees
    trees_.resize(n_trees_);

    for (size_t t = 0; t < n_trees_; t++) {
        // Create random subsample of indices
        std::vector<size_t> sample_indices(data.size());
        std::iota(sample_indices.begin(), sample_indices.end(), 0);

        // Shuffle and take first sample_size
        std::shuffle(sample_indices.begin(), sample_indices.end(), rng_);
        sample_indices.resize(actual_sample_size);

        // Build tree
        trees_[t].BuildTree(data, sample_indices, 0, max_depth, rng_);
    }
}

double IsolationForest::Score(const std::vector<double>& point) const {
    if (trees_.empty()) {
        return 0.0;
    }

    // Compute average path length across all trees
    double sum_path_length = 0.0;
    for (const auto& tree : trees_) {
        sum_path_length += tree.ComputePathLength(point);
    }
    double avg_path_length = sum_path_length / static_cast<double>(trees_.size());

    // Normalize by average path length for random BST
    double c = AveragePathLength(sample_size_);
    if (c == 0.0) {
        return 0.0;
    }

    // Anomaly score: 2^(-avg_path_length / c)
    // Range: [0, 1], where 1 = anomaly, 0 = inlier
    return std::pow(2.0, -avg_path_length / c);
}

std::vector<double> IsolationForest::ScoreBatch(
    const std::vector<std::vector<double>>& data
) const {
    std::vector<double> scores;
    scores.reserve(data.size());

    for (const auto& point : data) {
        scores.push_back(Score(point));
    }

    return scores;
}

double IsolationForest::ComputeThreshold(const std::vector<double>& scores) const {
    if (scores.empty()) {
        return 0.5;
    }

    // Sort scores to find quantile
    auto sorted_scores = scores;
    std::sort(sorted_scores.begin(), sorted_scores.end());

    // Find index for (1 - contamination) quantile
    // Points with score >= this threshold are anomalies
    double quantile_idx = (1.0 - contamination_) * static_cast<double>(sorted_scores.size());
    size_t idx = static_cast<size_t>(std::floor(quantile_idx));
    idx = std::min(idx, sorted_scores.size() - 1);

    return sorted_scores[idx];
}

} // namespace anofox
} // namespace duckdb
