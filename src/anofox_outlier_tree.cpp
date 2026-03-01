#include "include/anofox_outlier_tree.hpp"
#include "include/anofox_function_alias.hpp"
#include "anofox_sql_utils.hpp"
#include "telemetry.hpp"
#include "anofox_trace.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>

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

    // Find best split among all predictor columns
    std::optional<SplitCondition> best_split;
    double best_gain = 0.0;

    for (size_t pred_col = 0; pred_col < data.size(); ++pred_col) {
        if (pred_col == target_col) continue;  // Skip target column

        std::optional<SplitCondition> split;
        if (column_info[pred_col].type == FeatureType::NUMERIC) {
            split = FindBestNumericSplit(data, column_info, pred_col, target_col, row_indices);
        } else {
            split = FindBestCategoricalSplit(data, column_info, pred_col, target_col, row_indices);
        }

        if (split) {
            // Compute gain for this split
            std::vector<size_t> left_indices, right_indices;
            PartitionRows(data, *split, row_indices, left_indices, right_indices);

            double gain = 0.0;
            if (column_info[target_col].type == FeatureType::NUMERIC) {
                double current_var = ComputeVariance(data[target_col], row_indices);
                gain = ComputeVarianceGain(left_indices, right_indices, data[target_col], current_var);
            } else {
                auto stats = ComputeCategoricalStats(data[target_col], column_info[target_col], row_indices);
                double current_entropy = ComputeEntropy(stats.category_counts, stats.n_samples);
                gain = ComputeInfoGain(left_indices, right_indices, data[target_col],
                                       column_info[target_col], current_entropy);
            }

            if (gain > best_gain) {
                best_gain = gain;
                best_split = split;
            }
        }
    }

    // If no good split found, stop
    if (!best_split) {
        return;
    }

    // Partition and recurse
    std::vector<size_t> left_indices, right_indices;
    PartitionRows(data, *best_split, row_indices, left_indices, right_indices);

    // Create conditions for left and right branches
    std::vector<SplitCondition> left_conditions = current_conditions;
    left_conditions.push_back(*best_split);

    SplitCondition right_condition = *best_split;
    if (right_condition.column_type == FeatureType::NUMERIC) {
        right_condition.is_less_than = false;  // Invert for right branch
    } else {
        // For categorical, right branch gets complement of left categories
        // (handled implicitly by not being in left_categories)
    }
    std::vector<SplitCondition> right_conditions = current_conditions;
    right_conditions.push_back(right_condition);

    // Recurse on both branches
    if (left_indices.size() >= min_size) {
        BuildTreeForColumn(data, column_info, target_col, left_indices,
                          left_conditions, current_depth + 1, outliers);
    }
    if (right_indices.size() >= min_size) {
        BuildTreeForColumn(data, column_info, target_col, right_indices,
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

        // Check if we should skip (better outlier already exists)
        if (ShouldSkipDuplicateOutlier(outliers, row_idx, target_col, score)) {
            continue;
        }

        OutlierExplanation outlier;
        outlier.row_idx = row_idx;
        outlier.target_column_idx = target_col;
        outlier.target_column_name = column_info[target_col].name;
        outlier.outlier_value = data[target_col].numeric_values[row_idx];
        outlier.cluster_mean = stats.mean;
        outlier.cluster_sd = stats.sd;
        outlier.cluster_median = stats.median;
        outlier.cluster_size = stats.n_samples;
        outlier.z_score = z_score;
        outlier.lower_bound = lower_bound;
        outlier.upper_bound = upper_bound;
        outlier.conditions = conditions;
        outlier.outlier_score = score;

        GenerateExplanation(outlier, column_info);
        outliers.push_back(std::move(outlier));
        outlier_count++;
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

                if (ShouldSkipDuplicateOutlier(outliers, idx, target_col, score)) {
                    continue;
                }

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
                outliers.push_back(std::move(outlier));
                outlier_count++;
            }
        }
    }
}

