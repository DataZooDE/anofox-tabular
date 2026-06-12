#include "anofox_isolation_forest.hpp"
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace duckdb {
namespace anofox {

// ============================================================================
// SCiForest Helper Functions
// ============================================================================

/**
 * Compute variance of a vector of values
 */
static double ComputeVariance(const std::vector<double>& values) {
    if (values.size() <= 1) {
        return 0.0;
    }

    double sum = 0.0;
    size_t count = 0;
    for (double v : values) {
        if (!std::isnan(v)) {
            sum += v;
            count++;
        }
    }

    if (count <= 1) {
        return 0.0;
    }

    double mean = sum / static_cast<double>(count);
    double sq_sum = 0.0;
    for (double v : values) {
        if (!std::isnan(v)) {
            sq_sum += (v - mean) * (v - mean);
        }
    }

    return sq_sum / static_cast<double>(count);
}

/**
 * Compute split gain using variance reduction
 * @param parent_values: Values before split
 * @param left_count: Number of samples going left
 * @param right_count: Number of samples going right
 * @param left_variance: Variance of left partition
 * @param right_variance: Variance of right partition
 * @return Gain score (higher = better split)
 */
static double ComputeSplitGain(
    double parent_variance,
    size_t left_count,
    size_t right_count,
    double left_variance,
    double right_variance
) {
    size_t total = left_count + right_count;
    if (total == 0 || parent_variance <= 0.0) {
        return 0.0;
    }

    // Weighted average of child variances
    double weighted_child_var =
        (static_cast<double>(left_count) * left_variance +
         static_cast<double>(right_count) * right_variance) /
        static_cast<double>(total);

    // Gain = reduction in variance (always non-negative)
    return std::max(0.0, parent_variance - weighted_child_var);
}

/**
 * Generate a single axis-aligned numeric split candidate
 */
static bool GenerateNumericSplitCandidate(
    const std::vector<ColumnData>& data,
    const std::vector<size_t>& sample_indices,
    size_t feature_idx,
    std::mt19937& rng,
    SplitCandidate& candidate
) {
    const ColumnData& col = data[feature_idx];

    double min_val = std::numeric_limits<double>::max();
    double max_val = std::numeric_limits<double>::lowest();

    for (size_t idx : sample_indices) {
        double val = col.numeric_values[idx];
        if (!std::isnan(val)) {
            min_val = std::min(min_val, val);
            max_val = std::max(max_val, val);
        }
    }

    // If all values identical, can't split
    if (min_val >= max_val) {
        return false;
    }

    // Random split value
    std::uniform_real_distribution<double> split_dist(min_val, max_val);
    double split_value = split_dist(rng);

    // Partition samples
    candidate.left_indices.clear();
    candidate.right_indices.clear();

    std::vector<double> left_values, right_values;

    for (size_t idx : sample_indices) {
        double val = col.numeric_values[idx];
        if (std::isnan(val) || val < split_value) {
            candidate.left_indices.push_back(idx);
            if (!std::isnan(val)) {
                left_values.push_back(val);
            }
        } else {
            candidate.right_indices.push_back(idx);
            right_values.push_back(val);
        }
    }

    // Need both sides non-empty for a valid split
    if (candidate.left_indices.empty() || candidate.right_indices.empty()) {
        return false;
    }

    // Set candidate fields
    candidate.type = FeatureType::NUMERIC;
    candidate.ndim = 1;
    candidate.feature_idx = feature_idx;
    candidate.split_value = split_value;

    // Compute gain
    std::vector<double> all_values;
    for (size_t idx : sample_indices) {
        double val = col.numeric_values[idx];
        if (!std::isnan(val)) {
            all_values.push_back(val);
        }
    }

    double parent_var = ComputeVariance(all_values);
    double left_var = ComputeVariance(left_values);
    double right_var = ComputeVariance(right_values);

    candidate.gain_score = ComputeSplitGain(
        parent_var, left_values.size(), right_values.size(), left_var, right_var);

    return true;
}

/**
 * Generate a single categorical split candidate
 */
static bool GenerateCategoricalSplitCandidate(
    const std::vector<ColumnData>& data,
    const std::vector<size_t>& sample_indices,
    size_t feature_idx,
    std::mt19937& rng,
    SplitCandidate& candidate
) {
    const ColumnData& col = data[feature_idx];

    // Collect present categories
    std::unordered_set<int> present_categories;
    for (size_t idx : sample_indices) {
        int cat_idx = col.category_indices[idx];
        if (cat_idx >= 0) {
            present_categories.insert(cat_idx);
        }
    }

    // Need at least 2 categories to split
    if (present_categories.size() <= 1) {
        return false;
    }

    // Randomly partition categories
    std::vector<int> category_list(present_categories.begin(), present_categories.end());
    std::shuffle(category_list.begin(), category_list.end(), rng);

    std::uniform_int_distribution<size_t> k_dist(1, category_list.size() - 1);
    size_t k = k_dist(rng);

    candidate.left_categories.clear();
    for (size_t i = 0; i < k; i++) {
        candidate.left_categories.insert(category_list[i]);
    }

    // Partition samples
    candidate.left_indices.clear();
    candidate.right_indices.clear();

    for (size_t idx : sample_indices) {
        int cat_idx = col.category_indices[idx];
        if (cat_idx < 0 || candidate.left_categories.count(cat_idx)) {
            candidate.left_indices.push_back(idx);
        } else {
            candidate.right_indices.push_back(idx);
        }
    }

    // Need both sides non-empty
    if (candidate.left_indices.empty() || candidate.right_indices.empty()) {
        return false;
    }

    // Set candidate fields
    candidate.type = FeatureType::CATEGORICAL;
    candidate.ndim = 1;
    candidate.feature_idx = feature_idx;

    // For categorical, use a simple gain heuristic based on split balance
    // More balanced splits get lower gain (we want to isolate rare categories)
    double left_frac = static_cast<double>(candidate.left_indices.size()) /
                       static_cast<double>(sample_indices.size());
    double balance = std::min(left_frac, 1.0 - left_frac);

    // Gain: prefer imbalanced splits (isolate minorities faster)
    candidate.gain_score = 1.0 - 2.0 * balance;

    return true;
}

/**
 * Generate a hyperplane split candidate (Extended IF)
 */
static bool GenerateHyperplaneSplitCandidate(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    const std::vector<size_t>& sample_indices,
    const std::vector<size_t>& numeric_features,
    size_t effective_ndim,
    CoefType coef_type,
    std::mt19937& rng,
    SplitCandidate& candidate
) {
    // Select ndim features for the hyperplane
    std::vector<size_t> shuffled_numeric = numeric_features;
    std::shuffle(shuffled_numeric.begin(), shuffled_numeric.end(), rng);
    shuffled_numeric.resize(effective_ndim);

    // Compute min/max for each selected feature
    std::vector<double> min_vals(effective_ndim);
    std::vector<double> max_vals(effective_ndim);
    bool all_constant = true;

    for (size_t i = 0; i < effective_ndim; i++) {
        size_t feat_idx = shuffled_numeric[i];
        const ColumnData& col = data[feat_idx];
        min_vals[i] = std::numeric_limits<double>::max();
        max_vals[i] = std::numeric_limits<double>::lowest();

        for (size_t idx : sample_indices) {
            double val = col.numeric_values[idx];
            if (!std::isnan(val)) {
                min_vals[i] = std::min(min_vals[i], val);
                max_vals[i] = std::max(max_vals[i], val);
            }
        }

        if (max_vals[i] > min_vals[i]) {
            all_constant = false;
        }
    }

    if (all_constant) {
        return false;
    }

    // Generate coefficients
    std::vector<double> coefficients(effective_ndim);
    std::normal_distribution<double> normal_dist(0.0, 1.0);

    for (size_t i = 0; i < effective_ndim; i++) {
        if (coef_type == CoefType::Normal) {
            coefficients[i] = normal_dist(rng);
        } else {
            double range = max_vals[i] - min_vals[i];
            if (range > 0) {
                std::uniform_real_distribution<double> coef_dist(-1.0, 1.0);
                coefficients[i] = coef_dist(rng);
            } else {
                coefficients[i] = 1.0;
            }
        }
    }

    // Compute projections
    double min_proj = std::numeric_limits<double>::max();
    double max_proj = std::numeric_limits<double>::lowest();
    std::vector<double> projections(sample_indices.size());

    for (size_t s = 0; s < sample_indices.size(); s++) {
        size_t idx = sample_indices[s];
        double proj = 0.0;
        bool has_nan = false;

        for (size_t i = 0; i < effective_ndim; i++) {
            size_t feat_idx = shuffled_numeric[i];
            double val = data[feat_idx].numeric_values[idx];
            if (std::isnan(val)) {
                has_nan = true;
                break;
            }
            proj += coefficients[i] * val;
        }

        if (has_nan) {
            projections[s] = std::numeric_limits<double>::quiet_NaN();
        } else {
            projections[s] = proj;
            min_proj = std::min(min_proj, proj);
            max_proj = std::max(max_proj, proj);
        }
    }

    if (min_proj >= max_proj) {
        return false;
    }

    // Random intercept
    std::uniform_real_distribution<double> intercept_dist(min_proj, max_proj);
    double intercept = intercept_dist(rng);

    // Partition samples
    candidate.left_indices.clear();
    candidate.right_indices.clear();
    std::vector<double> left_projs, right_projs;

    for (size_t s = 0; s < sample_indices.size(); s++) {
        if (std::isnan(projections[s]) || projections[s] < intercept) {
            candidate.left_indices.push_back(sample_indices[s]);
            if (!std::isnan(projections[s])) {
                left_projs.push_back(projections[s]);
            }
        } else {
            candidate.right_indices.push_back(sample_indices[s]);
            right_projs.push_back(projections[s]);
        }
    }

    if (candidate.left_indices.empty() || candidate.right_indices.empty()) {
        return false;
    }

    // Set candidate fields
    candidate.type = FeatureType::NUMERIC;
    candidate.ndim = static_cast<uint8_t>(effective_ndim);

    candidate.hyperplane_features.resize(effective_ndim);
    for (size_t i = 0; i < effective_ndim; i++) {
        candidate.hyperplane_features[i] = static_cast<uint16_t>(shuffled_numeric[i]);
    }
    candidate.hyperplane_coefficients = std::move(coefficients);
    candidate.hyperplane_intercept = intercept;

    // Compute gain based on projection variance reduction
    std::vector<double> all_projs;
    for (size_t s = 0; s < projections.size(); s++) {
        if (!std::isnan(projections[s])) {
            all_projs.push_back(projections[s]);
        }
    }

    double parent_var = ComputeVariance(all_projs);
    double left_var = ComputeVariance(left_projs);
    double right_var = ComputeVariance(right_projs);

    candidate.gain_score = ComputeSplitGain(
        parent_var, left_projs.size(), right_projs.size(), left_var, right_var);

    return true;
}

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
    // Root call of a (re)build: drop any state from a previous build
    if (current_depth == 0) {
        nodes_.clear();
        root_idx_ = 0;
    }

    size_t n_samples = sample_indices.size();
    size_t n_features = data[0].size();

    // Terminal conditions: leaf node
    if (n_samples <= 1 || current_depth >= max_depth) {
        IsoNode leaf;
        leaf.is_leaf = true;
        leaf.feature_type = FeatureType::NUMERIC;
        leaf.depth = static_cast<uint16_t>(current_depth);
        leaf.leaf_size = n_samples;  // Store for path length adjustment
        nodes_.push_back(std::move(leaf));
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
        IsoNode leaf;
        leaf.is_leaf = true;
        leaf.feature_type = FeatureType::NUMERIC;
        leaf.depth = static_cast<uint16_t>(current_depth);
        leaf.leaf_size = n_samples;  // Store for path length adjustment
        nodes_.push_back(std::move(leaf));
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
    IsoNode node;
    node.is_leaf = false;
    node.feature_type = FeatureType::NUMERIC;
    node.feature_idx = static_cast<uint16_t>(split_feature);
    node.split_value = split_value;
    node.depth = static_cast<uint16_t>(current_depth);
    nodes_.push_back(std::move(node));

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

    // Terminal node: return current depth + adjustment for leaf size
    if (node.is_leaf) {
        double path_length = static_cast<double>(current_depth);
        // Adjustment for leaves with multiple samples (expected additional path length)
        if (node.leaf_size > 1) {
            // c(n) = 2*H(n-1) - (2*(n-1)/n) where H is harmonic number
            // Approximation: c(n) ≈ 2*(ln(n-1) + 0.5772) - 2*(n-1)/n
            double n = static_cast<double>(node.leaf_size);
            double c_n = 2.0 * (std::log(n - 1.0) + 0.5772156649) - (2.0 * (n - 1.0) / n);
            path_length += c_n;
        }
        return path_length;
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
// IsolationTree Mixed-Type Implementation
// ============================================================================

void IsolationTree::BuildTreeMixed(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    const std::vector<size_t>& sample_indices,
    size_t current_depth,
    size_t max_depth,
    std::mt19937& rng,
    size_t ndim,
    CoefType coef_type,
    size_t ntry,
    double prob_pick_avg_gain
) {
    // Root call of a (re)build: drop any state from a previous build
    if (current_depth == 0) {
        nodes_.clear();
        root_idx_ = 0;
    }

    size_t n_samples = sample_indices.size();
    size_t n_features = data.size();

    // Terminal conditions: leaf node
    if (n_samples <= 1 || current_depth >= max_depth) {
        IsoNode leaf;
        leaf.is_leaf = true;
        leaf.depth = static_cast<uint16_t>(current_depth);
        leaf.leaf_size = n_samples;
        nodes_.push_back(std::move(leaf));
        return;
    }

    // Count numeric and categorical features
    std::vector<size_t> numeric_features;
    std::vector<size_t> categorical_features;
    for (size_t i = 0; i < n_features; i++) {
        if (column_info[i].type == FeatureType::NUMERIC) {
            numeric_features.push_back(i);
        } else {
            categorical_features.push_back(i);
        }
    }

    // Extended IF (ndim > 1): use hyperplane splits for numeric features only
    size_t effective_ndim = std::min(ndim, numeric_features.size());
    bool use_hyperplane = (effective_ndim > 1 && numeric_features.size() >= 2);

    // Generate ntry split candidates
    std::vector<SplitCandidate> candidates;
    candidates.reserve(ntry);

    for (size_t t = 0; t < ntry; t++) {
        SplitCandidate candidate;
        bool success = false;

        if (use_hyperplane) {
            // Extended IF: hyperplane split
            success = GenerateHyperplaneSplitCandidate(
                data, column_info, sample_indices, numeric_features,
                effective_ndim, coef_type, rng, candidate);
        } else {
            // Standard axis-aligned split: randomly select feature
            std::uniform_int_distribution<size_t> feat_dist(0, n_features - 1);
            size_t split_feature = feat_dist(rng);

            if (column_info[split_feature].type == FeatureType::NUMERIC) {
                success = GenerateNumericSplitCandidate(
                    data, sample_indices, split_feature, rng, candidate);
            } else {
                success = GenerateCategoricalSplitCandidate(
                    data, sample_indices, split_feature, rng, candidate);
            }
        }

        if (success) {
            candidates.push_back(std::move(candidate));
        }
    }

    // If no valid candidates, create leaf
    if (candidates.empty()) {
        IsoNode leaf;
        leaf.is_leaf = true;
        leaf.depth = static_cast<uint16_t>(current_depth);
        leaf.leaf_size = n_samples;
        nodes_.push_back(std::move(leaf));
        return;
    }

    // Select candidate: either best gain or random
    size_t selected_idx = 0;

    if (candidates.size() > 1 && prob_pick_avg_gain > 0.0) {
        std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
        if (prob_dist(rng) < prob_pick_avg_gain) {
            // Select candidate with highest gain
            double best_gain = candidates[0].gain_score;
            for (size_t i = 1; i < candidates.size(); i++) {
                if (candidates[i].gain_score > best_gain) {
                    best_gain = candidates[i].gain_score;
                    selected_idx = i;
                }
            }
        } else {
            // Random selection
            std::uniform_int_distribution<size_t> idx_dist(0, candidates.size() - 1);
            selected_idx = idx_dist(rng);
        }
    } else if (candidates.size() > 1) {
        // Random selection when prob_pick_avg_gain == 0
        std::uniform_int_distribution<size_t> idx_dist(0, candidates.size() - 1);
        selected_idx = idx_dist(rng);
    }

    const SplitCandidate& selected = candidates[selected_idx];

    // Create internal node based on selected candidate
    size_t node_idx = nodes_.size();
    IsoNode node;
    node.is_leaf = false;
    node.depth = static_cast<uint16_t>(current_depth);

    if (selected.ndim > 1) {
        // Hyperplane split
        node.split_ndim = selected.ndim;
        node.split_feature_indices = selected.hyperplane_features;
        node.split_coefficients = selected.hyperplane_coefficients;
        node.split_intercept = selected.hyperplane_intercept;
    } else if (selected.type == FeatureType::NUMERIC) {
        // Numeric axis-aligned split
        node.feature_type = FeatureType::NUMERIC;
        node.feature_idx = static_cast<uint16_t>(selected.feature_idx);
        node.split_value = selected.split_value;
        node.split_ndim = 1;
    } else {
        // Categorical split
        node.feature_type = FeatureType::CATEGORICAL;
        node.feature_idx = static_cast<uint16_t>(selected.feature_idx);
        node.left_categories = selected.left_categories;
        node.split_ndim = 1;
    }

    nodes_.push_back(std::move(node));

    // Recursively build subtrees
    nodes_[node_idx].left_child = static_cast<int32_t>(nodes_.size());
    BuildTreeMixed(data, column_info, selected.left_indices, current_depth + 1,
                   max_depth, rng, ndim, coef_type, ntry, prob_pick_avg_gain);

    nodes_[node_idx].right_child = static_cast<int32_t>(nodes_.size());
    BuildTreeMixed(data, column_info, selected.right_indices, current_depth + 1,
                   max_depth, rng, ndim, coef_type, ntry, prob_pick_avg_gain);
}

double IsolationTree::ComputePathLengthMixed(
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info
) const {
    if (nodes_.empty()) {
        return 0.0;
    }
    return ComputePathLengthMixedRecursive(0, numeric_values, category_indices, column_info, 0);
}

double IsolationTree::ComputePathLengthMixedRecursive(
    size_t node_idx,
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info,
    size_t current_depth
) const {
    const IsoNode& node = nodes_[node_idx];

    // Terminal node: return current depth + adjustment for leaf size
    if (node.is_leaf) {
        double path_length = static_cast<double>(current_depth);
        // Adjustment for leaves with multiple samples (expected additional path length)
        if (node.leaf_size > 1) {
            // c(n) = 2*H(n-1) - (2*(n-1)/n) where H is harmonic number
            // Approximation: c(n) ≈ 2*(ln(n-1) + 0.5772) - 2*(n-1)/n
            double n = static_cast<double>(node.leaf_size);
            double c_n = 2.0 * (std::log(n - 1.0) + 0.5772156649) - (2.0 * (n - 1.0) / n);
            path_length += c_n;
        }
        return path_length;
    }

    // Check if this is a hyperplane split (ndim > 1)
    if (node.split_ndim > 1 && !node.split_coefficients.empty()) {
        // Hyperplane split: compute dot product and compare to intercept
        double projection = 0.0;
        bool has_nan = false;

        for (size_t i = 0; i < node.split_coefficients.size(); i++) {
            size_t feat_idx = node.split_feature_indices[i];
            double val = numeric_values[feat_idx];
            if (std::isnan(val)) {
                has_nan = true;
                break;
            }
            projection += node.split_coefficients[i] * val;
        }

        // NaN or projection < intercept -> go left
        if (has_nan || projection < node.split_intercept) {
            return ComputePathLengthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        } else {
            return ComputePathLengthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        }
    }

    // Axis-aligned split (ndim == 1)
    size_t feat_idx = node.feature_idx;

    if (node.feature_type == FeatureType::NUMERIC) {
        // Numeric split: compare value to threshold
        double val = numeric_values[feat_idx];

        if (std::isnan(val) || val < node.split_value) {
            return ComputePathLengthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        } else {
            return ComputePathLengthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        }
    } else {
        // Categorical split: check category membership
        int cat_idx = category_indices[feat_idx];

        // Unknown/NULL or in left subset -> go left
        if (cat_idx < 0 || node.left_categories.count(cat_idx)) {
            return ComputePathLengthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        } else {
            return ComputePathLengthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, current_depth + 1);
        }
    }
}

// ============================================================================
// IsolationTree Density Scoring Implementation
// ============================================================================

double IsolationTree::ComputeDensityScoreMixed(
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info,
    size_t total_samples,
    double total_volume
) const {
    if (nodes_.empty() || total_samples == 0 || total_volume <= 0.0) {
        return 0.0;
    }
    return ComputeDensityScoreMixedRecursive(0, numeric_values, category_indices, column_info);
}

double IsolationTree::ComputeDensityScoreMixedRecursive(
    size_t node_idx,
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info
) const {
    const IsoNode& node = nodes_[node_idx];

    // Terminal node: return log density based on node's sample fraction and volume fraction
    if (node.is_leaf) {
        // For density scoring, we use the leaf_size as an indicator
        // Higher leaf_size at same depth = higher density = more normal
        // The score represents relative density (log scale)
        // Note: Actual volume bounds would be tracked in nodes for precise calculation
        // This simplified version uses leaf_size as proxy
        if (node.leaf_size <= 1) {
            return 0.0;  // Isolated point - neutral density
        }
        // Higher leaf_size means denser region = lower anomaly
        return std::log(static_cast<double>(node.leaf_size));
    }

    // Traverse to appropriate child based on split type
    if (node.split_ndim > 1 && !node.split_coefficients.empty()) {
        // Hyperplane split
        double projection = 0.0;
        bool has_nan = false;

        for (size_t i = 0; i < node.split_coefficients.size(); i++) {
            size_t feat_idx = node.split_feature_indices[i];
            double val = numeric_values[feat_idx];
            if (std::isnan(val)) {
                has_nan = true;
                break;
            }
            projection += node.split_coefficients[i] * val;
        }

        if (has_nan || projection < node.split_intercept) {
            return ComputeDensityScoreMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info);
        } else {
            return ComputeDensityScoreMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info);
        }
    }

    // Axis-aligned split
    size_t feat_idx = node.feature_idx;

    if (node.feature_type == FeatureType::NUMERIC) {
        double val = numeric_values[feat_idx];
        if (std::isnan(val) || val < node.split_value) {
            return ComputeDensityScoreMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info);
        } else {
            return ComputeDensityScoreMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info);
        }
    } else {
        int cat_idx = category_indices[feat_idx];
        if (cat_idx < 0 || node.left_categories.count(cat_idx)) {
            return ComputeDensityScoreMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info);
        } else {
            return ComputeDensityScoreMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info);
        }
    }
}

