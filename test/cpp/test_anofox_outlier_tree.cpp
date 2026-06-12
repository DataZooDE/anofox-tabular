//===----------------------------------------------------------------------===//
// Catch2 pin tests for the OutlierTree implementation (issue #60).
//
// OutlierTree is fully deterministic (no RNG), so these tests pin the exact
// findings on a fixed mixed-type fixture. They guard the hot-path refactor
// (cached parent statistics, partition reuse): outputs must stay identical.
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_outlier_tree.hpp"

#include <algorithm>
#include <string>
#include <vector>

using namespace duckdb::anofox;

namespace {

//! 40-row fixture with a categorical group column, a numeric value column
//! whose distribution depends on the group (one extreme value per group), and
//! a categorical flag column with a rare category.
void MakeOutlierTreeFixture(std::vector<ColumnData> &data, std::vector<ColumnInfo> &info) {
	ColumnInfo grp_info(FeatureType::CATEGORICAL, "grp");
	ColumnInfo val_info(FeatureType::NUMERIC, "val");
	ColumnInfo flag_info(FeatureType::CATEGORICAL, "flag");

	ColumnData grp_col(FeatureType::CATEGORICAL);
	ColumnData val_col(FeatureType::NUMERIC);
	ColumnData flag_col(FeatureType::CATEGORICAL);

	for (int i = 0; i < 40; i++) {
		bool group_a = i < 20;
		grp_col.category_indices.push_back(grp_info.AddCategory(group_a ? "A" : "B"));

		double value = group_a ? 10.0 + 0.1 * i : 100.0 + 0.1 * (i - 20);
		if (i == 5) {
			value = 200.0; // extreme within group A
		}
		if (i == 25) {
			value = -50.0; // extreme within group B
		}
		val_col.numeric_values.push_back(value);

		bool rare = (i == 7 || i == 30);
		flag_col.category_indices.push_back(flag_info.AddCategory(rare ? "y" : "x"));
	}

	data = {grp_col, val_col, flag_col};
	info = {grp_info, val_info, flag_info};
}

OutlierTreeParams MakeFixtureParams() {
	// max_depth=2, max 10% outliers per cluster, small min cluster sizes,
	// z_norm=2.67, z_outlier=4.0
	return OutlierTreeParams(2, 0.1, 5, 5, 2.67, 4.0);
}

std::vector<OutlierExplanation> RunFixture(OutlierTree &tree) {
	std::vector<ColumnData> data;
	std::vector<ColumnInfo> info;
	MakeOutlierTreeFixture(data, info);
	auto outliers = tree.FitPredict(data, info);
	std::sort(outliers.begin(), outliers.end(), [](const OutlierExplanation &a, const OutlierExplanation &b) {
		return std::tie(a.row_idx, a.target_column_idx) < std::tie(b.row_idx, b.target_column_idx);
	});
	return outliers;
}

} // namespace

TEST_CASE("test/cpp/anofox_outlier_tree_fixture_pin", "[anofox]") {
	OutlierTree tree(MakeFixtureParams());
	auto outliers = RunFixture(tree);

	// Six findings: the two extreme numeric values (per-group), their group
	// labels (rare within the value-conditioned clusters), and the two rows
	// carrying the rare flag category.
	REQUIRE(outliers.size() == 6);

	// row 5: group label rare within the value-split cluster
	CHECK(outliers[0].row_idx == 5);
	CHECK(outliers[0].target_column_name == "grp");
	CHECK(outliers[0].z_score == Approx(4.4721359549995796).epsilon(1e-12));
	CHECK(outliers[0].outlier_score == Approx(0.05).epsilon(1e-12));
	CHECK(outliers[0].cluster_size == 20);
	REQUIRE(outliers[0].conditions.size() == 1);
	CHECK(outliers[0].conditions[0].column_name == "val");

	// row 5: extreme numeric value within group A
	CHECK(outliers[1].row_idx == 5);
	CHECK(outliers[1].target_column_name == "val");
	CHECK(std::get<double>(outliers[1].outlier_value) == 200.0);
	CHECK(outliers[1].z_score == Approx(254.82260893025767).epsilon(1e-12));
	CHECK(outliers[1].outlier_score == Approx(1.5400119271780761e-05).epsilon(1e-12));
	CHECK(outliers[1].cluster_size == 20);
	REQUIRE(outliers[1].conditions.size() == 1);
	CHECK(outliers[1].conditions[0].column_name == "grp");

	// row 7: rare flag category in the full table
	CHECK(outliers[2].row_idx == 7);
	CHECK(outliers[2].target_column_name == "flag");
	CHECK(outliers[2].GetOutlierValueString() == "y");
	CHECK(outliers[2].z_score == Approx(4.4721359549995796).epsilon(1e-12));
	CHECK(outliers[2].outlier_score == Approx(0.05).epsilon(1e-12));
	CHECK(outliers[2].cluster_size == 40);
	CHECK(outliers[2].conditions.empty());

	// row 25: group label rare within the value-split cluster
	CHECK(outliers[3].row_idx == 25);
	CHECK(outliers[3].target_column_name == "grp");
	CHECK(outliers[3].z_score == Approx(4.4721359549995796).epsilon(1e-12));
	CHECK(outliers[3].outlier_score == Approx(0.05).epsilon(1e-12));
	CHECK(outliers[3].cluster_size == 20);
	REQUIRE(outliers[3].conditions.size() == 1);
	CHECK(outliers[3].conditions[0].column_name == "val");

	// row 25: extreme numeric value within group B
	CHECK(outliers[4].row_idx == 25);
	CHECK(outliers[4].target_column_name == "val");
	CHECK(std::get<double>(outliers[4].outlier_value) == -50.0);
	CHECK(outliers[4].z_score == Approx(-169.74684113494476).epsilon(1e-12));
	CHECK(outliers[4].outlier_score == Approx(3.4705363519143227e-05).epsilon(1e-12));
	CHECK(outliers[4].cluster_size == 20);
	REQUIRE(outliers[4].conditions.size() == 1);
	CHECK(outliers[4].conditions[0].column_name == "grp");

	// row 30: rare flag category in the full table
	CHECK(outliers[5].row_idx == 30);
	CHECK(outliers[5].target_column_name == "flag");
	CHECK(outliers[5].GetOutlierValueString() == "y");
	CHECK(outliers[5].z_score == Approx(4.4721359549995796).epsilon(1e-12));
	CHECK(outliers[5].outlier_score == Approx(0.05).epsilon(1e-12));
	CHECK(outliers[5].cluster_size == 40);
	CHECK(outliers[5].conditions.empty());

	// Tree exploration shape
	CHECK(tree.GetClustersEvaluated() == 15);
	CHECK(tree.GetMaxDepthReached() == 2);
}

TEST_CASE("test/cpp/anofox_outlier_tree_repeated_fit_is_stable", "[anofox]") {
	// Two FitPredict runs on the same instance must produce identical findings
	// (no stale per-fit state).
	OutlierTree tree(MakeFixtureParams());
	auto first = RunFixture(tree);
	auto second = RunFixture(tree);

	REQUIRE(first.size() == second.size());
	for (size_t i = 0; i < first.size(); i++) {
		CHECK(first[i].row_idx == second[i].row_idx);
		CHECK(first[i].target_column_idx == second[i].target_column_idx);
		CHECK(first[i].z_score == second[i].z_score);
		CHECK(first[i].outlier_score == second[i].outlier_score);
		CHECK(first[i].explanation == second[i].explanation);
	}
}
