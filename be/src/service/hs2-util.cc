// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "service/hs2-util.h"

#include "common/logging.h"
#include "runtime/decimal-value.inline.h"
#include "runtime/raw-value.inline.h"
#include "runtime/types.h"

#include <gutil/strings/substitute.h>

#include "common/names.h"

using namespace apache::hive::service::cli;
using namespace impala;
using namespace strings;

inline int GetNullsRequiredSize(int numVals) {
  return (numvals + 7) / 8;
}

inline void SetNullsSize(uint32_t new_size, string* nulls) {
  nulls->resize(GetNullsRequiredSize(new_size));
}

// Set the null indicator bit for row 'row_idx', assuming this will be called for
// successive increasing values of row_idx. If 'is_null' is true, the row_idx'th bit will
// be set in 'nulls' (taking the LSB as bit 0). If 'is_null' is false, the row_idx'th bit
// will be unchanged. If 'nulls' does not contain 'row_idx' bits, it will be extended by
// one byte.
inline void SetNullBit(uint32_t row_idx, bool is_null, string* nulls) {
  DCHECK_LE(GetNullsRequiredSize(row_idx), nulls->size());
  int16_t mod_8 = row_idx % 8;
  if (mod_8 == 0) (*nulls) += '\0';
  (*nulls)[row_idx / 8] |= (1 << mod_8) * is_null;
}

inline void SetNullBitNoResize(uint32_t row_idx, bool is_null, string* nulls) {
  DCHECK_LE(GetNullsRequiredSize(row_idx+1), nulls->size());
  int16_t mod_8 = row_idx % 8;
  (*nulls)[row_idx / 8] |= (1 << mod_8) * is_null;
}

inline bool GetNullBit(const string& nulls, uint32_t row_idx) {
  DCHECK_LE(GetNullsRequiredSize(row_idx+1), nulls->size());
  return nulls[row_idx / 8] & (1 << row_idx % 8);
}

void impala::StitchNulls(uint32_t num_rows_before, uint32_t num_rows_added,
    uint32_t start_idx, const string& from, string* to) {
  to->reserve((num_rows_before + num_rows_added + 7) / 8);

  // TODO: This is very inefficient, since we could conceivably go one byte at a time
  // (although the operands should stay live in registers in the loop). However doing this
  // more efficiently leads to very complex code: we have to deal with the fact that
  // 'start_idx' and 'num_rows_before' might both lead to offsets into the null bitset
  // that don't start on a byte boundary. We should revisit this, ideally with a good
  // bitset implementation.
  for (int i = 0; i < num_rows_added; ++i) {
    SetNullBit(num_rows_before + i, GetNullBit(from, i + start_idx), to);
  }
}

template<typename S, typename T=S>
void AddValues(RowBatch * batch, ExprContext* expr_ctx, vector<T>& result,
    string* nulls, uint32_t src_start_idx, uint32_t result_start_idx,
    int num_vals) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;
  result.resize(new_size);
  SetNullsSize(new_size, nulls);
  for (int i=src_start_idx; i<source_end_idx; ++i) {
    void* value=expr_ctx->GetValue(batch->GetRow(i));
    result[result_idx]=(value == NULL ? static_cast<S>(0) : *reinterpret_cast<const S*>(value));
    SetNullBitNoResize(result_idx,value==NULL,nulls);
    ++result_idx;
  }
}

template<typename D>
void AddDecimalValues(RowBatch * batch, ExprContext* expr_ctx,
    vector<string>& result, const ColumnType& decimalType, string* nulls,
    uint32_t src_start_idx, uint32_t result_start_idx, int num_vals) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;
  result.resize(new_size);
  SetNullsSize(new_size, nulls);
  for (int i=src_start_idx; i<source_end_idx; ++i) {
    void* value=expr_ctx->GetValue(batch->GetRow(i));
    result[result_idx]=(value == NULL ? "" : reinterpret_cast<const D*>(value)->ToString(decimalType));
    SetNullBitNoResize(result_idx,value==NULL,nulls);
    ++result_idx;
  }
}

void AddTimestampValues(RowBatch * batch, ExprContext* expr_ctx,
    vector<string>& result, string* nulls, uint32_t src_start_idx,
    uint32_t result_start_idx, int num_vals ) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;
  result.resize(new_size);
  SetNullsSize(new_size, nulls);
  for (int i=src_start_idx; i<source_end_idx; ++i) {
    void* value=expr_ctx->GetValue(batch->GetRow(i));
    if (value!=NULL) {
      RawValue::PrintValue(value, TYPE_TIMESTAMP, -1,
          &(result[result_idx]));
    }
    SetNullBitNoResize(result_idx,value==NULL,nulls);
    ++result_idx;
  }
}

