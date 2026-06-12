#include "anofox_dbscan.hpp"
#include <numeric>
#include <algorithm>

namespace duckdb {
namespace anofox {

// ============================================================================
// Helper Functions
// ============================================================================

double DBSCAN::ComputeDistance(const std::vector<double> &p1, const std::vector<double> &p2) const {
	if (p1.size() != p2.size()) {
		return std::numeric_limits<double>::max();
	}

	if (metric_ == DistanceMetric::MANHATTAN) {
		// Manhattan distance
		double sum = 0.0;
		for (size_t i = 0; i < p1.size(); ++i) {
			sum += std::abs(p1[i] - p2[i]);
		}
		return sum;
	} else if (metric_ == DistanceMetric::CHEBYSHEV) {
		// Chebyshev distance (max difference)
		double max_diff = 0.0;
		for (size_t i = 0; i < p1.size(); ++i) {
			max_diff = std::max(max_diff, std::abs(p1[i] - p2[i]));
		}
		return max_diff;
	} else {
		// Euclidean distance (default)
		double sum_sq = 0.0;
		for (size_t i = 0; i < p1.size(); ++i) {
			double diff = p1[i] - p2[i];
			sum_sq += diff * diff;
		}
		return std::sqrt(sum_sq);
	}
}

double DBSCAN::ComputeDensityScore(size_t neighbor_count, size_t max_neighbors) const {
	if (max_neighbors == 0) {
		return 0.0;
	}
	// Density score: normalized by max possible neighbors
	return std::min(1.0, static_cast<double>(neighbor_count) / static_cast<double>(max_neighbors));
}

void DBSCAN::RegionQuery(
	const std::vector<std::vector<double>>& data,
	size_t point_idx,
	std::vector<size_t>& neighbors
) const {
	neighbors.clear();

	if (point_idx >= data.size()) {
		return;
	}

	const auto& query_point = data[point_idx];

	for (size_t i = 0; i < data.size(); ++i) {
		if (i != point_idx) {
			double dist = ComputeDistance(query_point, data[i]);
			if (dist <= eps_) {
				neighbors.push_back(i);
			}
		}
	}
}

void DBSCAN::ExpandCluster(
	const std::vector<std::vector<double>>& data,
	size_t point_idx,
	std::vector<size_t>& seeds,
	int32_t cluster_id,
	std::vector<bool>& visited,
	std::vector<PointType>& point_types
) {
	// Mark current point as core
	point_types[point_idx] = PointType::CORE;
	results_[point_idx].cluster_id = cluster_id;
	results_[point_idx].point_type = PointType::CORE;

	// BFS-based cluster expansion
	std::queue<size_t> queue;
	for (size_t seed_idx : seeds) {
		queue.push(seed_idx);
	}

	// Scratch buffer reused across all region queries of this expansion (issue #60)
	std::vector<size_t> current_neighbors;

	while (!queue.empty()) {
		size_t current_idx = queue.front();
		queue.pop();

		if (visited[current_idx]) {
			// A point provisionally labelled noise that is density-reachable from a
			// core point becomes a border point of this cluster (standard DBSCAN).
			if (point_types[current_idx] == PointType::NOISE) {
				point_types[current_idx] = PointType::BORDER;
				results_[current_idx].point_type = PointType::BORDER;
				results_[current_idx].cluster_id = cluster_id;
			}
			continue;
		}

		visited[current_idx] = true;
		results_[current_idx].cluster_id = cluster_id;

		// Find neighbors of current point
		RegionQuery(data, current_idx, current_neighbors);
		results_[current_idx].neighbor_count = current_neighbors.size();

		// Standard minPts semantics: the eps-neighborhood includes the point itself
		if (current_neighbors.size() + 1 >= min_pts_) {
			// Current point is a core point
			point_types[current_idx] = PointType::CORE;
			results_[current_idx].point_type = PointType::CORE;

			// Add neighbors to queue; already-visited noise points are promoted
			// to border points when popped above.
			for (size_t neighbor_idx : current_neighbors) {
				queue.push(neighbor_idx);
			}
		} else {
			// Current point is a border point
			point_types[current_idx] = PointType::BORDER;
			results_[current_idx].point_type = PointType::BORDER;
		}
	}
}

// ============================================================================
// Main Algorithm Implementation
// ============================================================================

void DBSCAN::Fit(const std::vector<std::vector<double>>& data) {
	if (data.empty()) {
		return;
	}

	n_features_ = data[0].size();
	size_t n_points = data.size();

	// Initialize results and tracking structures
	results_.clear();
	results_.resize(n_points);
	for (size_t i = 0; i < n_points; ++i) {
		results_[i].point_idx = i;
		results_[i].cluster_id = -1;  // Initially unassigned
		results_[i].point_type = PointType::UNVISITED;
		results_[i].neighbor_count = 0;
		results_[i].density_score = 0.0;
	}

	std::vector<bool> visited(n_points, false);
	std::vector<PointType> point_types(n_points, PointType::UNVISITED);
	int32_t cluster_id = 0;

	// Scratch buffer reused across all region queries of the fit (issue #60)
	std::vector<size_t> neighbors;

	// Main DBSCAN loop
	for (size_t i = 0; i < n_points; ++i) {
		if (!visited[i]) {
			visited[i] = true;

			// Find neighbors of point i
			RegionQuery(data, i, neighbors);
			results_[i].neighbor_count = neighbors.size();

			// Standard minPts semantics: the eps-neighborhood includes the point itself
			if (neighbors.size() + 1 < min_pts_) {
				// Mark as noise (for now; may become border point later)
				point_types[i] = PointType::NOISE;
				results_[i].cluster_id = -1;
				results_[i].point_type = PointType::NOISE;
			} else {
				// Start expanding cluster from this core point
				ExpandCluster(data, i, neighbors, cluster_id, visited, point_types);
				cluster_id++;
			}
		}
	}

	// Compute density scores based on neighbor counts
	size_t max_neighbors = 0;
	for (const auto& pt : results_) {
		max_neighbors = std::max(max_neighbors, pt.neighbor_count);
	}

	for (auto& pt : results_) {
		pt.density_score = ComputeDensityScore(pt.neighbor_count, max_neighbors > 0 ? max_neighbors : 1);
	}
}

size_t DBSCAN::GetClusterCount() const {
	int32_t max_cluster = -1;
	for (const auto& pt : results_) {
		if (pt.cluster_id > max_cluster) {
			max_cluster = pt.cluster_id;
		}
	}
	return (max_cluster >= 0) ? (max_cluster + 1) : 0;
}

size_t DBSCAN::GetNoiseCount() const {
	size_t count = 0;
	for (const auto& pt : results_) {
		if (pt.cluster_id == -1) {
			count++;
		}
	}
	return count;
}

std::vector<double> DBSCAN::ComputeAnomalyScores() const {
	std::vector<double> scores;
	scores.reserve(results_.size());

	for (const auto& pt : results_) {
		if (pt.cluster_id == -1) {
			// Noise point: maximum anomaly score
			scores.push_back(1.0);
		} else {
			// Non-noise point: inverse of density score
			// Core points have higher scores (lower anomaly), border points lower scores (higher anomaly)
			if (pt.point_type == PointType::CORE) {
				// Core points are central to clusters: low anomaly
				scores.push_back(0.1 + (1.0 - pt.density_score) * 0.2);
			} else {
				// Border points are peripheral: moderate anomaly
				scores.push_back(0.3 + (1.0 - pt.density_score) * 0.3);
			}
		}
	}

	return scores;
}

size_t DBSCAN::GetLargestClusterSize() const {
	std::vector<size_t> cluster_sizes(GetClusterCount(), 0);
	for (const auto& pt : results_) {
		if (pt.cluster_id >= 0) {
			cluster_sizes[pt.cluster_id]++;
		}
	}

	size_t max_size = 0;
	for (size_t size : cluster_sizes) {
		max_size = std::max(max_size, size);
	}

	return max_size;
}

} // namespace anofox
} // namespace duckdb
