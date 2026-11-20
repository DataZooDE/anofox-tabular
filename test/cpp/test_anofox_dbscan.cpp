//===----------------------------------------------------------------------===//
// Catch2 unit tests for the DBSCAN implementation (issue #44)
//
// Standard DBSCAN semantics (Ester et al., 1996):
//  - A point p is a core point if its eps-neighborhood *including p itself*
//    contains at least min_pts points.
//  - A point that was provisionally labelled noise but is density-reachable
//    from a core point must be promoted to a border point of that cluster.
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_dbscan.hpp"

#include <limits>
#include <vector>

using namespace duckdb::anofox;

TEST_CASE("test/cpp/anofox_dbscan_minpts_counts_query_point", "[anofox]") {
	// 1-D points 0.0, 1.0, 2.0 with eps = 1.0 and min_pts = 3.
	//
	// Standard semantics: the neighborhood of 1.0 is {0.0, 1.0, 2.0} (3 points,
	// including itself) so 1.0 is a core point; 0.0 and 2.0 are border points of
	// the same cluster. There is no noise.
	std::vector<std::vector<double>> data = {{0.0}, {1.0}, {2.0}};

	DBSCAN dbscan(1.0, 3, DistanceMetric::EUCLIDEAN);
	dbscan.Fit(data);

	const auto &results = dbscan.GetResults();
	REQUIRE(results.size() == 3);

	CHECK(dbscan.GetClusterCount() == 1);
	CHECK(dbscan.GetNoiseCount() == 0);

	CHECK(results[0].point_type == PointType::BORDER);
	CHECK(results[1].point_type == PointType::CORE);
	CHECK(results[2].point_type == PointType::BORDER);

	for (const auto &pt : results) {
		CHECK(pt.cluster_id == 0);
	}
}

TEST_CASE("test/cpp/anofox_dbscan_noise_promoted_to_border", "[anofox]") {
	// 1-D points with eps = 1.0 and min_pts = 3.
	//
	// eps-neighborhoods (excluding the point itself):
	//   p0 = 2.0  -> {p3}            not core (2 incl. self < 3)
	//   p1 = 0.0  -> {p2}            not core
	//   p2 = 0.9  -> {p1, p3}        core (3 incl. self)
	//   p3 = 1.1  -> {p2, p0}        core
	//   p4 = 10.0 -> {}              noise
	//
	// p0 and p1 are visited before any core point, so a naive implementation
	// labels them noise and the `visited` check prevents the later cluster
	// expansion from claiming them. Standard DBSCAN promotes both to border
	// points of cluster 0.
	std::vector<std::vector<double>> data = {{2.0}, {0.0}, {0.9}, {1.1}, {10.0}};

	DBSCAN dbscan(1.0, 3, DistanceMetric::EUCLIDEAN);
	dbscan.Fit(data);

	const auto &results = dbscan.GetResults();
	REQUIRE(results.size() == 5);

	CHECK(dbscan.GetClusterCount() == 1);
	CHECK(dbscan.GetNoiseCount() == 1);

	// Reachable points promoted to border members of cluster 0
	CHECK(results[0].point_type == PointType::BORDER);
	CHECK(results[0].cluster_id == 0);
	CHECK(results[1].point_type == PointType::BORDER);
	CHECK(results[1].cluster_id == 0);

	// Core points
	CHECK(results[2].point_type == PointType::CORE);
	CHECK(results[2].cluster_id == 0);
	CHECK(results[3].point_type == PointType::CORE);
	CHECK(results[3].cluster_id == 0);

	// Genuine noise stays noise
	CHECK(results[4].point_type == PointType::NOISE);
	CHECK(results[4].cluster_id == -1);
}

TEST_CASE("test/cpp/anofox_dbscan_1d_non_finite_keeps_sorted_search", "[anofox]") {
	// Issue #140: one NaN or +-Inf value must not switch a univariate fit to the all-pairs
	// search. A non-finite value has no neighbours (its distance to every point is Inf or NaN).
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();
	std::vector<std::vector<double>> data = {{1.0}, {1.1}, {1.2}, {nan}, {inf}, {-inf}, {1.15}, {5.0}, {5.05}, {5.1}};

	SECTION("min_pts 3: non-finite values are noise") {
		DBSCAN dbscan(0.1, 3, DistanceMetric::EUCLIDEAN);
		dbscan.Fit(data);
		REQUIRE(dbscan.UsesSortedSearch());

		const auto &results = dbscan.GetResults();
		REQUIRE(results.size() == data.size());
		for (size_t i : {size_t(3), size_t(4), size_t(5)}) {
			CHECK(results[i].point_type == PointType::NOISE);
			CHECK(results[i].cluster_id == -1);
			CHECK(results[i].neighbor_count == 0);
		}
		CHECK(results[0].point_type == PointType::NOISE); // 1.1 - 1.0 > 0.1 in binary
		for (size_t i : {size_t(1), size_t(2), size_t(6)}) {
			CHECK(results[i].cluster_id == 0);
			CHECK(results[i].point_type == PointType::CORE);
		}
		for (size_t i : {size_t(7), size_t(8), size_t(9)}) {
			CHECK(results[i].cluster_id == 1);
		}
		CHECK(dbscan.GetClusterCount() == 2);
		CHECK(dbscan.GetNoiseCount() == 4);
	}

	SECTION("min_pts 1: each non-finite value is a cluster of its own") {
		DBSCAN dbscan(0.1, 1, DistanceMetric::EUCLIDEAN);
		dbscan.Fit(data);
		REQUIRE(dbscan.UsesSortedSearch());

		const auto &results = dbscan.GetResults();
		CHECK(results[3].point_type == PointType::CORE);
		CHECK(results[4].point_type == PointType::CORE);
		CHECK(results[5].point_type == PointType::CORE);
		CHECK(results[3].cluster_id != results[4].cluster_id);
		CHECK(results[4].cluster_id != results[5].cluster_id);
		CHECK(dbscan.GetNoiseCount() == 0);
	}

	SECTION("finite 1-D data uses the sorted search, 2-D data does not") {
		DBSCAN finite(0.1, 3, DistanceMetric::EUCLIDEAN);
		finite.Fit({{1.0}, {1.05}, {1.1}});
		CHECK(finite.UsesSortedSearch());

		DBSCAN two_d(0.1, 3, DistanceMetric::EUCLIDEAN);
		two_d.Fit({{1.0, 0.0}, {1.05, 0.0}, {1.1, 0.0}});
		CHECK_FALSE(two_d.UsesSortedSearch());
	}
}
