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

#include <unordered_set>
#include "meta_server_interact.hpp"
#include "store_interact.hpp"
#include "namespace_manager.h"
#include "database_manager.h"
#include "region_manager.h"
#include "table_manager.h"
#include "cluster_manager.h"
#include "meta_util.h"
#include "meta_rocksdb.h"

namespace baikaldb {
DECLARE_int32(concurrency_num);
DEFINE_int32(region_replica_num, 3, "region replica num, default:3"); 
DEFINE_int32(region_region_size, 100 * 1024 * 1024, "region size, default:100M");
DEFINE_int64(incremental_info_gc_time, 600*1000*1000, "time interval to clear incremental info");

void TableManager::update_index_status(const pb::DdlWorkInfo& ddl_work) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    auto table_id = ddl_work.table_id();
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("update index table_id [%lld] table_info not exist.", table_id);
        return;
    }

    DB_DEBUG("DDL_LOG update_index_status req[%s]", ddl_work.ShortDebugString().c_str());
    pb::MetaManagerRequest request;
    request.set_op_type(pb::OP_UPDATE_INDEX_STATUS);
    request.mutable_ddlwork_info()->CopyFrom(ddl_work);
    request.mutable_table_info()->CopyFrom(_table_info_map[table_id].schema_pb);
    SchemaManager::get_instance()->process_schema_info(NULL, &request, NULL, NULL);
}

void TableManager::drop_index_request(const pb::DdlWorkInfo& ddl_work) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    auto table_id = ddl_work.table_id();
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("update index table_id [%lld] table_info not exist.", table_id);
        return;
    }
    std::string index_name;
    for (const auto& index_info : _table_info_map[table_id].schema_pb.indexs()) {
        if (index_info.index_id() == ddl_work.index_id()) {
            index_name = index_info.index_name();
        }
    }
    pb::MetaManagerRequest request;
    request.set_op_type(pb::OP_DROP_INDEX);
    request.mutable_table_info()->CopyFrom(_table_info_map[table_id].schema_pb);
    request.mutable_table_info()->clear_indexs();
    auto index_to_drop_iter = request.mutable_table_info()->add_indexs();
    index_to_drop_iter->set_index_name(index_name);
    DB_DEBUG("DDL_LOG drop_index_request req[%s]", request.ShortDebugString().c_str());
    SchemaManager::get_instance()->process_schema_info(NULL, &request, NULL, NULL);
}

void TableManager::update_ddlwork_info(const pb::DdlWorkInfo& ddl_work, pb::OpType update_op) {

    DB_DEBUG("DDL_LOG[update_ddlwork_info] ddlwork [%s]", ddl_work.ShortDebugString().c_str());
    pb::MetaManagerRequest request;
    request.set_op_type(update_op);
    request.mutable_ddlwork_info()->set_job_state(ddl_work.job_state());
    request.mutable_ddlwork_info()->set_table_id(ddl_work.table_id());
    request.mutable_ddlwork_info()->set_end_timestamp(std::chrono::seconds(std::time(nullptr)).count());
    SchemaManager::get_instance()->process_schema_info(NULL, &request, NULL, NULL);
}


void TableManager::process_ddl_common_init(pb::StoreHeartBeatResponse* response,
    const pb::DdlWorkInfo& work_info) {
        //DB_NOTICE("DDL_LOG store init ddl_work_info [%s]", 
        //    work_info.ShortDebugString().c_str());
        auto ddlwork_info_ptr = response->add_ddlwork_infos();
        ddlwork_info_ptr->set_table_id(work_info.table_id());
        ddlwork_info_ptr->set_op_type(work_info.op_type());
        ddlwork_info_ptr->set_index_id(work_info.index_id());
        ddlwork_info_ptr->set_job_state(work_info.job_state());
        //存入begin_timestamp，供store对齐ddlwork
        ddlwork_info_ptr->set_begin_timestamp(work_info.begin_timestamp());
}

bool TableManager::process_ddl_update_job_index(DdlWorkMem& meta_work_info, pb::IndexState expected_state,
    pb::IndexState state, pb::StoreHeartBeatResponse* response) {
    
    auto& meta_work_info_pb = meta_work_info.work_info;
    bool all_region_done = std::all_of(
        meta_work_info.region_ddl_infos.begin(),
        meta_work_info.region_ddl_infos.end(),
        [expected_state](typename std::unordered_map<int64_t, DdlRegionMem>::const_reference r) {
            return r.second.workstate == expected_state;
        });
    if (all_region_done) {
        DB_NOTICE("table [%lld] all region done get to [%s]", meta_work_info_pb.table_id(),
            pb::IndexState_Name(state).c_str());
        meta_work_info_pb.set_job_state(state);
        update_index_status(meta_work_info_pb);
    } 
    //整体工作完成后，不给store发送job信息。
    auto op_type = meta_work_info_pb.op_type(); 
    if (DdlHelper::ddlwork_is_finish(op_type, state) && all_region_done) {
        DB_NOTICE("ddlwork job done.");
    } else {
        pb::IndexState current_state;
        if (get_index_state(meta_work_info.table_id, meta_work_info_pb.index_id(), current_state) != 0) {
            DB_WARNING("ddl index not ready. table_id[%lld] index_id[%lld]", 
                meta_work_info.table_id, meta_work_info_pb.index_id());
            return false;
        }   
        auto ddlwork_info_ptr = response->add_ddlwork_infos();
        ddlwork_info_ptr->set_table_id(meta_work_info_pb.table_id());
        ddlwork_info_ptr->set_op_type(meta_work_info_pb.op_type());
        ddlwork_info_ptr->set_job_state(current_state);
        ddlwork_info_ptr->set_index_id(meta_work_info_pb.index_id());
        ddlwork_info_ptr->set_rollback(meta_work_info_pb.rollback());
        //存入begin_timestamp，供store对齐ddlwork
        ddlwork_info_ptr->set_begin_timestamp(meta_work_info_pb.begin_timestamp());
        //DB_NOTICE("ddlwork table_id[%lld], response[%s]", 
        //meta_work_info_pb.table_id(), ddlwork_info_ptr->ShortDebugString().c_str());
    }
    return all_region_done;
}

void TableManager::process_ddl_add_index_process(
    pb::StoreHeartBeatResponse* response,
    DdlWorkMem& meta_work) {
    auto& meta_work_info = meta_work.work_info;
    pb::IndexState current_state;
    if (get_index_state(meta_work.table_id, meta_work_info.index_id(), current_state) != 0) {
        DB_WARNING("ddl index not ready. table_id[%lld] index_id[%lld]", 
            meta_work.table_id, meta_work_info.index_id());
        return;
    }
    meta_work_info.set_job_state(current_state);
    switch (current_state) {
        case pb::IS_NONE:
            process_ddl_update_job_index(meta_work, pb::IS_NONE, pb::IS_DELETE_ONLY, response);
            break;
        case pb::IS_DELETE_LOCAL:
            process_ddl_update_job_index(meta_work, pb::IS_DELETE_LOCAL, pb::IS_WRITE_ONLY, response);
            break;
        case pb::IS_DELETE_ONLY:
            process_ddl_update_job_index(meta_work, pb::IS_DELETE_ONLY, pb::IS_WRITE_ONLY, response);
            break;
        case pb::IS_WRITE_ONLY:
            process_ddl_update_job_index(meta_work, pb::IS_WRITE_ONLY, pb::IS_WRITE_LOCAL, response);
            break;
        case pb::IS_WRITE_LOCAL:
            {
                bool job_done = process_ddl_update_job_index(meta_work, pb::IS_PUBLIC, pb::IS_PUBLIC, response);
                if (job_done) {
                   update_ddlwork_info(meta_work_info, pb::OP_DELETE_DDLWORK);
                }
            }
            break;   
        case pb::IS_PUBLIC:
            DB_DEBUG("DDL_LOG add index job done");
            break;
        default:
            DB_WARNING("DDL_LOG unknown index state[%s]", pb::IndexState_Name(current_state).c_str());
    }
}

void TableManager::process_ddl_del_index_process(
    pb::StoreHeartBeatResponse* response,
    DdlWorkMem& meta_work) {

    DB_DEBUG("process_del_index: store_ddl_work");
    auto& meta_work_info = meta_work.work_info;
    pb::IndexState current_state;
    if (get_index_state(meta_work.table_id, meta_work_info.index_id(), current_state) != 0) {
        DB_WARNING("ddl index not ready. table_id[%lld] index_id[%lld]", 
            meta_work.table_id, meta_work_info.index_id());
        return;
    }
    meta_work_info.set_job_state(current_state);
    switch (current_state) {
        case pb::IS_PUBLIC:
            process_ddl_update_job_index(meta_work, pb::IS_PUBLIC, pb::IS_WRITE_ONLY, response);
            break;
        case pb::IS_WRITE_LOCAL:
            process_ddl_update_job_index(meta_work, pb::IS_WRITE_LOCAL, pb::IS_WRITE_ONLY, response);
            break;
        case pb::IS_WRITE_ONLY:
            process_ddl_update_job_index(meta_work, pb::IS_WRITE_ONLY, pb::IS_DELETE_ONLY, response);
            break;
        case pb::IS_DELETE_ONLY:
            process_ddl_update_job_index(meta_work, pb::IS_DELETE_ONLY, pb::IS_DELETE_LOCAL, response);
            break;
        case pb::IS_DELETE_LOCAL:
            {
                bool job_done = process_ddl_update_job_index(meta_work, pb::IS_NONE, pb::IS_NONE, response);
                if (job_done) {
                   meta_work_info.set_deleted(true);
                   update_ddlwork_info(meta_work_info, pb::OP_DELETE_DDLWORK);
                   update_index_status(meta_work_info);
                }
            }
            break;
        case pb::IS_NONE:
            meta_work_info.set_deleted(true);
            update_ddlwork_info(meta_work_info, pb::OP_DELETE_DDLWORK);
            update_index_status(meta_work_info);
            break;
        default:
            DB_WARNING("DDL_LOG unknown index state[%s]", pb::IndexState_Name(current_state).c_str());
    }
}

int64_t TableManager::get_row_count(int64_t table_id) {
    std::vector<int64_t> region_ids;
    int64_t byte_size_per_record = 0;
    {
        BAIDU_SCOPED_LOCK(_table_mutex);
        if (_table_info_map.find(table_id) == _table_info_map.end()) {
            return 0;
        }
        byte_size_per_record = _table_info_map[table_id].schema_pb.byte_size_per_record();
        for (auto& partition_regions : _table_info_map[table_id].partition_regions) {
            for (auto& region_id :  partition_regions.second) {
                region_ids.push_back(region_id);    
            }
        }
    }
    if (byte_size_per_record == 0) {
        byte_size_per_record = 1;
    }
    std::vector<SmartRegionInfo> region_infos;
    RegionManager::get_instance()->get_region_info(region_ids, region_infos);
    int64_t total_byte_size = 0;
    for (auto& region : region_infos) {
        total_byte_size += region->used_size();
    }
    int64_t total_row_count = 0;
    for (auto& region : region_infos) {
        total_row_count += region->num_table_lines();
    }
    if (total_row_count == 0) {
        total_row_count = total_byte_size / byte_size_per_record;
    }
    return total_row_count;
}

void TableManager::update_table_internal(const pb::MetaManagerRequest& request, const int64_t apply_index, braft::Closure* done,
        std::function<void(const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb)> update_callback) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    update_callback(request, mem_schema_pb);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    set_table_pb(mem_schema_pb);  
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);   
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("update table internal success, request:%s", request.ShortDebugString().c_str());
}

