#include "duckdb/storage/table/row_group.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/transaction/transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/field_writer.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/update_segment.hpp"
#include "duckdb/common/chrono.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/storage/checkpoint/table_data_writer.hpp"
#include "duckdb/storage/meta_block_reader.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"

namespace duckdb {

constexpr const idx_t RowGroup::ROW_GROUP_VECTOR_COUNT;
constexpr const idx_t RowGroup::ROW_GROUP_SIZE;

RowGroup::RowGroup(RowGroupCollection &collection, idx_t start, idx_t count)
    : SegmentBase<RowGroup>(start, count), collection(collection) {
	Verify();
}

RowGroup::RowGroup(RowGroupCollection &collection, RowGroupPointer &&pointer)
    : SegmentBase<RowGroup>(pointer.row_start, pointer.tuple_count), collection(collection) {
	// deserialize the columns
	if (pointer.data_pointers.size() != collection.GetTypes().size()) {
		throw IOException("Row group column count is unaligned with table column count. Corrupt file?");
	}
	this->column_pointers = std::move(pointer.data_pointers);
	this->columns.resize(column_pointers.size());
	this->is_loaded = unique_ptr<atomic<bool>[]>(new atomic<bool>[columns.size()]);
	for (idx_t c = 0; c < columns.size(); c++) {
		this->is_loaded[c] = false;
	}
	this->version_info = std::move(pointer.versions);

	Verify();
}

void RowGroup::MoveToCollection(RowGroupCollection &collection, idx_t new_start) {
	this->collection = collection;
	this->start = new_start;
	for (auto &column : GetColumns()) {
		column->SetStart(new_start);
	}
	if (version_info) {
		version_info->SetStart(new_start);
	}
}

void VersionNode::SetStart(idx_t start) {
	idx_t current_start = start;
	for (idx_t i = 0; i < RowGroup::ROW_GROUP_VECTOR_COUNT; i++) {
		if (info[i]) {
			info[i]->start = current_start;
		}
		current_start += STANDARD_VECTOR_SIZE;
	}
}

RowGroup::~RowGroup() {
}

vector<shared_ptr<ColumnData>> &RowGroup::GetColumns() {
	// ensure all columns are loaded
	for (idx_t c = 0; c < GetColumnCount(); c++) {
		GetColumn(c);
	}
	return columns;
}

idx_t RowGroup::GetColumnCount() const {
	return columns.size();
}

ColumnData &RowGroup::GetColumn(storage_t c) {
	D_ASSERT(c < columns.size());
	if (!is_loaded) {
		// not being lazy loaded
		D_ASSERT(columns[c]);
		return *columns[c];
	}
	if (is_loaded[c]) {
		D_ASSERT(columns[c]);
		return *columns[c];
	}
	lock_guard<mutex> l(row_group_lock);
	if (columns[c]) {
		D_ASSERT(is_loaded[c]);
		return *columns[c];
	}
	if (column_pointers.size() != columns.size()) {
		throw InternalException("Lazy loading a column but the pointer was not set");
	}
	auto &block_manager = GetCollection().GetBlockManager();
	auto &types = GetCollection().GetTypes();
	auto &block_pointer = column_pointers[c];
	MetaBlockReader column_data_reader(block_manager, block_pointer.block_id);
	column_data_reader.offset = block_pointer.offset;
	this->columns[c] =
	    ColumnData::Deserialize(GetBlockManager(), GetTableInfo(), c, start, column_data_reader, types[c], nullptr);
	is_loaded[c] = true;
	return *columns[c];
}

DatabaseInstance &RowGroup::GetDatabase() {
	return GetCollection().GetDatabase();
}

BlockManager &RowGroup::GetBlockManager() {
	return GetCollection().GetBlockManager();
}
DataTableInfo &RowGroup::GetTableInfo() {
	return GetCollection().GetTableInfo();
}

void RowGroup::InitializeEmpty(const vector<LogicalType> &types) {
	// set up the segment trees for the column segments
	D_ASSERT(columns.empty());
	for (idx_t i = 0; i < types.size(); i++) {
		auto column_data = ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), i, start, types[i]);
		columns.push_back(std::move(column_data));
	}
}

void ColumnScanState::Initialize(const LogicalType &type) {
	if (type.id() == LogicalTypeId::VALIDITY) {
		// validity - nothing to initialize
		return;
	}
	if (type.InternalType() == PhysicalType::STRUCT) {
		// validity + struct children
		auto &struct_children = StructType::GetChildTypes(type);
		child_states.resize(struct_children.size() + 1);
		for (idx_t i = 0; i < struct_children.size(); i++) {
			child_states[i + 1].Initialize(struct_children[i].second);
		}
	} else if (type.InternalType() == PhysicalType::LIST) {
		// validity + list child
		child_states.resize(2);
		child_states[1].Initialize(ListType::GetChildType(type));
	} else {
		// validity
		child_states.resize(1);
	}
}

