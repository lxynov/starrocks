---
title: StarRocks Type Systems
---

## 1. Overview of StarRocks Type System

**Topics:**

- StarRocks logical type system
- Type categories: scalar, complex, and special types
- Type compatibility and conversion rules
- Internal representation vs. external format mapping

**Key Files to Reference:**

- `be/src/types/logical_type.h` - Core type definitions
- `be/src/runtime/types.h` - Runtime type descriptors
- `be/src/types/date_value.h` - Date type implementation
- `be/src/types/timestamp_value.h` - Timestamp type implementation

## 2. StarRocks Data Types

StarRocks supports a comprehensive set of data types organized into several categories:

### 2.1 Numeric Types

**Integer Types:**

- `TINYINT` (8-bit signed integer, -128 to 127)
- `SMALLINT` (16-bit signed integer, -32,768 to 32,767)
- `INT` (32-bit signed integer, -2^31 to 2^31-1)
- `BIGINT` (64-bit signed integer, -2^63 to 2^63-1)
- `LARGEINT` (128-bit signed integer)
- Unsigned variants: `UNSIGNED_TINYINT`, `UNSIGNED_SMALLINT`, `UNSIGNED_INT`, `UNSIGNED_BIGINT`

**Floating-Point Types:**

- `FLOAT` (32-bit IEEE 754 single precision)
- `DOUBLE` (64-bit IEEE 754 double precision)

**Decimal Types:**

- `DECIMAL` / `DECIMALV2` (legacy decimal type with fixed scale 9)
- `DECIMAL32` (32-bit decimal, precision up to 9)
- `DECIMAL64` (64-bit decimal, precision up to 18)
- `DECIMAL128` (128-bit decimal, precision up to 38)
- `DECIMAL256` (256-bit decimal, precision up to 76)

### 2.2 String and Binary Types

- `CHAR(n)` (fixed-length character string, padded with spaces)
- `VARCHAR(n)` (variable-length character string)
- `STRING` (variable-length character string, alias for VARCHAR)
- `BINARY` (fixed-length binary data)
- `VARBINARY` (variable-length binary data)

### 2.3 Date and Time Types

- `DATE` (date value, stored as Julian day number)
- `DATETIME` (date and time value with microsecond precision)
- `TIME` (time value, stored as seconds since midnight)

### 2.4 Boolean Type

- `BOOLEAN` (true/false value, stored as uint8_t)

### 2.5 Complex Types

- `ARRAY<T>` (array of elements of type T)
- `MAP<K, V>` (map with key type K and value type V)
- `STRUCT<field1:type1, field2:type2, ...>` (structured type with named fields)

### 2.6 Special Types

- `JSON` (semi-structured JSON data)
- `VARIANT` (variant type for flexible schema)
- `HLL` (HyperLogLog sketch for approximate distinct count)
- `BITMAP` (bitmap for set operations)
- `OBJECT` (generic object type)
- `PERCENTILE` (percentile sketch)

## 3. Parquet Type System

Parquet uses a two-level type system:

### 3.1 Physical Types

Parquet defines the following physical storage types:

- `BOOLEAN` - Single bit per value
- `INT32` - 32-bit signed integer
- `INT64` - 64-bit signed integer
- `INT96` - 96-bit timestamp (legacy format)
- `FLOAT` - 32-bit IEEE 754 floating point
- `DOUBLE` - 64-bit IEEE 754 floating point
- `BYTE_ARRAY` - Variable-length byte array
- `FIXED_LEN_BYTE_ARRAY` - Fixed-length byte array

### 3.2 Logical Types

Parquet logical types provide semantic meaning to physical types:

- `STRING` - UTF-8 encoded string (on BYTE_ARRAY)
- `DATE` - Days since Unix epoch (on INT32)
- `TIMESTAMP` - Timestamp with time unit (MILLIS, MICROS, NANOS) (on INT64 or INT96)
- `DECIMAL` - Decimal number with precision and scale (on INT32, INT64, BYTE_ARRAY, or FIXED_LEN_BYTE_ARRAY)
- `INT` - Signed integer with bit width (on INT32 or INT64)
- `LIST` - List of elements (on nested group)
- `MAP` - Map of key-value pairs (on nested group)
- `STRUCT` - Structured type (on nested group)