void TableManager::create_table(const pb::MetaManagerRequest& request, const int64_t apply_index, braft::Closure* done) {
    auto& table_info = const_cast<pb::SchemaInfo&>(request.table_info());
    table_info.set_timestamp(time(NULL));
    table_info.set_version(1);
    
    std::string namespace_name = table_info.namespace_name();
    std::string database_name = namespace_name + "\001" + table_info.database();
    std::string table_name = database_name + "\001" + table_info.table_name();
   
    TableMem table_mem;
    table_mem.whether_level_table = false;
    std::string upper_table_name;
    if (table_info.has_upper_table_name()) {
        table_mem.whether_level_table = true;
        upper_table_name = database_name + "\001" + table_info.upper_table_name();
    }
    //校验合法性, 准备数据
    int64_t namespace_id = NamespaceManager::get_instance()->get_namespace_id(namespace_name);
    if (namespace_id == 0) {
        DB_WARNING("request namespace:%s not exist", namespace_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "namespace not exist");
        return;
    }
    table_info.set_namespace_id(namespace_id);

    int64_t database_id = DatabaseManager::get_instance()->get_database_id(database_name);
    if (database_id == 0) {
        DB_WARNING("request database:%s not exist", database_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "database not exist");
        return;
    }
    table_info.set_database_id(database_id);

    if (_table_id_map.find(table_name) != _table_id_map.end()) {
        DB_WARNING("request table_name:%s already exist", table_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table already exist");
        return;
    }

    //分配table_id
    int64_t max_table_id_tmp = _max_table_id;
    table_info.set_table_id(++max_table_id_tmp);
    table_mem.main_table_id = max_table_id_tmp;
    table_mem.global_index_id = max_table_id_tmp;
    if (table_mem.whether_level_table) {
        if (_table_id_map.find(upper_table_name) == _table_id_map.end()) {
            DB_WARNING("request upper_table_name:%s not exist", upper_table_name.c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "upper table not exist");
            return;
        }
        int64_t upper_table_id = _table_id_map[upper_table_name];
        table_info.set_upper_table_id(upper_table_id);
        if (table_info.has_partition_num()) {
            DB_WARNING("table：%s is leve, partition num should be equal to upper table",
                        table_name.c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table already exist");
            return;
        }
        table_info.set_partition_num(1);
        //继承上次表的信息
        table_info.set_top_table_id(_table_info_map[upper_table_id].schema_pb.top_table_id());
        table_info.set_region_size(_table_info_map[upper_table_id].schema_pb.region_size());
        table_info.set_replica_num(_table_info_map[upper_table_id].schema_pb.replica_num());
    } else {
        if (!table_info.has_partition_num()) {
            table_info.set_partition_num(1);
        }
        //非层次表的顶层表填自己
        table_info.set_top_table_id(table_info.table_id());
        if (!table_info.has_region_size()) {
           table_info.set_region_size(FLAGS_region_region_size);
        }
        if (!table_info.has_replica_num()) {
           table_info.set_replica_num(FLAGS_region_replica_num);
        }
    }
    //分配field_id
    bool has_auto_increment = false;
    auto ret = alloc_field_id(table_info, has_auto_increment, table_mem);
    if (ret < 0) {
        DB_WARNING("table:%s 's field info not illegal", table_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field not illegal");
        return;
    }
    
    ret = alloc_index_id(table_info, table_mem, max_table_id_tmp);
    if (ret < 0) {
        DB_WARNING("table:%s 's index info not illegal", table_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "index not illegal");
        return;
    }
    table_mem.schema_pb = table_info;
    //发起交互， 层次表与非层次表区分对待，非层次表需要与store交互，创建第一个region
    //层级表直接继承后父层次的相关信息即可
    if (table_mem.whether_level_table) {
        ret = write_schema_for_level(table_mem, apply_index, done, max_table_id_tmp, has_auto_increment);
    } else {
        ret = write_schema_for_not_level(table_mem, done, max_table_id_tmp, has_auto_increment); 
    }
    if (ret != 0) { 
        DB_WARNING("write rocksdb fail when create table, table:%s", table_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    set_max_table_id(max_table_id_tmp);
    table_mem.schema_pb.clear_init_store();
    table_mem.schema_pb.clear_split_keys();
    set_table_info(table_mem);
    std::vector<pb::SchemaInfo> schema_infos{table_info};
    put_incremental_schemainfo(apply_index, schema_infos);
    DatabaseManager::get_instance()->add_table_id(database_id, table_info.table_id());
    DB_NOTICE("create table completely, _max_table_id:%ld, table_name:%s", _max_table_id, table_name.c_str());
}

void TableManager::drop_table(const pb::MetaManagerRequest& request, const int64_t apply_index, braft::Closure* done) {
    int64_t namespace_id;
    int64_t database_id;
    int64_t drop_table_id;
    auto ret = check_table_exist(request.table_info(), namespace_id, database_id, drop_table_id);
    if (ret < 0) {
        DB_WARNING("input table not exit, request: %s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    std::vector<std::string> delete_rocksdb_keys;
    std::vector<std::string> write_rocksdb_keys;
    std::vector<std::string> write_rocksdb_values;
    delete_rocksdb_keys.push_back(construct_table_key(drop_table_id));
   
    std::vector<int64_t> drop_index_ids;
    drop_index_ids.push_back(drop_table_id);
    for (auto& index_info : _table_info_map[drop_table_id].schema_pb.indexs()) {
        if (!is_global_index(index_info)) {
            continue;
        }
        drop_index_ids.push_back(index_info.index_id());
    } 
    //drop_region_ids用来保存该表的所有region，用来给store发送remove_region
    std::vector<std::int64_t> drop_region_ids;
    //如果table下有region， 直接删除region信息
    for (auto& drop_index_id : drop_index_ids) {
        for (auto& partition_region: _table_info_map[drop_index_id].partition_regions) {
            for (auto& drop_region_id : partition_region.second) {
                std::string drop_region_key = RegionManager::get_instance()->construct_region_key(drop_region_id);
                delete_rocksdb_keys.push_back(drop_region_key);
                drop_region_ids.push_back(drop_region_id);
            }
        }
    }
    //如果是层次表，需要修改顶层表的low_tables信息
    pb::SchemaInfo top_schema_pb;
    int64_t top_table_id = _table_info_map[drop_table_id].schema_pb.top_table_id();
    if (_table_info_map[drop_table_id].schema_pb.has_upper_table_name()
        && _table_info_map.find(top_table_id) != _table_info_map.end()) {
        top_schema_pb = _table_info_map[top_table_id].schema_pb;
        top_schema_pb.clear_lower_table_ids();
        for (auto low_table_id : _table_info_map[top_table_id].schema_pb.lower_table_ids()) {
            if (low_table_id != drop_table_id) {
                top_schema_pb.add_lower_table_ids(low_table_id);
            }
        }
        top_schema_pb.set_version(top_schema_pb.version() + 1);
        std::string top_table_value;
        if (!top_schema_pb.SerializeToString(&top_table_value)) {
            DB_WARNING("request serializeToArray fail when update upper table, request:%s", 
                        top_schema_pb.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
            return ;
        }
        write_rocksdb_keys.push_back(construct_table_key(top_table_id));
        write_rocksdb_values.push_back(top_table_value);
    }
    ret = MetaRocksdb::get_instance()->write_meta_info(write_rocksdb_keys, 
                                                                    write_rocksdb_values, 
                                                                    delete_rocksdb_keys);
    if (ret < 0) {
        DB_WARNING("drop table fail, request：%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    //删除内存中的值
    std::vector<pb::SchemaInfo> schema_infos;
    if (_table_info_map[drop_table_id].schema_pb.has_upper_table_name()
        && _table_info_map.find(top_table_id) != _table_info_map.end()) {
        set_table_pb(top_schema_pb);
        schema_infos.push_back(top_schema_pb);
    }
    erase_table_info(drop_table_id);
    pb::SchemaInfo schema_info;
    schema_info.set_table_id(drop_table_id);
    schema_info.set_deleted(true);
    schema_info.set_table_name("deleted");
    schema_info.set_database("deleted");
    schema_info.set_namespace_name("deleted");
    schema_infos.push_back(schema_info);
    put_incremental_schemainfo(apply_index, schema_infos);
    DatabaseManager::get_instance()->delete_table_id(database_id, drop_table_id);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("drop table success, request:%s", request.ShortDebugString().c_str());
    if (done) {
        Bthread bth_remove_region(&BTHREAD_ATTR_SMALL);
        std::function<void()> remove_function = [drop_region_ids]() {
                RegionManager::get_instance()->send_remove_region_request(drop_region_ids);
            };
        bth_remove_region.run(remove_function);
        Bthread bth_drop_auto(&BTHREAD_ATTR_SMALL);
        auto drop_function = [this, drop_table_id]() {
            pb::MetaManagerRequest request;
            request.set_op_type(pb::OP_DROP_ID_FOR_AUTO_INCREMENT);
            pb::AutoIncrementRequest* auto_incr = request.mutable_auto_increment();
            auto_incr->set_table_id(drop_table_id);
            send_auto_increment_request(request);
        };
        bth_drop_auto.run(drop_function);
    }
}

void TableManager::rename_table(const pb::MetaManagerRequest& request, 
                                const int64_t apply_index,
                                braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    std::string namespace_name = request.table_info().namespace_name();
    std::string database_name = namespace_name + "\001" + request.table_info().database();
    std::string old_table_name = database_name + "\001" + request.table_info().table_name();
    if (!request.table_info().has_new_table_name()) {
        DB_WARNING("request has no new table_name, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
        return;
    }
    std::string new_table_name = database_name + "\001" + request.table_info().new_table_name();
    if (_table_id_map.count(new_table_name) != 0) {
        DB_WARNING("table is existed, table_name:%s", new_table_name.c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "new table name already exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    //更新数据
    mem_schema_pb.set_table_name(request.table_info().new_table_name());
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    //更新内存
    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);
    swap_table_name(old_table_name, new_table_name);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("rename table success, request:%s", request.ShortDebugString().c_str());
}

bool TableManager::check_and_update_incremental(const pb::BaikalHeartBeatRequest* request,
                         pb::BaikalHeartBeatResponse* response, int64_t applied_index) {
    int64_t last_updated_index = request->last_updated_index();
    BAIDU_SCOPED_LOCK(_log_entry_mutex);
    SchemaIncrementalMap* background = _incremental_schemainfo_map.read_background();
    SchemaIncrementalMap* frontground = _incremental_schemainfo_map.read();
    if (frontground->size() == 0 && background->size() == 0) {
        if (last_updated_index < applied_index) {
            return true;
        }
        DB_NOTICE("no schema info need update last_updated_index:%ld", last_updated_index);
        if (response->last_updated_index() < last_updated_index) {
            response->set_last_updated_index(last_updated_index);
        }
        return false;
    } else if (frontground->size() == 0 && background->size() > 0) {
        if (last_updated_index < background->begin()->first) {              
            return true;
        } else {
            auto iter = background->upper_bound(last_updated_index);
            while (iter != background->end()) {
                for (auto info : iter->second) {
                    *(response->add_schema_change_info()) = info;
                }
                last_updated_index = iter->first;
                ++iter;
            }
            if (response->last_updated_index() < last_updated_index) {
                response->set_last_updated_index(last_updated_index);
            }
            return false;
        }
    } else if (frontground->size() > 0) {
        if (last_updated_index < frontground->begin()->first) {
            return true;
        } else {
            auto iter = frontground->upper_bound(last_updated_index);
            while (iter != frontground->end()) {
                for (auto info : iter->second) {
                    *(response->add_schema_change_info()) = info;
                }
                last_updated_index = iter->first;
                ++iter;
            }
            iter = background->upper_bound(last_updated_index);
            while (iter != background->end()) {
                for (auto info : iter->second) {
                    *(response->add_schema_change_info()) = info;
                }
                last_updated_index = iter->first;
                ++iter;
            }
            if (response->last_updated_index() < last_updated_index) {
                response->set_last_updated_index(last_updated_index);
            }
            return false;         
        }
    }
    return false;
}

void TableManager::put_incremental_schemainfo(const int64_t apply_index, std::vector<pb::SchemaInfo>& schema_infos) {
    BAIDU_SCOPED_LOCK(_log_entry_mutex);
    SchemaIncrementalMap* background = _incremental_schemainfo_map.read_background();
    SchemaIncrementalMap* frontground = _incremental_schemainfo_map.read();
    (*background)[apply_index] = schema_infos;
    if (FLAGS_incremental_info_gc_time < _gc_time_cost.get_time()) {
        if (background->size() < 100 && frontground->size() < 100) {
            _gc_time_cost.reset();
            return ;
        }
        if (frontground->size() > 0) {
            DB_WARNING("clear schemainfo frontground size:%d start:%ld end:%ld", 
                frontground->size(), frontground->begin()->first, frontground->rbegin()->first);
        }
        frontground->clear();
        _incremental_schemainfo_map.swap();
        _gc_time_cost.reset();
    }  
}

void TableManager::update_byte_size(const pb::MetaManagerRequest& request,
                                    const int64_t apply_index, 
                                    braft::Closure* done) {
    update_table_internal(request, apply_index, done, 
        [](const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb) {
            mem_schema_pb.set_byte_size_per_record(request.table_info().byte_size_per_record());
            mem_schema_pb.set_version(mem_schema_pb.version() + 1);
        });
}

void TableManager::update_split_lines(const pb::MetaManagerRequest& request,
                                      const int64_t apply_index,
                                      braft::Closure* done) {
    update_table_internal(request, apply_index, done, 
        [](const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb) {
                mem_schema_pb.set_region_split_lines(request.table_info().region_split_lines());
                mem_schema_pb.set_version(mem_schema_pb.version() + 1);
        });
}

void TableManager::update_schema_conf(const pb::MetaManagerRequest& request,
                                       const int64_t apply_index,
                                       braft::Closure* done) {
    update_table_internal(request, apply_index, done, 
    [](const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb) {
        const pb::SchemaConf& schema_conf = request.table_info().schema_conf();
        pb::SchemaConf* p_conf = mem_schema_pb.mutable_schema_conf();
        if (schema_conf.has_need_merge()) {
            p_conf->set_need_merge(schema_conf.need_merge());
        }
        if (schema_conf.has_storage_compute_separate()) {
            p_conf->set_storage_compute_separate(schema_conf.storage_compute_separate());
        }
        
        mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    });
}

void TableManager::update_resource_tag(const pb::MetaManagerRequest& request, 
                                       const int64_t apply_index, 
                                       braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    auto resource_tag = request.table_info().resource_tag();
    if (!ClusterManager::get_instance()->check_resource_tag_exist(resource_tag)) {
        DB_WARNING("check resource_tag exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "resource_tag not exist");
        return ;
    }
    mem_schema_pb.set_resource_tag(request.table_info().resource_tag());
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    set_table_pb(mem_schema_pb); 
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);   
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("update table internal success, request:%s", request.ShortDebugString().c_str());
}

void TableManager::update_dists(const pb::MetaManagerRequest& request,
                                const int64_t apply_index, 
                                braft::Closure* done) {
    update_table_internal(request, apply_index, done, 
        [](const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb) {
            mem_schema_pb.set_version(mem_schema_pb.version() + 1);
            mem_schema_pb.clear_dists();
            mem_schema_pb.clear_main_logical_room();
            for (auto& dist : request.table_info().dists()) {
                auto dist_ptr = mem_schema_pb.add_dists();
                *dist_ptr = dist;
            }
            if (request.table_info().has_main_logical_room()) {
                mem_schema_pb.set_main_logical_room(request.table_info().main_logical_room());
            }
            if (request.table_info().has_replica_num()) {
                mem_schema_pb.set_replica_num(request.table_info().replica_num());
            }
        });
}

void TableManager::add_field(const pb::MetaManagerRequest& request,
                             const int64_t apply_index,
                             braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    int32_t tmp_max_field_id = mem_schema_pb.max_field_id();
    std::unordered_map<std::string, int32_t> add_field_id_map;
    for (auto& field : request.table_info().fields()) {
        if (_table_info_map[table_id].field_id_map.count(field.field_name()) != 0) {
            DB_WARNING("field name:%s has already existed, request:%s",
                        field.field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field name already exist");
            return;
        }
        if (field.has_auto_increment() && field.auto_increment()) {
            DB_WARNING("not support auto increment, field name:%s, request:%s",
                        field.field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field can not be auto_increment");
            return;
        }
        pb::FieldInfo* add_field = mem_schema_pb.add_fields();
        *add_field = field;
        add_field->set_field_id(++tmp_max_field_id);
        add_field_id_map[field.field_name()] = tmp_max_field_id;
    }
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    mem_schema_pb.set_max_field_id(tmp_max_field_id);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    
    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);
    add_field_mem(table_id, add_field_id_map);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("add field success, request:%s", request.ShortDebugString().c_str());
}

void TableManager::drop_field(const pb::MetaManagerRequest& request,
                              const int64_t apply_index,
                              braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    std::vector<std::string> drop_field_names;
    for (auto& field : request.table_info().fields()) {
        if (_table_info_map[table_id].field_id_map.count(field.field_name()) == 0) {
            DB_WARNING("field name:%s not existed, request:%s",
                        field.field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field name not exist");
            return;
        }
        drop_field_names.push_back(field.field_name());
    }
    for (auto& field : *mem_schema_pb.mutable_fields()) {
        auto iter = std::find(drop_field_names.begin(),
                              drop_field_names.end(), 
                              field.field_name());
        if (iter != drop_field_names.end()) {
            field.set_deleted(true);
        }
    }
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }

    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);
    drop_field_mem(table_id, drop_field_names);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("drop field success, request:%s", request.ShortDebugString().c_str());
}

void TableManager::rename_field(const pb::MetaManagerRequest& request,
                                const int64_t apply_index, 
                                braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    std::unordered_map<int32_t, std::string> id_new_field_map;
    std::vector<std::string> drop_field_names;
    std::unordered_map<std::string, int32_t> add_field_id_map;
    for (auto& field : request.table_info().fields()) {
        if (_table_info_map[table_id].field_id_map.count(field.field_name()) == 0) {
            DB_WARNING("field name:%s not existed, request:%s",
                    field.field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field name not exist");
            return;
        }
        if (!field.has_new_field_name()) {
            DB_WARNING("request has no new field name, request:%s", request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "new field name is null");
            return;
        }
        if (_table_info_map[table_id].field_id_map.count(field.new_field_name()) != 0) {
            DB_WARNING("new field name:%s already existed, request:%s",
                        field.new_field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "new field name already exist");
            return;
        }
        int32_t field_id = 0;
        for (auto& mem_field : *mem_schema_pb.mutable_fields()) {
            if (mem_field.field_name() == field.field_name()) {
                mem_field.set_field_name(field.new_field_name());
                field_id = mem_field.field_id();
            }
        }
        id_new_field_map[field_id] = field.new_field_name();
        add_field_id_map[field.new_field_name()] = field_id;
        drop_field_names.push_back(field.field_name());
    }
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);
    drop_field_mem(table_id, drop_field_names);
    add_field_mem(table_id, add_field_id_map);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("rename field success, request:%s", request.ShortDebugString().c_str());
}

void TableManager::modify_field(const pb::MetaManagerRequest& request,
                                const int64_t apply_index, 
                                braft::Closure* done) {
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    std::vector<std::string> drop_field_names;
    for (auto& field : request.table_info().fields()) {
        std::string field_name = field.field_name();
        if (_table_info_map[table_id].field_id_map.count(field_name) == 0) {
            DB_WARNING("field name:%s not existed, request:%s",
                        field.field_name().c_str(), request.ShortDebugString().c_str());
            IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "field name not exist");
            return;
        }
        for (auto& mem_field : *mem_schema_pb.mutable_fields()) {
            if (mem_field.field_name() == field_name) {
                mem_field.set_mysql_type(field.mysql_type());
            }
        }
    }
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_NOTICE("modify field type success, request:%s", request.ShortDebugString().c_str());
}
void TableManager::process_schema_heartbeat_for_store(
        std::unordered_map<int64_t, int64_t>& store_table_id_version,
        pb::StoreHeartBeatResponse* response) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    for (auto& table_info_map : _table_info_map) {
        int64_t table_id = table_info_map.first;
        if (store_table_id_version.count(table_id) == 0
                || store_table_id_version[table_id]
                    < table_info_map.second.schema_pb.version()) {
            pb::SchemaInfo* new_table_info = response->add_schema_change_info();
            *new_table_info = table_info_map.second.schema_pb;
            DB_DEBUG("table_id[%lld] add schema info [%s] ", table_id, new_table_info->ShortDebugString().c_str())
            //DB_WARNING("add or update table_name:%s, table_id:%ld",
            //            new_table_info->table_name().c_str(), new_table_info->table_id());
        }
    }
    for (auto& store_table_id : store_table_id_version) {
        if (_table_info_map.find(store_table_id.first)  == _table_info_map.end()) {
            pb::SchemaInfo* new_table_info = response->add_schema_change_info();
            new_table_info->set_table_id(store_table_id.first);
            new_table_info->set_deleted(true);
            new_table_info->set_table_name("deleted");
            new_table_info->set_database("deleted");
            new_table_info->set_namespace_name("deleted");
            //DB_WARNING("delete table_info:%s, table_id: %ld",
            //        new_table_info->table_name().c_str(), new_table_info->table_id());
        } 
    }
}
void TableManager::check_update_or_drop_table(
        const pb::BaikalHeartBeatRequest* request,
        pb::BaikalHeartBeatResponse* response) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    for (auto& schema_heart_beat : request->schema_infos()) {
        int64_t table_id = schema_heart_beat.table_id();
        //表已经删除
        if (_table_info_map.find(table_id) == _table_info_map.end()) {
            auto schema_info = response->add_schema_change_info();
            schema_info->set_table_id(table_id);
            schema_info->set_deleted(true);
            schema_info->set_table_name("deleted");
            schema_info->set_database("deleted");
            schema_info->set_namespace_name("deleted");
            //相应的region也删除
            for (auto& region_heart_beat : schema_heart_beat.regions()) {
                auto region_info = response->add_region_change_info();
                region_info->set_region_id(region_heart_beat.region_id());
                region_info->set_deleted(true);
                region_info->set_table_id(table_id);
                region_info->set_table_name("deleted");
                region_info->set_partition_id(0);
                region_info->set_replica_num(0);
                region_info->set_version(0);
                region_info->set_conf_version(0);
            }   
            continue;
        }
        //全局二级索引没有schema信息
        if (_table_info_map[table_id].is_global_index) {
            continue;
        }
        //表更新
        if (_table_info_map[table_id].schema_pb.version() > schema_heart_beat.version()) {
            *(response->add_schema_change_info()) = _table_info_map[table_id].schema_pb;
        }
    }
}
void TableManager::check_add_table(std::set<int64_t>& report_table_ids, 
            std::vector<int64_t>& new_add_region_ids,
            pb::BaikalHeartBeatResponse* response) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    for (auto& table_info_pair : _table_info_map) {
        if (report_table_ids.find(table_info_pair.first) != report_table_ids.end()) {
            continue;
        }
        //如果是全局二级索引, 没有schema信息
        if (!table_info_pair.second.is_global_index) {
            auto schema_info = response->add_schema_change_info();
            *schema_info = table_info_pair.second.schema_pb;
        }
        for (auto& partition_region : table_info_pair.second.partition_regions) {
            for (auto& region_id : partition_region.second) {
                //DB_WARNING("new add region id: %ld", region_id);
                new_add_region_ids.push_back(region_id);
            }
        }
    }
}
void TableManager::check_add_region(const std::set<std::int64_t>& report_table_ids,
                std::unordered_map<int64_t, std::set<std::int64_t>>& report_region_ids,
                pb::BaikalHeartBeatResponse* response) {
    //获得每个表的regincount
    std::unordered_map<int64_t, int64_t> table_region_count;
    get_region_count(report_table_ids, table_region_count);
    
    std::vector<int64_t> table_for_add_region; //需要add_region的table_id
    for (auto& region_ids_pair : report_region_ids) {
        int64_t table_id = region_ids_pair.first;
        if (table_region_count[table_id] <= (int64_t)region_ids_pair.second.size()) {
            continue;
        }
        table_for_add_region.push_back(table_id);
    }

    std::unordered_map<int64_t, std::vector<int64_t>> region_ids;
    get_region_ids(table_for_add_region, region_ids);

    std::vector<int64_t> add_region_ids;
    std::vector<SmartRegionInfo> add_region_infos;
    for (auto& region_id_pair : region_ids) {
        int64_t table_id = region_id_pair.first;
        for (auto& region_id : region_id_pair.second) {
            if (report_region_ids[table_id].find(region_id) == report_region_ids[table_id].end()) {
                add_region_ids.push_back(region_id);
            }
        }
    }
    if (add_region_ids.size() > 0) {
        RegionManager::get_instance()->get_region_info(add_region_ids, add_region_infos);
        for (auto& ptr_region : add_region_infos) {
            *(response->add_region_change_info()) = *ptr_region;
        }
    }
}

int TableManager::load_table_snapshot(const std::string& value) {
    pb::SchemaInfo table_pb;
    if (!table_pb.ParseFromString(value)) {
        DB_FATAL("parse from pb fail when load table snapshot, key: %s", value.c_str());
        return -1;
    }
    DB_WARNING("table snapshot:%s", table_pb.ShortDebugString().c_str());
    TableMem table_mem;
    table_mem.schema_pb = table_pb;
    table_mem.whether_level_table = table_pb.has_upper_table_name();
    table_mem.main_table_id = table_pb.table_id();
    table_mem.global_index_id = table_pb.table_id();
    for (auto& field : table_pb.fields()) {
        if (!field.has_deleted() || !field.deleted()) {
            table_mem.field_id_map[field.field_name()] = field.field_id();
        }
    }
    for (auto& index : table_pb.indexs()) {
        table_mem.index_id_map[index.index_name()] = index.index_id();
    }
    set_table_info(table_mem); 
    DatabaseManager::get_instance()->add_table_id(table_pb.database_id(), table_pb.table_id());
    return 0;
}

int TableManager::write_schema_for_not_level(TableMem& table_mem, 
                                              braft::Closure* done, 
                                              int64_t max_table_id_tmp,
                                              bool has_auto_increment) {
    //如果创建成功，则不需要做任何操作
    //如果失败，则需要报错，手工调用删除table的接口
    std::vector<std::string> rocksdb_keys;
    std::vector<std::string> rocksdb_values;
   
    std::string max_table_id_value;
    max_table_id_value.append((char*)&max_table_id_tmp, sizeof(int64_t));
    rocksdb_keys.push_back(construct_max_table_id_key());
    rocksdb_values.push_back(max_table_id_value);

    //持久化region_info
    //与store交互
    //准备partition_num个数的regionInfo
    int64_t tmp_max_region_id = RegionManager::get_instance()->get_max_region_id();
    int64_t start_region_id = tmp_max_region_id + 1;
   
    std::shared_ptr<std::vector<pb::InitRegion>> init_regions(new std::vector<pb::InitRegion>{});
    init_regions->reserve(table_mem.schema_pb.init_store_size());
    int64_t instance_count = 0;
    pb::SchemaInfo simple_table_info = table_mem.schema_pb;
    int64_t main_table_id = simple_table_info.table_id();
    simple_table_info.clear_init_store();
    simple_table_info.clear_split_keys();
    //全局索引和主键索引需要建region
    std::unordered_map<std::string, int64_t> global_index;
    for (auto& index : table_mem.schema_pb.indexs()) {
        if (index.index_type() == pb::I_PRIMARY || index.is_global()) {
            DB_WARNING("index_name: %s is global", index.index_name().c_str());
            global_index[index.index_name()] = index.index_id();
        }
    }
    //有split_key的索引先处理
    for (auto i = 0; i < table_mem.schema_pb.partition_num() && 
            (table_mem.schema_pb.engine() == pb::ROCKSDB ||
            table_mem.schema_pb.engine() == pb::ROCKSDB_CSTORE); ++i) {
        for (auto& split_key : table_mem.schema_pb.split_keys()) {
            std::string index_name = split_key.index_name();
            for (auto j = 0; j <= split_key.split_keys_size(); ++j, ++instance_count) {
                pb::InitRegion init_region_request;
                pb::RegionInfo* region_info = init_region_request.mutable_region_info();
                region_info->set_region_id(++tmp_max_region_id);
                region_info->set_table_id(global_index[index_name]);
                region_info->set_main_table_id(main_table_id);
                region_info->set_table_name(table_mem.schema_pb.table_name());
                construct_common_region(region_info, table_mem.schema_pb.replica_num());
                region_info->set_partition_id(i);
                region_info->add_peers(table_mem.schema_pb.init_store(instance_count));
                region_info->set_leader(table_mem.schema_pb.init_store(instance_count));
                if (j != 0) {
                    region_info->set_start_key(split_key.split_keys(j-1));        
                }
                if (j < split_key.split_keys_size()) {
                    region_info->set_end_key(split_key.split_keys(j));
                }
                *(init_region_request.mutable_schema_info()) = simple_table_info;
                init_region_request.set_snapshot_times(2);
                init_regions->push_back(init_region_request);
            }
            global_index.erase(index_name);
        }
    }
    //没有指定split_key的索引
    for (auto i = 0; i < table_mem.schema_pb.partition_num() &&
            (table_mem.schema_pb.engine() == pb::ROCKSDB ||
            table_mem.schema_pb.engine() == pb::ROCKSDB_CSTORE); ++i) {
        for (auto& index : global_index) {
            pb::InitRegion init_region_request;
            pb::RegionInfo* region_info = init_region_request.mutable_region_info();
            region_info->set_region_id(++tmp_max_region_id);
            region_info->set_table_id(index.second);
            region_info->set_main_table_id(main_table_id);
            region_info->set_table_name(table_mem.schema_pb.table_name());
            construct_common_region(region_info, table_mem.schema_pb.replica_num());
            region_info->set_partition_id(i);
            region_info->add_peers(table_mem.schema_pb.init_store(instance_count));
            region_info->set_leader(table_mem.schema_pb.init_store(instance_count));
            *(init_region_request.mutable_schema_info()) = simple_table_info;
            init_region_request.set_snapshot_times(2);
            init_regions->push_back(init_region_request);
            DB_WARNING("init_region_request: %s", init_region_request.DebugString().c_str());
            ++instance_count;
        }
    }
    //持久化region_id
    std::string max_region_id_key = RegionManager::get_instance()->construct_max_region_id_key();
    std::string max_region_id_value;
    max_region_id_value.append((char*)&tmp_max_region_id, sizeof(int64_t));
    rocksdb_keys.push_back(max_region_id_key);
    rocksdb_values.push_back(max_region_id_value);

    //持久化schema_info
    int64_t table_id = table_mem.schema_pb.table_id();
    std::string table_value;
    if (!simple_table_info.SerializeToString(&table_value)) {
        DB_WARNING("request serializeToArray fail when create not level table, request:%s",
                    simple_table_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
        return -1;
    }
    rocksdb_keys.push_back(construct_table_key(table_id));
    rocksdb_values.push_back(table_value);
    
    int ret = MetaRocksdb::get_instance()->put_meta_info(rocksdb_keys, rocksdb_values);
    if (ret < 0) {                                                                        
        DB_WARNING("add new not level table:%s to rocksdb fail",
                        simple_table_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return -1;
    }
    RegionManager::get_instance()->set_max_region_id(tmp_max_region_id);
    uint64_t init_value = 1;
    if (table_mem.schema_pb.has_auto_increment_increment()) {
        init_value = table_mem.schema_pb.auto_increment_increment();
    }
    
    //leader发送请求
    if (done && (table_mem.schema_pb.engine() == pb::ROCKSDB
        || table_mem.schema_pb.engine() == pb::ROCKSDB_CSTORE)) {
        std::string namespace_name = table_mem.schema_pb.namespace_name();
        std::string database = table_mem.schema_pb.database();
        std::string table_name = table_mem.schema_pb.table_name();
        Bthread bth(&BTHREAD_ATTR_SMALL);
        auto create_table_fun = 
            [this, namespace_name, database, table_name, init_regions, 
                table_id, init_value, has_auto_increment]() {
                int ret = 0;
                if (has_auto_increment) {
                    pb::MetaManagerRequest request;
                    request.set_op_type(pb::OP_ADD_ID_FOR_AUTO_INCREMENT);
                    pb::AutoIncrementRequest* auto_incr = request.mutable_auto_increment();
                    auto_incr->set_table_id(table_id);
                    auto_incr->set_start_id(init_value);
                    ret = send_auto_increment_request(request);
                }
                if (ret == 0) {
                    send_create_table_request(namespace_name, database, table_name, init_regions);
                } else {
                    send_drop_table_request(namespace_name, database, table_name);
                    DB_FATAL("send add auto incrment request fail, table_name: %s", table_name.c_str());
                }
            };
        bth.run(create_table_fun);
    }
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    DB_WARNING("create table, table_id:%ld, table_name:%s, max_table_id: %ld"
                " alloc start_region_id:%ld, end_region_id :%ld", 
                table_mem.schema_pb.table_id(), table_mem.schema_pb.table_name().c_str(), 
                max_table_id_tmp,
                start_region_id, 
                RegionManager::get_instance()->get_max_region_id());
    return 0;
}
int TableManager::send_auto_increment_request(const pb::MetaManagerRequest& request) {
    MetaServerInteract meta_server_interact;
    if (meta_server_interact.init() != 0) {
        DB_FATAL("meta server interact init fail when send auto increment %s", 
                    request.ShortDebugString().c_str());
        return -1;
    }
    pb::MetaManagerResponse response;
    if (meta_server_interact.send_request("meta_manager", request, response) != 0) {
        DB_WARNING("send_auto_increment_request fail, response:%s", 
                    response.ShortDebugString().c_str());
        return -1;
    }
    return 0;
}
void TableManager::send_create_table_request(const std::string& namespace_name,
                               const std::string& database,
                               const std::string& table_name,
                               std::shared_ptr<std::vector<pb::InitRegion>> init_regions) {
    uint64_t log_id = butil::fast_rand();
    //40个线程并发发送
    BthreadCond concurrency_cond(-FLAGS_concurrency_num);
    bool success = true;
    std::string full_table_name = namespace_name + "." + database + "." + table_name;
    for (auto& init_region_request : *init_regions) {
        auto send_init_region = [&init_region_request, &success, &concurrency_cond, log_id, full_table_name] () {
            std::shared_ptr<BthreadCond> auto_decrease(&concurrency_cond, 
                                [](BthreadCond* cond) { cond->decrease_signal();});
            int64_t region_id = init_region_request.region_info().region_id();
            StoreInteract store_interact(init_region_request.region_info().leader().c_str());
            pb::StoreRes res;
            auto ret = store_interact.send_request(log_id, "init_region", init_region_request, res);
            if (ret < 0) {
                DB_FATAL("create table fail, address:%s, region_id: %ld", 
                            init_region_request.region_info().leader().c_str(),
                            region_id);
                success = false;
                return;
            }
            DB_NOTICE("new region_id: %ld success, table_name:%s", region_id, full_table_name.c_str());
        };
        if (!success) {
            break;
        }  
        Bthread bth;
        concurrency_cond.increase();
        concurrency_cond.wait();
        bth.run(send_init_region);
    }
    concurrency_cond.wait(-FLAGS_concurrency_num);
    if (!success) {
        DB_FATAL("create table:%s fail",
                    (namespace_name + "." + database + "." + table_name).c_str());
        send_drop_table_request(namespace_name, database, table_name);
    } else {
        DB_NOTICE("create table:%s success", 
                    (namespace_name + "." + database + "." + table_name).c_str());
    }
}

int TableManager::write_schema_for_level(const TableMem& table_mem, 
                                          const int64_t apply_index,
                                          braft::Closure* done, 
                                          int64_t max_table_id_tmp,
                                          bool has_auto_increment) {
    if (done && has_auto_increment) {
        int64_t table_id = table_mem.schema_pb.table_id(); 
        uint64_t init_value = 1;
        if (table_mem.schema_pb.has_auto_increment_increment()) {
            init_value = table_mem.schema_pb.auto_increment_increment();
        }
        pb::MetaManagerRequest request;
        request.set_op_type(pb::OP_ADD_ID_FOR_AUTO_INCREMENT);
        pb::AutoIncrementRequest* auto_incr = request.mutable_auto_increment();
        auto_incr->set_table_id(table_id);
        auto_incr->set_start_id(init_value);
        auto ret = send_auto_increment_request(request);
        if (ret < 0) {
            DB_FATAL("send add auto incrment request fail, table_id: %ld", table_id);
            return -1;
        }
    }
    std::vector<std::string> rocksdb_keys;
    std::vector<std::string> rocksdb_values;

    //持久化表信息
    std::string table_value;
    if (!table_mem.schema_pb.SerializeToString(&table_value)) {
        DB_WARNING("request serializeToArray fail when create table, request:%s",
                        table_mem.schema_pb.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
        return -1;
    }
    rocksdb_keys.push_back(construct_table_key(table_mem.schema_pb.table_id()));
    rocksdb_values.push_back(table_value);

    //持久化最大table_id
    std::string max_table_id_value;
    max_table_id_value.append((char*)&max_table_id_tmp, sizeof(int64_t));
    rocksdb_keys.push_back(construct_max_table_id_key());
    rocksdb_values.push_back(max_table_id_value);
    
    //更新最顶层表信息
    int64_t top_table_id = table_mem.schema_pb.top_table_id();
    std::string top_table_value;
    pb::SchemaInfo top_table = _table_info_map[top_table_id].schema_pb;
    top_table.add_lower_table_ids(table_mem.schema_pb.table_id());
    top_table.set_version(table_mem.schema_pb.version() + 1);
    if (!top_table.SerializeToString(&top_table_value)) { 
         DB_WARNING("request serializeToArray fail when update upper table, request:%s", 
                     top_table.ShortDebugString().c_str());
         IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
         return -1;
    }
    rocksdb_keys.push_back(construct_table_key(top_table_id));
    rocksdb_values.push_back(top_table_value);
    
    // write date to rocksdb
    int ret = MetaRocksdb::get_instance()->put_meta_info(rocksdb_keys, rocksdb_values);
    if (ret < 0) {                                                                        
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");                  
        return -1;                                                             
    }
    
    //更新顶层表的内存信息
    set_table_pb(top_table);
    std::vector<pb::SchemaInfo> schema_infos{top_table};
    put_incremental_schemainfo(apply_index, schema_infos);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    return 0;
}


int TableManager::update_schema_for_rocksdb(int64_t table_id,
                                               const pb::SchemaInfo& schema_info,
                                               braft::Closure* done) {
    
    std::string table_value;
    if (!schema_info.SerializeToString(&table_value)) {
        DB_WARNING("request serializeToArray fail when update upper table, request:%s", 
                    schema_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
        return -1;
    }
    int ret = MetaRocksdb::get_instance()->put_meta_info(construct_table_key(table_id), table_value);    
    if (ret < 0) {
        DB_WARNING("update schema info to rocksdb fail, request：%s", 
                    schema_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return -1;
    }    
    return 0;
}

void TableManager::send_drop_table_request(const std::string& namespace_name,
                             const std::string& database,
                             const std::string& table_name) {
    MetaServerInteract meta_server_interact;
    if (meta_server_interact.init() != 0) {
        DB_FATAL("meta server interact init fail when drop table:%s", table_name.c_str());
        return;
    }
    pb::MetaManagerRequest request;
    request.set_op_type(pb::OP_DROP_TABLE);
    pb::SchemaInfo* table_info = request.mutable_table_info();
    table_info->set_table_name(table_name);
    table_info->set_namespace_name(namespace_name);
    table_info->set_database(database);
    pb::MetaManagerResponse response;
    if (meta_server_interact.send_request("meta_manager", request, response) != 0) {
        DB_WARNING("drop table fail, response:%s", response.ShortDebugString().c_str());
        return;
    }
    DB_WARNING("drop table success, namespace:%s, database:%s, table_name:%s",
                namespace_name.c_str(), database.c_str(), table_name.c_str());
}
void TableManager::check_table_exist_for_peer(const pb::StoreHeartBeatRequest* request,
            pb::StoreHeartBeatResponse* response) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    for (auto& peer_info : request->peer_infos()) {
        if (_table_info_map.find(peer_info.table_id()) != _table_info_map.end()) {
            continue;
        }
        DB_WARNING("table id:%ld according to region_id:%ld not exit, drop region_id, store_address:%s",
                peer_info.table_id(), peer_info.region_id(),
                request->instance_info().address().c_str());
        //为了安全暂时关掉这个删除region的功能，后续稳定再打开，目前先报fatal(todo)
        if (SchemaManager::get_instance()->get_unsafe_decision()) {
            DB_WARNING("store response add delete region according to table id no exist, region_id: %ld", peer_info.region_id());
            response->add_delete_region_ids(peer_info.region_id());
        }
    }
}
int TableManager::check_table_exist(const pb::SchemaInfo& schema_info, 
                                      int64_t& namespace_id,
                                      int64_t& database_id,
                                      int64_t& table_id) { 
    std::string namespace_name = schema_info.namespace_name();
    std::string database_name = namespace_name + "\001" + schema_info.database();
    std::string table_name = database_name + "\001" + schema_info.table_name();
    namespace_id = NamespaceManager::get_instance()->get_namespace_id(namespace_name);
    if (namespace_id == 0) {
        DB_WARNING("namespace not exit, table_name:%s", table_name.c_str());
        return -1;
    }
    database_id = DatabaseManager::get_instance()->get_database_id(database_name);
    if (database_id == 0) {
        DB_WARNING("database not exit, table_name:%s", table_name.c_str());
        return -1;
    }
    table_id = get_table_id(table_name);
    if (table_id == 0) {
        DB_WARNING("table not exit, table_name:%s", table_name.c_str());
        return -1;
    }
    return 0;
}

int TableManager::alloc_field_id(pb::SchemaInfo& table_info, bool& has_auto_increment, TableMem& table_mem) {
    int32_t field_id = 0;
    std::string table_name = table_info.table_name();
    for (auto i = 0; i < table_info.fields_size(); ++i) {
        table_info.mutable_fields(i)->set_field_id(++field_id);
        const std::string& field_name = table_info.fields(i).field_name();
        if (table_mem.field_id_map.count(field_name) == 0) {
            table_mem.field_id_map[field_name] = field_id;
        } else {
            DB_WARNING("table:%s has duplicate field %s", table_name.c_str(), field_name.c_str());
            return -1;
        }
        if (!table_info.fields(i).has_auto_increment() 
                || table_info.fields(i).auto_increment() == false) {
            continue;
        }
        //一个表只能有一个自增列
        if (has_auto_increment == true) {
            DB_WARNING("table:%s has one more auto_increment field, field %s", 
                        table_name.c_str(), field_name.c_str());
            return -1;
        }
        pb::PrimitiveType data_type = table_info.fields(i).mysql_type();
        if (data_type != pb::INT8
                && data_type != pb::INT16
                && data_type != pb::INT32
                && data_type != pb::INT64
                && data_type != pb::UINT8
                && data_type != pb::UINT16
                && data_type != pb::UINT32
                && data_type != pb::UINT64) {
            DB_WARNING("table:%s auto_increment field not interger, field %s", 
                        table_name.c_str(), field_name.c_str());
            return -1;
        }
        if (table_info.fields(i).can_null()) {
            DB_WARNING("table:%s auto_increment field can not null, field %s", 
                        table_name.c_str(), field_name.c_str());
            return -1;
        }
        has_auto_increment = true;
    }
    table_info.set_max_field_id(field_id);
    return 0;
}

int TableManager::alloc_index_id(pb::SchemaInfo& table_info, TableMem& table_mem, int64_t& max_table_id_tmp) {
    bool has_primary_key = false;
    std::string table_name = table_info.table_name();
    //分配index_id， 序列与table_id共享, 必须有primary_key 
    for (auto i = 0; i < table_info.indexs_size(); ++i) {
        std::string index_name = table_info.indexs(i).index_name();
        for (auto j = 0; j < table_info.indexs(i).field_names_size(); ++j) {
            std::string field_name = table_info.indexs(i).field_names(j);
            if (table_mem.field_id_map.find(field_name) == table_mem.field_id_map.end()) {
                DB_WARNING("filed name:%s of index was not exist in table:%s", 
                            field_name.c_str(),
                            table_name.c_str());
                return -1;
            }
            int32_t field_id = table_mem.field_id_map[field_name];
            table_info.mutable_indexs(i)->add_field_ids(field_id);
        }
        if (table_info.indexs(i).index_type() == pb::I_NONE) {
            DB_WARNING("invalid index type: %d", table_info.indexs(i).index_type());
            return -1;
        }

        table_info.mutable_indexs(i)->set_state(pb::IS_PUBLIC);

        if (table_info.indexs(i).index_type() != pb::I_PRIMARY) {
            table_info.mutable_indexs(i)->set_index_id(++max_table_id_tmp);
            table_mem.index_id_map[index_name] = max_table_id_tmp;
            continue;
        } 
        //只能有一个primary key
        if (has_primary_key) {
            DB_WARNING("table:%s has one more primary key", table_name.c_str());
            return -1;
        }
        has_primary_key = true;
        table_info.mutable_indexs(i)->set_index_id(table_info.table_id());
        //有partition的表的主键不能是联合主键
        if (!table_mem.whether_level_table && table_info.partition_num() != 1) {
            if (table_info.indexs(i).field_names_size() > 1) {
                DB_WARNING("table:%s has partition_num, but not meet our rule", table_name.c_str());
                return -1;
            }
            //而且，带partiton_num的表主键必须是设置了auto_increment属性
            std::string primary_field = table_info.indexs(i).field_names(0);
            for (auto i = 0; i < table_info.fields_size(); ++i) {
                if (table_info.fields(i).field_name() == primary_field
                        && table_info.fields(i).auto_increment() == false) {
                        DB_WARNING("table:%s not auto increment", table_name.c_str());
                        return -1;
                }
            }
        }
        table_mem.index_id_map[index_name] = table_info.table_id();
    }
    if (!has_primary_key) {
        return -1;
    }
    return 0;
}

int64_t TableManager::get_pre_regionid(int64_t table_id, 
                                            const std::string& start_key) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return -1;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    if (startkey_regiondesc_map.size() <= 0) {
        DB_FATAL("table_id:%ld map empty", table_id);
        return -1;
    }
    auto iter = startkey_regiondesc_map.lower_bound(start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find region id start_key:%s",
                 table_id, str_to_hex(start_key).c_str()); 
    } else if (iter->first == start_key) {
        DB_FATAL("table_id:%ld start_key:%s exist", table_id, str_to_hex(start_key).c_str());
        return -1;
    }
    
    if (iter == startkey_regiondesc_map.begin()) {
        DB_WARNING("iter is the first");
        return -1;
    }
    --iter;
    DB_WARNING("table_id:%ld start_key:%s region_id:%ld", 
               table_id, str_to_hex(start_key).c_str(), iter->second.region_id);
    return iter->second.region_id;
}

int64_t TableManager::get_startkey_regionid(int64_t table_id, 
                                       const std::string& start_key) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return -1;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    if (startkey_regiondesc_map.size() <= 0) {
        DB_FATAL("table_id:%ld map empty", table_id);
        return -1;
    }
    auto iter = startkey_regiondesc_map.find(start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find region id start_key:%s",
                 table_id, str_to_hex(start_key).c_str()); 
        return -1;
    } 
    DB_WARNING("table_id:%ld start_key:%s region_id:%ld", 
               table_id, str_to_hex(start_key).c_str(), iter->second.region_id);    
    return iter->second.region_id;
}

int TableManager::erase_region(int64_t table_id, int64_t region_id, std::string start_key) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return -1;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    auto iter = startkey_regiondesc_map.find(start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find region id start_key:%s",
                 table_id, str_to_hex(start_key).c_str()); 
        return -1;
    }
    if (iter->second.region_id != region_id) {
        DB_FATAL("table_id:%ld diff region_id(%ld, %ld)",
                 table_id, iter->second.region_id, region_id); 
        return -1;
    }
    startkey_regiondesc_map.erase(start_key);
    DB_WARNING("table_id:%ld erase region_id:%ld",
               table_id, region_id);
}

int64_t TableManager::get_next_region_id(int64_t table_id, std::string start_key, 
                                        std::string end_key) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return -1;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    auto iter = startkey_regiondesc_map.find(start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find region id start_key:%s",
                 table_id, str_to_hex(start_key).c_str()); 
        return -1;
    }
    auto src_iter = iter;
    auto dst_iter = ++iter;
    if (dst_iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find region id start_key:%s",
                 table_id, str_to_hex(end_key).c_str()); 
        return -1;
    }
    if (dst_iter->first != end_key) {
        DB_FATAL("table_id:%ld start key nonsequence %s vs %s", table_id, 
                 str_to_hex(dst_iter->first).c_str(), str_to_hex(end_key).c_str()); 
        return -1;
    }
    if (src_iter->second.merge_status == MERGE_IDLE
            && dst_iter->second.merge_status == MERGE_IDLE) {
        src_iter->second.merge_status = MERGE_SRC;
        dst_iter->second.merge_status = MERGE_DST;
        DB_WARNING("table_id:%ld merge src region_id:%ld, dst region_id:%ld",
                  table_id, src_iter->second.region_id, dst_iter->second.region_id);
        return dst_iter->second.region_id;
    } else if (src_iter->second.merge_status == MERGE_SRC
            && dst_iter->second.merge_status == MERGE_DST) {
        DB_WARNING("table_id:%ld merge again src region_id:%ld, dst region_id:%ld",
                   table_id, src_iter->second.region_id, dst_iter->second.region_id);
        return dst_iter->second.region_id;
    } else {
        DB_WARNING("table_id:%ld merge get next region fail, src region_id:%ld, "
                   "merge_status:%d; dst region_id:%ld, merge_status:%d",
                   table_id, src_iter->second.region_id, src_iter->second.merge_status,
                   dst_iter->second.region_id, dst_iter->second.merge_status);
        return -1;
    }
}
int TableManager::check_startkey_regionid_map() {
    TimeCost time_cost; 
    BAIDU_SCOPED_LOCK(_table_mutex);
    for (auto table_info : _table_info_map) {
        int64_t table_id = table_info.first;
        SmartRegionInfo pre_region;
        bool is_first_region = true;
        auto& startkey_regiondesc_map = table_info.second.startkey_regiondesc_map;
        for (auto iter = startkey_regiondesc_map.begin(); iter != startkey_regiondesc_map.end(); iter++) {
            if (is_first_region == true) {
                //首个region
                auto first_region = RegionManager::get_instance()->
                                    get_region_info(iter->second.region_id);
                if (first_region == nullptr) {
                    DB_FATAL("table_id:%ld, can`t find region_id:%ld start_key:%s, in region info map", 
                             table_id, iter->second.region_id, str_to_hex(iter->first).c_str());
                    continue;
                }
                DB_WARNING("table_id:%ld, first region_id:%ld, version:%d, key(%s, %s)",
                           table_id, first_region->region_id(), first_region->version(), 
                           str_to_hex(first_region->start_key()).c_str(), 
                           str_to_hex(first_region->end_key()).c_str());
                pre_region = first_region;
                is_first_region = false;
                continue;
            }
            auto cur_region = RegionManager::get_instance()->
                                 get_region_info(iter->second.region_id); 
            if (cur_region == nullptr) {
                DB_FATAL("table_id:%ld, can`t find region_id:%ld start_key:%s, in region info map", 
                         table_id, iter->second.region_id, str_to_hex(iter->first).c_str());
                is_first_region = true;
                continue;
            }
            if (pre_region->end_key() != cur_region->start_key()) {
                DB_FATAL("table_id:%ld, key nonsequence (region_id, version, "
                         "start_key, end_key) pre vs cur (%ld, %ld, %s, %s) vs "
                         "(%ld, %ld, %s, %s)", table_id, 
                         pre_region->region_id(), pre_region->version(), 
                         str_to_hex(pre_region->start_key()).c_str(), 
                         str_to_hex(pre_region->end_key()).c_str(), 
                         cur_region->region_id(), cur_region->version(), 
                         str_to_hex(cur_region->start_key()).c_str(), 
                         str_to_hex(cur_region->end_key()).c_str());
                is_first_region = true;
                continue;
            }
            pre_region = cur_region;
        }
    }
    DB_WARNING("check finish timecost:%ld", time_cost.get_time());
    return 0;
}
int TableManager::add_startkey_regionid_map(const pb::RegionInfo& region_info) {
    int64_t table_id = region_info.table_id();
    int64_t region_id = region_info.region_id();
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_WARNING("table_id: %ld not exist", table_id);
        return -1;
    }
    if (region_info.start_key() == region_info.end_key() 
            && !region_info.start_key().empty()) {
        DB_WARNING("table_id: %ld, region_id: %ld, start_key: %s is empty",
                   table_id, region_id, str_to_hex(region_info.start_key()).c_str());
        return 0;
    }
    RegionDesc region;
    region.region_id = region_id;
    region.merge_status = MERGE_IDLE;
    std::map<std::string, RegionDesc>& key_region_map
        = _table_info_map[table_id].startkey_regiondesc_map;
    if (key_region_map.find(region_info.start_key()) == key_region_map.end()) {
        key_region_map[region_info.start_key()] = region;
    } else {
        int64_t origin_region_id = key_region_map[region_info.start_key()].region_id;
        RegionManager* region_manager = RegionManager::get_instance();
        auto origin_region = region_manager->get_region_info(origin_region_id);
        DB_FATAL("table_id:%ld two regions has same start key (%ld, %s, %s) vs (%ld, %s, %s)",
                 table_id, origin_region->region_id(), 
                 str_to_hex(origin_region->start_key()).c_str(),
                 str_to_hex(origin_region->end_key()).c_str(), 
                 region_id, 
                 str_to_hex(region_info.start_key()).c_str(),
                 str_to_hex(region_info.end_key()).c_str());
        return 0;
    }
    return 0;
}
bool TableManager::check_region_when_update(int64_t table_id, 
                                    std::string min_start_key, 
                                    std::string max_end_key) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return false;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    if (startkey_regiondesc_map.size() == 0) {
        //首个region
        DB_WARNING("table_id:%ld min_start_key:%s, max_end_key:%s", table_id,
                  str_to_hex(min_start_key).c_str(), str_to_hex(max_end_key).c_str());
        return true;
    }
    auto iter = startkey_regiondesc_map.find(min_start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find min_start_key:%s", 
                 table_id, str_to_hex(min_start_key).c_str());
        return false;
    }
    if (!max_end_key.empty()) {
        auto endkey_iter = startkey_regiondesc_map.find(max_end_key);
        if (endkey_iter == startkey_regiondesc_map.end()) {
            DB_FATAL("table_id:%ld can`t find max_end_key:%s", 
                     table_id, str_to_hex(max_end_key).c_str());
            return false;
        }
    }
    return true;
}
void TableManager::update_startkey_regionid_map_old_pb(int64_t table_id, 
                          std::map<std::string, int64_t>& key_id_map) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    for (auto& key_id : key_id_map) {
        RegionDesc region;
        region.region_id = key_id.second;
        region.merge_status = MERGE_IDLE;
        startkey_regiondesc_map[key_id.first] = region;
        DB_WARNING("table_id:%ld, startkey:%s region_id:%ld insert", 
                   table_id, str_to_hex(key_id.first).c_str(), key_id.second);
    }
}