std::optional<SplitCondition> OutlierTree::FindBestNumericSplit(
    const std::vector<ColumnData>& data,
    const std::vector<ColumnInfo>& column_info,
    size_t predictor_col,
    size_t target_col,
    const std::vector<size_t>& row_indices
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

    for (double split_val : candidates) {
        std::vector<size_t> left_idx, right_idx;
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

        double gain = 0.0;
        if (column_info[target_col].type == FeatureType::NUMERIC) {
            double current_var = ComputeVariance(data[target_col], row_indices);
            gain = ComputeVarianceGain(left_idx, right_idx, data[target_col], current_var);
        } else {
            auto stats = ComputeCategoricalStats(data[target_col], column_info[target_col], row_indices);
            double current_entropy = ComputeEntropy(stats.category_counts, stats.n_samples);
            gain = ComputeInfoGain(left_idx, right_idx, data[target_col],
                                   column_info[target_col], current_entropy);
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
    const std::vector<size_t>& row_indices
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

    // Simple binary split: put most common category on one side
    int most_common_cat = -1;
    size_t max_count = 0;
    for (const auto& [cat, rows] : category_rows) {
        if (rows.size() > max_count) {
            max_count = rows.size();
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

    // Compute gain
    double gain = 0.0;
    if (column_info[target_col].type == FeatureType::NUMERIC) {
        double current_var = ComputeVariance(data[target_col], row_indices);
        gain = ComputeVarianceGain(left_idx, right_idx, data[target_col], current_var);
    } else {
        auto stats = ComputeCategoricalStats(data[target_col], column_info[target_col], row_indices);
        double current_entropy = ComputeEntropy(stats.category_counts, stats.n_samples);
        gain = ComputeInfoGain(left_idx, right_idx, data[target_col],
                               column_info[target_col], current_entropy);
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

    // Mean
    double sum = std::accumulate(values.begin(), values.end(), 0.0);
    stats.mean = sum / stats.n_samples;

    // Standard deviation
    double sq_sum = 0.0;
    for (double v : values) {
        sq_sum += (v - stats.mean) * (v - stats.mean);
    }
    stats.sd = stats.n_samples > 1 ? std::sqrt(sq_sum / (stats.n_samples - 1)) : 0.0;

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
        oss << " is unusually " << (outlier.z_score > 0 ? "high" : "low")
            << " (mean: " << std::fixed << std::setprecision(2) << outlier.cluster_mean
            << ", SD: " << outlier.cluster_sd
            << ", z-score: " << outlier.z_score << ")";
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
        }

        if (goes_left) {
            left_indices.push_back(idx);
        } else {
            right_indices.push_back(idx);
        }
    }
}

bool OutlierTree::ShouldSkipDuplicateOutlier(
    const std::vector<OutlierExplanation>& outliers,
    size_t row_idx,
    size_t col_idx,
    double new_score
) {
    for (const auto& existing : outliers) {
        if (existing.row_idx == row_idx && existing.target_column_idx == col_idx) {
            // Keep the one with lower (more extreme) score
            return existing.outlier_score <= new_score;
        }
    }
    return false;
}

//===--------------------------------------------------------------------===//
// DuckDB Table Function Implementation
//===--------------------------------------------------------------------===//

// Bind data for outlier_tree table function
struct OutlierTreeBindData : public TableFunctionData {
    std::string table_name;
    std::vector<std::string> column_names;
    std::string output_mode;  // "summary" or "outliers"
    OutlierTreeParams params;

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
                                                 vector<LogicalType> &return_types, vector<string> &names) {
    PostHogTelemetry::Instance().CaptureFunctionExecution("outlier_tree");

    if (input.inputs.size() < 2) {
        throw BinderException("outlier_tree requires at least 2 arguments: table_name, columns");
    }

    std::string table_name = input.inputs[0].ToString();
    std::string columns_str = input.inputs[1].ToString();

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

    // Parse optional parameters
    std::string output_mode = input.inputs.size() > 2 ? input.inputs[2].ToString() : "summary";
    size_t max_depth = input.inputs.size() > 3 ? input.inputs[3].GetValue<int64_t>() : 4;
    double max_perc_outliers = input.inputs.size() > 4 ? input.inputs[4].GetValue<double>() : 0.01;
    size_t min_size_numeric = input.inputs.size() > 5 ? input.inputs[5].GetValue<int64_t>() : 25;
    size_t min_size_categ = input.inputs.size() > 6 ? input.inputs[6].GetValue<int64_t>() : 75;
    double z_norm = input.inputs.size() > 7 ? input.inputs[7].GetValue<double>() : 2.67;
    double z_outlier = input.inputs.size() > 8 ? input.inputs[8].GetValue<double>() : 8.0;

    // Validate parameters
    if (max_depth < 1) {
        throw BinderException("max_depth must be >= 1");
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

    OutlierTreeParams params(max_depth, max_perc_outliers, min_size_numeric,
                             min_size_categ, z_norm, z_outlier);

    // Define output schema based on mode
    if (output_mode == "outliers") {
        names.emplace_back("row_id");
        return_types.emplace_back(LogicalType(LogicalTypeId::BIGINT));
        names.emplace_back("column_name");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
        names.emplace_back("outlier_value");
        return_types.emplace_back(LogicalType(LogicalTypeId::VARCHAR));
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

    return make_uniq<OutlierTreeBindData>(table_name, column_names, output_mode, params);
}

// Execute function
static void OutlierTreeExecute(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
    auto &bind_data = data_p.bind_data->Cast<OutlierTreeBindData>();
    auto &state = data_p.global_state->Cast<OutlierTreeGlobalState>();

    // Execute algorithm only once
    if (!state.executed) {
        state.executed = true;

        Connection con(*context.db);

        // Build query to fetch data
        std::string column_list;
        for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
            if (i > 0) column_list += ", ";
            column_list += QuoteSqlIdentifier(bind_data.column_names[i]);
        }

        std::string query = "SELECT " + column_list + " FROM " + BuildQueryTableRef(bind_data.table_name);

        auto result = con.Query(query);
        if (result->HasError()) {
            throw InvalidInputException("Failed to query source table: %s", result->GetError());
        }

        // Detect column types and build column info
        std::vector<ColumnInfo> column_info(bind_data.column_names.size());
        std::vector<ColumnData> data(bind_data.column_names.size());

        for (size_t i = 0; i < bind_data.column_names.size(); ++i) {
            auto col_type = result->types[i].id();
            if (col_type == LogicalTypeId::VARCHAR || col_type == LogicalTypeId::ENUM) {
                column_info[i].type = FeatureType::CATEGORICAL;
                data[i].type = FeatureType::CATEGORICAL;
            } else {
                column_info[i].type = FeatureType::NUMERIC;
                data[i].type = FeatureType::NUMERIC;
            }
            column_info[i].name = bind_data.column_names[i];
        }

        // Read data
        while (true) {
            auto chunk = result->Fetch();
            if (!chunk || chunk->size() == 0) break;

            for (idx_t row = 0; row < chunk->size(); ++row) {
                bool valid_row = true;

                // Check for NULLs
                for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
                    auto val = chunk->GetValue(col, row);
                    if (val.IsNull()) {
                        valid_row = false;
                        break;
                    }
                }

                if (!valid_row) continue;

                // Add values to data structures
                for (idx_t col = 0; col < chunk->ColumnCount(); ++col) {
                    auto val = chunk->GetValue(col, row);
                    if (column_info[col].type == FeatureType::CATEGORICAL) {
                        std::string cat_val = val.ToString();
                        int cat_idx = column_info[col].AddCategory(cat_val);
                        data[col].category_indices.push_back(cat_idx);
                    } else {
                        double num_val = val.GetValue<double>();
                        data[col].numeric_values.push_back(num_val);
                    }
                }
            }
        }

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

    // Output results
    if (bind_data.output_mode == "outliers") {
        idx_t count = 0;
        idx_t max_count = STANDARD_VECTOR_SIZE;

        while (state.current_row < state.outliers.size() && count < max_count) {
            auto &outlier = state.outliers[state.current_row];

            output.SetValue(0, count, Value::BIGINT(static_cast<int64_t>(outlier.row_idx + 1)));  // 1-indexed
            output.SetValue(1, count, Value(outlier.target_column_name));
            output.SetValue(2, count, Value(outlier.GetOutlierValueString()));
            output.SetValue(3, count, Value::DOUBLE(outlier.cluster_mean));
            output.SetValue(4, count, Value::DOUBLE(outlier.cluster_sd));
            output.SetValue(5, count, Value::BIGINT(static_cast<int64_t>(outlier.cluster_size)));
            output.SetValue(6, count, Value::DOUBLE(outlier.z_score));
            output.SetValue(7, count, Value::DOUBLE(outlier.lower_bound));
            output.SetValue(8, count, Value::DOUBLE(outlier.upper_bound));
            output.SetValue(9, count, Value(outlier.GetConditionsJSON()));
            output.SetValue(10, count, Value(outlier.explanation));
            output.SetValue(11, count, Value::DOUBLE(outlier.outlier_score));

            state.current_row++;
            count++;
        }
        output.SetCardinality(count);
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

            output.SetCardinality(1);
            state.current_row = 1;
        } else {
            output.SetCardinality(0);
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
        OutlierTreeExecute, OutlierTreeBind, OutlierTreeInit);
    outlier_tree_set.AddFunction(outlier_tree_3);

    // 9-parameter version (all parameters)
    TableFunction outlier_tree_9("anofox_tab_outlier_tree",
        {LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR), LogicalType(LogicalTypeId::VARCHAR),
         LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::INTEGER),
         LogicalType(LogicalTypeId::INTEGER), LogicalType(LogicalTypeId::DOUBLE), LogicalType(LogicalTypeId::DOUBLE)},
        OutlierTreeExecute, OutlierTreeBind, OutlierTreeInit);
    outlier_tree_set.AddFunction(outlier_tree_9);

    CreateTableFunctionInfo outlier_tree_info(outlier_tree_set);
    loader.RegisterFunction(outlier_tree_info);

    // Register alias: outlier_tree
    TableFunctionSet alias_outlier_tree_set("outlier_tree");
    alias_outlier_tree_set.AddFunction(outlier_tree_3);
    alias_outlier_tree_set.AddFunction(outlier_tree_9);
    CreateTableFunctionInfo alias_outlier_tree_info(alias_outlier_tree_set);
    alias_outlier_tree_info.alias_of = "anofox_tab_outlier_tree";
    loader.RegisterFunction(alias_outlier_tree_info);
}

} // namespace anofox
} // namespace duckdb