## 4. StarRocks to Parquet Type Mapping

### 4.1 Reading from Parquet (Parquet → StarRocks)

When reading Parquet files, StarRocks maps Parquet types to StarRocks types with automatic type conversion when needed.

**Key Files:**

- `be/src/formats/parquet/column_converter.cpp` - Type conversion logic
- `be/src/formats/parquet/column_reader.cpp` - Column reading and type matching

#### 4.1.1 Direct Mappings (No Conversion Required)

| Parquet Physical Type | Parquet Logical Type | StarRocks Type | Notes |
|----------------------|---------------------|----------------|-------|
| BOOLEAN | None | BOOLEAN | Direct mapping |
| INT32 | None | INT | Direct mapping |
| INT64 | None | BIGINT | Direct mapping |
| FLOAT | None | FLOAT | Direct mapping |
| DOUBLE | None | DOUBLE | Direct mapping |
| BYTE_ARRAY | STRING | VARCHAR, CHAR | Direct mapping |
| BYTE_ARRAY | None | VARBINARY | Direct mapping |

#### 4.1.2 Type Conversions (Conversion Required)

**Integer Type Conversions:**

- Parquet `INT32` → StarRocks `TINYINT` (downcast with range check)
- Parquet `INT32` → StarRocks `SMALLINT` (downcast with range check)
- Parquet `INT32` → StarRocks `BIGINT` (upcast)
- Parquet `INT32` → StarRocks `DOUBLE` (numeric conversion)
- Parquet `INT64` → StarRocks `TINYINT`, `SMALLINT`, `INT` (downcast)
- Parquet `INT64` → StarRocks `BIGINT` (direct, but may need conversion for other integer types)

**Floating-Point Conversions:**

- Parquet `FLOAT` → StarRocks `DOUBLE` (upcast)

**Date/Time Conversions:**

- Parquet `INT32` (DATE logical type) → StarRocks `DATE`
  - Conversion: `starrocks_date._julian = parquet_int32 + date::UNIX_EPOCH_JULIAN`
- Parquet `INT32` → StarRocks `DATETIME` (when logical type indicates date)
  - Conversion: Days since epoch converted to timestamp
- Parquet `INT64` (TIMESTAMP logical type) → StarRocks `DATETIME`
  - Supports MILLIS, MICROS, NANOS time units
  - Handles timezone conversion (UTC to local timezone)
- Parquet `INT96` → StarRocks `DATETIME`
  - Legacy timestamp format
  - Converts 96-bit timestamp to StarRocks timestamp
  - Handles timezone adjustment
- Parquet `INT32` → StarRocks `TIME`
  - Conversion: `starrocks_time = parquet_int32 / 1000` (milliseconds to seconds)
- Parquet `INT64` → StarRocks `TIME`
  - Conversion: `starrocks_time = parquet_int64 / 1000000` (microseconds to seconds)

**Decimal Conversions:**

- Parquet `INT32` (DECIMAL) → StarRocks `DECIMAL32`, `DECIMAL64`, `DECIMAL128`, `DECIMALV2`
  - Handles scale conversion (scale up or scale down)
  - Precision preservation checks
- Parquet `INT64` (DECIMAL) → StarRocks `DECIMAL32`, `DECIMAL64`, `DECIMAL128`, `DECIMALV2`
  - Handles scale conversion
- Parquet `BYTE_ARRAY` (DECIMAL) → StarRocks `DECIMAL*`
  - Two's complement big-endian encoding
  - Supports up to 16 bytes (128 bits)
- Parquet `FIXED_LEN_BYTE_ARRAY` (DECIMAL) → StarRocks `DECIMAL*`
  - Fixed-length binary encoding
  - Supports various byte lengths (1-16 bytes)

**String Conversions:**

