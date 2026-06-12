#pragma once

#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <limits>
#include <cstdint>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <variant>

namespace duckdb {
namespace anofox {

/**
 * Feature type for mixed numeric/categorical data
 */
enum class FeatureType {
    NUMERIC,
    CATEGORICAL
};

/**
 * Coefficient type for Extended Isolation Forest hyperplane splits
 */
enum class CoefType {
    Uniform,  // Sample coefficients uniformly from feature value ranges
    Normal    // Sample coefficients from standard normal distribution N(0,1)
};

/**
 * Scoring metric for anomaly detection
 */
enum class ScoringMetric {
    Depth,      // Standard isolation depth (default)
    Density,    // Points-to-volume ratio (geometric mean)
    AdjDepth    // Adjusted depth (0-2 per split based on density)
};

/**
 * Column metadata for mixed-type data
 */
struct ColumnInfo {
    FeatureType type;
    std::string name;
    // For categorical columns: mapping between string values and integer indices
    std::unordered_map<std::string, int> category_to_index;
    std::vector<std::string> index_to_category;

    ColumnInfo() : type(FeatureType::NUMERIC) {}
    ColumnInfo(FeatureType t, const std::string& n) : type(t), name(n) {}

    // Add a category and return its index
    int AddCategory(const std::string& category) {
        auto it = category_to_index.find(category);
        if (it != category_to_index.end()) {
            return it->second;
        }
        int idx = static_cast<int>(index_to_category.size());
        category_to_index[category] = idx;
        index_to_category.push_back(category);
        return idx;
    }

    // Get category index, returns -1 if not found
    int GetCategoryIndex(const std::string& category) const {
        auto it = category_to_index.find(category);
        return (it != category_to_index.end()) ? it->second : -1;
    }

    size_t GetCategoryCount() const { return index_to_category.size(); }
};

/**
 * Column data for mixed-type datasets
 * Stores either numeric values or category indices
 */
struct ColumnData {
    FeatureType type;
    std::vector<double> numeric_values;    // For numeric columns
    std::vector<int> category_indices;     // For categorical columns (-1 for NULL/unknown)

    ColumnData() : type(FeatureType::NUMERIC) {}
    explicit ColumnData(FeatureType t) : type(t) {}

    size_t size() const {
        return type == FeatureType::NUMERIC ? numeric_values.size() : category_indices.size();
    }

    void reserve(size_t n) {
        if (type == FeatureType::NUMERIC) {
            numeric_values.reserve(n);
        } else {
            category_indices.reserve(n);
        }
    }
};

/**
 * Node in an isolation tree
 * Extended to support both numeric and categorical splits, and hyperplane splits (Extended IF)
 */
struct IsoNode {
    bool is_leaf;              // Whether this is a terminal node
    FeatureType feature_type;  // Type of the split feature (for axis-aligned splits)
    uint16_t feature_idx;      // Which feature (column) to split on (for axis-aligned splits)
    double split_value;        // Threshold value for numeric splits (axis-aligned)
    std::unordered_set<int> left_categories;  // Category indices for left branch (categorical splits)
    int32_t left_child;        // Index to left child (-1 if leaf)
    int32_t right_child;       // Index to right child (-1 if leaf)
    uint16_t depth;            // Depth of this node in the tree
    size_t leaf_size;          // Number of samples at this leaf (for path length adjustment)

    // Extended IF (hyperplane splits, ndim > 1)
    uint8_t split_ndim;                           // 1=axis-aligned, >1=hyperplane
    std::vector<uint16_t> split_feature_indices;  // Selected feature indices for hyperplane
    std::vector<double> split_coefficients;       // Coefficients for hyperplane equation
    double split_intercept;                       // Hyperplane intercept value

    // Density scoring: volume bounds per feature at this node
    std::vector<double> node_min_bounds;  // Min bounds per numeric feature
    std::vector<double> node_max_bounds;  // Max bounds per numeric feature
    size_t node_sample_count;             // Number of samples reaching this node