void AddStringValues(RowBatch * batch, ExprContext* expr_ctx,
    vector<string>& result, string* nulls, uint32_t src_start_idx,
    uint32_t result_start_idx, int num_vals ) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;
  result.resize(new_size);
  SetNullsSize(new_size, nulls);
  for (int i=src_start_idx; i<source_end_idx; ++i) {
    void* value=expr_ctx->GetValue(batch->GetRow(i));
    if (value!=NULL) {
      const StringValue * str_val=reinterpret_cast<const StringValue*>(value);
      result[result_idx].assign(static_cast<char*>(str_val->ptr), str_val->len);
    }
    SetNullBitNoResize(result_idx,value==NULL,nulls);
    ++result_idx;
  }
}

void AddCharValues(RowBatch * batch, ExprContext* expr_ctx,
    vector<string>& result, const ColumnType& char_type, string* nulls,
    uint32_t src_start_idx, uint32_t result_start_idx, int num_vals) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;
  result.resize(new_size);
  SetNullsSize(new_size, nulls);
  for (int i=src_start_idx; i<source_end_idx; ++i) {
    void* value=expr_ctx->GetValue(batch->GetRow(i));
    if (value!=NULL) result[result_idx].assign(StringValue::CharSlotToPtr(value, char_type), char_type.len);
    SetNullBitNoResize(result_idx,value==NULL,nulls);
    ++result_idx;
  }
}

// For V6 and above
void impala::TColumnValueToHS2TColumn(const TColumnValue& col_val,
    const TColumnType& type, uint32_t row_idx, thrift::TColumn* column) {
  string* nulls;
  bool is_null;
  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::NULL_TYPE:
    case TPrimitiveType::BOOLEAN:
      is_null = !col_val.__isset.bool_val;
      column->boolVal.values.push_back(col_val.bool_val);
      nulls = &column->boolVal.nulls;
      break;
    case TPrimitiveType::TINYINT:
      is_null = !col_val.__isset.byte_val;
      column->byteVal.values.push_back(col_val.byte_val);
      nulls = &column->byteVal.nulls;
      break;
    case TPrimitiveType::SMALLINT:
      is_null = !col_val.__isset.short_val;
      column->i16Val.values.push_back(col_val.short_val);
      nulls = &column->i16Val.nulls;
      break;
    case TPrimitiveType::INT:
      is_null = !col_val.__isset.int_val;
      column->i32Val.values.push_back(col_val.int_val);
      nulls = &column->i32Val.nulls;
      break;
    case TPrimitiveType::BIGINT:
      is_null = !col_val.__isset.long_val;
      column->i64Val.values.push_back(col_val.long_val);
      nulls = &column->i64Val.nulls;
      break;
    case TPrimitiveType::FLOAT:
    case TPrimitiveType::DOUBLE:
      is_null = !col_val.__isset.double_val;
      column->doubleVal.values.push_back(col_val.double_val);
      nulls = &column->doubleVal.nulls;
      break;
    case TPrimitiveType::TIMESTAMP:
    case TPrimitiveType::STRING:
    case TPrimitiveType::CHAR:
    case TPrimitiveType::VARCHAR:
    case TPrimitiveType::DECIMAL:
      is_null = !col_val.__isset.string_val;
      column->stringVal.values.push_back(col_val.string_val);
      nulls = &column->stringVal.nulls;
      break;
    default:
      DCHECK(false) << "Unhandled type: "
                    << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      return;
  }

  SetNullBit(row_idx, is_null, nulls);
}