void CollectionScanState::Initialize(const vector<LogicalType> &types) {
	auto &column_ids = GetColumnIds();
	column_scans = make_unsafe_uniq_array<ColumnScanState>(column_ids.size());
	for (idx_t i = 0; i < column_ids.size(); i++) {
		if (column_ids[i] == COLUMN_IDENTIFIER_ROW_ID) {
			continue;
		}
		column_scans[i].Initialize(types[column_ids[i]]);
	}
}

bool RowGroup::InitializeScanWithOffset(CollectionScanState &state, idx_t vector_offset) {
	auto &column_ids = state.GetColumnIds();
	auto filters = state.GetFilters();
	if (filters) {
		if (!CheckZonemap(*filters, column_ids)) {
			return false;
		}
	}

	state.row_group = this;
	state.vector_index = vector_offset;
	state.max_row_group_row =
	    this->start > state.max_row ? 0 : MinValue<idx_t>(this->count, state.max_row - this->start);
	D_ASSERT(state.column_scans);
	for (idx_t i = 0; i < column_ids.size(); i++) {
		const auto &column = column_ids[i];
		if (column != COLUMN_IDENTIFIER_ROW_ID) {
			auto &column_data = GetColumn(column);
			column_data.InitializeScanWithOffset(state.column_scans[i], start + vector_offset * STANDARD_VECTOR_SIZE);
		} else {
			state.column_scans[i].current = nullptr;
		}
	}
	return true;
}

bool RowGroup::InitializeScan(CollectionScanState &state) {
	auto &column_ids = state.GetColumnIds();
	auto filters = state.GetFilters();
	if (filters) {
		if (!CheckZonemap(*filters, column_ids)) {
			return false;
		}
	}
	state.row_group = this;
	state.vector_index = 0;
	state.max_row_group_row =
	    this->start > state.max_row ? 0 : MinValue<idx_t>(this->count, state.max_row - this->start);
	if (state.max_row_group_row == 0) {
		return false;
	}
	D_ASSERT(state.column_scans);
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto column = column_ids[i];
		if (column != COLUMN_IDENTIFIER_ROW_ID) {
			auto &column_data = GetColumn(column);
			column_data.InitializeScan(state.column_scans[i]);
		} else {
			state.column_scans[i].current = nullptr;
		}
	}
	return true;
}

unique_ptr<RowGroup> RowGroup::AlterType(RowGroupCollection &new_collection, const LogicalType &target_type,
                                         idx_t changed_idx, ExpressionExecutor &executor,
                                         CollectionScanState &scan_state, DataChunk &scan_chunk) {
	Verify();

	// construct a new column data for this type
	auto column_data = ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), changed_idx, start, target_type);

	ColumnAppendState append_state;
	column_data->InitializeAppend(append_state);

	// scan the original table, and fill the new column with the transformed value
	scan_state.Initialize(GetCollection().GetTypes());
	InitializeScan(scan_state);

	DataChunk append_chunk;
	vector<LogicalType> append_types;
	append_types.push_back(target_type);
	append_chunk.Initialize(Allocator::DefaultAllocator(), append_types);
	auto &append_vector = append_chunk.data[0];
	while (true) {
		// scan the table
		scan_chunk.Reset();
		ScanCommitted(scan_state, scan_chunk, TableScanType::TABLE_SCAN_COMMITTED_ROWS);
		if (scan_chunk.size() == 0) {
			break;
		}
		// execute the expression
		append_chunk.Reset();
		executor.ExecuteExpression(scan_chunk, append_vector);
		column_data->Append(append_state, append_vector, scan_chunk.size());
	}

	// set up the row_group based on this row_group
	auto row_group = make_uniq<RowGroup>(new_collection, this->start, this->count);
	row_group->version_info = version_info;
	auto &cols = GetColumns();
	for (idx_t i = 0; i < cols.size(); i++) {
		if (i == changed_idx) {
			// this is the altered column: use the new column
			row_group->columns.push_back(std::move(column_data));
		} else {
			// this column was not altered: use the data directly
			row_group->columns.push_back(cols[i]);
		}
	}
	row_group->Verify();
	return row_group;
}

unique_ptr<RowGroup> RowGroup::AddColumn(RowGroupCollection &new_collection, ColumnDefinition &new_column,
                                         ExpressionExecutor &executor, Expression *default_value, Vector &result) {
	Verify();

	// construct a new column data for the new column
	auto added_column =
	    ColumnData::CreateColumn(GetBlockManager(), GetTableInfo(), GetColumnCount(), start, new_column.Type());

	idx_t rows_to_write = this->count;
	if (rows_to_write > 0) {
		DataChunk dummy_chunk;

		ColumnAppendState state;
		added_column->InitializeAppend(state);
		for (idx_t i = 0; i < rows_to_write; i += STANDARD_VECTOR_SIZE) {
			idx_t rows_in_this_vector = MinValue<idx_t>(rows_to_write - i, STANDARD_VECTOR_SIZE);
			if (default_value) {
				dummy_chunk.SetCardinality(rows_in_this_vector);
				executor.ExecuteExpression(dummy_chunk, result);
			}
			added_column->Append(state, result, rows_in_this_vector);
		}
	}

	// set up the row_group based on this row_group
	auto row_group = make_uniq<RowGroup>(new_collection, this->start, this->count);
	row_group->version_info = version_info;
	row_group->columns = GetColumns();
	// now add the new column
	row_group->columns.push_back(std::move(added_column));

	row_group->Verify();
	return row_group;
}

