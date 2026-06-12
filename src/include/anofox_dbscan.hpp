#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <limits>
#include <cstdint>
#include <unordered_set>
#include <queue>

namespace duckdb {
namespace anofox {

/**
 * Point status in DBSCAN clustering
 */
enum class PointType : uint8_t {
	UNVISITED = 0,
	NOISE = 1,
	BORDER = 2,
	CORE = 3
};

/**
 * Result for a single point in DBSCAN clustering
 */
struct DBSCANPoint {
	size_t point_idx;           // Original row index
	int32_t cluster_id;         // Cluster ID (-1 for noise/outliers)
	PointType point_type;       // CORE, BORDER, or NOISE
	size_t neighbor_count;      // Number of neighbors within epsilon
	double density_score;       // Local density measure (0.0-1.0)
};

/**
 * Distance metric enumeration
 */
enum class DistanceMetric : uint8_t {
	EUCLIDEAN = 0,
	MANHATTAN = 1,
	CHEBYSHEV = 2
};

/**
 * DBSCAN clustering algorithm
 * Groups points by density connectivity
 */
class DBSCAN {
private:
	double eps_;                              // Epsilon: neighborhood radius
	size_t min_pts_;                          // Minimum points to form dense region
	size_t n_features_;                       // Number of dimensions
	std::vector<DBSCANPoint> results_;        // Clustering results
	DistanceMetric metric_;                   // Distance metric to use

	/**
	 * Compute distance between two points
	 * @param p1: First point
	 * @param p2: Second point
	 * @return Distance value
	 */
	double ComputeDistance(
		const std::vector<double>& p1,
		const std::vector<double>& p2
	) const;

	/**
	 * Find all neighbors within epsilon radius
	 * @param data: All data points
	 * @param point_idx: Index of query point
	 * @param neighbors: Output - indices of neighbor points; cleared first.
	 *        Passed in so callers can reuse one scratch buffer across the
	 *        many region queries of a fit (issue #60).
	 */
	void RegionQuery(
		const std::vector<std::vector<double>>& data,
		size_t point_idx,
		std::vector<size_t>& neighbors
	) const;

	/**
	 * Expand cluster from a core point
	 * @param data: All data points
	 * @param point_idx: Index of core point
	 * @param neighbors: Initial neighbors
	 * @param cluster_id: ID to assign to cluster
	 * @param visited: Tracking visited points
	 * @param point_types: Tracking point types
	 */
	void ExpandCluster(
		const std::vector<std::vector<double>>& data,
		size_t point_idx,
		std::vector<size_t>& neighbors,
		int32_t cluster_id,
		std::vector<bool>& visited,
		std::vector<PointType>& point_types
	);

	/**
	 * Compute local density score for a point
	 * @param neighbor_count: Number of neighbors within eps
	 * @param max_neighbors: Maximum possible neighbors
	 * @return Density score [0.0, 1.0]
	 */
	double ComputeDensityScore(size_t neighbor_count, size_t max_neighbors) const;

public:
	/**
	 * Constructor
	 * @param eps: Maximum distance for neighborhood (default 0.5)
	 * @param min_pts: Minimum points to form cluster (default 5)
	 * @param metric: Distance metric (default Euclidean)
	 */
	DBSCAN(
		double eps = 0.5,
		size_t min_pts = 5,
		DistanceMetric metric = DistanceMetric::EUCLIDEAN
	)
		: eps_(eps),
		  min_pts_(min_pts),
		  n_features_(0),
		  metric_(metric) {}

	/**
	 * Fit DBSCAN to data
	 * Performs clustering on all points
	 * @param data: Data where data[row][col]
	 */
	void Fit(const std::vector<std::vector<double>>& data);

	/**
	 * Get clustering results
	 * @return Vector of DBSCANPoint structures
	 */
	const std::vector<DBSCANPoint>& GetResults() const { return results_; }

	/**
	 * Get number of clusters found (excluding noise)
	 * @return Cluster count
	 */
	size_t GetClusterCount() const;

	/**
	 * Get noise/outlier count
	 * @return Number of noise points
	 */
	size_t GetNoiseCount() const;

	/**
	 * Compute anomaly score for each point
	 * Noise points get score 1.0, others based on cluster size/density
	 * @return Vector of anomaly scores [0.0, 1.0]
	 */
	std::vector<double> ComputeAnomalyScores() const;

	/**
	 * Get largest cluster size
	 * @return Size of largest cluster
	 */
	size_t GetLargestClusterSize() const;

	/**
	 * Get epsilon parameter
	 */
	double GetEps() const { return eps_; }

	/**
	 * Get min_pts parameter
	 */
	size_t GetMinPts() const { return min_pts_; }
};

} // namespace anofox
} // namespace duckdb