double IsolationTree::ComputeAdjDepthMixed(
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info
) const {
    if (nodes_.empty()) {
        return 0.0;
    }
    return ComputeAdjDepthMixedRecursive(0, numeric_values, category_indices, column_info, 0.0);
}

double IsolationTree::ComputeAdjDepthMixedRecursive(
    size_t node_idx,
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices,
    const std::vector<ColumnInfo>& column_info,
    double accumulated_depth
) const {
    const IsoNode& node = nodes_[node_idx];

    // Terminal node
    if (node.is_leaf) {
        double path_length = accumulated_depth;
        // Adjustment for leaves with multiple samples
        if (node.leaf_size > 1) {
            double n = static_cast<double>(node.leaf_size);
            double c_n = 2.0 * (std::log(n - 1.0) + 0.5772156649) - (2.0 * (n - 1.0) / n);
            path_length += c_n;
        }
        return path_length;
    }

    // For adjusted depth, each split contributes between 0 and 2 based on density
    // Formula: d = 2 / (1 + 1/(2p)) where p = (n_s/n_t) / (r_s/r_t)
    // Simplified: use 1.0 as base contribution (same as standard depth)
    // This is a simplified implementation; full version would track volume changes
    double depth_contribution = 1.0;

    // Traverse to appropriate child
    if (node.split_ndim > 1 && !node.split_coefficients.empty()) {
        double projection = 0.0;
        bool has_nan = false;

        for (size_t i = 0; i < node.split_coefficients.size(); i++) {
            size_t feat_idx = node.split_feature_indices[i];
            double val = numeric_values[feat_idx];
            if (std::isnan(val)) {
                has_nan = true;
                break;
            }
            projection += node.split_coefficients[i] * val;
        }

        if (has_nan || projection < node.split_intercept) {
            return ComputeAdjDepthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        } else {
            return ComputeAdjDepthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        }
    }

    size_t feat_idx = node.feature_idx;

    if (node.feature_type == FeatureType::NUMERIC) {
        double val = numeric_values[feat_idx];
        if (std::isnan(val) || val < node.split_value) {
            return ComputeAdjDepthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        } else {
            return ComputeAdjDepthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        }
    } else {
        int cat_idx = category_indices[feat_idx];
        if (cat_idx < 0 || node.left_categories.count(cat_idx)) {
            return ComputeAdjDepthMixedRecursive(node.left_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        } else {
            return ComputeAdjDepthMixedRecursive(node.right_child, numeric_values,
                category_indices, column_info, accumulated_depth + depth_contribution);
        }
    }
}

// ============================================================================
// IsolationForest Implementation
// ============================================================================

double IsolationForest::AveragePathLength(size_t n) {
    if (n <= 1) return 0.0;

    // Formula: 2 * H(n-1) - (2 * (n-1) / n)
    // Where H(n) is the harmonic number, approximated as ln(n) + γ (Euler-Mascheroni constant)
    // This approximation is accurate and O(1) instead of O(n)
    double h_approx = std::log(static_cast<double>(n - 1)) + 0.5772156649;
    return 2.0 * h_approx - (2.0 * static_cast<double>(n - 1) / static_cast<double>(n));
}

void IsolationForest::Fit(const std::vector<std::vector<double>>& data) {
    if (data.empty()) {
        return;
    }

    // Validate input shape at the public API boundary
    const size_t n_features = data[0].size();
    if (n_features == 0) {
        throw std::invalid_argument(
            "IsolationForest::Fit: rows must contain at least one feature");
    }
    if (n_features > std::numeric_limits<uint16_t>::max()) {
        throw std::invalid_argument(
            "IsolationForest::Fit: too many features (maximum 65535)");
    }
    for (const auto& row : data) {
        if (row.size() != n_features) {
            throw std::invalid_argument(
                "IsolationForest::Fit: all rows must have the same number of features");
        }
    }

    // Reset all model state so a refit behaves exactly like a freshly
    // constructed forest with the same parameters.
    rng_.seed(static_cast<std::mt19937::result_type>(seed_));
    trees_.assign(n_trees_, IsolationTree());
    is_mixed_type_ = false;
    column_info_.clear();
    sample_weights_.clear();
    total_volume_ = 0.0;

    n_features_ = n_features;

    // Ensure sample_size doesn't exceed data size
    actual_sample_size_ = std::min(sample_size_, data.size());

    // Compute max depth for trees
    // Default: ceil(log2(sample_size)) + 1
    size_t max_depth = static_cast<size_t>(std::ceil(std::log2(actual_sample_size_))) + 1;

    for (size_t t = 0; t < n_trees_; t++) {
        // Create random subsample of indices
        std::vector<size_t> sample_indices(data.size());
        std::iota(sample_indices.begin(), sample_indices.end(), 0);

        // Shuffle and take first sample_size
        std::shuffle(sample_indices.begin(), sample_indices.end(), rng_);
        sample_indices.resize(actual_sample_size_);

        // Build tree
        trees_[t].BuildTree(data, sample_indices, 0, max_depth, rng_);
    }
}

double IsolationForest::Score(const std::vector<double>& point) const {
    if (trees_.empty()) {
        return 0.0;
    }

    if (is_mixed_type_) {
        throw std::invalid_argument(
            "IsolationForest::Score: forest was fitted on mixed-type data; use ScoreMixed");
    }
    if (point.size() != n_features_) {
        throw std::invalid_argument(
            "IsolationForest::Score: point width does not match the fitted feature count");
    }

    // Compute average path length across all trees
    double sum_path_length = 0.0;
    for (const auto& tree : trees_) {
        sum_path_length += tree.ComputePathLength(point);
    }
    double avg_path_length = sum_path_length / static_cast<double>(trees_.size());

    // Normalize by average path length for random BST
    // Use actual_sample_size_ (the size actually used during training)
    double c = AveragePathLength(actual_sample_size_);
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

    // Find threshold at (1 - contamination) quantile
    // Use standard quantile formula: p-quantile at position p*(n-1)
    // Points with score > this threshold are anomalies
    double position = (1.0 - contamination_) * static_cast<double>(sorted_scores.size() - 1);
    size_t idx = static_cast<size_t>(std::floor(position));
    idx = std::min(idx, sorted_scores.size() - 1);

    return sorted_scores[idx];
}

// ============================================================================
// IsolationForest Mixed-Type Implementation
// ============================================================================

std::vector<size_t> IsolationForest::WeightedSample(
    const std::vector<double>& weights,
    size_t sample_size,
    std::mt19937& rng
) {
    // If weights empty or sample_size is 0, return empty
    if (weights.empty() || sample_size == 0) {
        return {};
    }

    // Compute cumulative weights for weighted sampling
    std::vector<double> cumulative(weights.size());
    cumulative[0] = weights[0];
    for (size_t i = 1; i < weights.size(); i++) {
        cumulative[i] = cumulative[i - 1] + weights[i];
    }

    double total_weight = cumulative.back();
    if (total_weight <= 0.0) {
        return {};  // No valid weights
    }

    // Sample without replacement using weighted random selection
    std::vector<size_t> selected;
    selected.reserve(sample_size);
    std::vector<bool> used(weights.size(), false);

    std::uniform_real_distribution<double> dist(0.0, 1.0);

    while (selected.size() < sample_size) {
        double target = dist(rng) * total_weight;

        // Binary search for the target
        size_t left = 0, right = cumulative.size();
        while (left < right) {
            size_t mid = left + (right - left) / 2;
            if (cumulative[mid] < target) {
                left = mid + 1;
            } else {
                right = mid;
            }
        }

        // Handle edge cases
        if (left >= cumulative.size()) {
            left = cumulative.size() - 1;
        }

        // If already used, find nearest unused
        if (used[left]) {
            // Linear search for nearest unused (simple fallback)
            bool found = false;
            for (size_t offset = 1; offset < weights.size() && !found; offset++) {
                if (left >= offset && !used[left - offset] && weights[left - offset] > 0) {
                    left = left - offset;
                    found = true;
                } else if (left + offset < weights.size() && !used[left + offset] && weights[left + offset] > 0) {
                    left = left + offset;
                    found = true;
                }
            }
            if (!found) {
                break;  // No more valid samples available
            }
        }

        if (weights[left] > 0 && !used[left]) {
            selected.push_back(left);
            used[left] = true;

            // Update cumulative weights by removing this sample's weight
            double removed_weight = weights[left];
            for (size_t i = left; i < cumulative.size(); i++) {
                cumulative[i] -= removed_weight;
            }
            total_weight -= removed_weight;

            if (total_weight <= 0.0) {
                break;  // No more weight to sample from
            }
        }
    }

    return selected;
}

void IsolationForest::FitMixed(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    const std::vector<double>& sample_weights
) {
    // Validate input shape at the public API boundary
    if (data.size() != column_info.size()) {
        throw std::invalid_argument(
            "IsolationForest::FitMixed: data and column_info must have the same number of columns");
    }
    if (data.empty() || data[0].size() == 0) {
        return;
    }
    if (data.size() > std::numeric_limits<uint16_t>::max()) {
        throw std::invalid_argument(
            "IsolationForest::FitMixed: too many columns (maximum 65535)");
    }
    if (ndim_ > std::numeric_limits<uint8_t>::max()) {
        throw std::invalid_argument(
            "IsolationForest::FitMixed: ndim exceeds the supported maximum (255)");
    }

    size_t n_rows = data[0].size();
    for (size_t col = 0; col < data.size(); col++) {
        if (data[col].type != column_info[col].type) {
            throw std::invalid_argument(
                "IsolationForest::FitMixed: column type mismatch between data and column_info");
        }
        if (data[col].size() != n_rows) {
            throw std::invalid_argument(
                "IsolationForest::FitMixed: all columns must have the same number of rows");
        }
    }

    // Reset all model state so a refit behaves exactly like a freshly
    // constructed forest with the same parameters.
    rng_.seed(static_cast<std::mt19937::result_type>(seed_));
    trees_.assign(n_trees_, IsolationTree());
    total_volume_ = 0.0;

    n_features_ = data.size();
    column_info_ = column_info;
    is_mixed_type_ = true;
    sample_weights_ = sample_weights;

    // Validate sample weights if provided
    if (!sample_weights_.empty()) {
        if (sample_weights_.size() != n_rows) {
            throw std::invalid_argument("sample_weights size must match data size");
        }
        // Check for negative weights
        for (size_t i = 0; i < sample_weights_.size(); i++) {
            if (sample_weights_[i] < 0.0) {
                throw std::invalid_argument("sample_weights cannot contain negative values");
            }
        }
    }

    // Compute total volume for density scoring (product of numeric ranges)
    total_volume_ = 1.0;
    for (size_t col = 0; col < n_features_; col++) {
        if (column_info[col].type == FeatureType::NUMERIC) {
            double min_val = std::numeric_limits<double>::max();
            double max_val = std::numeric_limits<double>::lowest();
            for (size_t row = 0; row < n_rows; row++) {
                double val = data[col].numeric_values[row];
                if (!std::isnan(val)) {
                    min_val = std::min(min_val, val);
                    max_val = std::max(max_val, val);
                }
            }
            double range = max_val - min_val;
            if (range > 0) {
                total_volume_ *= range;
            }
        }
        // Note: Categorical columns don't contribute to volume in the standard density formula
        // They are handled via category probability adjustments if needed
    }

    // Ensure sample_size doesn't exceed data size
    actual_sample_size_ = std::min(sample_size_, n_rows);

    // Compute max depth for trees
    // Default: ceil(log2(sample_size)) + 1
    size_t max_depth = static_cast<size_t>(std::ceil(std::log2(actual_sample_size_))) + 1;

    // Build all trees
    for (size_t t = 0; t < n_trees_; t++) {
        std::vector<size_t> sample_indices;

        if (sample_weights_.empty()) {
            // Uniform sampling (original behavior)
            sample_indices.resize(n_rows);
            std::iota(sample_indices.begin(), sample_indices.end(), 0);
            std::shuffle(sample_indices.begin(), sample_indices.end(), rng_);
            sample_indices.resize(actual_sample_size_);
        } else {
            // Weighted sampling
            sample_indices = WeightedSample(sample_weights_, actual_sample_size_, rng_);
        }

        // Build tree with mixed-type support (pass ndim, coef_type, ntry, prob_pick_avg_gain)
        trees_[t].BuildTreeMixed(data, column_info, sample_indices, 0, max_depth, rng_,
                                 ndim_, coef_type_, ntry_, prob_pick_avg_gain_);
    }
}

double IsolationForest::ScoreMixed(
    const std::vector<double>& numeric_values,
    const std::vector<int>& category_indices
) const {
    if (trees_.empty()) {
        return 0.0;
    }

    if (!is_mixed_type_) {
        throw std::invalid_argument(
            "IsolationForest::ScoreMixed: forest was fitted on numeric-only data; use Score");
    }
    if (numeric_values.size() != n_features_ || category_indices.size() != n_features_) {
        throw std::invalid_argument(
            "IsolationForest::ScoreMixed: point width does not match the fitted column count");
    }

    if (scoring_metric_ == ScoringMetric::Density) {
        // Density scoring: use geometric mean of density scores
        double sum_log_density = 0.0;
        for (const auto& tree : trees_) {
            double log_density = tree.ComputeDensityScoreMixed(
                numeric_values, category_indices, column_info_,
                actual_sample_size_, total_volume_);
            sum_log_density += log_density;
        }
        // Geometric mean via log: exp(mean(log(x)))
        double avg_log_density = sum_log_density / static_cast<double>(trees_.size());
        // Negate so that lower density = higher score (anomaly)
        // The score is unbounded, can be negative or positive
        return -avg_log_density;
    } else if (scoring_metric_ == ScoringMetric::AdjDepth) {
        // Adjusted depth scoring
        double sum_adj_depth = 0.0;
        for (const auto& tree : trees_) {
            sum_adj_depth += tree.ComputeAdjDepthMixed(
                numeric_values, category_indices, column_info_);
        }
        double avg_adj_depth = sum_adj_depth / static_cast<double>(trees_.size());

        // Normalize by average path length for random BST
        double c = AveragePathLength(actual_sample_size_);
        if (c == 0.0) {
            return 0.0;
        }

        // Same formula as depth scoring but with adjusted depth values
        return std::pow(2.0, -avg_adj_depth / c);
    } else {
        // Standard depth scoring (default)
        double sum_path_length = 0.0;
        for (const auto& tree : trees_) {
            sum_path_length += tree.ComputePathLengthMixed(numeric_values, category_indices, column_info_);
        }
        double avg_path_length = sum_path_length / static_cast<double>(trees_.size());

        // Normalize by average path length for random BST
        double c = AveragePathLength(actual_sample_size_);
        if (c == 0.0) {
            return 0.0;
        }

        // Anomaly score: 2^(-avg_path_length / c)
        // Range: [0, 1], where 1 = anomaly, 0 = inlier
        return std::pow(2.0, -avg_path_length / c);
    }
}

std::vector<double> IsolationForest::ScoreBatchMixed(
    const std::vector<ColumnData>& data
) const {
    if (data.empty() || data[0].size() == 0) {
        return {};
    }

    size_t n_rows = data[0].size();
    size_t n_features = data.size();

    for (size_t col = 0; col < n_features; col++) {
        if (data[col].size() != n_rows) {
            throw std::invalid_argument(
                "IsolationForest::ScoreBatchMixed: all columns must have the same number of rows");
        }
    }

    std::vector<double> scores;
    scores.reserve(n_rows);

    // Prepare vectors for each row
    std::vector<double> numeric_values(n_features);
    std::vector<int> category_indices(n_features);

    for (size_t row = 0; row < n_rows; row++) {
        // Extract values for this row
        for (size_t col = 0; col < n_features; col++) {
            if (data[col].type == FeatureType::NUMERIC) {
                numeric_values[col] = data[col].numeric_values[row];
                category_indices[col] = -1;  // Not used for numeric
            } else {
                numeric_values[col] = std::numeric_limits<double>::quiet_NaN();  // Not used for categorical
                category_indices[col] = data[col].category_indices[row];
            }
        }

        scores.push_back(ScoreMixed(numeric_values, category_indices));
    }

    return scores;
}

} // namespace anofox
} // namespace duckdb
