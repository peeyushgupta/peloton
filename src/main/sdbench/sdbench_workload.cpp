//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// sdbench_workload.cpp
//
// Identification: src/main/sdbench/sdbench_workload.cpp
//
// Copyright (c) 2015-16, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//


#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <iostream>
#include <ctime>
#include <thread>
#include <algorithm>
#include <chrono>

#include "brain/sample.h"
#include "brain/layout_tuner.h"
#include "brain/index_tuner.h"

#include "benchmark/sdbench/sdbench_workload.h"
#include "benchmark/sdbench/sdbench_loader.h"

#include "catalog/manager.h"
#include "catalog/schema.h"
#include "common/types.h"
#include "common/value.h"
#include "common/value_factory.h"
#include "common/logger.h"
#include "common/timer.h"
#include "common/macros.h"
#include "concurrency/transaction.h"
#include "concurrency/transaction_manager_factory.h"

#include "executor/executor_context.h"
#include "executor/abstract_executor.h"
#include "executor/aggregate_executor.h"
#include "executor/seq_scan_executor.h"
#include "executor/logical_tile.h"
#include "executor/logical_tile_factory.h"
#include "executor/materialization_executor.h"
#include "executor/projection_executor.h"
#include "executor/insert_executor.h"
#include "executor/update_executor.h"
#include "executor/nested_loop_join_executor.h"
#include "executor/hybrid_scan_executor.h"

#include "expression/abstract_expression.h"
#include "expression/constant_value_expression.h"
#include "expression/tuple_value_expression.h"
#include "expression/comparison_expression.h"
#include "expression/conjunction_expression.h"
#include "expression/expression_util.h"

#include "planner/abstract_plan.h"
#include "planner/aggregate_plan.h"
#include "planner/materialization_plan.h"
#include "planner/seq_scan_plan.h"
#include "planner/insert_plan.h"
#include "planner/update_plan.h"
#include "planner/projection_plan.h"
#include "planner/nested_loop_join_plan.h"
#include "planner/hybrid_scan_plan.h"

#include "storage/tile.h"
#include "storage/tile_group.h"
#include "storage/tile_group_header.h"
#include "storage/data_table.h"
#include "storage/table_factory.h"

