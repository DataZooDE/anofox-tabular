#pragma once

#include "anofox_isolation_forest.hpp"  // Reuse FeatureType, ColumnInfo, ColumnData
#include "duckdb/main/extension/extension_loader.hpp"

#include <vector>
#include <string>
#include <cmath>
#include <variant>
#include <optional>
#include <unordered_set>
#include <algorithm>
#include <iomanip>
#include <map>
#include <numeric>
#include <limits>
#include <sstream>

namespace duckdb {
namespace anofox {

/**
 * Escape a string for safe embedding inside a JSON string literal.
 * Handles quotes, backslashes, and control characters.
 */
inline std::string EscapeJSONString(const std::string& input) {
    std::ostringstream oss;
    for (char c : input) {
        switch (c) {
        case '"':  oss << "\\\""; break;
        case '\\': oss << "\\\\"; break;
        case '\b': oss << "\\b"; break;
        case '\f': oss << "\\f"; break;
        case '\n': oss << "\\n"; break;
        case '\r': oss << "\\r"; break;
        case '\t': oss << "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(static_cast<unsigned char>(c)) << std::dec;
            } else {
                oss << c;
            }
        }
    }
    return oss.str();
}

/**
 * Split condition for tree nodes
 * Describes how data is partitioned at each split
 */
struct SplitCondition {
    size_t column_idx;
    FeatureType column_type;
    std::string column_name;

    // For numeric splits
    double split_value = 0.0;
    bool is_less_than = true;  // true = "<=", false = ">"

    // For categorical splits
    std::unordered_set<int> left_categories;
    std::vector<std::string> left_category_names;  // Human-readable names
    bool is_negated = false;  // true = right branch: column NOT IN (left_categories)

    SplitCondition() : column_idx(0), column_type(FeatureType::NUMERIC) {}

    // Convert to human-readable string
    std::string ToString() const {
        std::ostringstream oss;
        if (column_type == FeatureType::NUMERIC) {
            oss << column_name << (is_less_than ? " <= " : " > ") << split_value;
        } else {
            oss << column_name << (is_negated ? " NOT IN (" : " IN (");
            for (size_t i = 0; i < left_category_names.size(); ++i) {
                if (i > 0) oss << ", ";
                oss << "'" << left_category_names[i] << "'";
            }
            oss << ")";
        }
        return oss.str();
    }

    // Convert to JSON representation
    std::string ToJSON() const {
        std::ostringstream oss;
        oss << "{\"column\":\"" << EscapeJSONString(column_name) << "\",";
        if (column_type == FeatureType::NUMERIC) {
            oss << "\"type\":\"numeric\",\"operator\":\""
                << (is_less_than ? "<=" : ">") << "\",\"value\":" << split_value;
        } else {
            oss << "\"type\":\"categorical\",\"operator\":\""
                << (is_negated ? "not_in" : "in") << "\",\"values\":[";
            for (size_t i = 0; i < left_category_names.size(); ++i) {
                if (i > 0) oss << ",";
                oss << "\"" << EscapeJSONString(left_category_names[i]) << "\"";
            }
            oss << "]";
        }
        oss << "}";
        return oss.str();
    }
};

/**
 * Outlier explanation structure
 * Contains all information about a detected outlier
 */
struct OutlierExplanation {
    size_t row_idx;                          // Index into the NULL-filtered working dataset
                                             // (0-indexed; mapped to source row ids at output)
    size_t target_column_idx;                // Column where outlier was detected
    std::string target_column_name;          // Name of the column

    // The outlier value (either numeric or string)
    std::variant<double, std::string> outlier_value;

    // Distribution statistics in the conditional cluster
    double cluster_mean = 0.0;               // Mean for numeric targets
    double cluster_sd = 0.0;                 // Standard deviation
    double cluster_median = 0.0;             // Median value
    size_t cluster_size = 0;                 // Number of points in cluster
    double z_score = 0.0;                    // Z-score of outlier vs cluster

    // Confidence interval bounds
    double lower_bound = 0.0;
    double upper_bound = 0.0;

    // Split conditions leading to this cluster
    std::vector<SplitCondition> conditions;

    // Human-readable explanation string
    std::string explanation;

    // Rarity/severity score (lower = rarer, based on Chebyshev bound)
    double outlier_score = 1.0;

    OutlierExplanation() : row_idx(0), target_column_idx(0), outlier_value(0.0) {}

    // Get outlier value as string
    std::string GetOutlierValueString() const {
        if (std::holds_alternative<double>(outlier_value)) {
            std::ostringstream oss;
            oss << std::get<double>(outlier_value);
            return oss.str();
        }
        return std::get<std::string>(outlier_value);
    }

    // Get conditions as JSON array
    std::string GetConditionsJSON() const {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < conditions.size(); ++i) {
            if (i > 0) oss << ",";
            oss << conditions[i].ToJSON();
        }
        oss << "]";
        return oss.str();
    }
};

/**
 * Model parameters for OutlierTree
 */