unique_ptr<RowGroup> RowGroup::RemoveColumn(RowGroupCollection &new_collection, idx_t removed_column) {
	Verify();

	D_ASSERT(removed_column < columns.size());

	auto row_group = make_uniq<RowGroup>(new_collection, this->start, this->count);
	row_group->version_info = version_info;
	// copy over all columns except for the removed one
	auto &cols = GetColumns();
	for (idx_t i = 0; i < cols.size(); i++) {
		if (i != removed_column) {
			row_group->columns.push_back(cols[i]);
		}
	}

	row_group->Verify();
	return row_group;
}

void RowGroup::CommitDrop() {
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		CommitDropColumn(column_idx);
	}
}

void RowGroup::CommitDropColumn(idx_t column_idx) {
	GetColumn(column_idx).CommitDropColumn();
}

void RowGroup::NextVector(CollectionScanState &state) {
	state.vector_index++;
	const auto &column_ids = state.GetColumnIds();
	for (idx_t i = 0; i < column_ids.size(); i++) {
		const auto &column = column_ids[i];
		if (column == COLUMN_IDENTIFIER_ROW_ID) {
			continue;
		}
		D_ASSERT(column < columns.size());
		GetColumn(column).Skip(state.column_scans[i]);
	}
}

bool RowGroup::CheckZonemap(TableFilterSet &filters, const vector<storage_t> &column_ids) {
	for (auto &entry : filters.filters) {
		auto column_index = entry.first;
		auto &filter = entry.second;
		const auto &base_column_index = column_ids[column_index];
		if (!GetColumn(base_column_index).CheckZonemap(*filter)) {
			return false;
		}
	}
	return true;
}

bool RowGroup::CheckZonemapSegments(CollectionScanState &state) {
	auto &column_ids = state.GetColumnIds();
	auto filters = state.GetFilters();
	if (!filters) {
		return true;
	}
	for (auto &entry : filters->filters) {
		D_ASSERT(entry.first < column_ids.size());
		auto column_idx = entry.first;
		const auto &base_column_idx = column_ids[column_idx];
		bool read_segment = GetColumn(base_column_idx).CheckZonemap(state.column_scans[column_idx], *entry.second);
		if (!read_segment) {
			idx_t target_row =
			    state.column_scans[column_idx].current->start + state.column_scans[column_idx].current->count;
			D_ASSERT(target_row >= this->start);
			D_ASSERT(target_row <= this->start + this->count);
			idx_t target_vector_index = (target_row - this->start) / STANDARD_VECTOR_SIZE;
			if (state.vector_index == target_vector_index) {
				// we can't skip any full vectors because this segment contains less than a full vector
				// for now we just bail-out
				// FIXME: we could check if we can ALSO skip the next segments, in which case skipping a full vector
				// might be possible
				// we don't care that much though, since a single segment that fits less than a full vector is
				// exceedingly rare
				return true;
			}
			while (state.vector_index < target_vector_index) {
				NextVector(state);
			}
			return false;
		}
	}

	return true;
}

