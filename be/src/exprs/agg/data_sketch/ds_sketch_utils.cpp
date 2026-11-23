// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "exprs/agg/data_sketch/ds_sketch_utils.h"

#include <fmt/format.h>

#include "base/hash/unaligned_access.h"
#include "column/chunk.h"
#include "column/column_helper.h"
#include "column/column_viewer.h"
#include "common/object_pool.h"
#include "exprs/cast_expr.h"
#include "exprs/column_ref.h"
#include "runtime/descriptors.h"

namespace starrocks {

DsItemType DsItemType::from(const TypeDescriptor& type_desc) {
    return from(type_desc.type, type_desc.precision, type_desc.scale);
}

DsItemType DsItemType::from(LogicalType type, int precision, int scale) {
    DsItemType item_type;
    item_type.type = type == TYPE_CHAR ? TYPE_VARCHAR : type;
    switch (type) {
    case TYPE_DECIMALV2:
        // DECIMALV2 has a single layout, whatever the declared precision and scale.
        item_type.precision = 27;
        item_type.scale = 9;
        break;
    case TYPE_DECIMAL32:
    case TYPE_DECIMAL64:
    case TYPE_DECIMAL128:
    case TYPE_DECIMAL256:
        item_type.precision = precision;
        item_type.scale = scale;
        break;
    default:
        break;
    }
    return item_type;
}

TypeDescriptor DsItemType::to_type_descriptor() const {
    return TypeDescriptor::from_logical_type(type, precision, scale);
}

std::string DsItemType::to_string() const {
    // A decimal is named with its precision and scale; a string without the length it does not have here.
    if (precision > 0) {
        return to_type_descriptor().debug_string();
    }
    return logical_type_to_string(type);
}

void DsSketchHeader::encode(uint8_t magic, const DsItemType& item_type, uint8_t* dst) {
    dst[0] = magic;
    dst[1] = VERSION;
    dst[2] = static_cast<uint8_t>(item_type.precision);
    dst[3] = static_cast<uint8_t>(item_type.scale);
    unaligned_store<int32_t>(dst + 4, static_cast<int32_t>(to_thrift(item_type.type)));
}

Status DsSketchHeader::decode(const Slice& src, uint8_t expected_magic, DsItemType* item_type) {
    if (src.size < SIZE) {
        return Status::InvalidArgument(fmt::format("sketch is too short: {} bytes", src.size));
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(src.data);
    if (bytes[0] != expected_magic) {
        return Status::InvalidArgument("not a sketch of the expected kind");
    }
    if (bytes[1] != VERSION) {
        return Status::InvalidArgument(fmt::format("unsupported sketch version {}", bytes[1]));
    }
    const LogicalType type = thrift_to_type(static_cast<TPrimitiveType::type>(unaligned_load<int32_t>(bytes + 4)));
    const int precision = bytes[2];
    const int scale = bytes[3];
    if (precision > TypeDescriptor::MAX_PRECISION || scale > precision) {
        return Status::InvalidArgument(
                fmt::format("invalid sketch value type: precision {}, scale {}", precision, scale));
    }
    *item_type = DsItemType::from(type, precision, scale);
    return Status::OK();
}

Status ds_values_to_strings(const DsItemType& item_type, const ColumnPtr& values, std::vector<std::string>* strings) {
    const size_t num_values = values->size();
    strings->clear();
    strings->reserve(num_values);
    if (num_values == 0) {
        return Status::OK();
    }

    ColumnPtr rendered = values;
    const TypeDescriptor varchar_type = TypeDescriptor::create_varchar_type(TypeDescriptor::MAX_VARCHAR_LENGTH);
    if (item_type.type != TYPE_VARCHAR) {
        // Going through the cast expression rather than formatting here keeps the text identical to what a query
        // returns for the same value, including the float, decimal and datetime corner cases.
        ObjectPool pool;
        const TypeDescriptor value_type = item_type.to_type_descriptor();
        const SlotId slot_id = 0;
        SlotDescriptor slot_desc(slot_id, "", value_type);
        Expr* column_ref = pool.add(new ColumnRef(&slot_desc));
        Expr* cast_expr = VectorizedCastExprFactory::from_type(value_type, varchar_type, column_ref, &pool);
        if (cast_expr == nullptr) {
            return Status::NotSupported(
                    fmt::format("sketch values of type {} cannot be rendered", item_type.to_string()));
        }
        Chunk chunk;
        chunk.append_column(values, slot_id);
        ASSIGN_OR_RETURN(rendered, cast_expr->evaluate_checked(nullptr, &chunk));
        rendered = ColumnHelper::unfold_const_column(varchar_type, num_values, rendered);
    }

    ColumnViewer<TYPE_VARCHAR> viewer(rendered);
    for (size_t i = 0; i < num_values; ++i) {
        if (viewer.is_null(i)) {
            return Status::InternalError(
                    fmt::format("a sketch value of type {} has no text form", item_type.to_string()));
        }
        strings->emplace_back(viewer.value(i).to_string());
    }
    return Status::OK();
}

void ds_append_json_string(std::string_view value, std::string* json) {
    json->push_back('"');
    for (const char c : value) {
        switch (c) {
        case '"':
            json->append("\\\"");
            break;
        case '\\':
            json->append("\\\\");
            break;
        case '\n':
            json->append("\\n");
            break;
        case '\r':
            json->append("\\r");
            break;
        case '\t':
            json->append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                fmt::format_to(std::back_inserter(*json), "\\u{:04x}", static_cast<unsigned char>(c));
            } else {
                json->push_back(c);
            }
        }
    }
    json->push_back('"');
}

} // namespace starrocks
