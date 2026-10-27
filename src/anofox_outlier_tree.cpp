#include "include/anofox_outlier_tree.hpp"
#include "include/anofox_function_alias.hpp"
#include "include/anofox_id_column.hpp"
#include "anofox_sql_utils.hpp"
#include "telemetry.hpp"
#include "anofox_trace.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb_compat.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include "anofox_tabular_banner.hpp"

namespace duckdb {
namespace anofox {

//===--------------------------------------------------------------------===//
// OutlierTree Implementation
//===--------------------------------------------------------------------===//

OutlierTree::OutlierTree(const OutlierTreeParams& params) : params_(params) {}

std::vector<OutlierExplanation> OutlierTree::FitPredict(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info
) {
    std::vector<OutlierExplanation> outliers;
    clusters_evaluated_ = 0;
    max_depth_reached_ = 0;
    outlier_slots_.clear();

    if (data.empty() || column_info.empty()) {
        return outliers;
    }

    size_t n_rows = data[0].size();
    if (n_rows < 2) {
        return outliers;  // Need at least 2 rows for outlier detection
    }

    // Create initial row indices (all rows)
    std::vector<size_t> all_indices(n_rows);
    std::iota(all_indices.begin(), all_indices.end(), 0);

    // For each column as target, build a tree and find outliers
    for (size_t target_col = 0; target_col < data.size(); ++target_col) {
        AnofoxTrace(AnofoxLogLevel::Debug,
                   "OutlierTree: Processing target column: " + column_info[target_col].name +
                   " (" + (column_info[target_col].type == FeatureType::NUMERIC ? "numeric" : "categorical") + ")");

        std::vector<SplitCondition> no_conditions;
        BuildTreeForColumn(data, column_info, target_col, all_indices,
                          no_conditions, 0, outliers);
    }

    AnofoxTrace(AnofoxLogLevel::Info,
               "OutlierTree: Found " + std::to_string(outliers.size()) + " outliers, evaluated " +
               std::to_string(clusters_evaluated_) + " clusters, max depth " +
               std::to_string(max_depth_reached_));

    return outliers;
}

void OutlierTree::BuildTreeForColumn(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t target_col,
    const std::vector<size_t>& row_indices,
    const std::vector<SplitCondition>& current_conditions,
    size_t current_depth,
    std::vector<OutlierExplanation>& outliers
) {
    // Update max depth reached
    if (current_depth > max_depth_reached_) {
        max_depth_reached_ = current_depth;
    }

    // Check minimum size requirements
    size_t min_size = column_info[target_col].type == FeatureType::NUMERIC
        ? params_.min_size_numeric
        : params_.min_size_categ;

    if (row_indices.size() < min_size) {
        return;
    }

    // Check for outliers in current cluster
    clusters_evaluated_++;
    if (column_info[target_col].type == FeatureType::NUMERIC) {
        FindOutliersInNumericCluster(data, column_info, target_col,
                                     row_indices, current_conditions, outliers);
    } else {
        FindOutliersInCategoricalCluster(data, column_info, target_col,
                                         row_indices, current_conditions, outliers);
    }

    // Check depth limit
    if (current_depth >= params_.max_depth) {
        return;
    }

    // Cache the parent-cluster statistic once: it depends only on the target
    // column and the current row subset, so it is identical for every
    // candidate split evaluated below (issue #60).
    const bool numeric_target = column_info[target_col].type == FeatureType::NUMERIC;
    double current_target_metric;
    if (numeric_target) {
        current_target_metric = ComputeVariance(data[target_col], row_indices);
    } else {
        auto stats = ComputeCategoricalStats(data[target_col], column_info[target_col], row_indices);
        current_target_metric = ComputeEntropy(stats.category_counts, stats.n_samples);
    }

    // Find best split among all predictor columns. The partitions of the best
    // split are kept so the winning split is not partitioned a second time.
    std::optional<SplitCondition> best_split;
    double best_gain = 0.0;
    std::vector<size_t> left_indices, right_indices;            // per-candidate scratch
    std::vector<size_t> best_left_indices, best_right_indices;  // partitions of best_split

    for (size_t pred_col = 0; pred_col < data.size(); ++pred_col) {
        if (pred_col == target_col) continue;  // Skip target column

        std::optional<SplitCondition> split;
        if (column_info[pred_col].type == FeatureType::NUMERIC) {
            split = FindBestNumericSplit(data, column_info, pred_col, target_col, row_indices,
                                         current_target_metric);
        } else {
            split = FindBestCategoricalSplit(data, column_info, pred_col, target_col, row_indices,
                                             current_target_metric);
        }

        if (split) {
            // Compute gain for this split
            PartitionRows(data, *split, row_indices, left_indices, right_indices);

            double gain = 0.0;
            if (numeric_target) {
                gain = ComputeVarianceGain(left_indices, right_indices, data[target_col], current_target_metric);
            } else {
                gain = ComputeInfoGain(left_indices, right_indices, data[target_col],
                                       column_info[target_col], current_target_metric);
            }

            if (gain > best_gain) {
                best_gain = gain;
                best_split = split;
                best_left_indices.swap(left_indices);
                best_right_indices.swap(right_indices);
            }
        }
    }

    // If no good split found, stop
    if (!best_split) {
        return;
    }

    // Create conditions for left and right branches
    std::vector<SplitCondition> left_conditions = current_conditions;
    left_conditions.push_back(*best_split);

    SplitCondition right_condition = *best_split;
    if (right_condition.column_type == FeatureType::NUMERIC) {
        right_condition.is_less_than = false;  // Invert for right branch
    } else {
        // For categorical, the right branch is the complement of the left
        // categories — mark the condition as negated (NOT IN left_categories)
        right_condition.is_negated = true;
    }
    std::vector<SplitCondition> right_conditions = current_conditions;
    right_conditions.push_back(right_condition);

    // Recurse on both branches
    if (best_left_indices.size() >= min_size) {
        BuildTreeForColumn(data, column_info, target_col, best_left_indices,
                          left_conditions, current_depth + 1, outliers);
    }
    if (best_right_indices.size() >= min_size) {
        BuildTreeForColumn(data, column_info, target_col, best_right_indices,
                          right_conditions, current_depth + 1, outliers);
    }
}

void OutlierTree::FindOutliersInNumericCluster(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t target_col,
    const std::vector<size_t>& row_indices,
    const std::vector<SplitCondition>& conditions,
    std::vector<OutlierExplanation>& outliers
) {
    if (row_indices.size() < params_.min_size_numeric) {
        return;
    }

    // Compute cluster statistics
    NumericClusterStats stats = ComputeNumericStats(data[target_col], row_indices);

    AnofoxTrace(AnofoxLogLevel::Debug,
               "OutlierTree: Evaluating cluster for " + column_info[target_col].name +
               ", n=" + std::to_string(stats.n_samples) +
               ", median=" + std::to_string(stats.median) +
               ", mad_scaled=" + std::to_string(stats.mad_scaled) +
               ", z_outlier=" + std::to_string(params_.z_outlier));

    if (stats.n_samples < 3 || stats.mad_scaled < 1e-10) {
        return;  // Not enough variation to detect outliers
    }

    // Maximum number of outliers allowed in this cluster
    size_t max_outliers = static_cast<size_t>(std::ceil(stats.n_samples * params_.max_perc_outliers));
    if (max_outliers == 0) max_outliers = 1;

    // Calculate confidence bounds
    double lower_bound = stats.GetLowerBound(params_.z_norm);
    double upper_bound = stats.GetUpperBound(params_.z_norm);

    // Collect potential outliers
    std::vector<std::pair<size_t, double>> potential_outliers;  // (row_idx, z_score)

    for (size_t idx : row_indices) {
        double value = data[target_col].numeric_values[idx];
        // Use robust z-score (based on median/MAD) for outlier detection
        double z = stats.GetRobustZScore(value);

        AnofoxTrace(AnofoxLogLevel::Debug,
                   "OutlierTree: Row " + std::to_string(idx) +
                   " value=" + std::to_string(value) +
                   " robust_z=" + std::to_string(z));

        if (std::abs(z) >= params_.z_outlier) {
            potential_outliers.push_back({idx, z});
        }
    }

    // Sort by absolute z-score (most extreme first)
    std::sort(potential_outliers.begin(), potential_outliers.end(),
              [](const auto& a, const auto& b) { return std::abs(a.second) > std::abs(b.second); });

    // Flag outliers up to max_outliers limit
    size_t outlier_count = 0;
    for (const auto& [row_idx, z_score] : potential_outliers) {
        if (outlier_count >= max_outliers) break;

        double score = NumericClusterStats::ChebyshevBound(z_score);

        OutlierExplanation outlier;
        outlier.row_idx = row_idx;
        outlier.target_column_idx = target_col;
        outlier.target_column_name = column_info[target_col].name;
        outlier.outlier_value = data[target_col].numeric_values[row_idx];
        outlier.cluster_mean = stats.mean;
        outlier.cluster_sd = stats.sd;
        outlier.cluster_median = stats.median;
        outlier.cluster_size = stats.n_samples;
        // An infinite value (or one so far out that the robust z overflows) is an outlier
        // without a meaningful z-score: reported as NULL, not inf
        outlier.z_score = std::isfinite(z_score) ? z_score : std::numeric_limits<double>::quiet_NaN();
        outlier.lower_bound = lower_bound;
        outlier.upper_bound = upper_bound;
        outlier.conditions = conditions;
        outlier.outlier_score = score;

        GenerateExplanation(outlier, column_info);
        if (AddOrReplaceOutlier(outliers, std::move(outlier))) {
            outlier_count++;
        }
    }
}

void OutlierTree::FindOutliersInCategoricalCluster(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t target_col,
    const std::vector<size_t>& row_indices,
    const std::vector<SplitCondition>& conditions,
    std::vector<OutlierExplanation>& outliers
) {
    if (row_indices.size() < params_.min_size_categ) {
        return;
    }

    // Compute category statistics
    CategoricalClusterStats stats = ComputeCategoricalStats(
        data[target_col], column_info[target_col], row_indices);

    if (stats.n_samples < 3 || stats.n_categories < 2) {
        return;
    }

    // Maximum number of outliers allowed
    size_t max_outliers = static_cast<size_t>(std::ceil(stats.n_samples * params_.max_perc_outliers));
    if (max_outliers == 0) max_outliers = 1;

    // Find categories that are rare enough to be outliers
    // Using a simple threshold: proportion < (1 / z_outlier^2)
    double threshold = 1.0 / (params_.z_outlier * params_.z_outlier);

    std::vector<std::pair<int, double>> rare_categories;  // (category_idx, proportion)

    for (size_t cat = 0; cat < stats.category_counts.size(); ++cat) {
        double prop = stats.GetProportion(static_cast<int>(cat));
        if (prop > 0 && prop < threshold) {
            rare_categories.push_back({static_cast<int>(cat), prop});
        }
    }

    // Sort by proportion (rarest first)
    std::sort(rare_categories.begin(), rare_categories.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });

