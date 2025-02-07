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

#include "delete_planner.h"
#include <gflags/gflags.h>
#include "network_socket.h"

namespace baikaldb {
DEFINE_bool(delete_all_to_truncate, false,  "delete from xxx; treat as truncate");
int DeletePlanner::plan() {
    if (_ctx->stmt_type == parser::NT_TRUNCATE) {
        if (_ctx->runtime_state.client_conn()->txn_id != 0) {
            DB_FATAL("not allowed truncate table in txn connection");
            return -1;
        }
        _truncate_stmt = (parser::TruncateStmt*)(_ctx->stmt);
        if (0 != parse_db_tables(_truncate_stmt->table_name)) {
            DB_WARNING("get truncate_table plan failed");
            return -1;
        }
        create_packet_node(pb::OP_TRUNCATE_TABLE);
        if (0 != create_truncate_node()) {
            DB_WARNING("get truncate_table plan failed");
            return -1;
        }
        return 0;
    }
    _delete_stmt = (parser::DeleteStmt*)(_ctx->stmt);
    if (!_delete_stmt) {
        return -1;
    }
    if (_delete_stmt->delete_table_list.size() != 0) {
        DB_WARNING("unsupport multi table delete");
        return -1;
    }
    if (_delete_stmt->from_table->node_type != parser::NT_TABLE) {
        DB_WARNING("unsupport multi table delete");
        return -1;
    }
    if (0 != parse_db_tables((parser::TableName*)_delete_stmt->from_table)) {
        return -1;
    }
    // delete from xxx; => truncate table xxx;
    if (FLAGS_delete_all_to_truncate &&
        _delete_stmt->where == nullptr &&
        _delete_stmt->limit == nullptr) {
        create_packet_node(pb::OP_TRUNCATE_TABLE);
        if (0 != create_truncate_node()) {
            DB_WARNING("get truncate_table plan failed");
            return -1;
        }
        return 0;
    }

    if (0 != parse_where()) {
        return -1;
    }
    if (0 != parse_orderby()) {
        return -1;
    }
    if (0 != parse_limit()) {
        return -1;
    }
    
    create_packet_node(pb::OP_DELETE);
    if (0 != create_delete_node()) {
        return -1;
    }
    if (0 != create_sort_node()) {
        return -1;
    }
    if (0 != create_filter_node(_where_filters, pb::WHERE_FILTER_NODE)) {
        return -1;
    }
    create_scan_tuple_descs();
    create_order_by_tuple_desc();
    if (0 != create_scan_nodes()) {
        return -1;
    }
    auto iter = _table_tuple_mapping.begin();
    int64_t table_id = iter->first;
    set_dml_txn_state(table_id);
    return 0;
}

int DeletePlanner::create_delete_node() {
    if (_table_tuple_mapping.size() != 1) {
        DB_WARNING("invalid sql format: %s", _ctx->sql.c_str());
        return -1;
    }
    auto iter = _table_tuple_mapping.begin();
    int64_t table_id = iter->first;

    pb::PlanNode* delete_node = _ctx->add_plan_node();
    delete_node->set_node_type(pb::DELETE_NODE);
    delete_node->set_limit(-1);
    delete_node->set_is_explain(_ctx->is_explain);
    delete_node->set_num_children(1); //TODO 
    pb::DerivePlanNode* derive = delete_node->mutable_derive_node();
    pb::DeleteNode* _delete = derive->mutable_delete_node();
    _delete->set_table_id(table_id);

    auto pk = _factory->get_index_info_ptr(iter->first);
    if (pk == nullptr) {
        DB_WARNING("no pk found with id: %ld", iter->first);
        return -1;
    }
    for (auto& field : pk->fields) {
        auto& slot = get_scan_ref_slot(iter->first, field.id, field.type);
        _delete->add_primary_slots()->CopyFrom(slot);
    }
    //全局唯一索引涉及的field_id都要放到slot_ref中
    //auto table_info = SchemaFactory::get_instance()->get_table_info_ptr(table_id);
    //for (auto& index_id : table_info->indices) {
    //    if (!SchemaFactory::get_instance()->is_global_index(index_id)) {
    //        continue;
    //    }
    //    auto index_info = SchemaFactory::get_instance()->get_index_info_ptr(index_id);
    //    for (auto& field : index_info->fields) {
    //        get_scan_ref_slot(table_id, field.id, field.type);
    //    }
    //}
    return 0;
}

int DeletePlanner::create_truncate_node() {
    if (_table_tuple_mapping.size() != 1) {
        DB_WARNING("invalid sql format: %s", _ctx->sql.c_str());
        return -1;
    }
    auto iter = _table_tuple_mapping.begin();

    pb::PlanNode* truncate_node = _ctx->add_plan_node();
    truncate_node->set_node_type(pb::TRUNCATE_NODE);
    truncate_node->set_limit(-1);
    truncate_node->set_num_children(0); //TODO

    pb::DerivePlanNode* derive = truncate_node->mutable_derive_node();
    pb::TruncateNode* _truncate = derive->mutable_truncate_node();
    _truncate->set_table_id(iter->first);
    return 0;
}

int DeletePlanner::parse_where() {
    if (_delete_stmt->where == nullptr) {
        return 0;
    }
    if (0 != flatten_filter(_delete_stmt->where, _where_filters)) {
        DB_WARNING("flatten_filter failed");
        return -1;
    }
    return 0;
}

int DeletePlanner::parse_orderby() {
    if (_delete_stmt != nullptr && _delete_stmt->order != nullptr) {
        DB_WARNING("delete does not support orderby");
        return -1;
    }
    return 0;
}

int DeletePlanner::parse_limit() {
    if (_delete_stmt->limit != nullptr) {
        _ctx->stat_info.error_code = ER_SYNTAX_ERROR;
        _ctx->stat_info.error_msg << "syntax error! delete does not support limit";
        return -1;
    }
    // parser::LimitClause* limit = _delete_stmt->limit;
    // if (limit->offset != nullptr && 0 != create_expr_tree(limit->offset, _limit_offset)) {
    //     DB_WARNING("create limit offset expr failed");
    //     return -1;
    // }
    // if (limit->count != nullptr && 0 != create_expr_tree(limit->count, _limit_count)) {
    //     DB_WARNING("create limit offset expr failed");
    //     return -1;
    // }
    return 0;
}

} //namespace baikaldb