    IsoNode() : is_leaf(true), feature_type(FeatureType::NUMERIC), feature_idx(0),
                split_value(0.0), left_child(-1), right_child(-1), depth(0), leaf_size(1),
                split_ndim(1), split_intercept(0.0), node_sample_count(0) {}
};

/**
 * Split candidate for SCiForest (information-gain guided splitting)
 * Holds a candidate split configuration and its computed gain score
 */
struct SplitCandidate {
    // Split type
    FeatureType type;
    uint8_t ndim;  // 1=axis-aligned, >1=hyperplane

    // For numeric axis-aligned splits (ndim=1, type=NUMERIC)
    size_t feature_idx;
    double split_value;

    // For categorical splits (ndim=1, type=CATEGORICAL)
    std::unordered_set<int> left_categories;

    // For hyperplane splits (ndim>1)
    std::vector<uint16_t> hyperplane_features;
    std::vector<double> hyperplane_coefficients;
    double hyperplane_intercept;

    // Partition result
    std::vector<size_t> left_indices;
    std::vector<size_t> right_indices;

    // Information gain score (variance reduction)
    double gain_score;

    SplitCandidate()
        : type(FeatureType::NUMERIC), ndim(1), feature_idx(0),
          split_value(0.0), hyperplane_intercept(0.0), gain_score(0.0) {}
};

/**
 * Single isolation tree
 * Builds recursively with random splits to isolate anomalies
 */
class IsolationTree {
private:
    std::vector<IsoNode> nodes_;
    size_t root_idx_;
    std::vector<ColumnInfo> column_info_;  // Column metadata for mixed-type support

public:
    IsolationTree() : root_idx_(0) {}

    /**
     * Build tree recursively from data samples (numeric only - legacy)
     * @param data: Column-major data: data[row][col]
     * @param sample_indices: Indices of rows to use for this tree
     * @param current_depth: Current recursion depth
     * @param max_depth: Maximum depth allowed
     * @param rng: Random number generator for reproducibility
     */
    void BuildTree(
        const std::vector<std::vector<double>>& data,
        const std::vector<size_t>& sample_indices,
        size_t current_depth,
        size_t max_depth,
        std::mt19937& rng
    );

    /**
     * Build tree from mixed-type data
     * @param data: Column data (mixed numeric/categorical)
     * @param column_info: Column metadata
     * @param sample_indices: Indices of rows to use
     * @param current_depth: Current recursion depth
     * @param max_depth: Maximum depth allowed
     * @param rng: Random number generator
     * @param ndim: Number of dimensions for hyperplane splits (1=axis-aligned)
     * @param coef_type: Coefficient distribution type
     * @param ntry: Number of split candidates to evaluate (1=random, >1=SCiForest)
     * @param prob_pick_avg_gain: Probability of selecting best-gain split vs random
     */
    void BuildTreeMixed(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        const std::vector<size_t>& sample_indices,
        size_t current_depth,
        size_t max_depth,
        std::mt19937& rng,
        size_t ndim = 1,
        CoefType coef_type = CoefType::Uniform,
        size_t ntry = 1,
        double prob_pick_avg_gain = 0.0
    );

    /**
     * Compute path length for a single point (numeric only - legacy)
     * @param point: Data point to score
     * @return Path length
     */
    double ComputePathLength(const std::vector<double>& point) const;

    /**
     * Compute path length for a mixed-type point
     * @param numeric_values: Numeric feature values (NaN for categorical columns)
     * @param category_indices: Category indices (-1 for numeric columns)
     * @param column_info: Column metadata
     * @return Path length
     */
    double ComputePathLengthMixed(
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info
    ) const;

    /**
     * Get number of nodes in this tree
     */
    size_t GetNodeCount() const { return nodes_.size(); }

    /**
     * Compute density score for a mixed-type point
     * Returns log of (points_fraction / volume_fraction) at terminal node
     * @param numeric_values: Numeric values (NaN for categorical columns)
     * @param category_indices: Category indices (-1 for numeric columns)
     * @param column_info: Column metadata
     * @param total_samples: Total samples used to build the tree
     * @param total_volume: Total volume of the feature space
     * @return Log density score
     */
    double ComputeDensityScoreMixed(
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info,
        size_t total_samples,
        double total_volume
    ) const;