    // Flag rows with rare categories as outliers
    size_t outlier_count = 0;
    for (const auto& [cat_idx, prop] : rare_categories) {
        for (size_t idx : row_indices) {
            if (outlier_count >= max_outliers) break;

            if (data[target_col].category_indices[idx] == cat_idx) {
                double score = prop;  // Lower proportion = rarer = lower score

                OutlierExplanation outlier;
                outlier.row_idx = idx;
                outlier.target_column_idx = target_col;
                outlier.target_column_name = column_info[target_col].name;
                outlier.outlier_value = column_info[target_col].index_to_category[cat_idx];
                outlier.cluster_size = stats.n_samples;
                outlier.outlier_score = score;
                outlier.conditions = conditions;

                // For categorical, use proportion-based "z-score" analog
                outlier.z_score = 1.0 / std::sqrt(prop);

                GenerateExplanation(outlier, column_info);
                if (AddOrReplaceOutlier(outliers, std::move(outlier))) {
                    outlier_count++;
                }
            }
        }
    }
}

std::optional<SplitCondition> OutlierTree::FindBestNumericSplit(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t predictor_col,
    size_t target_col,
    const std::vector<size_t>& row_indices,
    double current_target_metric
) {
    if (row_indices.size() < 2 * params_.min_size_numeric) {
        return std::nullopt;
    }

    // Collect valid values and sort
    std::vector<std::pair<double, size_t>> values;  // (value, original_index)
    values.reserve(row_indices.size());

    for (size_t idx : row_indices) {
        double val = data[predictor_col].numeric_values[idx];
        if (!std::isnan(val) && !std::isinf(val)) {
            values.push_back({val, idx});
        }
    }

    if (values.size() < 2 * params_.min_size_numeric) {
        return std::nullopt;
    }

    std::sort(values.begin(), values.end());

    // Find best split point (try quantiles)
    double best_gain = 0.0;
    double best_split_value = 0.0;

    size_t min_left = params_.min_size_numeric;
    size_t min_right = params_.min_size_numeric;

    // Try several candidate split points
    std::vector<double> candidates;
    for (double q : {0.25, 0.5, 0.75}) {
        size_t idx = static_cast<size_t>(q * values.size());
        if (idx > 0 && idx < values.size()) {
            candidates.push_back((values[idx - 1].first + values[idx].first) / 2.0);
        }
    }

    // Scratch buffers reused across candidate split points (issue #60)
    std::vector<size_t> left_idx, right_idx;
    left_idx.reserve(values.size());
    right_idx.reserve(values.size());

    for (double split_val : candidates) {
        left_idx.clear();
        right_idx.clear();
        for (const auto& [val, idx] : values) {
            if (val <= split_val) {
                left_idx.push_back(idx);
            } else {
                right_idx.push_back(idx);
            }
        }

        if (left_idx.size() < min_left || right_idx.size() < min_right) {
            continue;
        }

        // The parent statistic is supplied by the caller; it is constant for
        // all candidates of this (target, row subset) pair.
        double gain = 0.0;
        if (column_info[target_col].type == FeatureType::NUMERIC) {
            gain = ComputeVarianceGain(left_idx, right_idx, data[target_col], current_target_metric);
        } else {
            gain = ComputeInfoGain(left_idx, right_idx, data[target_col],
                                   column_info[target_col], current_target_metric);
        }

        if (gain > best_gain) {
            best_gain = gain;
            best_split_value = split_val;
        }
    }

    if (best_gain <= 0) {
        return std::nullopt;
    }

    SplitCondition split;
    split.column_idx = predictor_col;
    split.column_type = FeatureType::NUMERIC;
    split.column_name = column_info[predictor_col].name;
    split.split_value = best_split_value;
    split.is_less_than = true;

    return split;
}

std::optional<SplitCondition> OutlierTree::FindBestCategoricalSplit(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t predictor_col,
    size_t target_col,
    const std::vector<size_t>& row_indices,
    double current_target_metric
) {
    if (row_indices.size() < 2 * params_.min_size_categ) {
        return std::nullopt;
    }

    // Count occurrences of each category
    std::unordered_map<int, std::vector<size_t>> category_rows;
    for (size_t idx : row_indices) {
        int cat = data[predictor_col].category_indices[idx];
        if (cat >= 0) {  // Valid category
            category_rows[cat].push_back(idx);
        }
    }

    if (category_rows.size() < 2) {
        return std::nullopt;  // Need at least 2 categories to split
    }

    // Simple binary split: put most common category on one side.
    // Iterate categories in ascending index order rather than over the
    // unordered_map directly: its iteration order is implementation-defined and
    // differs across STL implementations (libstdc++/libc++/MSVC), which would
    // otherwise break ties for "most common" differently per platform and emit
    // a logically-equivalent but textually-different split (e.g. IN ["A"] vs
    // NOT IN ["B"]). Sorting makes the chosen category — and the emitted
    // explanation — deterministic everywhere; ties resolve to the lowest index.
    std::vector<int> sorted_cats;
    sorted_cats.reserve(category_rows.size());
    for (const auto& [cat, rows] : category_rows) {
        sorted_cats.push_back(cat);
    }
    std::sort(sorted_cats.begin(), sorted_cats.end());

    int most_common_cat = -1;
    size_t max_count = 0;
    for (int cat : sorted_cats) {
        if (category_rows[cat].size() > max_count) {
            max_count = category_rows[cat].size();
            most_common_cat = cat;
        }
    }

    // Left branch: most common category
    // Right branch: all others
    std::vector<size_t> left_idx, right_idx;
    for (size_t idx : row_indices) {
        int cat = data[predictor_col].category_indices[idx];
        if (cat == most_common_cat) {
            left_idx.push_back(idx);
        } else if (cat >= 0) {
            right_idx.push_back(idx);
        }
    }

    if (left_idx.size() < params_.min_size_categ ||
        right_idx.size() < params_.min_size_categ) {
        return std::nullopt;
    }

    // Compute gain against the caller-supplied parent statistic (issue #60)
    double gain = 0.0;
    if (column_info[target_col].type == FeatureType::NUMERIC) {
        gain = ComputeVarianceGain(left_idx, right_idx, data[target_col], current_target_metric);
    } else {
        gain = ComputeInfoGain(left_idx, right_idx, data[target_col],
                               column_info[target_col], current_target_metric);
    }

    if (gain <= 0) {
        return std::nullopt;
    }

    SplitCondition split;
    split.column_idx = predictor_col;
    split.column_type = FeatureType::CATEGORICAL;
    split.column_name = column_info[predictor_col].name;
    split.left_categories.insert(most_common_cat);
    if (most_common_cat >= 0 &&
        most_common_cat < static_cast<int>(column_info[predictor_col].index_to_category.size())) {
        split.left_category_names.push_back(
            column_info[predictor_col].index_to_category[most_common_cat]);
    }

    return split;
}

NumericClusterStats OutlierTree::ComputeNumericStats(
    const ColumnData& data,
    const std::vector<size_t>& row_indices
) {
    NumericClusterStats stats;

    std::vector<double> values;
    values.reserve(row_indices.size());

    for (size_t idx : row_indices) {
        double val = data.numeric_values[idx];
        if (!std::isnan(val) && !std::isinf(val)) {
            values.push_back(val);
        }
    }

    if (values.empty()) {
        return stats;
    }

    stats.n_samples = values.size();

    // Mean and standard deviation over the values scaled by their largest magnitude, so values
    // near the DOUBLE range (1e308) do not overflow the sums. A result that is itself out of
    // range stays inf and is reported as NULL.
    double scale = 0.0;
    for (double v : values) {
        scale = std::max(scale, std::abs(v));
    }
    if (scale == 0.0) {
        scale = 1.0;
    }
    double scaled_sum = 0.0;
    for (double v : values) {
        scaled_sum += v / scale;
    }
    double scaled_mean = scaled_sum / stats.n_samples;
    stats.mean = scaled_mean * scale;

    double scaled_sq_sum = 0.0;
    for (double v : values) {
        double diff = v / scale - scaled_mean;
        scaled_sq_sum += diff * diff;
    }
    stats.sd = stats.n_samples > 1 ? std::sqrt(scaled_sq_sum / (stats.n_samples - 1)) * scale : 0.0;

    // Sort for quantiles
    std::sort(values.begin(), values.end());
    stats.min_val = values.front();
    stats.max_val = values.back();
    stats.median = values[values.size() / 2];
    stats.q1 = values[values.size() / 4];
    stats.q3 = values[3 * values.size() / 4];

    // Compute MAD (Median Absolute Deviation) - robust measure of spread
    // MAD = median(|x_i - median|)
    std::vector<double> abs_deviations;
    abs_deviations.reserve(values.size());
    for (double v : values) {
        abs_deviations.push_back(std::abs(v - stats.median));
    }
    std::sort(abs_deviations.begin(), abs_deviations.end());
    stats.mad = abs_deviations[abs_deviations.size() / 2];
    // Scale factor 1.4826 makes MAD consistent with SD for normal distributions
    stats.mad_scaled = stats.mad * 1.4826;

    return stats;
}

CategoricalClusterStats OutlierTree::ComputeCategoricalStats(
    const ColumnData& data,
    const ColumnInfo& column_info,
    const std::vector<size_t>& row_indices
) {
    CategoricalClusterStats stats;
    stats.n_categories = column_info.GetCategoryCount();
    stats.category_counts.resize(stats.n_categories, 0);

    for (size_t idx : row_indices) {
        int cat = data.category_indices[idx];
        if (cat >= 0 && cat < static_cast<int>(stats.n_categories)) {
            stats.category_counts[cat]++;
            stats.n_samples++;
        }
    }

    return stats;
}

void OutlierTree::GenerateExplanation(
    OutlierExplanation& outlier,
    const std::vector<ColumnInfo>& column_info
) {
    std::ostringstream oss;

    // Start with the outlier value
    oss << "Value " << outlier.GetOutlierValueString()
        << " for column '" << outlier.target_column_name << "'";

    // Describe the outlier
    if (column_info[outlier.target_column_idx].type == FeatureType::NUMERIC) {
        // The direction from the value itself: the z-score is NaN for an infinite value
        bool is_high = std::holds_alternative<double>(outlier.outlier_value) &&
                       std::get<double>(outlier.outlier_value) > outlier.cluster_median;
        // A statistic out of the DOUBLE range is NULL in the result and "n/a" here
        auto format_stat = [](double stat) {
            std::ostringstream stat_text;
            if (std::isfinite(stat)) {
                stat_text << std::fixed << std::setprecision(2) << stat;
            } else {
                stat_text << "n/a";
            }
            return stat_text.str();
        };
        oss << " is unusually " << (is_high ? "high" : "low")
            << " (mean: " << format_stat(outlier.cluster_mean)
            << ", SD: " << format_stat(outlier.cluster_sd)
            << ", z-score: " << format_stat(outlier.z_score) << ")";
    } else {
        oss << " is unusually rare (cluster size: " << outlier.cluster_size << ")";
    }

    // Add conditions
    if (!outlier.conditions.empty()) {
        oss << " when ";
        for (size_t i = 0; i < outlier.conditions.size(); ++i) {
            if (i > 0) oss << " AND ";
            oss << outlier.conditions[i].ToString();
        }
    }

    outlier.explanation = oss.str();
}

double OutlierTree::ComputeVarianceGain(
    const std::vector<size_t>& left_indices,
    const std::vector<size_t>& right_indices,
    const ColumnData& data,
    double current_variance
) {
    if (left_indices.empty() || right_indices.empty()) {
        return 0.0;
    }

    double left_var = ComputeVariance(data, left_indices);
    double right_var = ComputeVariance(data, right_indices);

    size_t total = left_indices.size() + right_indices.size();
    double weighted_var = (left_indices.size() * left_var + right_indices.size() * right_var) / total;

    return current_variance - weighted_var;
}

double OutlierTree::ComputeInfoGain(
    const std::vector<size_t>& left_indices,
    const std::vector<size_t>& right_indices,
    const ColumnData& data,
    const ColumnInfo& column_info,
    double current_entropy
) {
    if (left_indices.empty() || right_indices.empty()) {
        return 0.0;
    }

    auto left_stats = ComputeCategoricalStats(data, column_info, left_indices);
    auto right_stats = ComputeCategoricalStats(data, column_info, right_indices);

    double left_entropy = ComputeEntropy(left_stats.category_counts, left_stats.n_samples);
    double right_entropy = ComputeEntropy(right_stats.category_counts, right_stats.n_samples);

    size_t total = left_stats.n_samples + right_stats.n_samples;
    double weighted_entropy = (left_stats.n_samples * left_entropy +
                               right_stats.n_samples * right_entropy) / total;

    return current_entropy - weighted_entropy;
}

double OutlierTree::ComputeEntropy(const std::vector<size_t>& category_counts, size_t total) {
    if (total == 0) return 0.0;

    double entropy = 0.0;
    for (size_t count : category_counts) {
        if (count > 0) {
            double p = static_cast<double>(count) / total;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

double OutlierTree::ComputeVariance(const ColumnData& data, const std::vector<size_t>& row_indices) {
    if (row_indices.size() < 2) return 0.0;

    double sum = 0.0;
    size_t count = 0;
    for (size_t idx : row_indices) {
        double val = data.numeric_values[idx];
        if (!std::isnan(val) && !std::isinf(val)) {
            sum += val;
            count++;
        }
    }

    if (count < 2) return 0.0;

    double mean = sum / count;
    double sq_sum = 0.0;
    for (size_t idx : row_indices) {
        double val = data.numeric_values[idx];
        if (!std::isnan(val) && !std::isinf(val)) {
            sq_sum += (val - mean) * (val - mean);
        }
    }

    return sq_sum / (count - 1);
}

void OutlierTree::PartitionRows(
    const std::vector<ColumnData>& data,
    const SplitCondition& split,
    const std::vector<size_t>& row_indices,
    std::vector<size_t>& left_indices,
    std::vector<size_t>& right_indices
) {
    left_indices.clear();
    right_indices.clear();
    left_indices.reserve(row_indices.size());
    right_indices.reserve(row_indices.size());

    for (size_t idx : row_indices) {
        bool goes_left = false;

        if (split.column_type == FeatureType::NUMERIC) {
            double val = data[split.column_idx].numeric_values[idx];
            if (std::isnan(val) || std::isinf(val)) {
                continue;  // Skip invalid values
            }
            goes_left = split.is_less_than ? (val <= split.split_value) : (val > split.split_value);
        } else {
            int cat = data[split.column_idx].category_indices[idx];
            if (cat < 0) {
                continue;  // Skip invalid categories
            }
            goes_left = split.left_categories.count(cat) > 0;
            if (split.is_negated) {
                goes_left = !goes_left;
            }
        }

        if (goes_left) {
            left_indices.push_back(idx);
        } else {
            right_indices.push_back(idx);
        }
    }
}

bool OutlierTree::AddOrReplaceOutlier(
    std::vector<OutlierExplanation>& outliers,
    OutlierExplanation&& outlier
) {
    auto key = std::make_pair(outlier.row_idx, outlier.target_column_idx);
    auto it = outlier_slots_.find(key);
    if (it == outlier_slots_.end()) {
        outlier_slots_.emplace(key, outliers.size());
        outliers.push_back(std::move(outlier));
        return true;
    }
    // Keep the finding with the lower (more extreme) score
    if (outliers[it->second].outlier_score <= outlier.outlier_score) {
        return false;
    }
    outliers[it->second] = std::move(outlier);
    return true;
}

//===--------------------------------------------------------------------===//
// DuckDB Table Function Implementation
//===--------------------------------------------------------------------===//

// Bind data for outlier_tree table function
struct OutlierTreeBindData : public TableFunctionData {
    std::string table_name;
    std::vector<std::string> column_names;
    std::vector<FeatureType> column_types;  // resolved at bind: VARCHAR/ENUM categorical, else numeric
    std::string output_mode;  // "summary" or "outliers"
    OutlierTreeParams params;
    IdColumn id_column;       // optional pass-through identifier ('outliers' mode)

    OutlierTreeBindData(std::string tbl, std::vector<std::string> cols,
                        std::string mode, OutlierTreeParams p)
        : table_name(std::move(tbl)), column_names(std::move(cols)),
          output_mode(std::move(mode)), params(p) {}
};

// Global state for outlier_tree execution
struct OutlierTreeGlobalState : public GlobalTableFunctionState {
    bool executed = false;
    idx_t current_row = 0;

    // Results
    std::vector<OutlierExplanation> outliers;
    // Maps a compacted (NULL-filtered) row index back to its 1-based
    // position in the source table, so reported row ids match the source.
    std::vector<int64_t> source_row_ids;
    // id_column values, indexed like source_row_ids (empty without id_column)
    std::vector<Value> id_values;
    int64_t total_rows = 0;
    int64_t columns_analyzed = 0;
    int64_t clusters_evaluated = 0;
    int64_t max_depth_reached = 0;
};

// Initialize global state
static unique_ptr<GlobalTableFunctionState> OutlierTreeInit(ClientContext &, TableFunctionInitInput &) {
    return make_uniq<OutlierTreeGlobalState>();
}

// Bind function
static unique_ptr<FunctionData> OutlierTreeBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, duckdb_compat::BindNames &names) {
    PostHogTelemetry::Instance().RecordFunctionCall("outlier_tree");

    if (input.inputs.size() < 2) {
        throw BinderException("outlier_tree requires at least 2 arguments: table_name, columns");
    }

    std::string table_name = RequireNonNullArgument(input, 0, "outlier_tree", "table_name");
    RejectTempTable(context, "outlier_tree", table_name);
    std::string columns_str = RequireNonNullArgument(input, 1, "outlier_tree", "columns");

    // Parse column names
    std::vector<std::string> column_names;
    std::istringstream iss(columns_str);
    std::string col;
    while (std::getline(iss, col, ',')) {
        // Trim whitespace
        size_t start = col.find_first_not_of(" \t");
        size_t end = col.find_last_not_of(" \t");
        if (start != std::string::npos) {
            col = col.substr(start, end - start + 1);
            // Remove quotes if present
            if ((col.front() == '"' && col.back() == '"') ||
                (col.front() == '\'' && col.back() == '\'')) {
                col = col.substr(1, col.length() - 2);
            }
            column_names.push_back(col);
        }
    }

    if (column_names.empty()) {
        throw BinderException("columns parameter must contain at least one column name");
    }

    // Resolve the feature columns now (case-insensitive, the table's spelling from here on) and
    // check their types: numeric columns (BOOLEAN as 0/1) are read as DOUBLE, VARCHAR and ENUM
    // as categories. Anything else (DATE, TIMESTAMP, LIST, ...) failed late with a cast error.
    std::vector<FeatureType> column_types;
    {
        std::vector<std::string> resolved_names;
        for (auto &column : ResolveColumns(context, "outlier_tree", table_name,
                                                    vector<string>(column_names.begin(), column_names.end()))) {
            auto type_id = column.type.id();
            if (type_id == LogicalTypeId::VARCHAR || type_id == LogicalTypeId::ENUM) {
                column_types.push_back(FeatureType::CATEGORICAL);
            } else if (column.type.IsNumeric() || type_id == LogicalTypeId::BOOLEAN) {
                column_types.push_back(FeatureType::NUMERIC);
            } else {
                throw BinderException("outlier_tree: column '%s' must be numeric or categorical (VARCHAR), got %s",
                                      column.name, column.type.ToString());
            }
            resolved_names.push_back(std::move(column.name));
        }
        column_names = std::move(resolved_names);
    }

    // Parse optional parameters (read integral values as signed so that
    // negative inputs can be rejected before any cast to size_t)
    // An absent or NULL parameter takes its default (GetValue on NULL would be an INTERNAL error)
    auto optional = [&](idx_t index, Value default_value) {
        return input.inputs.size() > index && !input.inputs[index].IsNull() ? input.inputs[index] : default_value;
    };
    std::string output_mode = LowerOption(optional(2, Value("summary")).ToString());
    int64_t max_depth_raw = optional(3, Value::BIGINT(4)).GetValue<int64_t>();
    double max_perc_outliers = optional(4, Value::DOUBLE(0.01)).GetValue<double>();
    int64_t min_size_numeric_raw = optional(5, Value::BIGINT(25)).GetValue<int64_t>();
    int64_t min_size_categ_raw = optional(6, Value::BIGINT(75)).GetValue<int64_t>();
    double z_norm = optional(7, Value::DOUBLE(2.67)).GetValue<double>();
    double z_outlier = optional(8, Value::DOUBLE(8.0)).GetValue<double>();

    if (output_mode != "summary" && output_mode != "outliers") {
        throw BinderException("outlier_tree: output_mode must be 'summary' or 'outliers'");
    }

    // Validate parameters while still signed
    static constexpr int64_t MAX_TREE_DEPTH = 1024;
    if (max_depth_raw < 1 || max_depth_raw > MAX_TREE_DEPTH) {
        throw BinderException("max_depth must be >= 1 and <= %lld", MAX_TREE_DEPTH);
    }
    if (min_size_numeric_raw < 1) {
        throw BinderException("min_size_numeric must be >= 1");
    }
    if (min_size_categ_raw < 1) {
        throw BinderException("min_size_categ must be >= 1");
    }
    if (max_perc_outliers <= 0 || max_perc_outliers > 1) {
        throw BinderException("max_perc_outliers must be between 0 and 1");
    }
    if (z_outlier <= 0) {
        throw BinderException("z_outlier must be positive");
    }
    if (z_norm <= 0) {
        throw BinderException("z_norm must be positive");
    }

    OutlierTreeParams params(static_cast<size_t>(max_depth_raw), max_perc_outliers,
                             static_cast<size_t>(min_size_numeric_raw),
                             static_cast<size_t>(min_size_categ_raw), z_norm, z_outlier);

    // Define output schema based on mode
    if (output_mode == "outliers") {
        names.emplace_back("row_id");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("column_name");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        names.emplace_back("outlier_value");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        // The value as a number for numeric columns (NULL for categorical
        // ones), without the precision loss of the VARCHAR rendering (#111)
        names.emplace_back("outlier_value_num");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("cluster_mean");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("cluster_sd");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("cluster_size");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("z_score");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("lower_bound");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("upper_bound");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
        names.emplace_back("conditions");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        names.emplace_back("explanation");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        names.emplace_back("outlier_score");
        return_types.emplace_back(LogicalType(LogicalTypeId::DOUBLE));
    } else {
        // Summary mode
        names.emplace_back("status");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        names.emplace_back("total_rows");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("outlier_count");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("columns_analyzed");
        return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        names.emplace_back("clusters_evaluated");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("max_depth_reached");
        return_types.emplace_back(LogicalType(LogicalTypeId::INTEGER));
        names.emplace_back("message");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
    }

    auto id_column = BindIdColumn(context, input, "outlier_tree", table_name, output_mode == "outliers", "outliers");
    InsertIdColumn(id_column, "outlier_tree", return_types, names);

    auto bind_data = make_uniq<OutlierTreeBindData>(table_name, column_names, output_mode, params);
    bind_data->id_column = std::move(id_column);
    bind_data->column_types = std::move(column_types);
    return std::move(bind_data);
}

// Execute function
static void OutlierTreeExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<OutlierTreeBindData>();
    auto &state = data_p.global_state->Cast<OutlierTreeGlobalState>();

    // Execute algorithm only once
    if (!state.executed) {
        state.executed = true;

        Connection con(*context.db);

        // Column types were resolved and checked at bind time
        const idx_t n_cols = bind_data.column_names.size();
        std::vector<ColumnInfo> column_info(n_cols);
        std::vector<ColumnData> data(n_cols);
        for (size_t i = 0; i < n_cols; ++i) {
            column_info[i].type = bind_data.column_types[i];
            data[i].type = bind_data.column_types[i];
            column_info[i].name = bind_data.column_names[i];
        }

        // Build the data query: categorical columns as VARCHAR, numeric
        // columns as DOUBLE, so chunks can be read through typed accessors
        std::string column_list;
        for (size_t i = 0; i < n_cols; ++i) {
            if (i > 0) column_list += ", ";
            column_list += "CAST(" + QuoteSqlIdentifier(bind_data.column_names[i]) +
                           (column_info[i].type == FeatureType::CATEGORICAL ? " AS VARCHAR)" : " AS DOUBLE)");
        }
        // The id column (if any) is read in the same query, after the
        // features, so each id stays with its own row
        column_list += bind_data.id_column.SelectSuffix();
        std::string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name);

        // Stream the input instead of materializing the full result; the
        // algorithm itself requires the complete dataset for tree fitting,
        // so this is the only full copy that is kept.
        auto result = StreamQuery(con, query);
        if (result->HasError()) {
            throw InvalidInputException("Failed to query source table: %s", result->GetError());
        }

        // Read data; rows containing NULL in any selected column are skipped,
        // but their source positions are preserved for row id reporting.
        int64_t source_row = 0;
        std::vector<UnifiedVectorFormat> formats(n_cols);
        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;

            for (idx_t col = 0; col < n_cols; ++col) {
                chunk->data[col].ToUnifiedFormat(chunk->size(), formats[col]);
            }

            for (idx_t row = 0; row < chunk->size(); ++row) {
                source_row++;

                // Check for NULLs
                bool valid_row = true;
                for (idx_t col = 0; col < n_cols; ++col) {
                    if (!formats[col].validity.RowIsValid(formats[col].sel->get_index(row))) {
                        valid_row = false;
                        break;
                    }
                }
                if (!valid_row) continue;

                state.source_row_ids.push_back(source_row);
                if (bind_data.id_column.Enabled()) {
                    state.id_values.push_back(chunk->data[n_cols].GetValue(row));
                }

                // Add values to data structures
                for (idx_t col = 0; col < n_cols; ++col) {
                    auto idx = formats[col].sel->get_index(row);
                    if (column_info[col].type == FeatureType::CATEGORICAL) {
                        std::string cat_val = UnifiedVectorFormat::GetData<string_t>(formats[col])[idx].GetString();
                        int cat_idx = column_info[col].AddCategory(cat_val);
                        data[col].category_indices.push_back(cat_idx);
                    } else {
                        data[col].numeric_values.push_back(UnifiedVectorFormat::GetData<double>(formats[col])[idx]);
                    }
                }
            }
        }
        if (result->HasError()) {
            throw InvalidInputException("Failed to query source table: %s", result->GetError());
        }

        // total_rows counts the analyzed rows, i.e. rows that are non-NULL in
        // every selected column (NULL-containing rows are excluded above)
        state.total_rows = data.empty() ? 0 : static_cast<int64_t>(data[0].size());
        state.columns_analyzed = static_cast<int64_t>(data.size());

        // Run OutlierTree algorithm
        if (state.total_rows > 0) {
            OutlierTree tree(bind_data.params);
            state.outliers = tree.FitPredict(data, column_info);
            state.clusters_evaluated = static_cast<int64_t>(tree.GetClustersEvaluated());
            state.max_depth_reached = static_cast<int64_t>(tree.GetMaxDepthReached());
        }
    }