namespace peloton {
namespace benchmark {
namespace sdbench {

// Tuple id counter
oid_t sdbench_tuple_counter = -1000000;

std::vector<oid_t> column_counts = {50, 500};

expression::AbstractExpression *CreatePredicate(const int tuple_start_offset,
                                                const int tuple_end_offset) {
  // ATTR0 >= LOWER_BOUND && < UPPER_BOUND

  // First, create tuple value expression.
  expression::AbstractExpression *tuple_value_expr_left =
      expression::ExpressionUtil::TupleValueFactory(VALUE_TYPE_INTEGER, 0, 0);

  // Second, create constant value expression.
  Value constant_value_left = ValueFactory::GetIntegerValue(tuple_start_offset);

  expression::AbstractExpression *constant_value_expr_left =
      expression::ExpressionUtil::ConstantValueFactory(constant_value_left);

  // Finally, link them together using an greater than expression.
  expression::AbstractExpression *predicate_left =
      expression::ExpressionUtil::ComparisonFactory(
          EXPRESSION_TYPE_COMPARE_GREATERTHANOREQUALTO,
          tuple_value_expr_left,
          constant_value_expr_left);

  expression::AbstractExpression *tuple_value_expr_right =
      expression::ExpressionUtil::TupleValueFactory(VALUE_TYPE_INTEGER, 0, 0);

  Value constant_value_right = ValueFactory::GetIntegerValue(tuple_end_offset);

  expression::AbstractExpression *constant_value_expr_right =
      expression::ExpressionUtil::ConstantValueFactory(constant_value_right);

  expression::AbstractExpression *predicate_right =
      expression::ExpressionUtil::ComparisonFactory(
          EXPRESSION_TYPE_COMPARE_LESSTHAN,
          tuple_value_expr_right,
          constant_value_expr_right);

  expression::AbstractExpression *predicate =
      expression::ExpressionUtil::ConjunctionFactory(EXPRESSION_TYPE_CONJUNCTION_AND,
                                                     predicate_left,
                                                     predicate_right);

  return predicate;
}

void CreateIndexScanPredicate(std::vector<oid_t>& key_column_ids,
                              std::vector<ExpressionType>& expr_types,
                              std::vector<Value>& values,
                              const int tuple_start_offset,
                              const int tuple_end_offset) {
  key_column_ids.push_back(0);
  expr_types.push_back(ExpressionType::EXPRESSION_TYPE_COMPARE_GREATERTHANOREQUALTO);
  values.push_back(ValueFactory::GetIntegerValue(tuple_start_offset));

  key_column_ids.push_back(0);
  expr_types.push_back(ExpressionType::EXPRESSION_TYPE_COMPARE_LESSTHAN);
  values.push_back(ValueFactory::GetIntegerValue(tuple_end_offset));
}

std::ofstream out("outputfile.summary");

oid_t query_itr;

static void WriteOutput(double duration) {
  // Convert to ms
  duration *= 1000;

  LOG_INFO("----------------------------------------------------------");
  LOG_INFO("%d %d %.3lf %.3lf %.1lf %d %d %d :: %.1lf ms",
           state.layout_mode,
           state.operator_type,
           state.selectivity,
           state.projectivity,
           state.write_ratio,
           state.scale_factor,
           state.column_count,
           state.tuples_per_tilegroup,
           duration);

  out << state.layout_mode << " ";
  out << state.operator_type << " ";
  out << state.selectivity << " ";
  out << state.projectivity << " ";
  out << query_itr << " ";
  out << state.write_ratio << " ";
  out << state.scale_factor << " ";
  out << state.column_count << " ";
  out << state.tuples_per_tilegroup << " ";
  out << duration << "\n";

  out.flush();
}

static int GetLowerBound() {
  int tuple_count = state.scale_factor * state.tuples_per_tilegroup;
  int predicate_offset = 0.1 * tuple_count;

  LOG_TRACE("Tuple count : %d", tuple_count);

  int lower_bound = predicate_offset;
  return lower_bound;
}

static int GetUpperBound() {
  int tuple_count = state.scale_factor * state.tuples_per_tilegroup;
  int selected_tuple_count = state.selectivity * tuple_count;
  int predicate_offset = 0.1 * tuple_count;

  int upper_bound = predicate_offset + selected_tuple_count;
  return upper_bound;
}

static void ExecuteTest(std::vector<executor::AbstractExecutor *> &executors,
                        std::vector<double> columns_accessed,
                        double io_cost,
                        brain::SampleType sample_type,
                        std::vector<double> index_columns_accessed,
                        double selectivity) {
  Timer<> timer;

  auto txn_count = state.transactions;
  bool status = false;

  // Construct sample
  brain::Sample layout_sample(columns_accessed, io_cost, brain::SAMPLE_TYPE_ACCESS);
  brain::Sample index_sample(index_columns_accessed, selectivity, sample_type);

  // Run these many transactions
  for (oid_t txn_itr = 0; txn_itr < txn_count; txn_itr++) {
    // Increment query counter
    query_itr++;

    // Reset timer
    timer.Reset();
    timer.Start();

    // Run all the executors
    for (auto executor : executors) {
      status = executor->Init();
      if (status == false) {
        throw Exception("Init failed");
      }

      std::vector<std::unique_ptr<executor::LogicalTile>> result_tiles;

      while (executor->Execute() == true) {
        std::unique_ptr<executor::LogicalTile> result_tile(
            executor->GetOutput());
        result_tiles.emplace_back(result_tile.release());
      }

      // Execute stuff
      executor->Execute();
    }

    // Record samples
    sdbench_table->RecordLayoutSample(layout_sample);
    sdbench_table->RecordIndexSample(index_sample);

    // Emit time
    timer.Stop();
    auto time_per_transaction = timer.GetDuration();

    if(txn_itr % 20 == 0) {
      WriteOutput(time_per_transaction);
    }
  }

}

std::vector<double> GetColumnsAccessed(const std::vector<oid_t> &column_ids) {
  std::vector<double> columns_accessed;
  std::map<oid_t, oid_t> columns_accessed_map;

  // Init map
  for (auto col : column_ids) columns_accessed_map[col] = 1;

  for (int column_itr = 0; column_itr < state.column_count; column_itr++) {
    auto location = columns_accessed_map.find(column_itr);
    auto end = columns_accessed_map.end();
    if (location != end)
      columns_accessed.push_back(1);
    else
      columns_accessed.push_back(0);
  }

  return columns_accessed;
}

void RunDirectTest() {
  const int lower_bound = GetLowerBound();
  const int upper_bound = GetUpperBound();

  LOG_TRACE("Lower bound : %d", lower_bound);
  LOG_TRACE("Upper bound : %d", upper_bound);

  const bool is_inlined = true;
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();

  auto txn = txn_manager.BeginTransaction();

  /////////////////////////////////////////////////////////
  // SEQ SCAN + PREDICATE
  /////////////////////////////////////////////////////////

  std::unique_ptr<executor::ExecutorContext> context(
      new executor::ExecutorContext(txn));

  // Column ids to be added to logical tile after scan.
  std::vector<oid_t> column_ids;
  oid_t column_count = state.projectivity * state.column_count;

  for (oid_t col_itr = 0; col_itr < column_count; col_itr++) {
    column_ids.push_back(sdbench_column_ids[col_itr]);
  }

  // Create and set up seq scan executor
  auto predicate = CreatePredicate(lower_bound, upper_bound);

  auto index_count = sdbench_table->GetIndexCount();
  index::Index *index = nullptr;
  planner::IndexScanPlan::IndexScanDesc index_scan_desc;

  // Check if ad-hoc index exists
  if(index_count != 0) {

    index = sdbench_table->GetIndex(0);

    std::vector<oid_t> key_column_ids;
    std::vector<ExpressionType> expr_types;
    std::vector<Value> values;
    std::vector<expression::AbstractExpression *> runtime_keys;

    CreateIndexScanPredicate(key_column_ids, expr_types, values,
                             lower_bound, upper_bound);

    index_scan_desc = planner::IndexScanPlan::IndexScanDesc(index,
                                                            key_column_ids,
                                                            expr_types,
                                                            values,
                                                            runtime_keys);
  }

  // Determine hybrid scan type
  auto hybrid_scan_type = HYBRID_SCAN_TYPE_SEQUENTIAL;

  if(index_count != 0) {
    hybrid_scan_type = HYBRID_SCAN_TYPE_HYBRID;
  }

  planner::HybridScanPlan hybrid_scan_node(sdbench_table.get(),
                                           predicate,
                                           column_ids,
                                           index_scan_desc,
                                           hybrid_scan_type);

  executor::HybridScanExecutor hybrid_scan_executor(&hybrid_scan_node,
                                                    context.get());

  /////////////////////////////////////////////////////////
  // MATERIALIZE
  /////////////////////////////////////////////////////////

  // Create and set up materialization executor
  std::vector<catalog::Column> output_columns;
  std::unordered_map<oid_t, oid_t> old_to_new_cols;
  oid_t col_itr = 0;
  for (auto column_id : column_ids) {
    auto column =
        catalog::Column(VALUE_TYPE_INTEGER,
                        GetTypeSize(VALUE_TYPE_INTEGER),
                        std::to_string(column_id),
                        is_inlined);
    output_columns.push_back(column);

    old_to_new_cols[col_itr] = col_itr;
    col_itr++;
  }

  std::shared_ptr<const catalog::Schema> output_schema(
      new catalog::Schema(output_columns));
  bool physify_flag = true;  // is going to create a physical tile
  planner::MaterializationPlan mat_node(old_to_new_cols,
                                        output_schema,
                                        physify_flag);

  executor::MaterializationExecutor mat_executor(&mat_node, nullptr);
  mat_executor.AddChild(&hybrid_scan_executor);

  /////////////////////////////////////////////////////////
  // EXECUTE
  /////////////////////////////////////////////////////////

  std::vector<executor::AbstractExecutor *> executors;
  executors.push_back(&mat_executor);

  /////////////////////////////////////////////////////////
  // COLLECT STATS
  /////////////////////////////////////////////////////////
  double io_cost = 10;
  column_ids.push_back(0);

  auto columns_accessed = GetColumnsAccessed(column_ids);

  std::vector<double> index_columns_accessed;
  index_columns_accessed.push_back(0);
  auto selectivity = state.selectivity;

  ExecuteTest(executors,
              columns_accessed,
              io_cost,
              brain::SAMPLE_TYPE_ACCESS,
              index_columns_accessed,
              selectivity);

  txn_manager.CommitTransaction();
}

void RunInsertTest() {
  auto &txn_manager = concurrency::TransactionManagerFactory::GetInstance();

  auto txn = txn_manager.BeginTransaction();

  /////////////////////////////////////////////////////////
  // INSERT
  /////////////////////////////////////////////////////////

  std::unique_ptr<executor::ExecutorContext> context(
      new executor::ExecutorContext(txn));

  std::vector<Value> values;
  Value insert_val = ValueFactory::GetIntegerValue(++sdbench_tuple_counter);
  TargetList target_list;
  DirectMapList direct_map_list;
  std::vector<oid_t> column_ids;

  target_list.clear();
  direct_map_list.clear();

  for (auto col_id = 0; col_id <= state.column_count; col_id++) {
    auto expression =
        expression::ExpressionUtil::ConstantValueFactory(insert_val);
    target_list.emplace_back(col_id, expression);
    column_ids.push_back(col_id);
  }

  std::unique_ptr<const planner::ProjectInfo> project_info(
      new planner::ProjectInfo(std::move(target_list),
                               std::move(direct_map_list)));

  auto orig_tuple_count = state.scale_factor * state.tuples_per_tilegroup;
  auto bulk_insert_count = state.write_ratio * orig_tuple_count;

  LOG_TRACE("Bulk insert count : %lf", bulk_insert_count);
  planner::InsertPlan insert_node(sdbench_table.get(),
                                  std::move(project_info),
                                  bulk_insert_count);
  executor::InsertExecutor insert_executor(&insert_node,
                                           context.get());

  /////////////////////////////////////////////////////////
  // EXECUTE
  /////////////////////////////////////////////////////////

  std::vector<executor::AbstractExecutor *> executors;
  executors.push_back(&insert_executor);

  /////////////////////////////////////////////////////////
  // COLLECT STATS
  /////////////////////////////////////////////////////////
  std::vector<double> columns_accessed;
  double io_cost = 0;
  std::vector<double> index_columns_accessed;
  double selectivity = 0;

  ExecuteTest(executors,
              columns_accessed,
              io_cost,
              brain::SAMPLE_TYPE_UPDATE,
              index_columns_accessed,
              selectivity);

  txn_manager.CommitTransaction();
}

static void RunAdaptTest() {
  double direct_low_proj = 0.06;
  double insert_write_ratio = 0.01;

  state.projectivity = direct_low_proj;
  state.operator_type = OPERATOR_TYPE_DIRECT;
  RunDirectTest();

  state.write_ratio = insert_write_ratio;
  state.operator_type = OPERATOR_TYPE_INSERT;
  RunInsertTest();
  state.write_ratio = 0.0;

  state.projectivity = direct_low_proj;
  state.operator_type = OPERATOR_TYPE_DIRECT;
  RunDirectTest();

  state.write_ratio = insert_write_ratio;
  state.operator_type = OPERATOR_TYPE_INSERT;
  RunInsertTest();
  state.write_ratio = 0.0;

  state.projectivity = direct_low_proj;
  state.operator_type = OPERATOR_TYPE_DIRECT;
  RunDirectTest();

  state.write_ratio = insert_write_ratio;
  state.operator_type = OPERATOR_TYPE_INSERT;
  RunInsertTest();
  state.write_ratio = 0.0;

  state.projectivity = direct_low_proj;
  state.operator_type = OPERATOR_TYPE_DIRECT;
  RunDirectTest();

  state.write_ratio = insert_write_ratio;
  state.operator_type = OPERATOR_TYPE_INSERT;
  RunInsertTest();
  state.write_ratio = 0.0;
}

void RunAdaptExperiment() {
  auto orig_transactions = state.transactions;
  std::thread index_builder;

  // Setup layout tuner
  auto& index_tuner = brain::IndexTuner::GetInstance();

  state.transactions = 100;   // 25

  state.projectivity = 1.0;
  state.selectivity = 0.06;
  state.adapt = true;
  state.column_count = 50;
  state.layout_mode = LAYOUT_TYPE_HYBRID;
  peloton_layout_mode = state.layout_mode;

  // Generate sequence
  GenerateSequence(state.column_count);

  CreateAndLoadTable((LayoutType)peloton_layout_mode);

  // Reset query counter
  query_itr = 0;

  // Start index tuner
  index_tuner.Start();
  index_tuner.AddTable(sdbench_table.get());

  // Run adapt test
  RunAdaptTest();

  // Stop index tuner
  index_tuner.Stop();
  index_tuner.ClearTables();

  // Reset
  state.transactions = orig_transactions;
  state.adapt = false;
  query_itr = 0;

  out.close();
}


}  // namespace sdbench
}  // namespace benchmark
}  // namespace peloton