    /**
     * Compute adjusted depth score for a mixed-type point
     * Uses density-adjusted depth (0-2 per split)
     * @param numeric_values: Numeric values (NaN for categorical columns)
     * @param category_indices: Category indices (-1 for numeric columns)
     * @param column_info: Column metadata
     * @return Adjusted depth score
     */
    double ComputeAdjDepthMixed(
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info
    ) const;

    /**
     * Get root node (for volume calculation)
     */
    const IsoNode& GetRootNode() const { return nodes_[root_idx_]; }

private:
    /**
     * Recursive path traversal helper (numeric only)
     */
    double ComputePathLengthRecursive(
        size_t node_idx,
        const std::vector<double>& point,
        size_t current_depth
    ) const;

    /**
     * Recursive path traversal helper (mixed type)
     */
    double ComputePathLengthMixedRecursive(
        size_t node_idx,
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info,
        size_t current_depth
    ) const;

    /**
     * Recursive density score computation (mixed type)
     */
    double ComputeDensityScoreMixedRecursive(
        size_t node_idx,
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info
    ) const;

    /**
     * Recursive adjusted depth computation (mixed type)
     */
    double ComputeAdjDepthMixedRecursive(
        size_t node_idx,
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices,
        const std::vector<ColumnInfo>& column_info,
        double accumulated_depth
    ) const;
};

/**
 * Isolation Forest ensemble
 * Collection of isolation trees for anomaly detection
 */
class IsolationForest {
private:
    std::vector<IsolationTree> trees_;
    size_t n_trees_;
    size_t sample_size_;
    size_t actual_sample_size_;  // Actual sample size used (min of sample_size_ and data size)
    size_t n_features_;
    double contamination_;
    uint64_t seed_;       // Stored so refits reproduce a freshly constructed forest
    std::mt19937 rng_;
    std::vector<ColumnInfo> column_info_;  // Column metadata for mixed-type support
    bool is_mixed_type_;  // Whether the forest was trained with mixed-type data
    size_t ndim_;         // Extended IF: number of dimensions for hyperplane splits (1=axis-aligned)
    CoefType coef_type_;  // Extended IF: coefficient distribution type
    ScoringMetric scoring_metric_;  // Scoring method: depth, density, or adj_depth
    std::vector<double> sample_weights_;  // Sample weights for weighted sampling (empty = uniform)
    double total_volume_;  // Total feature space volume (for density scoring)
    size_t ntry_;          // SCiForest: number of split candidates to evaluate (1=random IF)
    double prob_pick_avg_gain_;  // SCiForest: probability of selecting best-gain split

    /**
     * Compute average path length for unsuccessful search in BST
     * Used for normalizing anomaly scores
     * @param n: Sample size
     * @return Average path length
     */
    static double AveragePathLength(size_t n);

    /**
     * Perform weighted random sampling without replacement
     * @param weights: Sampling weights (higher = more likely to be selected)
     * @param sample_size: Number of samples to select
     * @param rng: Random number generator
     * @return Vector of selected indices
     */
    static std::vector<size_t> WeightedSample(
        const std::vector<double>& weights,
        size_t sample_size,
        std::mt19937& rng
    );

public:
    /**
     * Constructor
     * @param n_trees: Number of trees in ensemble (default 100)
     * @param sample_size: Subsample size for each tree (default 256)
     * @param contamination: Expected fraction of outliers (default 0.1)
     * @param seed: Random seed for reproducibility
     * @param ndim: Number of dimensions for hyperplane splits (1=axis-aligned, default 1)
     * @param coef_type: Coefficient distribution type (default Uniform)
     * @param scoring_metric: Scoring method (default Depth)
     * @param ntry: Number of split candidates to evaluate (1=random, >1=SCiForest)
     * @param prob_pick_avg_gain: Probability of selecting best-gain split (0.0=always random)
     */
    IsolationForest(
        size_t n_trees,
        size_t sample_size,
        double contamination,
        uint64_t seed,
        size_t ndim = 1,
        CoefType coef_type = CoefType::Uniform,
        ScoringMetric scoring_metric = ScoringMetric::Depth,
        size_t ntry = 1,
        double prob_pick_avg_gain = 0.0
    )
        : n_trees_(n_trees),
          sample_size_(sample_size),
          actual_sample_size_(sample_size),
          n_features_(0),
          contamination_(contamination),
          seed_(seed),
          rng_(seed),
          is_mixed_type_(false),
          ndim_(ndim > 0 ? ndim : 1),
          coef_type_(coef_type),
          scoring_metric_(scoring_metric),
          total_volume_(0.0),
          ntry_(ntry > 0 ? ntry : 1),
          prob_pick_avg_gain_(prob_pick_avg_gain) {}