template <TableScanType TYPE>
void RowGroup::TemplatedScan(TransactionData transaction, CollectionScanState &state, DataChunk &result) {
	const bool ALLOW_UPDATES = TYPE != TableScanType::TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES &&
	                           TYPE != TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED;
	auto table_filters = state.GetFilters();
	const auto &column_ids = state.GetColumnIds();
	auto adaptive_filter = state.GetAdaptiveFilter();
	while (true) {
		if (state.vector_index * STANDARD_VECTOR_SIZE >= state.max_row_group_row) {
			// exceeded the amount of rows to scan
			return;
		}
		idx_t current_row = state.vector_index * STANDARD_VECTOR_SIZE;
		auto max_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, state.max_row_group_row - current_row);

		//! first check the zonemap if we have to scan this partition
		if (!CheckZonemapSegments(state)) {
			continue;
		}
		// second, scan the version chunk manager to figure out which tuples to load for this transaction
		idx_t count;
		SelectionVector valid_sel(STANDARD_VECTOR_SIZE);
		if (TYPE == TableScanType::TABLE_SCAN_REGULAR) {
			count = state.row_group->GetSelVector(transaction, state.vector_index, valid_sel, max_count);
			if (count == 0) {
				// nothing to scan for this vector, skip the entire vector
				NextVector(state);
				continue;
			}
		} else if (TYPE == TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED) {
			count = state.row_group->GetCommittedSelVector(transaction.start_time, transaction.transaction_id,
			                                               state.vector_index, valid_sel, max_count);
			if (count == 0) {
				// nothing to scan for this vector, skip the entire vector
				NextVector(state);
				continue;
			}
		} else {
			count = max_count;
		}
		if (count == max_count && !table_filters) {
			// scan all vectors completely: full scan without deletions or table filters
			for (idx_t i = 0; i < column_ids.size(); i++) {
				const auto &column = column_ids[i];
				if (column == COLUMN_IDENTIFIER_ROW_ID) {
					// scan row id
					D_ASSERT(result.data[i].GetType().InternalType() == ROW_TYPE);
					result.data[i].Sequence(this->start + current_row, 1, count);
				} else {
					auto &col_data = GetColumn(column);
					if (TYPE != TableScanType::TABLE_SCAN_REGULAR) {
						col_data.ScanCommitted(state.vector_index, state.column_scans[i], result.data[i],
						                       ALLOW_UPDATES);
					} else {
						col_data.Scan(transaction, state.vector_index, state.column_scans[i], result.data[i]);
					}
				}
			}
		} else {
			// partial scan: we have deletions or table filters
			idx_t approved_tuple_count = count;
			SelectionVector sel;
			if (count != max_count) {
				sel.Initialize(valid_sel);
			} else {
				sel.Initialize(nullptr);
			}
			//! first, we scan the columns with filters, fetch their data and generate a selection vector.
			//! get runtime statistics
			auto start_time = high_resolution_clock::now();
			if (table_filters) {
				D_ASSERT(adaptive_filter);
				D_ASSERT(ALLOW_UPDATES);
				for (idx_t i = 0; i < table_filters->filters.size(); i++) {
					auto tf_idx = adaptive_filter->permutation[i];
					auto col_idx = column_ids[tf_idx];
					auto &col_data = GetColumn(col_idx);
					col_data.Select(transaction, state.vector_index, state.column_scans[tf_idx], result.data[tf_idx],
					                sel, approved_tuple_count, *table_filters->filters[tf_idx]);
				}
				for (auto &table_filter : table_filters->filters) {
					result.data[table_filter.first].Slice(sel, approved_tuple_count);
				}
			}
			if (approved_tuple_count == 0) {
				// all rows were filtered out by the table filters
				// skip this vector in all the scans that were not scanned yet
				D_ASSERT(table_filters);
				result.Reset();
				for (idx_t i = 0; i < column_ids.size(); i++) {
					auto col_idx = column_ids[i];
					if (col_idx == COLUMN_IDENTIFIER_ROW_ID) {
						continue;
					}
					if (table_filters->filters.find(i) == table_filters->filters.end()) {
						auto &col_data = GetColumn(col_idx);
						col_data.Skip(state.column_scans[i]);
					}
				}
				state.vector_index++;
				continue;
			}
			//! Now we use the selection vector to fetch data for the other columns.
			for (idx_t i = 0; i < column_ids.size(); i++) {
				if (!table_filters || table_filters->filters.find(i) == table_filters->filters.end()) {
					auto column = column_ids[i];
					if (column == COLUMN_IDENTIFIER_ROW_ID) {
						D_ASSERT(result.data[i].GetType().InternalType() == PhysicalType::INT64);
						result.data[i].SetVectorType(VectorType::FLAT_VECTOR);
						auto result_data = FlatVector::GetData<int64_t>(result.data[i]);
						for (size_t sel_idx = 0; sel_idx < approved_tuple_count; sel_idx++) {
							result_data[sel_idx] = this->start + current_row + sel.get_index(sel_idx);
						}
					} else {
						auto &col_data = GetColumn(column);
						if (TYPE == TableScanType::TABLE_SCAN_REGULAR) {
							col_data.FilterScan(transaction, state.vector_index, state.column_scans[i], result.data[i],
							                    sel, approved_tuple_count);
						} else {
							col_data.FilterScanCommitted(state.vector_index, state.column_scans[i], result.data[i], sel,
							                             approved_tuple_count, ALLOW_UPDATES);
						}
					}
				}
			}
			auto end_time = high_resolution_clock::now();
			if (adaptive_filter && table_filters->filters.size() > 1) {
				adaptive_filter->AdaptRuntimeStatistics(duration_cast<duration<double>>(end_time - start_time).count());
			}
			D_ASSERT(approved_tuple_count > 0);
			count = approved_tuple_count;
		}
		result.SetCardinality(count);
		state.vector_index++;
		break;
	}
}

void RowGroup::Scan(TransactionData transaction, CollectionScanState &state, DataChunk &result) {
	TemplatedScan<TableScanType::TABLE_SCAN_REGULAR>(transaction, state, result);
}

void RowGroup::ScanCommitted(CollectionScanState &state, DataChunk &result, TableScanType type) {
	auto &transaction_manager = DuckTransactionManager::Get(GetCollection().GetAttached());

	auto lowest_active_start = transaction_manager.LowestActiveStart();
	auto lowest_active_id = transaction_manager.LowestActiveId();
	TransactionData data(lowest_active_id, lowest_active_start);
	switch (type) {
	case TableScanType::TABLE_SCAN_COMMITTED_ROWS:
		TemplatedScan<TableScanType::TABLE_SCAN_COMMITTED_ROWS>(data, state, result);
		break;
	case TableScanType::TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES:
		TemplatedScan<TableScanType::TABLE_SCAN_COMMITTED_ROWS_DISALLOW_UPDATES>(data, state, result);
		break;
	case TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED:
		TemplatedScan<TableScanType::TABLE_SCAN_COMMITTED_ROWS_OMIT_PERMANENTLY_DELETED>(data, state, result);
		break;
	default:
		throw InternalException("Unrecognized table scan type");
	}
}