- Parquet `BYTE_ARRAY` → StarRocks `VARCHAR`, `CHAR`, `VARBINARY`
  - Direct byte copy for binary types
  - UTF-8 validation for string types

### 4.2 Writing to Parquet (StarRocks → Parquet)

When writing StarRocks data to Parquet format, the following mappings are used.

**Key Files:**

- `be/src/formats/parquet/file_writer.cpp` - Schema creation and type mapping
- `be/src/formats/parquet/level_builder.cpp` - Data writing logic

#### 4.2.1 Complete Type Mapping Table

| StarRocks Type | Parquet Physical Type | Parquet Logical Type | Notes |
|----------------|----------------------|---------------------|-------|
| BOOLEAN | BOOLEAN | None | Direct mapping |
| TINYINT | INT32 | Int(8, true) | Signed 8-bit integer |
| SMALLINT | INT32 | Int(16, true) | Signed 16-bit integer |
| INT | INT32 | Int(32, true) | Signed 32-bit integer |
| BIGINT | INT64 | Int(64, true) | Signed 64-bit integer |
| FLOAT | FLOAT | None | IEEE 754 single precision |
| DOUBLE | DOUBLE | None | IEEE 754 double precision |
| CHAR | BYTE_ARRAY | String() | UTF-8 encoded string |
| VARCHAR | BYTE_ARRAY | String() | UTF-8 encoded string |
| BINARY | BYTE_ARRAY | None | Raw binary data |
| VARBINARY | BYTE_ARRAY | None | Raw binary data |
| DATE | INT32 | Date() | Days since Unix epoch |
| DATETIME | INT64 or INT96 | Timestamp(MICROS) or None | Configurable: INT96 (legacy) or INT64 with MICROS |
| DECIMAL32 | INT32 or FIXED_LEN_BYTE_ARRAY | Decimal(precision, scale) | Modern encoding: INT32; Legacy: FLBA |
| DECIMAL64 | INT64 or FIXED_LEN_BYTE_ARRAY | Decimal(precision, scale) | Modern encoding: INT64; Legacy: FLBA |
| DECIMAL128 | FIXED_LEN_BYTE_ARRAY | Decimal(precision, scale) | Always uses FLBA (16 bytes) |
| DECIMAL256 | FIXED_LEN_BYTE_ARRAY | Decimal(precision, scale) | Uses FLBA with appropriate byte length |
| ARRAY | Nested Group | List() | Three-level structure: list → list → element |
| MAP | Nested Group | Map() | Key-value pairs |
| STRUCT | Nested Group | None | Group with named fields |

#### 4.2.2 Special Cases and Configuration

**DATETIME Encoding:**

- Default: `INT64` with `TIMESTAMP(MICROS)` logical type
- Legacy mode: `INT96` (when `use_int96_timestamp_encoding` is enabled)
- The INT96 format is compatible with older Parquet readers but less efficient

**Decimal Encoding:**

- Modern encoding (default for DECIMAL32/64): Uses native integer types (INT32/INT64)
  - More efficient storage and faster processing
  - Requires matching precision and scale
- Legacy encoding: Uses `FIXED_LEN_BYTE_ARRAY`
  - Always used for DECIMAL128 and DECIMAL256
  - Can be enabled for DECIMAL32/64 via `use_legacy_decimal_encoding`
  - Byte length calculated from precision: `ceil(precision * log2(10) / 8)`

**Array Encoding:**

- Parquet uses a three-level nested structure:
  1. Outer group (repetition: OPTIONAL) - the array column
  2. Middle group named "list" (repetition: REPEATED) - array elements
  3. Inner element (repetition: OPTIONAL) - individual array values

**Map Encoding:**

- Parquet uses a nested structure:
  1. Outer group (repetition: OPTIONAL) - the map column
  2. Inner group (repetition: REPEATED) - key-value pairs
  3. Key and value fields (repetition: OPTIONAL)

**Struct Encoding:**

- Parquet uses a group with named child fields
- Each field can have its own type and repetition level

