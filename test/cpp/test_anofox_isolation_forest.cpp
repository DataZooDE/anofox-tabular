//===----------------------------------------------------------------------===//
// Catch2 unit tests for the IsolationForest implementation (issue #44)
//
//  - Refitting the same forest instance must behave exactly like fitting a
//    fresh instance constructed with the same parameters (no stale tree state).
//  - The public Fit/Score APIs must validate input shapes and throw clear
//    exceptions instead of reading out of bounds.
//===----------------------------------------------------------------------===//

#include "catch.hpp"

#include "anofox_isolation_forest.hpp"

#include <stdexcept>
#include <vector>

using namespace duckdb::anofox;

namespace {

std::vector<std::vector<double>> MakeNumericData(double offset, double scale) {
	std::vector<std::vector<double>> data;
	for (int i = 0; i < 64; i++) {
		data.push_back({offset + scale * static_cast<double>(i), offset - scale * static_cast<double>(i)});
	}
	// Obvious outlier
	data.push_back({offset + scale * 1000.0, offset - scale * 1000.0});
	return data;
}

void MakeMixedData(std::vector<ColumnData> &data, std::vector<ColumnInfo> &info, double offset) {
	ColumnInfo num_info(FeatureType::NUMERIC, "x");
	ColumnInfo cat_info(FeatureType::CATEGORICAL, "c");
	ColumnData num_col(FeatureType::NUMERIC);
	ColumnData cat_col(FeatureType::CATEGORICAL);

	for (int i = 0; i < 40; i++) {
		num_col.numeric_values.push_back(offset + static_cast<double>(i));
		cat_col.category_indices.push_back(cat_info.AddCategory(i % 4 == 0 ? "a" : "b"));
	}
	// Obvious outlier with a rare category
	num_col.numeric_values.push_back(offset + 5000.0);
	cat_col.category_indices.push_back(cat_info.AddCategory("rare"));

	data = {num_col, cat_col};
	info = {num_info, cat_info};
}

} // namespace

TEST_CASE("test/cpp/anofox_isolation_forest_refit_equals_fresh_fit", "[anofox]") {
	auto first_data = MakeNumericData(0.0, 1.0);
	auto second_data = MakeNumericData(500.0, 3.0);

	// Fit, then refit on different data: the refitted model must not score
	// through stale trees from the first fit.
	IsolationForest refit_forest(50, 32, 0.1, 42);
	refit_forest.Fit(first_data);
	refit_forest.Fit(second_data);
	auto refit_scores = refit_forest.ScoreBatch(second_data);

	IsolationForest fresh_forest(50, 32, 0.1, 42);
	fresh_forest.Fit(second_data);
	auto fresh_scores = fresh_forest.ScoreBatch(second_data);

	REQUIRE(refit_scores.size() == fresh_scores.size());
	for (size_t i = 0; i < fresh_scores.size(); i++) {
		REQUIRE(refit_scores[i] == fresh_scores[i]);
	}
}

TEST_CASE("test/cpp/anofox_isolation_forest_mixed_refit_equals_fresh_fit", "[anofox]") {
	std::vector<ColumnData> first_data, second_data;
	std::vector<ColumnInfo> first_info, second_info;
	MakeMixedData(first_data, first_info, 0.0);
	MakeMixedData(second_data, second_info, 700.0);

	IsolationForest refit_forest(50, 32, 0.1, 42);
	refit_forest.FitMixed(first_data, first_info);
	refit_forest.FitMixed(second_data, second_info);
	auto refit_scores = refit_forest.ScoreBatchMixed(second_data);

	IsolationForest fresh_forest(50, 32, 0.1, 42);
	fresh_forest.FitMixed(second_data, second_info);
	auto fresh_scores = fresh_forest.ScoreBatchMixed(second_data);

	REQUIRE(refit_scores.size() == fresh_scores.size());
	for (size_t i = 0; i < fresh_scores.size(); i++) {
		REQUIRE(refit_scores[i] == fresh_scores[i]);
	}
}

TEST_CASE("test/cpp/anofox_isolation_forest_shape_validation", "[anofox]") {
	IsolationForest forest(10, 16, 0.1, 7);

	SECTION("ragged rows throw") {
		std::vector<std::vector<double>> ragged = {{1.0, 2.0}, {3.0}};
		REQUIRE_THROWS_AS(forest.Fit(ragged), std::invalid_argument);
	}

	SECTION("rows without features throw") {
		std::vector<std::vector<double>> empty_rows = {{}, {}};
		REQUIRE_THROWS_AS(forest.Fit(empty_rows), std::invalid_argument);
	}

	SECTION("scoring with mismatching point width throws") {
		std::vector<std::vector<double>> data;
		for (int i = 0; i < 8; i++) {
			data.push_back({static_cast<double>(i), static_cast<double>(-i)});
		}
		forest.Fit(data);

		REQUIRE_THROWS_AS(forest.Score({1.0}), std::invalid_argument);
		REQUIRE_THROWS_AS(forest.Score({1.0, 2.0, 3.0}), std::invalid_argument);
		// Matching width must not throw
		REQUIRE_NOTHROW(forest.Score({1.0, 2.0}));
	}
}

TEST_CASE("test/cpp/anofox_isolation_forest_mixed_shape_validation", "[anofox]") {
	IsolationForest forest(10, 16, 0.1, 7);

	std::vector<ColumnData> data;
	std::vector<ColumnInfo> info;
	MakeMixedData(data, info, 0.0);

	SECTION("column_info count mismatch throws") {
		std::vector<ColumnInfo> short_info = {info[0]};
		REQUIRE_THROWS_AS(forest.FitMixed(data, short_info), std::invalid_argument);
	}

	SECTION("mismatching column lengths throw") {
		auto ragged = data;
		ragged[0].numeric_values.pop_back();
		REQUIRE_THROWS_AS(forest.FitMixed(ragged, info), std::invalid_argument);
	}

	SECTION("column type mismatch throws") {
		auto swapped_info = info;
		swapped_info[0].type = FeatureType::CATEGORICAL;
		REQUIRE_THROWS_AS(forest.FitMixed(data, swapped_info), std::invalid_argument);
	}

	SECTION("scoring with mismatching widths throws") {
		forest.FitMixed(data, info);

		// Two features were fitted; one value per row is too few
		REQUIRE_THROWS_AS(forest.ScoreMixed({1.0}, {-1}), std::invalid_argument);
		// Inconsistent numeric/category widths
		REQUIRE_THROWS_AS(forest.ScoreMixed({1.0, 2.0}, {-1}), std::invalid_argument);

		// Numeric Score() on a mixed-type forest is a contract violation
		REQUIRE_THROWS_AS(forest.Score({1.0, 2.0}), std::invalid_argument);
	}
}