ChunkInfo *RowGroup::GetChunkInfo(idx_t vector_idx) {
	if (!version_info) {
		return nullptr;
	}
	return version_info->info[vector_idx].get();
}

idx_t RowGroup::GetSelVector(TransactionData transaction, idx_t vector_idx, SelectionVector &sel_vector,
                             idx_t max_count) {
	lock_guard<mutex> lock(row_group_lock);

	auto info = GetChunkInfo(vector_idx);
	if (!info) {
		return max_count;
	}
	return info->GetSelVector(transaction, sel_vector, max_count);
}

idx_t RowGroup::GetCommittedSelVector(transaction_t start_time, transaction_t transaction_id, idx_t vector_idx,
                                      SelectionVector &sel_vector, idx_t max_count) {
	lock_guard<mutex> lock(row_group_lock);

	auto info = GetChunkInfo(vector_idx);
	if (!info) {
		return max_count;
	}
	return info->GetCommittedSelVector(start_time, transaction_id, sel_vector, max_count);
}

bool RowGroup::Fetch(TransactionData transaction, idx_t row) {
	D_ASSERT(row < this->count);
	lock_guard<mutex> lock(row_group_lock);

	idx_t vector_index = row / STANDARD_VECTOR_SIZE;
	auto info = GetChunkInfo(vector_index);
	if (!info) {
		return true;
	}
	return info->Fetch(transaction, row - vector_index * STANDARD_VECTOR_SIZE);
}

void RowGroup::FetchRow(TransactionData transaction, ColumnFetchState &state, const vector<column_t> &column_ids,
                        row_t row_id, DataChunk &result, idx_t result_idx) {
	for (idx_t col_idx = 0; col_idx < column_ids.size(); col_idx++) {
		auto column = column_ids[col_idx];
		if (column == COLUMN_IDENTIFIER_ROW_ID) {
			// row id column: fill in the row ids
			D_ASSERT(result.data[col_idx].GetType().InternalType() == PhysicalType::INT64);
			result.data[col_idx].SetVectorType(VectorType::FLAT_VECTOR);
			auto data = FlatVector::GetData<row_t>(result.data[col_idx]);
			data[result_idx] = row_id;
		} else {
			// regular column: fetch data from the base column
			auto &col_data = GetColumn(column);
			col_data.FetchRow(transaction, state, row_id, result.data[col_idx], result_idx);
		}
	}
}

void RowGroup::AppendVersionInfo(TransactionData transaction, idx_t count) {
	idx_t row_group_start = this->count.load();
	idx_t row_group_end = row_group_start + count;
	if (row_group_end > RowGroup::ROW_GROUP_SIZE) {
		row_group_end = RowGroup::ROW_GROUP_SIZE;
	}
	lock_guard<mutex> lock(row_group_lock);

	// create the version_info if it doesn't exist yet
	if (!version_info) {
		version_info = make_shared<VersionNode>();
	}
	idx_t start_vector_idx = row_group_start / STANDARD_VECTOR_SIZE;
	idx_t end_vector_idx = (row_group_end - 1) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx <= end_vector_idx; vector_idx++) {
		idx_t start = vector_idx == start_vector_idx ? row_group_start - start_vector_idx * STANDARD_VECTOR_SIZE : 0;
		idx_t end =
		    vector_idx == end_vector_idx ? row_group_end - end_vector_idx * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;
		if (start == 0 && end == STANDARD_VECTOR_SIZE) {
			// entire vector is encapsulated by append: append a single constant
			auto constant_info = make_uniq<ChunkConstantInfo>(this->start + vector_idx * STANDARD_VECTOR_SIZE);
			constant_info->insert_id = transaction.transaction_id;
			constant_info->delete_id = NOT_DELETED_ID;
			version_info->info[vector_idx] = std::move(constant_info);
		} else {
			// part of a vector is encapsulated: append to that part
			ChunkVectorInfo *info;
			if (!version_info->info[vector_idx]) {
				// first time appending to this vector: create new info
				auto insert_info = make_uniq<ChunkVectorInfo>(this->start + vector_idx * STANDARD_VECTOR_SIZE);
				info = insert_info.get();
				version_info->info[vector_idx] = std::move(insert_info);
			} else {
				D_ASSERT(version_info->info[vector_idx]->type == ChunkInfoType::VECTOR_INFO);
				// use existing vector
				info = &version_info->info[vector_idx]->Cast<ChunkVectorInfo>();
			}
			info->Append(start, end, transaction.transaction_id);
		}
	}
	this->count = row_group_end;
}