void TableManager::update_startkey_regionid_map(int64_t table_id, std::string min_start_key, 
                                  std::string max_end_key, 
                                  std::map<std::string, int64_t>& key_id_map) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_FATAL("table_id: %ld not exist", table_id);
        return;
    }
    auto& startkey_regiondesc_map = _table_info_map[table_id].startkey_regiondesc_map;
    if (startkey_regiondesc_map.size() == 0) {
        //首个region加入
        for (auto& key_id : key_id_map) {
            RegionDesc region;
            region.region_id = key_id.second;
            region.merge_status = MERGE_IDLE;
            startkey_regiondesc_map[key_id.first] = region;
            DB_WARNING("table_id:%ld, startkey:%s region_id:%ld insert", 
                       table_id, str_to_hex(key_id.first).c_str(), key_id.second);
        }
        return;
    }
    auto iter = startkey_regiondesc_map.find(min_start_key);
    if (iter == startkey_regiondesc_map.end()) {
        DB_FATAL("table_id:%ld can`t find start_key:%s", 
                 table_id, str_to_hex(min_start_key).c_str());
        return;
    }
    while (iter != startkey_regiondesc_map.end()) {
        if (!max_end_key.empty() && iter->first == max_end_key) {
            break;
        }
        auto delete_iter = iter++;
        DB_WARNING("table_id:%ld startkey:%s regiong_id:%ld merge_status:%d, erase",
                   table_id, str_to_hex(delete_iter->first).c_str(), 
                   delete_iter->second.region_id, delete_iter->second.merge_status);
        startkey_regiondesc_map.erase(delete_iter->first);
    }
    for (auto& key_id : key_id_map) {
        RegionDesc region;
        region.region_id = key_id.second;
        region.merge_status = MERGE_IDLE;
        startkey_regiondesc_map[key_id.first] = region;
        DB_WARNING("table_id:%ld, startkey:%s region_id:%ld insert", 
                   table_id, str_to_hex(key_id.first).c_str(), key_id.second);
    }
}
void TableManager::add_new_region(const pb::RegionInfo& leader_region_info) {
    int64_t table_id  = leader_region_info.table_id();
    int64_t region_id = leader_region_info.region_id();
    std::string start_key = leader_region_info.start_key();
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_WARNING("table_id: %ld not exist", table_id);
        return;
    }
    auto& key_region_map = _table_info_map[table_id].startkey_newregion_map;
    auto iter = key_region_map.find(start_key);
    if (iter != key_region_map.end()) {
        auto origin_region_info = iter->second;
        if (region_id != origin_region_info->region_id()) {
            DB_FATAL("two diffrent regions:%ld, %ld has same start_key:%s",
                     region_id, origin_region_info->region_id(),
                     str_to_hex(start_key).c_str());
            return;
        }
        if (leader_region_info.log_index() < origin_region_info->log_index()) {
            DB_WARNING("leader: %s log_index:%ld in heart is less than in "
                       "origin:%ld, region_id:%ld",
                       leader_region_info.leader().c_str(), 
                       leader_region_info.log_index(), 
                       origin_region_info->log_index(),
                       region_id);
            return;
        }
        if (leader_region_info.version() > origin_region_info->version()) {
            if (end_key_compare(leader_region_info.end_key(), origin_region_info->end_key()) > 0) {
                //end_key不可能变大
                DB_FATAL("region_id:%ld, version %ld to %ld, end_key %s to %s",
                         region_id, origin_region_info->version(),
                         leader_region_info.version(), 
                         str_to_hex(origin_region_info->end_key()).c_str(), 
                         str_to_hex(leader_region_info.end_key()).c_str());
                return;
            }
            key_region_map.erase(iter);
            auto ptr_region = std::make_shared<pb::RegionInfo>(leader_region_info);
            key_region_map[start_key] = ptr_region;
            DB_WARNING("region_id:%ld has changed (version, start_key, end_key)"
                       "(%ld, %s, %s) to (%ld, %s, %s)", region_id, 
                       origin_region_info->version(), 
                       str_to_hex(origin_region_info->start_key()).c_str(),
                       str_to_hex(origin_region_info->end_key()).c_str(),
                       leader_region_info.version(), 
                       str_to_hex(leader_region_info.start_key()).c_str(),
                       str_to_hex(leader_region_info.end_key()).c_str());
        }
    } else {
        auto ptr_region = std::make_shared<pb::RegionInfo>(leader_region_info);
        key_region_map[start_key] = ptr_region;
        DB_WARNING("table_id:%ld add new region_id:%ld, key:(%s, %s) version:%ld", 
                  table_id, region_id, str_to_hex(start_key).c_str(), 
                  str_to_hex(leader_region_info.end_key()).c_str(),
                  leader_region_info.version());
    }        
}
void TableManager::add_update_region(const pb::RegionInfo& leader_region_info, bool is_none) {
    int64_t table_id  = leader_region_info.table_id();
    int64_t region_id = leader_region_info.region_id();
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_WARNING("table_id:%ld not exist", table_id);
        return;
    }
    std::map<int64_t, SmartRegionInfo> *id_region_map;
    if (is_none) {
        id_region_map = &_table_info_map[table_id].id_noneregion_map;
    } else {
        id_region_map = &_table_info_map[table_id].id_keyregion_map;
    }
    auto iter = id_region_map->find(region_id);
    if (iter != id_region_map->end()) {
        auto origin_region_info = iter->second;
        if (leader_region_info.log_index() < origin_region_info->log_index()) {
            DB_WARNING("leader: %s log_index:%ld in heart is less than in "
                       "origin:%ld, region_id:%ld",
                       leader_region_info.leader().c_str(), 
                       leader_region_info.log_index(), 
                       origin_region_info->log_index(),
                       region_id);
            return;
        }
        if (leader_region_info.version() > origin_region_info->version()) {
            id_region_map->erase(iter);
            auto ptr_region = std::make_shared<pb::RegionInfo>(leader_region_info);
            id_region_map->insert(std::make_pair(region_id, ptr_region));
            DB_WARNING("table_id:%ld, region_id:%ld has changed (version, start_key, end_key)"
                       "(%ld, %s, %s) to (%ld, %s, %s)", table_id, region_id, 
                       origin_region_info->version(), 
                       str_to_hex(origin_region_info->start_key()).c_str(),
                       str_to_hex(origin_region_info->end_key()).c_str(),
                       leader_region_info.version(), 
                       str_to_hex(leader_region_info.start_key()).c_str(),
                       str_to_hex(leader_region_info.end_key()).c_str());
        }
    } else {
        auto ptr_region = std::make_shared<pb::RegionInfo>(leader_region_info);
        id_region_map->insert(std::make_pair(region_id, ptr_region));
        DB_WARNING("table_id:%ld, region_id:%ld (version, start_key, end_key)"
                   "(%ld, %s, %s)", table_id, region_id, 
                   leader_region_info.version(), 
                   str_to_hex(leader_region_info.start_key()).c_str(),
                   str_to_hex(leader_region_info.end_key()).c_str());
    }        
}
int TableManager::get_merge_regions(int64_t table_id, 
                      std::string new_start_key, std::string origin_start_key, 
                      std::map<std::string, RegionDesc>& startkey_regiondesc_map,
                      std::map<int64_t, SmartRegionInfo>& id_noneregion_map,
                      std::vector<SmartRegionInfo>& regions) {
    if (new_start_key == origin_start_key) {
        return 0;
    }
    if (new_start_key > origin_start_key) {
        return -1;
    }
    for (auto region_iter = startkey_regiondesc_map.find(new_start_key); 
            region_iter != startkey_regiondesc_map.end(); region_iter++) {
        if (region_iter->first > origin_start_key) {
            DB_WARNING("table_id:%ld region_id:%ld start_key:%s bigger than end_key:%s",
                    table_id, region_iter->second.region_id, str_to_hex(region_iter->first).c_str(), 
                    str_to_hex(origin_start_key).c_str());
            return -1;
        }
        if (region_iter->first == origin_start_key) {
            return 0;
        }
        int64_t region_id = region_iter->second.region_id;
        auto iter = id_noneregion_map.find(region_id);
        if (iter != id_noneregion_map.end()) {
            regions.push_back(iter->second);
            DB_WARNING("table_id:%ld, find region_id:%ld in id_noneregion_map"
                       "start_key:%s", table_id, region_id,
                       str_to_hex(region_iter->first).c_str());
        } else {
            DB_WARNING("table_id:%ld, can`t find region_id:%ld in id_noneregion_map",
                       table_id, region_id);
            return -1;
        }
    }
    return -1;
}
int TableManager::get_split_regions(int64_t table_id, 
                      std::string new_end_key, std::string origin_end_key, 
                      std::map<std::string, SmartRegionInfo>& key_newregion_map,
                      std::vector<SmartRegionInfo>& regions) {
    if (new_end_key == origin_end_key) {
        return 0;
    }
    if (end_key_compare(new_end_key, origin_end_key) > 0) {
        return -1;
    }
    std::string key = new_end_key;
    for (auto region_iter = key_newregion_map.find(new_end_key);
            region_iter != key_newregion_map.end(); region_iter++) {
        SmartRegionInfo ptr_region = region_iter->second;
        if (key != ptr_region->start_key()) {
            DB_WARNING("table_id:%ld can`t find start_key:%s, in key_region_map", 
                       table_id, str_to_hex(key).c_str());
            return -1;
        }
        DB_WARNING("table_id:%ld, find region_id:%ld in key_region_map"
                   "start_key:%s, end_key:%s", table_id, ptr_region->region_id(),
                   str_to_hex(ptr_region->start_key()).c_str(),
                   str_to_hex(ptr_region->end_key()).c_str());
        regions.push_back(ptr_region);
        if (ptr_region->end_key() == origin_end_key) {
            return 0;
        }
        if (end_key_compare(ptr_region->end_key(), origin_end_key) > 0) {
            DB_FATAL("table_id:%ld region_id:%ld end_key:%s bigger than end_key:%s",
                     table_id, ptr_region->region_id(), 
                     str_to_hex(ptr_region->end_key()).c_str(), 
                     str_to_hex(origin_end_key).c_str());
            return -1;
        }
        key = ptr_region->end_key();
    }
    return -1;
}