// For V6 and above
void impala::TColumnValuesToHS2TColumn(const vector<const TColumnValue*> & col_vals,
    const TColumnType& type, uint32_t src_start_idx, uint32_t result_start_idx,
    int num_vals, thrift::TColumn* column) {
  uint32_t new_size=result_start_idx+num_vals;
  uint32_t source_end_idx=src_start_idx+num_vals;
  uint32_t result_idx=result_start_idx;

  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::NULL_TYPE:
    case TPrimitiveType::BOOLEAN: {
      auto& result=column->boolVal.values;
      auto nulls = &column->boolVal.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->bool_val;
        SetNullBitNoResize(result_idx,value->__isset.bool_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::TINYINT: {
      auto& result=column->byteVal.values;
      auto nulls = &column->byteVal.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->byte_val;
        SetNullBitNoResize(result_idx,value->__isset.byte_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::SMALLINT: {
      auto& result=column->i16Val.values;
      auto nulls = &column->i16Val.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->short_val;
        SetNullBitNoResize(result_idx,value->__isset.short_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::INT: {
      auto& result=column->i32Val.values;
      auto nulls = &column->i32Val.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->int_val;
        SetNullBitNoResize(result_idx,value->__isset.int_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::BIGINT: {
      auto& result=column->i64Val.values;
      auto nulls = &column->i64Val.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->long_val;
        SetNullBitNoResize(result_idx,value->__isset.long_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::FLOAT:
    case TPrimitiveType::DOUBLE: {
      auto& result=column->doubleVal.values;
      auto nulls = &column->doubleVal.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->double_val;
        SetNullBitNoResize(result_idx,value->__isset.double_val,nulls);
        ++result_idx;
      }
      break;
    }
    case TPrimitiveType::TIMESTAMP:
    case TPrimitiveType::STRING:
    case TPrimitiveType::CHAR:
    case TPrimitiveType::VARCHAR:
    case TPrimitiveType::DECIMAL: {
      auto& result=column->stringVal.values;
      auto nulls = &column->i64Val.nulls;
      result.resize(new_size);
      SetNullsSize(new_size,nulls);
      for (int i=src_start_idx; i<source_end_idx; ++i) {
        const TColumnValue* value=col_vals[i];
        result[result_idx]=value->long_val;
        SetNullBitNoResize(result_idx,value->__isset.long_val,nulls);
        ++result_idx;
      }
      break;
    }
    default:
      DCHECK(false) << "Unhandled type: "
                    << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      return;
  }
}

// For V6 and above
void impala::ExprValueToHS2TColumn(const void* value, const TColumnType& type,
    uint32_t row_idx, thrift::TColumn* column) {
  string* nulls;
  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::NULL_TYPE:
    case TPrimitiveType::BOOLEAN:
      column->boolVal.values.push_back(
          value == NULL ? false : *reinterpret_cast<const bool*>(value));
      nulls = &column->boolVal.nulls;
      break;
    case TPrimitiveType::TINYINT:
      column->byteVal.values.push_back(
          value == NULL ? 0 : *reinterpret_cast<const int8_t*>(value));
      nulls = &column->byteVal.nulls;
      break;
    case TPrimitiveType::SMALLINT:
      column->i16Val.values.push_back(
          value == NULL ? 0 : *reinterpret_cast<const int16_t*>(value));
      nulls = &column->i16Val.nulls;
      break;
    case TPrimitiveType::INT:
      column->i32Val.values.push_back(
          value == NULL ? 0 : *reinterpret_cast<const int32_t*>(value));
      nulls = &column->i32Val.nulls;
      break;
    case TPrimitiveType::BIGINT:
      column->i64Val.values.push_back(
          value == NULL ? 0 : *reinterpret_cast<const int64_t*>(value));
      nulls = &column->i64Val.nulls;
      break;
    case TPrimitiveType::FLOAT:
      column->doubleVal.values.push_back(
          value == NULL ? 0.f : *reinterpret_cast<const float*>(value));
      nulls = &column->doubleVal.nulls;
      break;
    case TPrimitiveType::DOUBLE:
      column->doubleVal.values.push_back(
          value == NULL ? 0.0 : *reinterpret_cast<const double*>(value));
      nulls = &column->doubleVal.nulls;
      break;
    case TPrimitiveType::TIMESTAMP:
      column->stringVal.values.push_back("");
      if (value != NULL) {
        RawValue::PrintValue(value, TYPE_TIMESTAMP, -1,
            &(column->stringVal.values.back()));
      }
      nulls = &column->stringVal.nulls;
      break;
    case TPrimitiveType::STRING:
    case TPrimitiveType::VARCHAR:
      column->stringVal.values.push_back("");
      if (value != NULL) {
        const StringValue* str_val = reinterpret_cast<const StringValue*>(value);
        column->stringVal.values.back().assign(
            static_cast<char*>(str_val->ptr), str_val->len);
      }
      nulls = &column->stringVal.nulls;
      break;
    case TPrimitiveType::CHAR:
      column->stringVal.values.push_back("");
      if (value != NULL) {
        ColumnType char_type = ColumnType::CreateCharType(type.types[0].scalar_type.len);
        column->stringVal.values.back().assign(
            StringValue::CharSlotToPtr(value, char_type), char_type.len);
      }
      nulls = &column->stringVal.nulls;
      break;
    case TPrimitiveType::DECIMAL: {
      // HiveServer2 requires decimal to be presented as string.
      column->stringVal.values.push_back("");
      const ColumnType& decimalType = ColumnType::FromThrift(type);
      if (value != NULL) {
        switch (decimalType.GetByteSize()) {
          case 4:
            column->stringVal.values.back() =
                reinterpret_cast<const Decimal4Value*>(value)->ToString(decimalType);
            break;
          case 8:
            column->stringVal.values.back() =
                reinterpret_cast<const Decimal8Value*>(value)->ToString(decimalType);
            break;
          case 16:
            column->stringVal.values.back() =
                reinterpret_cast<const Decimal16Value*>(value)->ToString(decimalType);
            break;
          default:
            DCHECK(false) << "bad type: " << decimalType;
        }
      }
      nulls = &column->stringVal.nulls;
      break;
    }
    default:
      DCHECK(false) << "Unhandled type: "
                    << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      return;
  }

  SetNullBit(row_idx, (value == NULL), nulls);
}

// For V6 and above
void impala::ExprValuesToHS2TColumn(RowBatch* rows, ExprContext* expr_ctx,
    const TColumnType& type, uint32_t src_start_idx, uint32_t result_start_idx,
    int num_vals, thrift::TColumn* column) {
  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::NULL_TYPE:
    case TPrimitiveType::BOOLEAN: {
      auto& result=column->boolVal.values;
      auto nulls = &column->boolVal.nulls;
      AddValues<bool>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::TINYINT: {
      auto& result=column->byteVal.values;
      auto nulls = &column->byteVal.nulls;
      AddValues<int8_t>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::SMALLINT: {
      auto& result=column->i16Val.values;
      auto nulls = &column->i16Val.nulls;
      AddValues<int16_t>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::INT: {
      auto& result=column->i32Val.values;
      auto nulls = &column->i32Val.nulls;
      AddValues<int32_t>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::BIGINT: {
      auto& result=column->i64Val.values;
      auto nulls = &column->i64Val.nulls;
      AddValues<int64_t>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::FLOAT: {
      auto& result=column->doubleVal.values;
      nulls = &column->doubleVal.nulls;
      AddValues<float,double>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::DOUBLE: {
      auto& result=column->doubleVal.values;
      auto nulls = &column->doubleVal.nulls;
      AddValues<double>(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::TIMESTAMP: {
      auto& result=column->stringVal.values;
      auto nulls = &column->stringVal.nulls;
      AddTimestampValues(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::STRING:
    case TPrimitiveType::VARCHAR: {
      auto& result=column->stringVal.values;
      auto nulls = &column->stringVal.nulls;
      AddStringValues(rows, expr_ctx, result, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::CHAR: {
      auto& result=column->stringVal.values;
      auto nulls = &column->stringVal.nulls;
      ColumnType char_type = ColumnType::CreateCharType(type.types[0].scalar_type.len);
      AddCharValues(rows, expr_ctx, result, char_type, nulls, src_start_idx, result_start_idx, num_vals);
      break;
    }
    case TPrimitiveType::DECIMAL: {
      // HiveServer2 requires decimal to be presented as string.
      auto& result=column->stringVal.values;
      auto nulls = &column->stringVal.nulls;
      const ColumnType& decimalType = ColumnType::FromThrift(type);
      switch (decimalType.GetByteSize()) {
        case 4:
          AddDecimalValues<Decimal4Value>(rows, expr_ctx, result, decimalType, nulls, src_start_idx, result_start_idx, num_vals);
          break;
        case 8:
          AddDecimalValues<Decimal8Value>(rows, expr_ctx, result, decimalType, nulls, src_start_idx, result_start_idx, num_vals);
          break;
        case 16:
          AddDecimalValues<Decimal16Value>(rows, expr_ctx, result, decimalType, nulls, src_start_idx, result_start_idx, num_vals);
          break;
        default:
          DCHECK(false) << "bad type: " << decimalType;
      }
      break;
    }
    default:
      DCHECK(false) << "Unhandled type: "
                    << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      return;
  }
}

// For V1 -> V5
void impala::TColumnValueToHS2TColumnValue(const TColumnValue& col_val,
    const TColumnType& type, thrift::TColumnValue* hs2_col_val) {
  // TODO: Handle complex types.
  DCHECK_EQ(1, type.types.size());
  DCHECK_EQ(TTypeNodeType::SCALAR, type.types[0].type);
  DCHECK_EQ(true, type.types[0].__isset.scalar_type);
  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::BOOLEAN:
      hs2_col_val->__isset.boolVal = true;
      hs2_col_val->boolVal.value = col_val.bool_val;
      hs2_col_val->boolVal.__isset.value = col_val.__isset.bool_val;
      break;
    case TPrimitiveType::TINYINT:
      hs2_col_val->__isset.byteVal = true;
      hs2_col_val->byteVal.value = col_val.byte_val;
      hs2_col_val->byteVal.__isset.value = col_val.__isset.byte_val;
      break;
    case TPrimitiveType::SMALLINT:
      hs2_col_val->__isset.i16Val = true;
      hs2_col_val->i16Val.value = col_val.short_val;
      hs2_col_val->i16Val.__isset.value = col_val.__isset.short_val;
      break;
    case TPrimitiveType::INT:
      hs2_col_val->__isset.i32Val = true;
      hs2_col_val->i32Val.value = col_val.int_val;
      hs2_col_val->i32Val.__isset.value = col_val.__isset.int_val;
      break;
    case TPrimitiveType::BIGINT:
      hs2_col_val->__isset.i64Val = true;
      hs2_col_val->i64Val.value = col_val.long_val;
      hs2_col_val->i64Val.__isset.value = col_val.__isset.long_val;
      break;
    case TPrimitiveType::FLOAT:
    case TPrimitiveType::DOUBLE:
      hs2_col_val->__isset.doubleVal = true;
      hs2_col_val->doubleVal.value = col_val.double_val;
      hs2_col_val->doubleVal.__isset.value = col_val.__isset.double_val;
      break;
    case TPrimitiveType::DECIMAL:
    case TPrimitiveType::STRING:
    case TPrimitiveType::TIMESTAMP:
    case TPrimitiveType::VARCHAR:
    case TPrimitiveType::CHAR:
      // HiveServer2 requires timestamp to be presented as string. Note that the .thrift
      // spec says it should be a BIGINT; AFAICT Hive ignores that and produces a string.
      hs2_col_val->__isset.stringVal = true;
      hs2_col_val->stringVal.__isset.value = col_val.__isset.string_val;
      if (col_val.__isset.string_val) {
        hs2_col_val->stringVal.value = col_val.string_val;
      }
      break;
    default:
      DCHECK(false) << "bad type: "
                     << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      break;
  }
}

// For V1 -> V5
void impala::ExprValueToHS2TColumnValue(const void* value, const TColumnType& type,
    thrift::TColumnValue* hs2_col_val) {
  bool not_null = (value != NULL);
  // TODO: Handle complex types.
  DCHECK_EQ(1, type.types.size());
  DCHECK_EQ(TTypeNodeType::SCALAR, type.types[0].type);
  DCHECK_EQ(1, type.types[0].__isset.scalar_type);
  switch (type.types[0].scalar_type.type) {
    case TPrimitiveType::NULL_TYPE:
      // Set NULLs in the bool_val.
      hs2_col_val->__isset.boolVal = true;
      hs2_col_val->boolVal.__isset.value = false;
      break;
    case TPrimitiveType::BOOLEAN:
      hs2_col_val->__isset.boolVal = true;
      if (not_null) hs2_col_val->boolVal.value = *reinterpret_cast<const bool*>(value);
      hs2_col_val->boolVal.__isset.value = not_null;
      break;
    case TPrimitiveType::TINYINT:
      hs2_col_val->__isset.byteVal = true;
      if (not_null) hs2_col_val->byteVal.value = *reinterpret_cast<const int8_t*>(value);
      hs2_col_val->byteVal.__isset.value = not_null;
      break;
    case TPrimitiveType::SMALLINT:
      hs2_col_val->__isset.i16Val = true;
      if (not_null) hs2_col_val->i16Val.value = *reinterpret_cast<const int16_t*>(value);
      hs2_col_val->i16Val.__isset.value = not_null;
      break;
    case TPrimitiveType::INT:
      hs2_col_val->__isset.i32Val = true;
      if (not_null) hs2_col_val->i32Val.value = *reinterpret_cast<const int32_t*>(value);
      hs2_col_val->i32Val.__isset.value = not_null;
      break;
    case TPrimitiveType::BIGINT:
      hs2_col_val->__isset.i64Val = true;
      if (not_null) hs2_col_val->i64Val.value = *reinterpret_cast<const int64_t*>(value);
      hs2_col_val->i64Val.__isset.value = not_null;
      break;
    case TPrimitiveType::FLOAT:
      hs2_col_val->__isset.doubleVal = true;
      if (not_null) hs2_col_val->doubleVal.value = *reinterpret_cast<const float*>(value);
      hs2_col_val->doubleVal.__isset.value = not_null;
      break;
    case TPrimitiveType::DOUBLE:
      hs2_col_val->__isset.doubleVal = true;
      if (not_null) {
        hs2_col_val->doubleVal.value = *reinterpret_cast<const double*>(value);
      }
      hs2_col_val->doubleVal.__isset.value = not_null;
      break;
    case TPrimitiveType::STRING:
    case TPrimitiveType::VARCHAR:
      hs2_col_val->__isset.stringVal = true;
      hs2_col_val->stringVal.__isset.value = not_null;
      if (not_null) {
        const StringValue* string_val = reinterpret_cast<const StringValue*>(value);
        hs2_col_val->stringVal.value.assign(static_cast<char*>(string_val->ptr),
                                            string_val->len);
      }
      break;
    case TPrimitiveType::CHAR:
      hs2_col_val->__isset.stringVal = true;
      hs2_col_val->stringVal.__isset.value = not_null;
      if (not_null) {
        ColumnType char_type = ColumnType::CreateCharType(type.types[0].scalar_type.len);
        hs2_col_val->stringVal.value.assign(
           StringValue::CharSlotToPtr(value, char_type), char_type.len);
      }
      break;
    case TPrimitiveType::TIMESTAMP:
      // HiveServer2 requires timestamp to be presented as string.
      hs2_col_val->__isset.stringVal = true;
      hs2_col_val->stringVal.__isset.value = not_null;
      if (not_null) {
        RawValue::PrintValue(value, TYPE_TIMESTAMP, -1, &(hs2_col_val->stringVal.value));
      }
      break;
    case TPrimitiveType::DECIMAL: {
      // HiveServer2 requires decimal to be presented as string.
      hs2_col_val->__isset.stringVal = true;
      hs2_col_val->stringVal.__isset.value = not_null;
      const ColumnType& decimalType = ColumnType::FromThrift(type);
      if (not_null) {
        switch (decimalType.GetByteSize()) {
          case 4:
            hs2_col_val->stringVal.value =
              reinterpret_cast<const Decimal4Value*>(value)->ToString(decimalType);
            break;
          case 8:
            hs2_col_val->stringVal.value =
              reinterpret_cast<const Decimal8Value*>(value)->ToString(decimalType);
            break;
          case 16:
            hs2_col_val->stringVal.value =
              reinterpret_cast<const Decimal16Value*>(value)->ToString(decimalType);
            break;
          default:
            DCHECK(false) << "bad type: " << decimalType;
        }
      }
      break;
    }
    default:
      DCHECK(false) << "bad type: "
                     << TypeToString(ThriftToType(type.types[0].scalar_type.type));
      break;
  }
}

template<typename T>
void PrintVal(const T& val, ostream* ss) {
  if (val.__isset.value) {
    (*ss) << val.value;
  } else {
    (*ss) << "NULL";
  }
}

// Specialisation for byte values that would otherwise be interpreted as character values,
// not integers, when printed to the stringstream.
template<>
void PrintVal(const apache::hive::service::cli::thrift::TByteValue& val, ostream* ss) {
  if (val.__isset.value) {
    (*ss) << static_cast<int16_t>(val.value);
  } else {
    (*ss) << "NULL";
  }
}

void impala::PrintTColumnValue(
    const apache::hive::service::cli::thrift::TColumnValue& colval, stringstream* out) {
  if (colval.__isset.boolVal) {
    if (colval.boolVal.__isset.value) {
      (*out) << ((colval.boolVal.value) ? "true" : "false");
    } else {
      (*out) << "NULL";
    }
  } else if (colval.__isset.doubleVal) {
    PrintVal(colval.doubleVal, out);
  } else if (colval.__isset.byteVal) {
    PrintVal(colval.byteVal, out);
  } else if (colval.__isset.i32Val) {
    PrintVal(colval.i32Val, out);
  } else if (colval.__isset.i16Val) {
    PrintVal(colval.i16Val, out);
  } else if (colval.__isset.i64Val) {
    PrintVal(colval.i64Val, out);
  } else if (colval.__isset.stringVal) {
    PrintVal(colval.stringVal, out);
  } else {
    (*out) << "NULL";
  }
}