## 5. Type Conversion Details

### 5.1 Numeric Type Conversions

**Integer Downcasting:**

- When reading Parquet INT32/INT64 into smaller StarRocks integer types, values are checked for overflow
- Conversion is performed using C++ static_cast, which may truncate values outside the target range
- No explicit range checking is performed during conversion (relies on data correctness)

**Integer Upcasting:**

- Upcasting (e.g., INT32 → BIGINT) is always safe and performed via direct assignment

**Floating-Point Conversion:**

- FLOAT → DOUBLE: Direct upcast, preserves precision
- INT32/INT64 → DOUBLE: Numeric conversion, may lose precision for very large integers

### 5.2 Decimal Type Conversions

**Scale Conversion:**

- When source and destination scales differ, unscaled values are adjusted:
  - Scale up: Multiply by `10^(dst_scale - src_scale)`
  - Scale down: Divide by `10^(src_scale - dst_scale)`
- Precision must be sufficient to avoid overflow

**Binary Decimal Encoding:**

- Decimals stored as BYTE_ARRAY or FIXED_LEN_BYTE_ARRAY use two's complement big-endian encoding
- The unscaled integer value is encoded as a signed integer in big-endian byte order
- Byte length is determined by precision: `ceil(precision * log2(10) / 8)`

### 5.3 Date/Time Conversions

**Date Conversion:**

- StarRocks DATE stores Julian day numbers
- Parquet DATE stores days since Unix epoch (1970-01-01)
- Conversion: `starrocks_julian = parquet_days + date::UNIX_EPOCH_JULIAN`

**Timestamp Conversion:**

- Parquet INT64 timestamps support multiple time units:
  - MILLIS: Milliseconds since epoch
  - MICROS: Microseconds since epoch (default for StarRocks writes)
  - NANOS: Nanoseconds since epoch
- Timezone handling:
  - If `isAdjustedToUTC` is true, timestamp is in UTC and converted to local timezone
  - Timezone offset is calculated using cctz library
  - Fast timezone conversion mode available for fixed-offset timezones

**INT96 Timestamp:**

- Legacy format used by older Parquet writers (e.g., Impala)
- 96-bit structure: 12 bytes
  - First 8 bytes (lo): nanoseconds since midnight
  - Last 4 bytes (hi): days since Unix epoch
- Conversion: `timestamp = (hi << 40) | (lo / 1000)` (converts nanoseconds to microseconds)

### 5.4 String and Binary Conversions

**String Encoding:**

- Parquet strings are UTF-8 encoded BYTE_ARRAY
- StarRocks VARCHAR/CHAR store UTF-8 strings
- No encoding conversion needed, direct byte copy
- CHAR types may be padded with spaces in StarRocks but stored without padding in Parquet

**Binary Data:**

- Parquet BYTE_ARRAY stores raw binary data
- StarRocks VARBINARY stores raw binary data
- Direct byte copy, no transformation

## 6. Type Compatibility and Limitations

### 6.1 Supported Conversions

**Fully Supported:**

- All numeric type conversions (with appropriate range)
- Date and timestamp conversions (with timezone support)
- Decimal conversions (with scale adjustment)
- String and binary conversions
- Complex type conversions (ARRAY, MAP, STRUCT)

### 6.2 Limitations and Unsupported Cases

**Unsupported Conversions:**

- Parquet INT32/INT64 to StarRocks LARGEINT (when value exceeds INT64 range)
- Parquet FIXED_LEN_BYTE_ARRAY to StarRocks CHAR (length mismatch issues)
- Parquet complex types to StarRocks scalar types
- Parquet BYTE_ARRAY (non-decimal) to StarRocks decimal types

**Precision Loss Scenarios:**

- Decimal conversions when destination precision is insufficient
- Floating-point to integer conversions (fractional part lost)
- Large integer to smaller integer conversions (overflow)

**Type Mismatch Handling:**

- When a conversion is not supported, StarRocks returns `Status::NotSupported`
- Error message includes both source and destination types
- Query execution fails with a clear error message