int TableManager::get_presplit_regions(int64_t table_id, 
                      std::map<std::string, SmartRegionInfo>& key_newregion_map,
                                    pb::MetaManagerRequest& request) {
    std::string key = "";
    for (auto region_iter = key_newregion_map.find("");
            region_iter != key_newregion_map.end(); region_iter++) {
        SmartRegionInfo ptr_region = region_iter->second;
        if (key != ptr_region->start_key()) {
            DB_WARNING("table_id:%ld can`t find start_key:%s, in key_region_map", 
                       table_id, str_to_hex(key).c_str());
            return -1;
        }
        pb::RegionInfo* region_info = request.add_region_infos();
        *region_info = *ptr_region;
        if (ptr_region->end_key() == "") {
            return 0;
        }
        key = ptr_region->end_key();
    }
    return -1;
}

void TableManager::get_update_region_requests(int64_t table_id, 
                                std::vector<pb::MetaManagerRequest>& requests) {
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_WARNING("table_id: %ld not exist", table_id);
        return;
    }
    auto& table_info = _table_info_map[table_id];
    auto& startkey_regiondesc_map  = table_info.startkey_regiondesc_map;
    auto& key_newregion_map = table_info.startkey_newregion_map;
    auto& id_noneregion_map = table_info.id_noneregion_map;
    auto& id_keyregion_map  = table_info.id_keyregion_map;
    //已经没有发生变化的region，startkey_newregion_map和id_noneregion_map可清空
    if (id_keyregion_map.size() == 0) {
        return;
    }
    int ret = 0;
    std::vector<SmartRegionInfo> regions;
    for (auto iter = id_keyregion_map.begin(); iter != id_keyregion_map.end();) {
        auto cur_iter = iter++;
        regions.clear();
        int64_t region_id = cur_iter->first;
        auto ptr_region = cur_iter->second;
        auto master_region = RegionManager::get_instance()->get_region_info(region_id); 
        if (master_region == nullptr) {
            DB_FATAL("can`t find region_id:%ld in region info map", region_id);
            continue;
        }
        DB_WARNING("table_id:%ld, region_id:%ld key has changed "
                   "(version, start_key, end_key),(%ld, %s, %s)->(%ld, %s, %s)",
                   table_id, region_id, master_region->version(),
                   str_to_hex(master_region->start_key()).c_str(), 
                   str_to_hex(master_region->end_key()).c_str(),
                   ptr_region->version(),
                   str_to_hex(ptr_region->start_key()).c_str(),
                   str_to_hex(ptr_region->end_key()).c_str());
        if (ptr_region->version() <= master_region->version()) {
            DB_WARNING("table_id:%ld, region_id:%ld, version too small need erase",
                    table_id, region_id);
            id_keyregion_map.erase(cur_iter);
            continue;
        }
        if (!ptr_region->end_key().empty() 
                && ptr_region->end_key() < master_region->start_key()) {
            continue;
        }
        ret = get_merge_regions(table_id, ptr_region->start_key(), 
                                master_region->start_key(), 
                                startkey_regiondesc_map, id_noneregion_map, regions);
        if (ret < 0) {
            DB_WARNING("table_id:%ld, region_id:%ld get merge region failed",
                       table_id, region_id);
            continue;
        }
        regions.push_back(ptr_region);
        ret = get_split_regions(table_id, ptr_region->end_key(), 
                                master_region->end_key(), 
                                key_newregion_map, regions);
        if (ret < 0) {
            DB_WARNING("table_id:%ld, region_id:%ld get split region failed",
                       table_id, region_id);
            continue;
        }
        
        pb::MetaManagerRequest request;
        request.set_op_type(pb::OP_UPDATE_REGION);
        for (auto region : regions) {
            pb::RegionInfo* region_info = request.add_region_infos();
            *region_info = *region;
        }
        requests.push_back(request);
    }
}
void TableManager::recycle_update_region() {
    std::vector<pb::MetaManagerRequest> requests;
    {
        BAIDU_SCOPED_LOCK(_table_mutex);
        for (auto& table_info : _table_info_map) {
            auto& key_newregion_map = table_info.second.startkey_newregion_map;
            auto& id_noneregion_map = table_info.second.id_noneregion_map;
            auto& id_keyregion_map  = table_info.second.id_keyregion_map;
            auto& startkey_regiondesc_map  = table_info.second.startkey_regiondesc_map;
            
            for (auto iter = id_keyregion_map.begin(); iter != id_keyregion_map.end(); ) {
                auto cur_iter = iter++;
                int64_t region_id = cur_iter->first;
                auto ptr_region = cur_iter->second;
                auto master_region = RegionManager::get_instance()->get_region_info(region_id); 
                if (master_region == nullptr) {
                    DB_FATAL("can`t find region_id:%ld in region info map", region_id);
                    continue;
                }
                if (ptr_region->version() <= master_region->version()) {
                    id_keyregion_map.erase(cur_iter);
                    DB_WARNING("table_id:%ld, region_id:%ld key has changed "
                               "(version, start_key, end_key),(%ld, %s, %s)->(%ld, %s, %s)",
                               table_info.first, region_id, master_region->version(),
                               str_to_hex(master_region->start_key()).c_str(), 
                               str_to_hex(master_region->end_key()).c_str(),
                               ptr_region->version(),
                               str_to_hex(ptr_region->start_key()).c_str(),
                               str_to_hex(ptr_region->end_key()).c_str());
                    continue;
                }
            }
            
            if (startkey_regiondesc_map.size() == 0 && id_keyregion_map.size() == 0 
                    && key_newregion_map.size() != 0 && id_noneregion_map.size() == 0) {
                //如果该table没有region，但是存在store上报的新region，为预分裂region，特殊处理
                pb::MetaManagerRequest request;
                request.set_op_type(pb::OP_UPDATE_REGION);
                auto ret = get_presplit_regions(table_info.first, key_newregion_map, request);
                if (ret < 0) {
                    continue;
                }
                requests.push_back(request);
                continue;
            }
            if (id_keyregion_map.size() == 0) {
                if (key_newregion_map.size() != 0 || id_noneregion_map.size() != 0) {
                    key_newregion_map.clear();
                    id_noneregion_map.clear();
                    DB_WARNING("table_id:%ld tmp map clear", table_info.first);
                }
            }
        }
    }
    
    for (auto& request : requests) {
        SchemaManager::get_instance()->process_schema_info(NULL, &request, NULL, NULL);
    } 

}
void TableManager::check_update_region(const pb::LeaderHeartBeat& leader_region,
                         const SmartRegionInfo& master_region_info) {
    const pb::RegionInfo& leader_region_info = leader_region.region();
    if (leader_region_info.start_key() == leader_region_info.end_key()) {
        //空region，加入id_noneregion_map
        add_update_region(leader_region_info, true);
    } else {
        //key范围改变，加入id_keyregion_map
        add_update_region(leader_region_info, false);
    }
    //获取可以整体修改的region
    std::vector<pb::MetaManagerRequest> requests;
    get_update_region_requests(leader_region_info.table_id(), requests);
    if (requests.size() == 0) {
        return;
    }
    for (auto& request : requests) {
        SchemaManager::get_instance()->process_schema_info(NULL, &request, NULL, NULL);
    }    
}
void TableManager::drop_index(const pb::MetaManagerRequest& request, const int64_t apply_index, braft::Closure* done) {
    //检查参数有效性
    BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
    DB_DEBUG("drop index, request:%s", request.ShortDebugString().c_str());
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    if (request.table_info().indexs_size() != 1) {
        DB_WARNING("check index info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "index info fail");
        return;
    }
    if (_table_ddlinfo_map.find(table_id) != _table_ddlinfo_map.end()) {
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "this table has ddlwork in processing.");
        DB_WARNING("this table[%lld] has ddlwork in processing [%s].", table_id);
        return;
    }

    pb::SchemaInfo& schema_info = _table_info_map[table_id].schema_pb;
    auto index_req = request.table_info().indexs(0);
    auto index_to_del = std::find_if(std::begin(schema_info.indexs()), std::end(schema_info.indexs()), 
        [&index_req](const pb::IndexInfo& info) {
            return info.index_name() == index_req.index_name() &&
                (info.index_type() == pb::I_UNIQ || info.index_type() == pb::I_KEY || 
                info.index_type() == pb::I_FULLTEXT);
        });
    if (index_to_del != std::end(schema_info.indexs())) {
        DdlWorkMem ddl_work_mem; 
        ddl_work_mem.table_id = table_id;
        ddl_work_mem.work_info.set_index_id(index_to_del->index_id());
        ddl_work_mem.work_info.set_job_state(index_to_del->state());
        ddl_work_mem.resource_tag = schema_info.resource_tag();
        DB_NOTICE("DDL_LOG drop index ddlwork[%s]", ddl_work_mem.work_info.ShortDebugString().c_str());
        DB_NOTICE("DDL_LOG resource_tag : %s", ddl_work_mem.resource_tag.c_str());

        if (index_to_del->state() == pb::IS_DELETE_ONLY || index_to_del->state() == pb::IS_NONE) {
            //这两个状态，store不能初始化，可直接删除。
            ddl_work_mem.work_info.set_deleted(true);
            ddl_work_mem.work_info.set_table_id(table_id);
            update_index_status(ddl_work_mem.work_info);
        } else {
            if (init_ddlwork_drop_index(request, ddl_work_mem) != 0) {
                IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "init ddlwork drop index error");
                DB_WARNING("DDL_LOG init ddlwork drop index error.");
                return;
            }
            update_ddlwork_for_rocksdb(table_id, ddl_work_mem.work_info, nullptr);
            _table_ddlinfo_map.emplace(
                table_id,
                ddl_work_mem
            );
        }
        IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
    } else {
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "index not found");
        DB_WARNING("DDL_LOG drop_index can't find index [%s].", index_req.index_name().c_str());
    }
}

