#pragma once

#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <limits>
#include <cstdint>

namespace duckdb {
namespace anofox {

/**
 * Node in an isolation tree
 * Compact representation following isotree design principles
 */
struct IsoNode {
    bool is_leaf;              // Whether this is a terminal node
    uint16_t feature_idx;      // Which feature (column) to split on
    double split_value;        // Threshold value for the split
    int32_t left_child;        // Index to left child (-1 if leaf)
    int32_t right_child;       // Index to right child (-1 if leaf)
    uint16_t depth;            // Depth of this node in the tree
};

/**
 * Single isolation tree
 * Builds recursively with random splits to isolate anomalies
 */
class IsolationTree {
private:
    std::vector<IsoNode> nodes_;
    size_t root_idx_;

public:
    IsolationTree() : root_idx_(0) {}

    /**
     * Build tree recursively from data samples
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
     * Compute path length for a single point
     * Path length is number of edges traversed from root to leaf
     * @param point: Data point to score
     * @return Path length (higher = easier to isolate = more anomalous)
     */
    double ComputePathLength(const std::vector<double>& point) const;

    /**
     * Get number of nodes in this tree
     */
    size_t GetNodeCount() const { return nodes_.size(); }

private:
    /**
     * Recursive path traversal helper
     * @param node_idx: Current node index
     * @param point: Data point
     * @param current_depth: Current depth in tree
     * @return Path length from this node to leaf
     */
    double ComputePathLengthRecursive(
        size_t node_idx,
        const std::vector<double>& point,
        size_t current_depth
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
    size_t n_features_;
    double contamination_;
    std::mt19937 rng_;

    /**
     * Compute average path length for unsuccessful search in BST
     * Used for normalizing anomaly scores
     * Formula from: https://math.stackexchange.com/questions/3333220/
     * @param n: Sample size
     * @return Average path length
     */
    static double AveragePathLength(size_t n);

public:
    /**
     * Constructor
     * @param n_trees: Number of trees in ensemble (default 100)
     * @param sample_size: Subsample size for each tree (default 256)
     * @param contamination: Expected fraction of outliers (default 0.1)
     * @param seed: Random seed for reproducibility
     */
    IsolationForest(
        size_t n_trees,
        size_t sample_size,
        double contamination,
        uint64_t seed
    )
        : n_trees_(n_trees),
          sample_size_(sample_size),
          n_features_(0),
          contamination_(contamination),
          rng_(seed) {}

    /**
     * Fit forest to data
     * Builds all trees by subsampling and splitting randomly
     * @param data: Data where data[row][col] is column-major format
     */
    void Fit(const std::vector<std::vector<double>>& data);

    /**
     * Compute anomaly score for single point
     * Range: [0, 1] where higher values indicate more anomalous
     * @param point: Data point to score
     * @return Anomaly score
     */
    double Score(const std::vector<double>& point) const;

    /**
     * Compute anomaly scores for batch of points
     * @param data: Data in column-major format
     * @return Vector of anomaly scores
     */
    std::vector<double> ScoreBatch(const std::vector<std::vector<double>>& data) const;

    /**
     * Compute anomaly threshold based on contamination parameter
     * Uses quantile of scores
     * @param scores: Vector of anomaly scores
     * @return Threshold value (points >= threshold are anomalies)
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
};

} // namespace anofox
} // namespace duckdb
