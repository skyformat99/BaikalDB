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

#pragma once

#include <stdint.h>
#include <string>
#include <type_traits>
#include <boost/lexical_cast.hpp>
#include "proto/common.pb.h"
#include "common.h"
#include "datetime.h"
#include "type_utils.h"

namespace baikaldb {
struct ExprValue {
    pb::PrimitiveType type;
    union {
        bool bool_val;
        int8_t int8_val;
        int16_t int16_val;
        int32_t int32_val;
        int64_t int64_val;
        uint8_t uint8_val;
        uint16_t uint16_val;
        uint32_t uint32_val;
        uint64_t uint64_val;
        float float_val;
        double double_val;
    } _u;
    std::string str_val;

    explicit ExprValue(pb::PrimitiveType type_ = pb::NULL_TYPE) : type(type_) {
        _u.int64_val = 0;
    }
    template <class T>
    T get_numberic() const {
        switch (type) {
            case pb::BOOL:
                return _u.bool_val;
            case pb::INT8:
                return _u.int8_val;
            case pb::INT16:
                return _u.int16_val;
            case pb::INT32:
                return _u.int32_val;
            case pb::INT64:
                return _u.int64_val;
            case pb::UINT8:
                return _u.uint8_val;
            case pb::UINT16:
                return _u.uint16_val;
            case pb::UINT32:
                return _u.uint32_val;
            case pb::UINT64:
                return _u.uint64_val;
            case pb::FLOAT:
                return _u.float_val;
            case pb::DOUBLE:
                return _u.double_val;
            case pb::STRING:
                if (std::is_integral<T>::value) {
                    return strtoull(str_val.c_str(), NULL, 10);
                } else if (std::is_floating_point<T>::value) {
                    return strtod(str_val.c_str(), NULL);
                } else {
                    return 0;
                }
            case pb::DATETIME: {
                return _u.uint64_val;
            }
            case pb::TIME: {
                return _u.int32_val;
            }
            case pb::TIMESTAMP: {
                // internally timestamp is stored in uint32
                return _u.uint32_val;
            }
            case pb::DATE:
                return _u.uint32_val; 
            default:
                return 0;
        }
    }

    ExprValue& cast_to(pb::PrimitiveType type_) {
        if (is_null() || type == type_) {
            return *this;
        }
        switch (type_) {
            case pb::BOOL:
                _u.bool_val = get_numberic<bool>();
                break;
            case pb::INT8:
                _u.int8_val = get_numberic<int8_t>();
                break;
            case pb::INT16:
                _u.int16_val = get_numberic<int16_t>();
                break;
            case pb::INT32:
                _u.int32_val = get_numberic<int32_t>();
                break;
            case pb::INT64:
                _u.int64_val = get_numberic<int64_t>();
                break;
            case pb::UINT8:
                _u.uint8_val = get_numberic<uint8_t>();
                break;
            case pb::UINT16:
                _u.uint16_val = get_numberic<uint16_t>();
                break;
            case pb::UINT32:
                _u.uint32_val = get_numberic<uint32_t>();
                break;
            case pb::UINT64:
                _u.uint64_val = get_numberic<uint64_t>();
                break;
            case pb::DATETIME: {
                if (type == pb::STRING) {
                    _u.uint64_val = str_to_datetime(str_val.c_str());
                    str_val.clear();
                } else if (type == pb::TIMESTAMP) {
                    _u.uint64_val = timestamp_to_datetime(_u.uint32_val);
                } else if (type == pb::DATE) {
                    _u.uint64_val = date_to_datetime(_u.uint32_val);
                } else if (type == pb::TIME) {
                    _u.uint64_val = time_to_datetime(_u.int32_val);
                } else {
                    _u.uint64_val = get_numberic<uint64_t>();
                }
                break;
            }
            case pb::TIMESTAMP:
                if (!is_numberic()) {
                    _u.uint32_val = datetime_to_timestamp(cast_to(pb::DATETIME)._u.uint64_val);
                } else {
                    _u.uint32_val = get_numberic<uint32_t>();
                }
                break;
            case pb::DATE: {
                if (!is_numberic()) {
                    _u.uint32_val = datetime_to_date(cast_to(pb::DATETIME)._u.uint64_val);
                } else {
                    _u.uint32_val = get_numberic<uint32_t>();
                }
                break;
            }
            case pb::TIME: {
                if (is_numberic()) {
                    _u.int32_val = get_numberic<int32_t>();
                } else if (is_string()) {
                    _u.int32_val = str_to_time(str_val.c_str());
                } else {
                    _u.int32_val = datetime_to_time(cast_to(pb::DATETIME)._u.uint64_val);
                }
                break;
            }
            case pb::FLOAT:
                _u.float_val = get_numberic<float>();
                break;
            case pb::DOUBLE:
                _u.double_val = get_numberic<double>();
                break;
            case pb::STRING:
                str_val = get_string();
                break;
            default:
                break;
        }
        type = type_;
        return *this;
    }