void RowGroup::CommitAppend(transaction_t commit_id, idx_t row_group_start, idx_t count) {
	D_ASSERT(version_info.get());
	idx_t row_group_end = row_group_start + count;
	lock_guard<mutex> lock(row_group_lock);

	idx_t start_vector_idx = row_group_start / STANDARD_VECTOR_SIZE;
	idx_t end_vector_idx = (row_group_end - 1) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx <= end_vector_idx; vector_idx++) {
		idx_t start = vector_idx == start_vector_idx ? row_group_start - start_vector_idx * STANDARD_VECTOR_SIZE : 0;
		idx_t end =
		    vector_idx == end_vector_idx ? row_group_end - end_vector_idx * STANDARD_VECTOR_SIZE : STANDARD_VECTOR_SIZE;

		auto info = version_info->info[vector_idx].get();
		info->CommitAppend(commit_id, start, end);
	}
}

void RowGroup::RevertAppend(idx_t row_group_start) {
	if (!version_info) {
		return;
	}
	idx_t start_row = row_group_start - this->start;
	idx_t start_vector_idx = (start_row + (STANDARD_VECTOR_SIZE - 1)) / STANDARD_VECTOR_SIZE;
	for (idx_t vector_idx = start_vector_idx; vector_idx < RowGroup::ROW_GROUP_VECTOR_COUNT; vector_idx++) {
		version_info->info[vector_idx].reset();
	}
	for (auto &column : columns) {
		column->RevertAppend(row_group_start);
	}
	this->count = MinValue<idx_t>(row_group_start - this->start, this->count);
	Verify();
}

void RowGroup::InitializeAppend(RowGroupAppendState &append_state) {
	append_state.row_group = this;
	append_state.offset_in_row_group = this->count;
	// for each column, initialize the append state
	append_state.states = make_unsafe_uniq_array<ColumnAppendState>(GetColumnCount());
	for (idx_t i = 0; i < GetColumnCount(); i++) {
		auto &col_data = GetColumn(i);
		col_data.InitializeAppend(append_state.states[i]);
	}
}

void RowGroup::Append(RowGroupAppendState &state, DataChunk &chunk, idx_t append_count) {
	// append to the current row_group
	for (idx_t i = 0; i < GetColumnCount(); i++) {
		auto &col_data = GetColumn(i);
		col_data.Append(state.states[i], chunk.data[i], append_count);
	}
	state.offset_in_row_group += append_count;
}

void RowGroup::Update(TransactionData transaction, DataChunk &update_chunk, row_t *ids, idx_t offset, idx_t count,
                      const vector<PhysicalIndex> &column_ids) {
#ifdef DEBUG
	for (size_t i = offset; i < offset + count; i++) {
		D_ASSERT(ids[i] >= row_t(this->start) && ids[i] < row_t(this->start + this->count));
	}
#endif
	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto column = column_ids[i];
		D_ASSERT(column.index != COLUMN_IDENTIFIER_ROW_ID);
		auto &col_data = GetColumn(column.index);
		D_ASSERT(col_data.type.id() == update_chunk.data[i].GetType().id());
		if (offset > 0) {
			Vector sliced_vector(update_chunk.data[i], offset, offset + count);
			sliced_vector.Flatten(count);
			col_data.Update(transaction, column.index, sliced_vector, ids + offset, count);
		} else {
			col_data.Update(transaction, column.index, update_chunk.data[i], ids, count);
		}
		MergeStatistics(column.index, *col_data.GetUpdateStatistics());
	}
}

void RowGroup::UpdateColumn(TransactionData transaction, DataChunk &updates, Vector &row_ids,
                            const vector<column_t> &column_path) {
	D_ASSERT(updates.ColumnCount() == 1);
	auto ids = FlatVector::GetData<row_t>(row_ids);

	auto primary_column_idx = column_path[0];
	D_ASSERT(primary_column_idx != COLUMN_IDENTIFIER_ROW_ID);
	D_ASSERT(primary_column_idx < columns.size());
	auto &col_data = GetColumn(primary_column_idx);
	col_data.UpdateColumn(transaction, column_path, updates.data[0], ids, updates.size(), 1);
	MergeStatistics(primary_column_idx, *col_data.GetUpdateStatistics());
}

unique_ptr<BaseStatistics> RowGroup::GetStatistics(idx_t column_idx) {
	auto &col_data = GetColumn(column_idx);
	lock_guard<mutex> slock(stats_lock);
	return col_data.GetStatistics();
}

void RowGroup::MergeStatistics(idx_t column_idx, const BaseStatistics &other) {
	auto &col_data = GetColumn(column_idx);
	lock_guard<mutex> slock(stats_lock);
	col_data.MergeStatistics(other);
}

void RowGroup::MergeIntoStatistics(idx_t column_idx, BaseStatistics &other) {
	auto &col_data = GetColumn(column_idx);
	lock_guard<mutex> slock(stats_lock);
	col_data.MergeIntoStatistics(other);
}