int TableManager::init_ddlwork_add_index(const pb::MetaManagerRequest& request, 
    DdlWorkMem& ddl_work_mem, pb::IndexInfo& index_info) {
    int ret = 0;
    if ((ret = init_ddlwork(request, ddl_work_mem)) != 0) {
        return ret;
    }
    ddl_work_mem.work_info.set_job_state(pb::IS_NONE);
    index_info.CopyFrom(request.table_info().indexs(0));
    //初始化第一个index。 
    index_info.set_state(pb::IS_NONE);
    
    auto table_id = ddl_work_mem.table_id;
    for (const auto& field_name : index_info.field_names()) {
        auto field_id_iter = _table_info_map[table_id].field_id_map.find(field_name);
        if (field_id_iter == _table_info_map[table_id].field_id_map.end()) {
            DB_WARNING("field_id not found field_name[%s] in field_id_map.", field_name.c_str());
            return -1;
        } else {
            index_info.add_field_ids(field_id_iter->second);
            DB_DEBUG("DDL_LOG add field id[%d] field_name[%s]", field_id_iter->second, field_name.c_str());
        }
    }

    return 0;
}

int TableManager::init_ddlwork_drop_index(const pb::MetaManagerRequest& request, DdlWorkMem& ddl_work_mem) {
    int ret = 0;
    if ((ret = init_ddlwork(request, ddl_work_mem)) != 0) {
        return ret;
    }
    return 0;
}