   uint64_t hash(uint32_t seed = 0x110) const {
        uint64_t out[2];
        switch (type) {
            case pb::BOOL:
            case pb::INT8:
            case pb::UINT8:
                butil::MurmurHash3_x64_128(&_u, 1, seed, out);
                return out[0];
            case pb::INT16:
            case pb::UINT16:
                butil::MurmurHash3_x64_128(&_u, 2, seed, out);
                return out[0];
            case pb::INT32:
            case pb::UINT32:
            case pb::FLOAT:
            case pb::TIMESTAMP:
            case pb::DATE:
            case pb::TIME:
                butil::MurmurHash3_x64_128(&_u, 4, seed, out);
                return out[0];
            case pb::INT64:
            case pb::UINT64:
            case pb::DOUBLE:
            case pb::DATETIME: 
                butil::MurmurHash3_x64_128(&_u, 8, seed, out);
                return out[0];
            case pb::STRING: {
                butil::MurmurHash3_x64_128(str_val.c_str(), str_val.size(), seed, out);
                return out[0];
            }
            default:
                return 0;
        }
    }

    std::string get_string() const {
        switch (type) {
            case pb::BOOL:
                return std::to_string(_u.bool_val);
            case pb::INT8:
                return std::to_string(_u.int8_val);
            case pb::INT16:
                return std::to_string(_u.int16_val);
            case pb::INT32:
                return std::to_string(_u.int32_val);
            case pb::INT64:
                return std::to_string(_u.int64_val);
            case pb::UINT8:
                return std::to_string(_u.uint8_val);
            case pb::UINT16:
                return std::to_string(_u.uint16_val);
            case pb::UINT32:
                return std::to_string(_u.uint32_val);
            case pb::UINT64:
                return std::to_string(_u.uint64_val);
            case pb::FLOAT:
                return std::to_string(_u.float_val);
            case pb::DOUBLE:
                return std::to_string(_u.double_val);
            case pb::STRING:
            case pb::HLL:
                return str_val;
            case pb::DATETIME:
                return datetime_to_str(_u.uint64_val);
            case pb::TIME:
                return time_to_str(_u.int32_val);
            case pb::TIMESTAMP:
                return timestamp_to_str(_u.uint32_val);
            case pb::DATE:
                return date_to_str(_u.uint32_val);
            default:
                return "";
        }
    }

    void add(ExprValue& value) {
        switch (type) {
            case pb::BOOL:
                value._u.bool_val += value.get_numberic<bool>();
                return;
            case pb::INT8:
                _u.int8_val += value.get_numberic<int8_t>();
                return;
            case pb::INT16:
                _u.int16_val += value.get_numberic<int16_t>();
                return;
            case pb::INT32:
                _u.int32_val += value.get_numberic<int32_t>();
                return;
            case pb::INT64:
                _u.int64_val += value.get_numberic<int64_t>();
                return;
            case pb::UINT8:
                _u.uint8_val += value.get_numberic<uint8_t>();
                return;
            case pb::UINT16:
                _u.uint16_val += value.get_numberic<uint16_t>();
                return;
            case pb::UINT32:
                _u.uint32_val += value.get_numberic<uint32_t>();
                return;
            case pb::UINT64:
                _u.uint64_val += value.get_numberic<uint64_t>();
                return;
            case pb::FLOAT:
                _u.float_val+= value.get_numberic<float>();
                return;
            case pb::DOUBLE:
                _u.double_val+= value.get_numberic<double>();
                return;
            case pb::NULL_TYPE:
                *this = value;
                return;
            default:
                return;
        }
    }