    // Output results, streamed chunk-wise from the computed explanations
    if (bind_data.output_mode == "outliers") {
        idx_t count = MinValue<idx_t>(state.outliers.size() - state.current_row, STANDARD_VECTOR_SIZE);
        duckdb_compat::SetChildCardinality(output, count);

        // With id_column the id sits at index 1 and shifts the rest by one
        const idx_t id_col = bind_data.id_column.Enabled() ? 1 : 0;
        const idx_t base = 1 + id_col;
        auto row_ids = duckdb_compat::FlatData<int64_t>(output.data[0]);
        auto &column_name_vec = output.data[base];
        auto column_names = duckdb_compat::FlatData<string_t>(column_name_vec);
        auto &outlier_value_vec = output.data[base + 1];
        auto outlier_values = duckdb_compat::FlatData<string_t>(outlier_value_vec);
        auto &outlier_value_num_vec = output.data[base + 2];
        auto outlier_value_nums = duckdb_compat::FlatData<double>(outlier_value_num_vec);
        auto cluster_means = duckdb_compat::FlatData<double>(output.data[base + 3]);
        auto cluster_sds = duckdb_compat::FlatData<double>(output.data[base + 4]);
        auto cluster_sizes = duckdb_compat::FlatData<int64_t>(output.data[base + 5]);
        auto z_scores = duckdb_compat::FlatData<double>(output.data[base + 6]);
        auto lower_bounds = duckdb_compat::FlatData<double>(output.data[base + 7]);
        auto upper_bounds = duckdb_compat::FlatData<double>(output.data[base + 8]);
        auto &conditions_vec = output.data[base + 9];
        auto conditions = duckdb_compat::FlatData<string_t>(conditions_vec);
        auto &explanation_vec = output.data[base + 10];
        auto explanations = duckdb_compat::FlatData<string_t>(explanation_vec);
        auto outlier_scores = duckdb_compat::FlatData<double>(output.data[base + 11]);

        for (idx_t i = 0; i < count; ++i) {
            auto &outlier = state.outliers[state.current_row + i];

            // Report the 1-based source table position, not the index in the
            // NULL-compacted working dataset
            row_ids[i] = state.source_row_ids[outlier.row_idx];
            if (id_col) {
                // Same compacted index as source_row_ids: the id of this row
                output.data[id_col].SetValue(i, state.id_values[outlier.row_idx]);
            }
            column_names[i] = StringVector::AddString(column_name_vec, outlier.target_column_name);
            outlier_values[i] = StringVector::AddString(outlier_value_vec, outlier.GetOutlierValueString());
            if (std::holds_alternative<double>(outlier.outlier_value)) {
                outlier_value_nums[i] = std::get<double>(outlier.outlier_value);
            } else {
                FlatVector::SetNull(outlier_value_num_vec, i, true);
            }
            // NaN/inf statistics (an infinite outlier value, values near the DOUBLE range) are NULL
            auto set_finite_or_null = [&](idx_t column, auto target, double stat) {
                if (std::isfinite(stat)) {
                    target[i] = stat;
                } else {
                    FlatVector::SetNull(output.data[base + column], i, true);
                }
            };
            set_finite_or_null(3, cluster_means, outlier.cluster_mean);
            set_finite_or_null(4, cluster_sds, outlier.cluster_sd);
            cluster_sizes[i] = static_cast<int64_t>(outlier.cluster_size);
            set_finite_or_null(6, z_scores, outlier.z_score);
            set_finite_or_null(7, lower_bounds, outlier.lower_bound);
            set_finite_or_null(8, upper_bounds, outlier.upper_bound);
            conditions[i] = StringVector::AddString(conditions_vec, outlier.GetConditionsJSON());
            explanations[i] = StringVector::AddString(explanation_vec, outlier.explanation);
            outlier_scores[i] = outlier.outlier_score;
        }
        state.current_row += count;
    } else {
        // Summary mode - single row
        if (state.current_row == 0) {
            std::string status = state.outliers.empty() ? "pass" : "fail";
            std::string message = state.outliers.empty()
                ? "No outliers detected"
                : std::to_string(state.outliers.size()) + " outlier(s) detected";

            output.SetValue(0, 0, Value(status));
            output.SetValue(1, 0, Value::BIGINT(state.total_rows));
            output.SetValue(2, 0, Value::BIGINT(static_cast<int64_t>(state.outliers.size())));
            output.SetValue(3, 0, Value::INTEGER(static_cast<int32_t>(state.columns_analyzed)));
            output.SetValue(4, 0, Value::BIGINT(state.clusters_evaluated));
            output.SetValue(5, 0, Value::INTEGER(static_cast<int32_t>(state.max_depth_reached)));
            output.SetValue(6, 0, Value(message));

            duckdb_compat::SetChildCardinality(output, 1);
            state.current_row = 1;
        } else {
            duckdb_compat::SetChildCardinality(output, 0);
        }
    }
}

