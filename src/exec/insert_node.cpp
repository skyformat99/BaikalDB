// Copyright (c) 2018-present Baidu, Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "insert_node.h"
#include "runtime_state.h"
#include <unordered_set>

namespace baikaldb {
DECLARE_bool(disable_writebatch_index);
int InsertNode::init(const pb::PlanNode& node) {
    int ret = 0;
    ret = ExecNode::init(node);
    if (ret < 0) {
        DB_WARNING("ExecNode::init fail, ret:%d", ret);
        return ret;
    }
    const pb::InsertNode& insert_node = node.derive_node().insert_node();
    _table_id = insert_node.table_id();
    _global_index_id = _table_id;
    _tuple_id = insert_node.tuple_id();
    _values_tuple_id = insert_node.values_tuple_id();
    _is_replace = insert_node.is_replace();
    if (_node_type == pb::REPLACE_NODE) {
        _is_replace = true;
    }
    _need_ignore = insert_node.need_ignore();
    for (auto& slot : insert_node.update_slots()) {
        _update_slots.push_back(slot);
    }
    for (auto& expr : insert_node.update_exprs()) {
        ExprNode* up_expr = nullptr;
        ret = ExprNode::create_tree(expr, &up_expr);
        if (ret < 0) {
            return ret;
        }
        _update_exprs.push_back(up_expr);
    }
    for (auto id : insert_node.field_ids()) {
        _prepared_field_ids.push_back(id);
    }
    for (auto& expr : insert_node.insert_values()) {
        ExprNode* value_expr = nullptr;
        ret = ExprNode::create_tree(expr, &value_expr);
        if (ret < 0) {
            return ret;
        }
        _insert_values.push_back(value_expr);
    }
    _on_dup_key_update = _update_slots.size() > 0;
    return 0;
}
int InsertNode::open(RuntimeState* state) {
    int ret = 0;
    ret = ExecNode::open(state);
    if (ret < 0) {
        DB_WARNING_STATE(state, "ExecNode::open fail:%d", ret);
        return ret;
    }
    if (_is_explain) {
        return 0;
    }
    for (auto expr : _update_exprs) {
        ret = expr->open();
        if (ret < 0) {
            DB_WARNING_STATE(state, "expr open fail, ret:%d", ret);
            return ret;
        }
    }
    ret = init_schema_info(state);
    if (ret == -1) {
        DB_WARNING_STATE(state, "init schema failed fail:%d", ret);
        return ret;
    }
    int cnt = 0;
    for (auto& pb_record : _pb_node.derive_node().insert_node().records()) {
        SmartRecord record = _factory->new_record(*_table_info);
        record->decode(pb_record);
        _records.push_back(record);
        cnt++;
    }
    //DB_WARNING_STATE(state, "insert_size:%d", cnt);
    if (_on_dup_key_update) {
        _dup_update_row = state->mem_row_desc()->fetch_mem_row();
        if (_tuple_id >= 0) {
            _tuple_desc = state->get_tuple_desc(_tuple_id);
        }
        if (_values_tuple_id >= 0) {
            _values_tuple_desc = state->get_tuple_desc(_values_tuple_id);
        }
    }

    int num_affected_rows = 0;
    AtomicManager<std::atomic<long>> ams[state->reverse_index_map().size()];
    int i = 0;
    for (auto& pair : state->reverse_index_map()) {
        pair.second->sync(ams[i]);
        i++;
    }
    for (auto& record : _records) {
        ret = insert_row(state, record);
        if (ret < 0) {
            DB_WARNING_STATE(state, "insert_row fail");
            return -1;
        }
        num_affected_rows += ret;
    }
    // auto_rollback.release();
    // txn->commit();
    _txn->batch_num_increase_rows = _num_increase_rows;
    state->set_num_increase_rows(_num_increase_rows);
    return num_affected_rows;
}

void InsertNode::transfer_pb(int64_t region_id, pb::PlanNode* pb_node) {
    ExecNode::transfer_pb(region_id, pb_node);
    auto insert_node = pb_node->mutable_derive_node()->mutable_insert_node();
    insert_node->clear_update_exprs();
    for (auto expr : _update_exprs) {
        ExprNode::create_pb_expr(insert_node->add_update_exprs(), expr);
    }
    if (region_id == 0 || _records_by_region.count(region_id) == 0) {
        return;
    }
    std::vector<SmartRecord>& records = _records_by_region[region_id];
    insert_node->clear_records();
    for (auto& record : records) {
        std::string* str = insert_node->add_records();
        record->encode(*str);
    }
}

int InsertNode::expr_optimize(std::vector<pb::TupleDescriptor>* tuple_descs) {
    int ret = 0;
    ret = DMLNode::expr_optimize(tuple_descs);
    if (ret < 0) {
        DB_WARNING("expr type_inferer fail:%d", ret);
        return ret;
    }
    for (auto expr : _insert_values) {
        ret = expr->type_inferer();
        if (ret < 0) {
            DB_WARNING("expr type_inferer fail:%d", ret);
            return ret;
        }
        expr->const_pre_calc();
        if (!expr->is_constant()) {
            DB_WARNING("insert expr must be constant");
            return -1;
        }
    }
    return 0;
}

int InsertNode::insert_values_for_prepared_stmt(std::vector<SmartRecord>& insert_records) {
    if (_prepared_field_ids.size() == 0) {
        DB_WARNING("not execute a prepared stmt");
        return 0;
    }
    if ((_insert_values.size() % _prepared_field_ids.size()) != 0) {
        DB_WARNING("_prepared_field_ids should not be empty()");
        return -1;
    }
    auto tbl_ptr = _factory->get_table_info_ptr(_table_id);
    if (tbl_ptr == nullptr) {
        DB_WARNING("no table found with table_id: %ld", _table_id);
        return -1;
    }
    auto& tbl = *tbl_ptr;
    std::unordered_map<int32_t, FieldInfo> table_field_map;
    std::unordered_set<int32_t> insert_prepared_field_ids;
    std::vector<FieldInfo>  insert_fields;
    std::vector<FieldInfo>  default_fields;

    for (auto& field : tbl.fields) {
        table_field_map.insert({field.id, field});
    }
    for (auto id : _prepared_field_ids) {
        if (table_field_map.count(id) == 0) {
            DB_WARNING("No field for field id: %d", id);
            return -1;
        }
        insert_prepared_field_ids.insert(id);
        insert_fields.push_back(table_field_map[id]);
    }
    for (auto& field : tbl.fields) {
        if (insert_prepared_field_ids.count(field.id) == 0) {
            default_fields.push_back(field);
        }
    }
    size_t row_size = _insert_values.size() / _prepared_field_ids.size();
    for (size_t row_idx = 0; row_idx < row_size; ++row_idx) {
        SmartRecord row = _factory->new_record(_table_id);
        for (size_t col_idx = 0; col_idx < _prepared_field_ids.size(); ++col_idx) {
            size_t idx = row_idx * _prepared_field_ids.size() + col_idx;
            ExprNode* expr = _insert_values[idx];
            if (0 != expr->open()) {
                DB_WARNING("expr open fail");
                return -1;
            }
            if (0 != row->set_value(row->get_field_by_tag(insert_fields[col_idx].id), 
                    expr->get_value(nullptr).cast_to(insert_fields[col_idx].type))) {
                DB_WARNING("fill insert value failed");
                expr->close();
                return -1;
            }
            expr->close();
        }
        for (auto& field : default_fields) {
            if (0 != row->set_value(row->get_field_by_tag(field.id), field.default_expr_value)) {
                DB_WARNING("fill insert value failed");
                return -1;
            }
        }
        //DB_WARNING("row: %s", row->to_string().c_str());
        insert_records.push_back(row);
    }
    for (auto expr : _insert_values) {
        ExprNode::destroy_tree(expr);
    }
    _insert_values.clear();
    return 0;
}

void InsertNode::find_place_holder(std::map<int, ExprNode*>& placeholders) {
    DMLNode::find_place_holder(placeholders);
    for (auto& expr : _insert_values) {
        expr->find_place_holder(placeholders);
    }
}
}

/* vim: set ts=4 sw=4 sts=4 tw=100 */