    int64_t compare(const ExprValue& other) const {
        switch (type) {
            case pb::BOOL:
                return _u.bool_val - other._u.bool_val;
            case pb::INT8:
                return _u.int8_val - other._u.int8_val;
            case pb::INT16:
                return _u.int16_val - other._u.int16_val;
            case pb::INT32:
            case pb::TIME:
                return (int64_t)_u.int32_val - (int64_t)other._u.int32_val;
            case pb::INT64:
                return _u.int64_val > other._u.int64_val ? 1 :
                    (_u.int64_val < other._u.int64_val ? -1 : 0);
            case pb::UINT8:
                return _u.uint8_val - other._u.uint8_val;
            case pb::UINT16:
                return _u.uint16_val - other._u.uint16_val;
            case pb::UINT32:
            case pb::TIMESTAMP:
            case pb::DATE:
                return (int64_t)_u.uint32_val - (int64_t)other._u.uint32_val;
            case pb::UINT64:
            case pb::DATETIME:
                return _u.uint64_val > other._u.uint64_val ? 1 :
                    (_u.uint64_val < other._u.uint64_val ? -1 : 0);
            case pb::FLOAT:
                return _u.float_val > other._u.float_val ? 1 : 
                    (_u.float_val < other._u.float_val ? -1 : 0);
            case pb::DOUBLE:
                return _u.double_val > other._u.double_val ? 1 : 
                    (_u.double_val < other._u.double_val ? -1 : 0);
            case pb::STRING:
                return str_val.compare(other.str_val);
            case pb::NULL_TYPE:
                return -1;
            default:
                return 0;
        }
    }

    int64_t compare_diff_type(ExprValue& other) {
        if (type == other.type) {
            return compare(other);
        }
        if (is_int() && other.is_int()) {
            if (is_uint() || other.is_uint()) {
                cast_to(pb::UINT64);
                other.cast_to(pb::UINT64);
            } else {
                cast_to(pb::INT64);
                other.cast_to(pb::INT64);
            }
        } else if (is_datetime() || other.is_datetime()) {
            cast_to(pb::DATETIME);
            other.cast_to(pb::DATETIME);
        } else if (is_timestamp() || other.is_timestamp()) {
            cast_to(pb::TIMESTAMP);
            other.cast_to(pb::TIMESTAMP);
        } else if (is_date() || other.is_date()) {
            cast_to(pb::DATE);
            other.cast_to(pb::DATE);
        } else if (is_time() || other.is_time()) {
            cast_to(pb::TIME);
            other.cast_to(pb::TIME);
        } else if (is_double() || other.is_double()) {
            cast_to(pb::DOUBLE);
            other.cast_to(pb::DOUBLE);
        } else if (is_int() || other.is_int()) {
            cast_to(pb::DOUBLE);
            other.cast_to(pb::DOUBLE);
        } else {
            cast_to(pb::STRING);
            other.cast_to(pb::STRING);
        }
        return compare(other);
    }
    
    bool is_null() const { 
        return type == pb::NULL_TYPE;
    }

    bool is_bool() const {
        return type == pb::BOOL;
    }

    bool is_string() const {
        return type == pb::STRING;
    }

    bool is_double() const {
        return ::baikaldb::is_double(type);
    }

    bool is_int() const {
        return ::baikaldb::is_int(type);
    }

    bool is_uint() const {
        return ::baikaldb::is_uint(type);
    }

    bool is_datetime() const {
        return type == pb::DATETIME;
    }

    bool is_time() const {
        return type == pb::TIME;
    }

    bool is_timestamp() const {
        return type == pb::TIMESTAMP;
    }

    bool is_date() const {
        return type == pb::DATE;
    }

    bool is_hll() const {
        return type == pb::HLL;
    }

    bool is_numberic() const {
        return is_int() || is_bool() || is_double();
    }
    
    bool is_place_holder() const {
        return type == pb::PLACE_HOLDER;
    }

    SerializeStatus serialize_to_mysql_text_packet(char* buf, size_t size, size_t& len) const;

    static ExprValue Null() {
        ExprValue ret(pb::NULL_TYPE);
        return ret;
    }
    static ExprValue False() {
        ExprValue ret(pb::BOOL);
        ret._u.bool_val = false;
        return ret;
    }
    static ExprValue True() {
        ExprValue ret(pb::BOOL);
        ret._u.bool_val = true;
        return ret;
    }
    static ExprValue Now() {
        ExprValue tmp(pb::TIMESTAMP);
        tmp._u.uint32_val = time(NULL);
        tmp.cast_to(pb::DATETIME);
        timeval tv;
        gettimeofday(&tv, NULL);
        tmp._u.uint64_val |= tv.tv_usec;
        return tmp;
    }
};
}

/* vim: set ts=4 sw=4 sts=4 tw=100 */