struct OutlierTreeParams {
    size_t max_depth = 4;                    // Maximum tree depth
    double max_perc_outliers = 0.01;         // Maximum fraction of outliers per cluster
    size_t min_size_numeric = 25;            // Minimum samples for numeric target cluster
    size_t min_size_categ = 75;              // Minimum samples for categorical target cluster
    double z_norm = 2.67;                    // Z-threshold for normal range (confidence interval)
    double z_outlier = 8.0;                  // Z-threshold for flagging as outlier

    OutlierTreeParams() = default;

    OutlierTreeParams(size_t depth, double max_perc, size_t min_num,
                      size_t min_cat, double z_n, double z_o)
        : max_depth(depth), max_perc_outliers(max_perc),
          min_size_numeric(min_num), min_size_categ(min_cat),
          z_norm(z_n), z_outlier(z_o) {}
};

/**
 * Cluster statistics for numeric targets
 */
struct NumericClusterStats {
    double mean = 0.0;
    double sd = 0.0;
    double median = 0.0;
    double mad = 0.0;              // Median Absolute Deviation
    double mad_scaled = 0.0;       // MAD * 1.4826 (comparable to SD for normal dist)
    double q1 = 0.0;
    double q3 = 0.0;
    double min_val = 0.0;
    double max_val = 0.0;
    size_t n_samples = 0;

    // Compute lower/upper bounds based on z_norm
    double GetLowerBound(double z_norm) const {
        return mean - z_norm * sd;
    }

    double GetUpperBound(double z_norm) const {
        return mean + z_norm * sd;
    }

    // Compute z-score for a value (non-robust)
    double GetZScore(double value) const {
        if (sd < 1e-10) return 0.0;
        return (value - mean) / sd;
    }

    // Compute robust z-score using median and MAD
    // This is resistant to outliers in the data
    double GetRobustZScore(double value) const {
        if (mad_scaled < 1e-10) return 0.0;
        return (value - median) / mad_scaled;
    }

    // Compute Chebyshev bound probability (lower is rarer)
    static double ChebyshevBound(double z_score) {
        double z_sq = z_score * z_score;
        return z_sq > 0 ? 1.0 / z_sq : 1.0;
    }
};

/**
 * Cluster statistics for categorical targets
 */
struct CategoricalClusterStats {
    std::vector<size_t> category_counts;
    size_t n_samples = 0;
    size_t n_categories = 0;

    // Get proportion for a category
    double GetProportion(int category_idx) const {
        if (n_samples == 0 || category_idx < 0 ||
            category_idx >= static_cast<int>(category_counts.size())) {
            return 0.0;
        }
        return static_cast<double>(category_counts[category_idx]) / n_samples;
    }

    // Get most common category index
    int GetMostCommonCategory() const {
        if (category_counts.empty()) return -1;
        return static_cast<int>(std::max_element(category_counts.begin(),
                                                   category_counts.end()) -
                                category_counts.begin());
    }
};

/**
 * Main OutlierTree class
 * Implements explainable outlier detection using decision tree conditioning
 */
class OutlierTree {
public:
    /**
     * Constructor
     * @param params: Model parameters
     */
    explicit OutlierTree(const OutlierTreeParams& params);

    /**
     * Fit model and detect outliers in single pass
     * @param data: Column data (mixed numeric/categorical)
     * @param column_info: Column metadata
     * @return Vector of detected outliers with explanations
     */
    std::vector<OutlierExplanation> FitPredict(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info
    );

    /**
     * Get total number of clusters evaluated
     */
    size_t GetClustersEvaluated() const { return clusters_evaluated_; }

    /**
     * Get maximum depth reached
     */
    size_t GetMaxDepthReached() const { return max_depth_reached_; }

private:
    OutlierTreeParams params_;
    size_t clusters_evaluated_ = 0;
    size_t max_depth_reached_ = 0;
    // Maps (row_idx, target_column_idx) to the slot of the current best
    // finding in the outliers vector, so duplicates replace the worse entry.
    std::map<std::pair<size_t, size_t>, size_t> outlier_slots_;

    /**
     * Build tree for predicting target_col using other columns
     * @param data: All column data
     * @param column_info: Column metadata
     * @param target_col: Index of target column
     * @param row_indices: Indices of rows in current subset
     * @param current_conditions: Split conditions leading to current cluster
     * @param current_depth: Current tree depth
     * @param outliers: Output vector for detected outliers
     */
    void BuildTreeForColumn(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        size_t target_col,
        const std::vector<size_t>& row_indices,
        const std::vector<SplitCondition>& current_conditions,
        size_t current_depth,
        std::vector<OutlierExplanation>& outliers
    );

    /**
     * Find outliers in a numeric cluster
     * @param data: All column data
     * @param column_info: Column metadata
     * @param target_col: Index of target column
     * @param row_indices: Indices of rows in cluster
     * @param conditions: Split conditions defining this cluster
     * @param outliers: Output vector for detected outliers
     */
    void FindOutliersInNumericCluster(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        size_t target_col,
        const std::vector<size_t>& row_indices,
        const std::vector<SplitCondition>& conditions,
        std::vector<OutlierExplanation>& outliers
    );