RowGroupWriteData RowGroup::WriteToDisk(PartialBlockManager &manager,
                                        const vector<CompressionType> &compression_types) {
	RowGroupWriteData result;
	result.states.reserve(columns.size());
	result.statistics.reserve(columns.size());

	// Checkpoint the individual columns of the row group
	// Here we're iterating over columns. Each column can have multiple segments.
	// (Some columns will be wider than others, and require different numbers
	// of blocks to encode.) Segments cannot span blocks.
	//
	// Some of these columns are composite (list, struct). The data is written
	// first sequentially, and the pointers are written later, so that the
	// pointers all end up densely packed, and thus more cache-friendly.
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		auto &column = GetColumn(column_idx);
		ColumnCheckpointInfo checkpoint_info {compression_types[column_idx]};
		auto checkpoint_state = column.Checkpoint(*this, manager, checkpoint_info);
		D_ASSERT(checkpoint_state);

		auto stats = checkpoint_state->GetStatistics();
		D_ASSERT(stats);

		result.statistics.push_back(stats->Copy());
		result.states.push_back(std::move(checkpoint_state));
	}
	D_ASSERT(result.states.size() == result.statistics.size());
	return result;
}

RowGroupPointer RowGroup::Checkpoint(RowGroupWriter &writer, TableStatistics &global_stats) {
	RowGroupPointer row_group_pointer;

	vector<CompressionType> compression_types;
	compression_types.reserve(columns.size());
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		compression_types.push_back(writer.GetColumnCompressionType(column_idx));
	}
	auto result = WriteToDisk(writer.GetPartialBlockManager(), compression_types);
	for (idx_t column_idx = 0; column_idx < GetColumnCount(); column_idx++) {
		global_stats.GetStats(column_idx).Statistics().Merge(result.statistics[column_idx]);
	}

	// construct the row group pointer and write the column meta data to disk
	D_ASSERT(result.states.size() == columns.size());
	row_group_pointer.row_start = start;
	row_group_pointer.tuple_count = count;
	for (auto &state : result.states) {
		// get the current position of the table data writer
		auto &data_writer = writer.GetPayloadWriter();
		auto pointer = data_writer.GetBlockPointer();

		// store the stats and the data pointers in the row group pointers
		row_group_pointer.data_pointers.push_back(pointer);

		// Write pointers to the column segments.
		//
		// Just as above, the state can refer to many other states, so this
		// can cascade recursively into more pointer writes.
		state->WriteDataPointers(writer);
	}
	row_group_pointer.versions = version_info;
	Verify();
	return row_group_pointer;
}

void RowGroup::CheckpointDeletes(VersionNode *versions, Serializer &serializer) {
	if (!versions) {
		// no version information: write nothing
		serializer.Write<idx_t>(0);
		return;
	}
	// first count how many ChunkInfo's we need to deserialize
	idx_t chunk_info_count = 0;
	for (idx_t vector_idx = 0; vector_idx < RowGroup::ROW_GROUP_VECTOR_COUNT; vector_idx++) {
		auto chunk_info = versions->info[vector_idx].get();
		if (!chunk_info) {
			continue;
		}
		chunk_info_count++;
	}
	// now serialize the actual version information
	serializer.Write<idx_t>(chunk_info_count);
	for (idx_t vector_idx = 0; vector_idx < RowGroup::ROW_GROUP_VECTOR_COUNT; vector_idx++) {
		auto chunk_info = versions->info[vector_idx].get();
		if (!chunk_info) {
			continue;
		}
		serializer.Write<idx_t>(vector_idx);
		chunk_info->Serialize(serializer);
	}
}

shared_ptr<VersionNode> RowGroup::DeserializeDeletes(Deserializer &source) {
	auto chunk_count = source.Read<idx_t>();
	if (chunk_count == 0) {
		// no deletes
		return nullptr;
	}
	auto version_info = make_shared<VersionNode>();
	for (idx_t i = 0; i < chunk_count; i++) {
		idx_t vector_index = source.Read<idx_t>();
		if (vector_index >= RowGroup::ROW_GROUP_VECTOR_COUNT) {
			throw Exception("In DeserializeDeletes, vector_index is out of range for the row group. Corrupted file?");
		}
		version_info->info[vector_index] = ChunkInfo::Deserialize(source);
	}
	return version_info;
}

void RowGroup::Serialize(RowGroupPointer &pointer, Serializer &main_serializer) {
	FieldWriter writer(main_serializer);
	writer.WriteField<uint64_t>(pointer.row_start);
	writer.WriteField<uint64_t>(pointer.tuple_count);
	auto &serializer = writer.GetSerializer();
	for (auto &data_pointer : pointer.data_pointers) {
		serializer.Write<block_id_t>(data_pointer.block_id);
		serializer.Write<uint64_t>(data_pointer.offset);
	}
	CheckpointDeletes(pointer.versions.get(), serializer);
	writer.Finalize();
}