## 7. Implementation Details

### 7.1 Type Conversion Framework

**ColumnConverter Interface:**

- Base class: `ColumnConverter` in `be/src/formats/parquet/column_converter.h`
- Derived converters for specific type pairs:
  - `NumericToNumericConverter<SourceType, DestType>`
  - `PrimitiveToDecimalConverter<SourceType, DestType>`
  - `BinaryToDecimalConverter<DestType>`
  - `Int32ToDateConverter`
  - `Int32ToDateTimeConverter`
  - `Int64ToDateTimeConverter`
  - `Int96ToDateTimeConverter`
  - `Int32ToTimeConverter`
  - `Int64ToTimeConverter`

**Converter Factory:**

- `ColumnConverterFactory::create_converter()` analyzes Parquet field and StarRocks type descriptor
- Determines if conversion is needed and creates appropriate converter
- Handles nullable columns (all converters work with NullableColumn)

### 7.2 Schema Building

**Parquet Schema Creation:**

- `ParquetBuildHelper::make_schema_node()` creates Parquet schema nodes
- Maps StarRocks TypeDescriptor to Parquet schema elements
- Handles nested types (ARRAY, MAP, STRUCT) recursively
- Configurable options:
  - `use_int96_timestamp_encoding`: Use legacy INT96 for timestamps
  - `use_legacy_decimal_encoding`: Use FLBA for all decimals

### 7.3 Data Writing

**Level Builder:**

- `LevelBuilder` in `be/src/formats/parquet/level_builder.cpp` handles data writing
- Converts StarRocks columns to Parquet column chunks
- Manages repetition and definition levels for nested types
- Optimized for vectorized processing

## 8. Best Practices

### 8.1 Type Selection

**For Maximum Compatibility:**

- Use standard types (INT, BIGINT, DOUBLE, VARCHAR, DATE, DATETIME)
- Avoid legacy types (DECIMALV2, INT96 timestamps) when possible
- Use DECIMAL32/64/128 instead of DECIMALV2 for new schemas

**For Performance:**

- Use native integer types (INT32/INT64) for decimals when precision allows
- Use INT64 timestamps instead of INT96
- Use fixed-length types (CHAR, BINARY) when length is known and constant

### 8.2 Schema Design

**Decimal Precision:**

- Choose precision that fits in native types (DECIMAL32 for ≤9 digits, DECIMAL64 for ≤18 digits)
- Use DECIMAL128 for high-precision requirements (up to 38 digits)
- Consider scale when designing schemas to minimize conversions

**Timestamp Encoding:**

- Prefer INT64 with MICROS for new Parquet files
- Use INT96 only for compatibility with legacy systems
- Ensure timezone information is properly specified

**Complex Types:**

- Design nested structures to minimize nesting depth
- Use appropriate repetition levels (OPTIONAL vs REQUIRED vs REPEATED)
- Consider query patterns when designing ARRAY and MAP structures

## 9. Key Files Reference

**Type Definitions:**

- `be/src/types/logical_type.h` - StarRocks logical type enum and utilities
- `be/src/runtime/types.h` - TypeDescriptor class for runtime type information

**Parquet Reading:**

- `be/src/formats/parquet/column_converter.cpp` - Type conversion implementations
- `be/src/formats/parquet/column_reader.cpp` - Column reading and type matching
- `be/src/formats/parquet/column_converter.h` - Converter interface definitions

**Parquet Writing:**

- `be/src/formats/parquet/file_writer.cpp` - Schema creation and file writing
- `be/src/formats/parquet/level_builder.cpp` - Data writing and level management
- `be/src/formats/parquet/file_writer.h` - Writer interface definitions

**Date/Time Utilities:**

- `be/src/types/date_value.h` - Date type implementation
- `be/src/types/timestamp_value.h` - Timestamp type implementation
- `be/src/runtime/time_types.h` - Time-related utilities

**Decimal Utilities:**

- `be/src/util/decimal_types.h` - Decimal type definitions and utilities