//===--------------------------------------------------------------------===//
// Registration
//===--------------------------------------------------------------------===//

void RegisterOutlierTreeFunctions(ExtensionLoader &loader) {
    // Create function set for multiple overloads
    TableFunctionSet outlier_tree_set("anofox_tab_outlier_tree");

    // 3-parameter version (table, columns, mode)
    TableFunction outlier_tree_3("anofox_tab_outlier_tree",
        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR)},
        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, OutlierTreeExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, OutlierTreeBind), OutlierTreeInit);
    AddIdColumnParameter(outlier_tree_3);
    outlier_tree_set.AddFunction(outlier_tree_3);

    // 9-parameter version (all parameters)
    TableFunction outlier_tree_9("anofox_tab_outlier_tree",
        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
         LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::INTEGER),
         LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::DOUBLE)},
        DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, OutlierTreeExecute), DATAZOO_GUARD(ANOFOX_TABULAR_BANNER, OutlierTreeBind), OutlierTreeInit);
    AddIdColumnParameter(outlier_tree_9);
    outlier_tree_set.AddFunction(outlier_tree_9);

    FunctionDescription ot_desc;
    ot_desc.description = "Identifies statistical outliers in a table using the OutlierTree algorithm, returning an explanation of which conditions make each row an outlier.";
    // Covers the LONGEST overload. A shorter overload takes a prefix of this list and
    // renders correctly; naming only the first three left the 9-argument form as
    // (table_name, columns, mode, col3, col4, col5, col6, col7, col8) -- documented at
    // the front, placeholders at the back. Names and defaults are from the bind at
    // OutlierTreeBind (inputs[3..8]) and docs/API_REFERENCE.md.
    ot_desc.parameter_names = {"table_name",        "columns",          "mode",
                               "max_depth",         "max_perc_outliers", "min_size_numeric",
                               "min_size_categ",    "z_norm",            "z_outlier"};
    ot_desc.examples = {"SELECT * FROM outlier_tree('transactions', 'amount,balance', 'summary');"};
    ot_desc.categories = {"metric", "anomaly-detection"};
    // The alias carries the SAME description, which is what #51 settled and what
    // anofox_function_alias.hpp does for every alias in this extension. Without it an
    // agent handed the short name gets nothing back, and the arguments render as
    // col0, col1, ...
    RegisterTableFunctionSetWithAlias(loader, std::move(outlier_tree_set), "outlier_tree", {std::move(ot_desc)});
}

} // namespace anofox
} // namespace duckdb