    /**
     * Fit forest to numeric-only data (legacy)
     * Resets all model state, so refitting behaves exactly like fitting a
     * freshly constructed forest with the same parameters.
     * @param data: Data where data[row][col] is column-major format
     * @throws std::invalid_argument on ragged rows or rows without features
     */
    void Fit(const std::vector<std::vector<double>>& data);

    /**
     * Fit forest to mixed-type data
     * Resets all model state, so refitting behaves exactly like fitting a
     * freshly constructed forest with the same parameters.
     * @param data: Column data (mixed numeric/categorical)
     * @param column_info: Column metadata
     * @param sample_weights: Optional sample weights (empty = uniform sampling)
     * @throws std::invalid_argument on column count/type/length mismatches
     */
    void FitMixed(
        const std::vector<ColumnData>& data,
        const std::vector<ColumnInfo>& column_info,
        const std::vector<double>& sample_weights = {}
    );

    /**
     * Compute anomaly score for single point (numeric only - legacy)
     * @param point: Data point to score
     * @return Anomaly score
     */
    double Score(const std::vector<double>& point) const;

    /**
     * Compute anomaly score for mixed-type point
     * @param numeric_values: Numeric values (NaN for categorical columns)
     * @param category_indices: Category indices (-1 for numeric columns)
     * @return Anomaly score
     */
    double ScoreMixed(
        const std::vector<double>& numeric_values,
        const std::vector<int>& category_indices
    ) const;

    /**
     * Compute anomaly scores for batch of points (numeric only - legacy)
     * @param data: Data in column-major format
     * @return Vector of anomaly scores
     */
    std::vector<double> ScoreBatch(const std::vector<std::vector<double>>& data) const;

    /**
     * Compute anomaly scores for batch of mixed-type points
     * @param data: Column data (mixed numeric/categorical)
     * @return Vector of anomaly scores
     */
    std::vector<double> ScoreBatchMixed(const std::vector<ColumnData>& data) const;

    /**
     * Compute anomaly threshold based on contamination parameter
     * @param scores: Vector of anomaly scores
     * @return Threshold value (points > threshold are anomalies)
     */
    double ComputeThreshold(const std::vector<double>& scores) const;

    /**
     * Get number of trees in forest
     */
    size_t GetTreeCount() const { return trees_.size(); }

    /**
     * Get sample size used for each tree
     */
    size_t GetSampleSize() const { return sample_size_; }

    /**
     * Get contamination parameter
     */
    double GetContamination() const { return contamination_; }

    /**
     * Get column info (for mixed-type forests)
     */
    const std::vector<ColumnInfo>& GetColumnInfo() const { return column_info_; }

    /**
     * Check if forest was trained with mixed-type data
     */
    bool IsMixedType() const { return is_mixed_type_; }

    /**
     * Get ndim parameter (number of dimensions for hyperplane splits)
     */
    size_t GetNdim() const { return ndim_; }

    /**
     * Get coefficient type for hyperplane splits
     */
    CoefType GetCoefType() const { return coef_type_; }

    /**
     * Get scoring metric
     */
    ScoringMetric GetScoringMetric() const { return scoring_metric_; }

    /**
     * Get total volume of feature space (for density scoring)
     */
    double GetTotalVolume() const { return total_volume_; }

    /**
     * Get sample weights (empty if uniform sampling)
     */
    const std::vector<double>& GetSampleWeights() const { return sample_weights_; }

    /**
     * Get ntry parameter (number of split candidates, 1=random IF)
     */
    size_t GetNtry() const { return ntry_; }

    /**
     * Get prob_pick_avg_gain parameter (probability of selecting best-gain split)
     */
    double GetProbPickAvgGain() const { return prob_pick_avg_gain_; }
};

} // namespace anofox
} // namespace duckdb