RowGroupPointer RowGroup::Deserialize(Deserializer &main_source, const vector<LogicalType> &columns) {
	RowGroupPointer result;

	FieldReader reader(main_source);
	result.row_start = reader.ReadRequired<uint64_t>();
	result.tuple_count = reader.ReadRequired<uint64_t>();

	auto physical_columns = columns.size();
	result.data_pointers.reserve(physical_columns);

	auto &source = reader.GetSource();
	for (idx_t i = 0; i < physical_columns; i++) {
		BlockPointer pointer;
		pointer.block_id = source.Read<block_id_t>();
		pointer.offset = source.Read<uint64_t>();
		result.data_pointers.push_back(pointer);
	}
	result.versions = DeserializeDeletes(source);

	reader.Finalize();
	return result;
}

//===--------------------------------------------------------------------===//
// GetStorageInfo
//===--------------------------------------------------------------------===//
void RowGroup::GetStorageInfo(idx_t row_group_index, TableStorageInfo &result) {
	for (idx_t col_idx = 0; col_idx < GetColumnCount(); col_idx++) {
		auto &col_data = GetColumn(col_idx);
		col_data.GetStorageInfo(row_group_index, {col_idx}, result);
	}
}

//===--------------------------------------------------------------------===//
// Version Delete Information
//===--------------------------------------------------------------------===//
class VersionDeleteState {
public:
	VersionDeleteState(RowGroup &info, TransactionData transaction, DataTable &table, idx_t base_row)
	    : info(info), transaction(transaction), table(table), current_info(nullptr),
	      current_chunk(DConstants::INVALID_INDEX), count(0), base_row(base_row), delete_count(0) {
	}

	RowGroup &info;
	TransactionData transaction;
	DataTable &table;
	ChunkVectorInfo *current_info;
	idx_t current_chunk;
	row_t rows[STANDARD_VECTOR_SIZE];
	idx_t count;
	idx_t base_row;
	idx_t chunk_row;
	idx_t delete_count;

public:
	void Delete(row_t row_id);
	void Flush();
};

idx_t RowGroup::Delete(TransactionData transaction, DataTable &table, row_t *ids, idx_t count) {
	lock_guard<mutex> lock(row_group_lock);
	VersionDeleteState del_state(*this, transaction, table, this->start);

	// obtain a write lock
	for (idx_t i = 0; i < count; i++) {
		D_ASSERT(ids[i] >= 0);
		D_ASSERT(idx_t(ids[i]) >= this->start && idx_t(ids[i]) < this->start + this->count);
		del_state.Delete(ids[i] - this->start);
	}
	del_state.Flush();
	return del_state.delete_count;
}

void RowGroup::Verify() {
#ifdef DEBUG
	for (auto &column : GetColumns()) {
		column->Verify(*this);
	}
#endif
}

void VersionDeleteState::Delete(row_t row_id) {
	D_ASSERT(row_id >= 0);
	idx_t vector_idx = row_id / STANDARD_VECTOR_SIZE;
	idx_t idx_in_vector = row_id - vector_idx * STANDARD_VECTOR_SIZE;
	if (current_chunk != vector_idx) {
		Flush();

		if (!info.version_info) {
			info.version_info = make_shared<VersionNode>();
		}

		if (!info.version_info->info[vector_idx]) {
			// no info yet: create it
			info.version_info->info[vector_idx] =
			    make_uniq<ChunkVectorInfo>(info.start + vector_idx * STANDARD_VECTOR_SIZE);
		} else if (info.version_info->info[vector_idx]->type == ChunkInfoType::CONSTANT_INFO) {
			auto &constant = info.version_info->info[vector_idx]->Cast<ChunkConstantInfo>();
			// info exists but it's a constant info: convert to a vector info
			auto new_info = make_uniq<ChunkVectorInfo>(info.start + vector_idx * STANDARD_VECTOR_SIZE);
			new_info->insert_id = constant.insert_id.load();
			for (idx_t i = 0; i < STANDARD_VECTOR_SIZE; i++) {
				new_info->inserted[i] = constant.insert_id.load();
			}
			info.version_info->info[vector_idx] = std::move(new_info);
		}
		D_ASSERT(info.version_info->info[vector_idx]->type == ChunkInfoType::VECTOR_INFO);
		current_info = &info.version_info->info[vector_idx]->Cast<ChunkVectorInfo>();
		current_chunk = vector_idx;
		chunk_row = vector_idx * STANDARD_VECTOR_SIZE;
	}
	rows[count++] = idx_in_vector;
}

void VersionDeleteState::Flush() {
	if (count == 0) {
		return;
	}
	// it is possible for delete statements to delete the same tuple multiple times when combined with a USING clause
	// in the current_info->Delete, we check which tuples are actually deleted (excluding duplicate deletions)
	// this is returned in the actual_delete_count
	auto actual_delete_count = current_info->Delete(transaction.transaction_id, rows, count);
	delete_count += actual_delete_count;
	if (transaction.transaction && actual_delete_count > 0) {
		// now push the delete into the undo buffer, but only if any deletes were actually performed
		transaction.transaction->PushDelete(table, current_info, rows, actual_delete_count, base_row + chunk_row);
	}
	count = 0;
}

} // namespace duckdb