    /**
     * Find outliers in a categorical cluster
     * @param data: All column data
     * @param column_info: Column metadata
     * @param target_col: Index of target column
     * @param row_indices: Indices of rows in cluster
     * @param conditions: Split conditions defining this cluster
     * @param outliers: Output vector for detected outliers
     */
    void FindOutliersInCategoricalCluster(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        size_t target_col,
        const std::vector<size_t>& row_indices,
        const std::vector<SplitCondition>& conditions,
        std::vector<OutlierExplanation>& outliers
    );

    /**
     * Find best split for a numeric predictor column
     * @param data: All column data
     * @param column_info: Column metadata
     * @param predictor_col: Index of predictor column
     * @param target_col: Index of target column
     * @param row_indices: Indices of rows in current subset
     * @param current_target_metric: Cached parent statistic of the target
     *        column over row_indices (variance for numeric targets, entropy
     *        for categorical targets)
     * @return Best split condition, or nullopt if no good split found
     */
    std::optional<SplitCondition> FindBestNumericSplit(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        size_t predictor_col,
        size_t target_col,
        const std::vector<size_t>& row_indices,
        double current_target_metric
    );

    /**
     * Find best split for a categorical predictor column
     * @param data: All column data
     * @param column_info: Column metadata
     * @param predictor_col: Index of predictor column
     * @param target_col: Index of target column
     * @param row_indices: Indices of rows in current subset
     * @param current_target_metric: Cached parent statistic of the target
     *        column over row_indices (variance for numeric targets, entropy
     *        for categorical targets)
     * @return Best split condition, or nullopt if no good split found
     */
    std::optional<SplitCondition> FindBestCategoricalSplit(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        size_t predictor_col,
        size_t target_col,
        const std::vector<size_t>& row_indices,
        double current_target_metric
    );

    /**
     * Compute statistics for a numeric cluster
     * @param data: Column data for target
     * @param row_indices: Indices of rows in cluster
     * @return Cluster statistics
     */
    NumericClusterStats ComputeNumericStats(
        const ColumnData& data,
        const std::vector<size_t>& row_indices
    );

    /**
     * Compute statistics for a categorical cluster
     * @param data: Column data for target
     * @param column_info: Column metadata
     * @param row_indices: Indices of rows in cluster
     * @return Cluster statistics
     */
    CategoricalClusterStats ComputeCategoricalStats(
        const ColumnData& data,
        const ColumnInfo& column_info,
        const std::vector<size_t>& row_indices
    );

    /**
     * Generate human-readable explanation for an outlier
     * @param outlier: Outlier explanation (to be filled)
     * @param column_info: Column metadata
     */
    void GenerateExplanation(
        OutlierExplanation& outlier,
        const std::vector<ColumnInfo>& column_info
    );

    /**
     * Compute variance reduction gain for a numeric split
     * @param left_indices: Indices in left branch
     * @param right_indices: Indices in right branch
     * @param data: Column data for target
     * @param current_variance: Variance before split
     * @return Variance reduction gain
     */
    double ComputeVarianceGain(
        const std::vector<size_t>& left_indices,
        const std::vector<size_t>& right_indices,
        const ColumnData& data,
        double current_variance
    );

    /**
     * Compute information gain for a categorical split
     * @param left_indices: Indices in left branch
     * @param right_indices: Indices in right branch
     * @param data: Column data for target
     * @param column_info: Column metadata for target
     * @param current_entropy: Entropy before split
     * @return Information gain
     */
    double ComputeInfoGain(
        const std::vector<size_t>& left_indices,
        const std::vector<size_t>& right_indices,
        const ColumnData& data,
        const ColumnInfo& column_info,
        double current_entropy
    );

    /**
     * Compute entropy of a categorical distribution
     * @param category_counts: Counts per category
     * @param total: Total count
     * @return Entropy value
     */
    static double ComputeEntropy(const std::vector<size_t>& category_counts, size_t total);

    /**
     * Compute variance of numeric values
     * @param data: Column data
     * @param row_indices: Indices to include
     * @return Variance value
     */
    static double ComputeVariance(const ColumnData& data, const std::vector<size_t>& row_indices);

    /**
     * Partition row indices based on split condition
     * @param data: All column data
     * @param split: Split condition
     * @param row_indices: Indices to partition
     * @param left_indices: Output - indices going left
     * @param right_indices: Output - indices going right
     */
    void PartitionRows(
        const std::vector<ColumnData>& data,
        const SplitCondition& split,
        const std::vector<size_t>& row_indices,
        std::vector<size_t>& left_indices,
        std::vector<size_t>& right_indices
    );

    /**
     * Insert an outlier, replacing any existing finding for the same
     * (row, column) pair when the new score is more extreme (lower).
     * @param outliers: Existing outliers
     * @param outlier: New outlier candidate
     * @return true if the outlier was inserted or replaced an existing one
     */
    bool AddOrReplaceOutlier(
        std::vector<OutlierExplanation>& outliers,
        OutlierExplanation&& outlier
    );
};

/**
 * Register OutlierTree table functions with DuckDB
 */
void RegisterOutlierTreeFunctions(ExtensionLoader &loader);

} // namespace anofox
} // namespace duckdb