int TableManager::init_region_ddlwork(DdlWorkMem& ddl_work_mem) {
    std::vector<int64_t> region_ids;
    auto table_id = ddl_work_mem.work_info.table_id();
    std::unordered_map<pb::OpType, pb::IndexState, std::hash<int>> op_index_type_map {
        {pb::OP_ADD_INDEX, pb::IS_NONE},
    };
    op_index_type_map[pb::OP_DROP_INDEX] = ddl_work_mem.work_info.job_state();
    {
        BAIDU_SCOPED_LOCK(_table_mutex);
        if (_table_info_map.find(table_id) == _table_info_map.end()) {
            DB_WARNING("ddlwork table_id has no region");
            return -1;
        }
        for (const auto& partition_regions : _table_info_map[table_id].partition_regions) {
            for (auto& region_id :  partition_regions.second) {
                DB_DEBUG("DDL_LOG[init_ddlwork] get region %d", region_id);
                region_ids.push_back(region_id);    
            }
        }
        std::vector<SmartRegionInfo> region_infos;
        RegionManager::get_instance()->get_region_info(region_ids, region_infos);

        auto replica_num = _table_info_map[table_id].schema_pb.replica_num();
        DB_DEBUG("DDL_LOG replica_num [%lld]", replica_num);
        if (!std::all_of(region_infos.begin(), region_infos.end(), 
            [replica_num](const SmartRegionInfo& smart_region) {
                DB_DEBUG("DDL_LOG region_id [%lld] peers_size [%lld].", 
                    smart_region->region_id(), smart_region->peers_size());
                return smart_region->peers_size() > 0;
            })) {
            DB_WARNING("DDL_LOG peers_size less then 0.");
            return -1;
        }
        if (op_index_type_map.find(ddl_work_mem.work_info.op_type()) != op_index_type_map.end()) {
            for (const auto& smart_region : region_infos) {
                //过滤空region
                if (smart_region->start_key() == smart_region->end_key() && smart_region->start_key() != "") {
                    DB_DEBUG("filter null region [%lld]", smart_region->region_id());
                    continue;
                }
                DdlRegionMem ddl_region_mem;
                init_ddlwork_region_info(ddl_region_mem, *smart_region.get(), pb::IS_UNKNOWN);
                ddl_work_mem.region_ddl_infos.emplace(smart_region->region_id(), ddl_region_mem);
            }
        } else {
            DB_FATAL("DDL_LOG unknown optype.");
            return -1;
        }
    }
    return 0;
}

int TableManager::init_ddlwork(const pb::MetaManagerRequest& request, DdlWorkMem& ddl_work_mem) {

    auto table_id = ddl_work_mem.table_id;
    ddl_work_mem.work_info.set_table_id(table_id);
    ddl_work_mem.work_info.set_op_type(request.op_type());
    ddl_work_mem.work_info.set_rollback(false);
    ddl_work_mem.work_info.set_begin_timestamp(std::chrono::seconds(std::time(nullptr)).count());
    //init ddlwork regions info
    return init_region_ddlwork(ddl_work_mem);
}

void TableManager::add_index(const pb::MetaManagerRequest& request, 
                             const int64_t apply_index, 
                             braft::Closure* done) {

    DB_DEBUG("DDL_LOG[add_index] add index, request:%s", request.ShortDebugString().c_str());
    BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
    int64_t table_id;
    if (check_table_exist(request.table_info(), table_id) != 0 && 
        request.table_info().table_id() == table_id) {
        DB_WARNING("DDL_LOG[add_index] check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist");
        return;
    }
    //检查field有效性
    if (request.table_info().indexs_size() != 1) {
        DB_WARNING("DDL_LOG[add_index] check index info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "index info fail");
        return;
    }
    auto&& first_index_fields = request.table_info().indexs(0).field_names();
    auto all_fields_exist = std::all_of(
        std::begin(first_index_fields),
        std::end(first_index_fields),
        [&](const std::string& field_name) -> bool {
            return check_field_exist(field_name, table_id);
        }
    );
    if (!all_fields_exist) {
        DB_WARNING("DDL_LOG[add_index] check fields info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "fields info fail");
        return;
    }
    DB_DEBUG("DDL_LOG[add_index] check field success.");
    if (_table_info_map.find(table_id) == _table_info_map.end()) {
        DB_WARNING("DDL_LOG[add_index] table not in table_info_map, request:%s", request.DebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not in table_info_map");
        return;
    }

    int64_t index_id;
    int index_ret = check_index(request.table_info().indexs(0), 
        _table_info_map[table_id].schema_pb, index_id);
    
    if (index_ret == -1) {
        DB_WARNING("check index info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "index info fail");
        return;
    }
    DB_DEBUG("DDL_LOG[add_index] check index info success.");
    //初始化DdlWorkInfo
    if (_table_ddlinfo_map.find(table_id) != _table_ddlinfo_map.end()) {
        //该table有ddl操作在进行，返回。
        DB_WARNING("DDL_LOG[add_index] check ddlinfo_map info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "ddlinfo_map already processing");
        return;
    }
    DB_DEBUG("DDL_LOG[add_index] init ddlwork start.");

    DdlWorkMem ddl_work_mem; 
    pb::IndexInfo index_info;
    ddl_work_mem.table_id = table_id;
    if (init_ddlwork_add_index(request, ddl_work_mem, index_info) != 0) {
        DB_WARNING("DDL_LOG[add_index] init ddlwork info fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "init ddlwork info fail.");
        return;
    }
    if (index_ret == 1) {
        index_info.set_index_id(index_id);
    } else {
        int64_t tmp_max_table_id = get_max_table_id();
        index_info.set_index_id(++tmp_max_table_id);
        set_max_table_id(tmp_max_table_id);
        std::string max_table_id_value;
        max_table_id_value.append((char*)&tmp_max_table_id, sizeof(int64_t));
        int ret = MetaRocksdb::get_instance()->put_meta_info(construct_max_table_id_key(), max_table_id_value);    
        if (ret < 0) {
            DB_WARNING("update max_table_id to rocksdb fail.");
            IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
            return;
        }  
        DB_NOTICE("alloc new index_id[%ld]", tmp_max_table_id);
    }
    ddl_work_mem.work_info.set_index_id(index_info.index_id());

    DB_DEBUG("DDL_LOG[add_index] ddlwork [%s] after init.", ddl_work_mem.work_info.ShortDebugString().c_str());
    // 更新现有的schemainfo
    pb::SchemaInfo mem_schema_pb =  _table_info_map[table_id].schema_pb;
    auto index_iter = mem_schema_pb.mutable_indexs()->begin();
    for (; index_iter != mem_schema_pb.mutable_indexs()->end();) {
        if (index_info.index_id() == index_iter->index_id()) {
            DB_NOTICE("DDL_LOG udpate_index delete index [%lld].", index_iter->index_id());
            mem_schema_pb.mutable_indexs()->erase(index_iter);
        } else {
            index_iter++;
        }
    }
    ddl_work_mem.resource_tag = mem_schema_pb.resource_tag();
    DB_NOTICE("resource_tag : %s", ddl_work_mem.resource_tag.c_str());
    pb::IndexInfo* add_index = mem_schema_pb.add_indexs();
    add_index->CopyFrom(index_info);
    mem_schema_pb.set_version(mem_schema_pb.version() + 1);
    _table_info_map[table_id].index_id_map[add_index->index_name()] = add_index->index_id();
    set_table_pb(mem_schema_pb);
    std::vector<pb::SchemaInfo> schema_infos{mem_schema_pb};
    put_incremental_schemainfo(apply_index, schema_infos);

    auto ret = update_schema_for_rocksdb(table_id, mem_schema_pb, done);
    if (ret < 0) {
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return;
    }
    DB_DEBUG("DDL_LOG add_index index_info [%s]", add_index->ShortDebugString().c_str());
    DB_NOTICE("DDL_LOG add_index schema_info [%s]", _table_info_map[table_id].schema_pb.ShortDebugString().c_str());
    //持久化ddlwork
    update_ddlwork_for_rocksdb(table_id, ddl_work_mem.work_info, nullptr);
    _table_ddlinfo_map.emplace(
        table_id,
        ddl_work_mem
    );
    DB_NOTICE("DDL_LOG table_id[%lld], ddlwork info : %s", table_id, ddl_work_mem.work_info.ShortDebugString().c_str());
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
}

bool TableManager::check_field_exist(const std::string &field_name,
                        int64_t table_id) {
    auto table_mem_iter = _table_info_map.find(table_id);
    if (table_mem_iter == _table_info_map.end()) {
        DB_WARNING("table_id:[%lld] not exist.", table_id);
        return false;
    }
    auto &&table_mem = table_mem_iter->second;
    if (table_mem.field_id_map.find(field_name) != table_mem.field_id_map.end()) {
        return true;
    }
    return false;
}

int TableManager::check_index(const pb::IndexInfo& index_info_to_check,
                   const pb::SchemaInfo& schema_info, int64_t& index_id) {
    
    auto same_index = [](const pb::IndexInfo& index_l, const pb::IndexInfo& index_r) -> bool{
        if (index_l.field_names_size() != index_r.field_names_size()) {
            return false;
        }
        for (auto field_index = 0; field_index < index_l.field_names_size(); ++field_index) {
            if (index_l.field_names(field_index) != index_r.field_names(field_index)) {
                   return false;
            }
        }
        return true;
    };

    for (const auto& index_info : schema_info.indexs()) {
        if (index_info.index_name() == index_info_to_check.index_name()) {
            //索引状态为NONE、IS_DELETE_ONLY、IS_DELETE_LOCAL，并且索引的field一致，可以重建。
            if (index_info.state() == pb::IS_NONE || index_info.state() == pb::IS_DELETE_ONLY ||
                index_info.state() == pb::IS_DELETE_LOCAL) {
                if (same_index(index_info, index_info_to_check)) {
                    index_id = index_info.index_id();
                    DB_NOTICE("DDL_LOG rebuild index[%lld]", index_id);
                    return 1;
                } else {
                    DB_WARNING("DDL_LOG same index name, diff fields.");
                    return -1;
                }
            } else {
                DB_WARNING("DDL_LOG rebuild index failed, index state not satisfy.");
                return -1;
            }
        } else {
            if (same_index(index_info, index_info_to_check)) {
               DB_WARNING("DDL_LOG diff index name, same fields.");
                return -1;
            }
        }
    }
    return 0;
}

void TableManager::check_delete_ddl_region_info(DdlWorkMem& ddlwork) {
    auto check_lambda = [this, &ddlwork](pb::IndexState job_state) {
        if (ddlwork.work_info.job_state() == job_state) {
            auto& region_infos = ddlwork.region_ddl_infos;
            /*
            auto delete_state_num = std::count_if(
                std::begin(region_infos), std::end(region_infos),
                [job_state, &region_ids](typename std::unordered_map<int64_t, DdlRegionMem>::const_reference region_info){
                    if (region_info.second.workstate == job_state) {
                        return true;
                    } else {
                        region_ids.push_back(region_info.second.region_id);
                        return false;
                    }
                }
            );
            */
            auto delete_state_num = ddlwork.state_count[job_state];
            auto all_num = ddlwork.all_peer_num;
            if ((delete_state_num * 10 > all_num * 9) && 
                ((ddlwork.check_del_region_num++) % 100 == 0)) {
                
                std::vector<int64_t> region_ids;
                for (const auto& region_info : region_infos) {
                    if (region_info.second.workstate != job_state) {
                        region_ids.push_back(region_info.first);
                    }
                }
                delete_ddl_region_info(ddlwork, region_ids);
            }
        }
    };
    switch (ddlwork.work_info.op_type()) {
        case pb::OP_ADD_INDEX:
            check_lambda(pb::IS_NONE);
            break;
        case pb::OP_DROP_INDEX:
            //删除索引，初始状态可能是任何状态
            check_lambda(ddlwork.work_info.job_state());
            break; 
        default:
            DB_FATAL("unkown op_type");
            break;
    }
}

void TableManager::delete_ddl_region_info(DdlWorkMem& ddlwork, std::vector<int64_t>& region_ids) {
    std::vector<SmartRegionInfo> region_infos;
    RegionManager::get_instance()->get_region_info(region_ids, region_infos);

    for (const auto& smart_region : region_infos) {
        //过滤空region
        if (smart_region->start_key() == smart_region->end_key() && smart_region->start_key() != "") {
            DB_NOTICE("filter null region [%lld]", smart_region->region_id());
            ddlwork.region_ddl_infos.erase(smart_region->region_id());
        }
    }
}

void TableManager::add_ddl_region_info(const pb::StoreHeartBeatRequest& store_ddlinfo_req) {
    for (const auto& store_ddlinfo : store_ddlinfo_req.ddlwork_infos()) {
        
        auto table_id = store_ddlinfo.table_id();
        auto region_id = store_ddlinfo.region_id();
        auto& peer = store_ddlinfo_req.instance_info().address();

        // 当ddlwork的job_state达到working状态时（WRITE_LOCAL\DELETE_LOCAL），不允许新增peer。
        // 新增的peer为先完成ddlwork的peer分裂出来的。
        pb::IndexState state;
        pb::OpType op_type;
        if (get_ddlwork_state(table_id, state, op_type) == 0) {
            if (!DdlHelper::can_init_ddlwork(store_ddlinfo.op_type(), state)) {
                DB_DEBUG("skip init ddlwork.");
                continue;
            }

            if (!exist_ddlwork_region(table_id, region_id)) {
                //加region
                add_ddlwork_region(table_id, region_id, peer);
            } else {
                if (!exist_ddlwork_peer(table_id, region_id, peer)) {
                    //加peer
                    add_ddlwork_peer(table_id, region_id, peer);
                }
            }
        }
    }
}

void TableManager::init_ddlwork_for_store(const pb::StoreHeartBeatRequest* request,
    pb::StoreHeartBeatResponse* response) {
    //遍历meta工作队列，如果有属于该store的未开始的work，则发送work给store。
    //DB_DEBUG("store request [%s]", request->ShortDebugString().c_str());
    std::unordered_set<int64_t> store_ddl_work_started_table_ids;
    for (const auto& ddl_info : request->ddlwork_infos()) {
        store_ddl_work_started_table_ids.insert(ddl_info.table_id());
    }
    std::unordered_set<int64_t> ddl_table_ids;
    get_ddlwork_table_ids(ddl_table_ids);
    for (auto table_id : ddl_table_ids) {

        if (store_ddl_work_started_table_ids.find(table_id) != store_ddl_work_started_table_ids.end()) {
            continue;
        }
        pb::DdlWorkInfo pb_ddlwork_info;
        if (get_pb_ddlwork_info(table_id, pb_ddlwork_info) == 0) {
            //DB_DEBUG("DDL_LOG meta has ddlwork table_id[%lld], ddlwork_info[%s]", 
            //    table_id, pb_ddlwork_info.ShortDebugString().c_str());
            auto op_type = pb_ddlwork_info.op_type();
            if (DdlHelper::can_init_ddlwork(op_type, pb_ddlwork_info.job_state())) {
                
                DB_NOTICE("process_ddl_heartbeat init ddl_work_info for store table_id[%lld] address[%s]", 
                    table_id, request->instance_info().address().c_str());
                //初始化work，设置start字段，发请求给store，持久化。
                switch (op_type) {
                    case pb::OP_ADD_INDEX:
                    case pb::OP_DROP_INDEX:
                        process_ddl_common_init(response, pb_ddlwork_info);
                        break;
                    default:
                        DB_WARNING("unknown optype");
                }
            } else {
                DB_DEBUG("DDL_LOG[process_ddl_heartbeat] table_id[%lld] ddl_work_info has already been initialized for store.", table_id);
            }
        } else {
            DB_WARNING("no table_id[%ld] in ddlwork.", table_id);
        }
    }
}

void TableManager::process_ddl_heartbeat_for_store(const pb::StoreHeartBeatRequest* request,
    pb::StoreHeartBeatResponse* response,
    uint64_t log_id) {

    TimeCost ddl_time;
    {
        BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
        if (_table_ddlinfo_map.size() < 1) {
            return;
        }
        std::unordered_set<std::string> resource_set;
        for (const auto& ddlinfo : _table_ddlinfo_map) {
            resource_set.insert(ddlinfo.second.resource_tag);
        }
        if (resource_set.count(request->instance_info().resource_tag()) == 0) {
            return;
        }
    }
    init_ddlwork_for_store(request, response);   
    auto init_ddlwork_time = ddl_time.get_time();
    ddl_time.reset();
    //遍历store工作队列，更新meta ddlwork。
    common_update_ddlwork_info_heartbeat_for_store(request);
    auto update_ddlwork_time = ddl_time.get_time();
    ddl_time.reset();
    //处理store中所有的ddlwork
    std::set<int64_t> processed_ddlwork_table_ids;
    for (const auto& store_ddl_work : request->ddlwork_infos()) {
        auto store_ddl_table_id = store_ddl_work.table_id();
        if (processed_ddlwork_table_ids.find(store_ddl_table_id) != processed_ddlwork_table_ids.end()) {
            continue;
        }
        DB_NOTICE("process ddlwork table_id[%lld]", store_ddl_table_id);
        processed_ddlwork_table_ids.insert(store_ddl_table_id);
        BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
        auto ddlwork_iter = _table_ddlinfo_map.find(store_ddl_table_id);
        if (ddlwork_iter != _table_ddlinfo_map.end()) {
            DdlWorkMem& meta_work = ddlwork_iter->second;
            auto& meta_work_info = meta_work.work_info;
            //检查是否为同一ddlwork，如果不是同一个，直接给store初始化新ddlwork。
            if (store_ddl_work.begin_timestamp() != meta_work_info.begin_timestamp()) {
                DB_WARNING("store work begin_timestamp[%ld] and meta work begin_timestamp[%ld] not equal.",
                    store_ddl_work.begin_timestamp(), meta_work_info.begin_timestamp());
                //初始化新的ddlwork。
                DB_NOTICE("process_ddl_heartbeat init ddl_work_info for store table_id[%lld] ddlwork[%s]", 
                    store_ddl_table_id, meta_work.work_info.ShortDebugString().c_str());
                process_ddl_common_init(response, meta_work.work_info);
                continue;
            }
            //检查回滚，为避免状态机延迟导致多次回滚，设置内存变量控制。
            if (meta_work_info.rollback() == true && !meta_work.is_rollback) {
                meta_work.is_rollback = true;
                DB_WARNING("DDL_LOG rollback and delete ddlwork table_id[%lld]", store_ddl_table_id);
                rollback_ddlwork(meta_work);         
                continue;
            }
            switch (meta_work_info.op_type()) {
                case pb::OP_ADD_INDEX:
                    process_ddl_add_index_process(response, meta_work);
                    break;
                case pb::OP_DROP_INDEX:
                    process_ddl_del_index_process(response, meta_work);
                    break;
                default:
                    DB_WARNING("DDL_LOG unknown optype");
            }
        } else {
            DB_WARNING("DDL_LOG store table_id[%lld] not in meta.", store_ddl_table_id);
        }
    }
    auto response_ddlwork_time = ddl_time.get_time();
    DB_NOTICE("DDL_LOG ddlwork_time: init_time[%lld], update_time[%lld], response_time[%lld], store_ddlwork_size[%zu]",
        init_ddlwork_time, update_ddlwork_time, response_ddlwork_time, request->ddlwork_infos_size());
}

void TableManager::update_index_status(const pb::MetaManagerRequest& request,
                                       const int64_t apply_index,
                                       braft::Closure* done) {
    update_table_internal(request, apply_index, done, 
        [](const pb::MetaManagerRequest& request, pb::SchemaInfo& mem_schema_pb) {
            auto&& request_index_info = request.ddlwork_info();
            auto index_iter = mem_schema_pb.mutable_indexs()->begin();
            for (; index_iter != mem_schema_pb.mutable_indexs()->end(); index_iter++) {
                if (request_index_info.index_id() == index_iter->index_id()) {
                    if (request_index_info.has_deleted() && 
                        request_index_info.deleted()) {
                        //删除索引
                        DB_NOTICE("DDL_LOG udpate_index_status delete index [%lld].", index_iter->index_id());
                        mem_schema_pb.mutable_indexs()->erase(index_iter);
                    } else {
                        //改变索引状态
                        DB_NOTICE("DDL_LOG set state index state to [%s]", 
                            pb::IndexState_Name(request_index_info.job_state()).c_str());
                        index_iter->set_state(request_index_info.job_state());
                    }
                    break;
                }
            }
            mem_schema_pb.set_version(mem_schema_pb.version() + 1);
        }); 
}

void TableManager::delete_ddlwork(const pb::MetaManagerRequest& request, braft::Closure* done) {
    BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
    std::vector<std::string> ddlwork_info_vector;
    int64_t table_id = request.ddlwork_info().table_id();
    DB_DEBUG("DDL_LOG delete ddlwork table_id[%lld]", table_id);
    if (_table_ddlinfo_map.find(table_id) == _table_ddlinfo_map.end()) {
        DB_WARNING("check table exist fail, request:%s", request.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INPUT_PARAM_ERROR, "table not exist or ddl not exist");
        return;
    }   
    _table_ddlinfo_map[table_id].work_info.set_end_timestamp(request.ddlwork_info().end_timestamp());
    ddlwork_info_vector.push_back(construct_ddl_key(table_id));
    int ret = MetaRocksdb::get_instance()->delete_meta_info(ddlwork_info_vector);    
    if (ret < 0) {
        DB_WARNING("delete ddl work info to rocksdb fail, request：%s", 
                    request.ddlwork_info().ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
    } 

    BAIDU_SCOPED_LOCK(_all_table_ddlinfo_mutex);
    _all_table_ddlinfo_map.insert(std::make_pair(table_id, _table_ddlinfo_map[table_id]));
    _table_ddlinfo_map.erase(table_id);
    IF_DONE_SET_RESPONSE(done, pb::SUCCESS, "success");
}

int TableManager::update_ddlwork_for_rocksdb(int64_t table_id,
                                               const pb::DdlWorkInfo& ddlwork_info,
                                               braft::Closure* done) {
    std::string ddlwork_info_value;
    if (!ddlwork_info.SerializeToString(&ddlwork_info_value)) {
        DB_WARNING("DDL_LOG request serializeToArray fail when update ddl work info, request:%s", 
                    ddlwork_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::PARSE_TO_PB_FAIL, "serializeToArray fail");
        return -1;
    }
    int ret = MetaRocksdb::get_instance()->put_meta_info(construct_ddl_key(table_id), ddlwork_info_value);    
    if (ret < 0) {
        DB_WARNING("update ddl work info to rocksdb fail, request：%s", 
                    ddlwork_info.ShortDebugString().c_str());
        IF_DONE_SET_RESPONSE(done, pb::INTERNAL_ERROR, "write db fail");
        return -1;
    }    
    DB_DEBUG("DDL_LOG update_ddlwork_for_rocksdb success, table_id[%lld], ddlwork_info[%s]", 
        table_id, ddlwork_info.ShortDebugString().c_str());
    return 0;
}

void TableManager::common_update_ddlwork_info_heartbeat_for_store(const pb::StoreHeartBeatRequest* request) {
    if (request->ddlwork_infos_size() < 1) {
        return;
    }

    bool debug_flag = true;
    for (const auto& store_ddl_info : request->ddlwork_infos()) {
        auto table_id = store_ddl_info.table_id();
        DB_DEBUG("update store ddl_work table_id[%lld] ddlwork[%s]", table_id, 
            store_ddl_info.ShortDebugString().c_str());
        {
            BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
            auto meta_ddlinfo_iter = _table_ddlinfo_map.find(table_id);
            if (meta_ddlinfo_iter == _table_ddlinfo_map.end()) {
                //meta已没有该ddl信息，store会自动删除ddlwork。
                DB_WARNING("DDL_LOG store table_id[%lld] ddlwork info not in table_ddlinfo_map.", table_id);
                continue;
            }

            auto& mem_meta_ddlinfo = meta_ddlinfo_iter->second;
            pb::DdlWorkInfo& meta_ddlinfo = meta_ddlinfo_iter->second.work_info;
            //检查是否为同一ddlwork，如果不是同一个，不更新。
            if (store_ddl_info.begin_timestamp() != meta_ddlinfo.begin_timestamp()) {
                DB_WARNING("store work begin_timestamp[%ld] and meta work begin_timestamp[%ld] not equal.",
                    store_ddl_info.begin_timestamp(), meta_ddlinfo.begin_timestamp());
                continue;
            }
            if (store_ddl_info.rollback() == true) {
                DB_FATAL("DDL_LOG rollback table_id [%lld] error[%s]", table_id, 
                    pb::ErrCode_Name(store_ddl_info.errcode()).c_str());
                meta_ddlinfo.set_rollback(true);
                meta_ddlinfo.set_errcode(store_ddl_info.errcode());
                continue;
            }
            //清理region
            check_delete_ddl_region_info(mem_meta_ddlinfo);
        }

        auto region_id = store_ddl_info.region_id();
        const auto& peer = request->instance_info().address();
        if (exist_ddlwork_peer(table_id, region_id, peer)) {
            update_ddlwork_peer_state(table_id, region_id, peer, store_ddl_info.job_state(), debug_flag);
        } else {
            pb::IndexState state;
            pb::OpType op_type;
            if (get_ddlwork_state(table_id, state, op_type) == 0) {
                if (!DdlHelper::can_init_ddlwork(op_type, state)) {
                    DB_DEBUG("skip init ddlwork.");
                    continue;
                }

                if (!exist_ddlwork_region(table_id, region_id)) {
                    //加region
                    add_ddlwork_region(table_id, region_id, peer);
                } else {
                    if (!exist_ddlwork_peer(table_id, region_id, peer)) {
                        //加peer
                        add_ddlwork_peer(table_id, region_id, peer);
                    }
                }
            }
            DB_WARNING("ddlwork common update region region_id[%lld] peer [%s].", 
                region_id, peer.c_str())
        }
    }
}
int TableManager::load_ddl_snapshot(const std::string& value) {
    pb::DdlWorkInfo work_info_pb;
    if (!work_info_pb.ParseFromString(value)) {
        DB_FATAL("parse from pb fail when load ddl snapshot, key: %s", value.c_str());
        return -1;
    }
    DdlWorkMem ddl_mem;
    ddl_mem.work_info = work_info_pb;
    ddl_mem.table_id = work_info_pb.table_id();
    pb::IndexState current_state;
    if (get_index_state(ddl_mem.table_id, work_info_pb.index_id(), current_state) != 0) {
        DB_FATAL("ddl index not ready. table_id[%lld] index_id[%lld]", 
            ddl_mem.table_id, work_info_pb.index_id());
    } else {
        work_info_pb.set_job_state(current_state);
    }
    DB_NOTICE("ddl snapshot:%s", work_info_pb.ShortDebugString().c_str());
    init_region_ddlwork(ddl_mem);
    
    BAIDU_SCOPED_LOCK(_table_mutex);
    if (_table_info_map.count(ddl_mem.table_id) == 1) {
        pb::SchemaInfo& schema_info = _table_info_map[ddl_mem.table_id].schema_pb;
        ddl_mem.resource_tag = schema_info.resource_tag();
        DB_NOTICE("set ddlwork resource_tag[%s]", ddl_mem.resource_tag.c_str());
        BAIDU_SCOPED_LOCK(_table_ddlinfo_mutex);
        _table_ddlinfo_map[ddl_mem.table_id] = ddl_mem;
    } else {
        DB_FATAL("load table_id[%ld] ddlwork schema error.", ddl_mem.table_id);
    }

    return 0;
}

void TableManager::rollback_ddlwork(DdlWorkMem& meta_work) {
    auto& meta_work_info = meta_work.work_info;
    switch (meta_work_info.op_type()) {
        case pb::OP_ADD_INDEX:
            {
                //回滚
                pb::DdlWorkInfo rollback_ddlwork;
                rollback_ddlwork.set_table_id(meta_work_info.table_id());
                rollback_ddlwork.CopyFrom(meta_work_info);
                update_ddlwork_info(meta_work_info, pb::OP_DELETE_DDLWORK);
                drop_index_request(rollback_ddlwork);
            }
            break;
        case pb::OP_DROP_INDEX:
            //删除索引回滚操作：删除ddlwork，暂无其他操作。
            update_ddlwork_info(meta_work_info, pb::OP_DELETE_DDLWORK);
            break;
        default:
            DB_WARNING("unknown op_type.");
    }
}
}//namespace 

/* vim: set expandtab ts=4 sw=4 sts=4 tw=100: */
