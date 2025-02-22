//===- AttributeDetail.h - MLIR Affine Map details Class --------*- C++ -*-===//
//
// Copyright 2019 The MLIR Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// =============================================================================
//
// This holds implementation details of Attribute.
//
//===----------------------------------------------------------------------===//

#ifndef ATTRIBUTEDETAIL_H_
#define ATTRIBUTEDETAIL_H_

#include "mlir/IR/AffineMap.h"
#include "mlir/IR/Attributes.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/Identifier.h"
#include "mlir/IR/IntegerSet.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/StandardTypes.h"
#include "mlir/Support/StorageUniquer.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/TrailingObjects.h"

namespace mlir {
namespace detail {
/// Opaque Attribute Storage and Uniquing.
struct OpaqueAttributeStorage : public AttributeStorage {
  OpaqueAttributeStorage(Identifier dialectNamespace, StringRef attrData)
      : dialectNamespace(dialectNamespace), attrData(attrData) {}

  /// The hash key used for uniquing.
  using KeyTy = std::pair<Identifier, StringRef>;
  bool operator==(const KeyTy &key) const {
    return key == KeyTy(dialectNamespace, attrData);
  }

  static OpaqueAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                           const KeyTy &key) {
    return new (allocator.allocate<OpaqueAttributeStorage>())
        OpaqueAttributeStorage(key.first, allocator.copyInto(key.second));
  }

  // The dialect namespace.
  Identifier dialectNamespace;

  // The parser attribute data for this opaque attribute.
  StringRef attrData;
};

/// An attribute representing a boolean value.
struct BoolAttributeStorage : public AttributeStorage {
  using KeyTy = std::pair<MLIRContext *, bool>;

  BoolAttributeStorage(Type type, bool value)
      : AttributeStorage(type), value(value) {}

  /// We only check equality for and hash with the boolean key parameter.
  bool operator==(const KeyTy &key) const { return key.second == value; }
  static unsigned hashKey(const KeyTy &key) {
    return llvm::hash_value(key.second);
  }

  static BoolAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                         const KeyTy &key) {
    return new (allocator.allocate<BoolAttributeStorage>())
        BoolAttributeStorage(IntegerType::get(1, key.first), key.second);
  }

  bool value;
};

/// An attribute representing a integral value.
struct IntegerAttributeStorage final
    : public AttributeStorage,
      public llvm::TrailingObjects<IntegerAttributeStorage, uint64_t> {
  using KeyTy = std::pair<Type, APInt>;

  IntegerAttributeStorage(Type type, size_t numObjects)
      : AttributeStorage(type), numObjects(numObjects) {
    assert((type.isIndex() || type.isa<IntegerType>()) && "invalid type");
  }

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key == KeyTy(getType(), getValue());
  }
  static unsigned hashKey(const KeyTy &key) {
    return llvm::hash_combine(key.first, llvm::hash_value(key.second));
  }

  /// Construct a new storage instance.
  static IntegerAttributeStorage *
  construct(AttributeStorageAllocator &allocator, const KeyTy &key) {
    Type type;
    APInt value;
    std::tie(type, value) = key;

    auto elements = ArrayRef<uint64_t>(value.getRawData(), value.getNumWords());
    auto size =
        IntegerAttributeStorage::totalSizeToAlloc<uint64_t>(elements.size());
    auto rawMem = allocator.allocate(size, alignof(IntegerAttributeStorage));
    auto result = ::new (rawMem) IntegerAttributeStorage(type, elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            result->getTrailingObjects<uint64_t>());
    return result;
  }

  /// Returns an APInt representing the stored value.
  APInt getValue() const {
    if (getType().isIndex())
      return APInt(64, {getTrailingObjects<uint64_t>(), numObjects});
    return APInt(getType().getIntOrFloatBitWidth(),
                 {getTrailingObjects<uint64_t>(), numObjects});
  }

  size_t numObjects;
};

/// An attribute representing a floating point value.
struct FloatAttributeStorage final
    : public AttributeStorage,
      public llvm::TrailingObjects<FloatAttributeStorage, uint64_t> {
  using KeyTy = std::pair<Type, APFloat>;

  FloatAttributeStorage(const llvm::fltSemantics &semantics, Type type,
                        size_t numObjects)
      : AttributeStorage(type), semantics(semantics), numObjects(numObjects) {}

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key.first == getType() && key.second.bitwiseIsEqual(getValue());
  }
  static unsigned hashKey(const KeyTy &key) {
    return llvm::hash_combine(key.first, llvm::hash_value(key.second));
  }

  /// Construct a key with a type and double.
  static KeyTy getKey(Type type, double value) {
    // Treat BF16 as double because it is not supported in LLVM's APFloat.
    // TODO(b/121118307): add BF16 support to APFloat?
    if (type.isBF16() || type.isF64())
      return KeyTy(type, APFloat(value));

    // This handles, e.g., F16 because there is no APFloat constructor for it.
    bool unused;
    APFloat val(value);
    val.convert(type.cast<FloatType>().getFloatSemantics(),
                APFloat::rmNearestTiesToEven, &unused);
    return KeyTy(type, val);
  }

  /// Construct a new storage instance.
  static FloatAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                          const KeyTy &key) {
    const auto &apint = key.second.bitcastToAPInt();

    // Here one word's bitwidth equals to that of uint64_t.
    auto elements = ArrayRef<uint64_t>(apint.getRawData(), apint.getNumWords());

    auto byteSize =
        FloatAttributeStorage::totalSizeToAlloc<uint64_t>(elements.size());
    auto rawMem = allocator.allocate(byteSize, alignof(FloatAttributeStorage));
    auto result = ::new (rawMem) FloatAttributeStorage(
        key.second.getSemantics(), key.first, elements.size());
    std::uninitialized_copy(elements.begin(), elements.end(),
                            result->getTrailingObjects<uint64_t>());
    return result;
  }

  /// Returns an APFloat representing the stored value.
  APFloat getValue() const {
    auto val = APInt(APFloat::getSizeInBits(semantics),
                     {getTrailingObjects<uint64_t>(), numObjects});
    return APFloat(semantics, val);
  }

  const llvm::fltSemantics &semantics;
  size_t numObjects;
};

/// An attribute representing a string value.
struct StringAttributeStorage : public AttributeStorage {
  using KeyTy = StringRef;

  StringAttributeStorage(StringRef value) : value(value) {}

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == value; }

  /// Construct a new storage instance.
  static StringAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                           const KeyTy &key) {
    return new (allocator.allocate<StringAttributeStorage>())
        StringAttributeStorage(allocator.copyInto(key));
  }

  StringRef value;
};

/// An attribute representing an array of other attributes.
struct ArrayAttributeStorage : public AttributeStorage {
  using KeyTy = ArrayRef<Attribute>;

  ArrayAttributeStorage(ArrayRef<Attribute> value) : value(value) {}

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == value; }

  /// Construct a new storage instance.
  static ArrayAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                          const KeyTy &key) {
    return new (allocator.allocate<ArrayAttributeStorage>())
        ArrayAttributeStorage(allocator.copyInto(key));
  }

  ArrayRef<Attribute> value;
};

/// An attribute representing a dictionary of sorted named attributes.
struct DictionaryAttributeStorage final
    : public AttributeStorage,
      private llvm::TrailingObjects<DictionaryAttributeStorage,
                                    NamedAttribute> {
  using KeyTy = ArrayRef<NamedAttribute>;

  /// Given a list of NamedAttribute's, canonicalize the list (sorting
  /// by name) and return the unique'd result.
  static DictionaryAttributeStorage *get(ArrayRef<NamedAttribute> attrs);

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == getElements(); }

  /// Construct a new storage instance.
  static DictionaryAttributeStorage *
  construct(AttributeStorageAllocator &allocator, const KeyTy &key) {
    auto size = DictionaryAttributeStorage::totalSizeToAlloc<NamedAttribute>(
        key.size());
    auto rawMem = allocator.allocate(size, alignof(NamedAttribute));

    // Initialize the storage and trailing attribute list.
    auto result = ::new (rawMem) DictionaryAttributeStorage(key.size());
    std::uninitialized_copy(key.begin(), key.end(),
                            result->getTrailingObjects<NamedAttribute>());
    return result;
  }

  /// Return the elements of this dictionary attribute.
  ArrayRef<NamedAttribute> getElements() const {
    return {getTrailingObjects<NamedAttribute>(), numElements};
  }

private:
  friend class llvm::TrailingObjects<DictionaryAttributeStorage,
                                     NamedAttribute>;

  // This is used by the llvm::TrailingObjects base class.
  size_t numTrailingObjects(OverloadToken<NamedAttribute>) const {
    return numElements;
  }
  DictionaryAttributeStorage(unsigned numElements) : numElements(numElements) {}

  /// This is the number of attributes.
  const unsigned numElements;
};

// An attribute representing a reference to an affine map.
struct AffineMapAttributeStorage : public AttributeStorage {
  using KeyTy = AffineMap;

  AffineMapAttributeStorage(AffineMap value)
      : AttributeStorage(IndexType::get(value.getContext())), value(value) {}

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == value; }

  /// Construct a new storage instance.
  static AffineMapAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    return new (allocator.allocate<AffineMapAttributeStorage>())
        AffineMapAttributeStorage(key);
  }

  AffineMap value;
};

// An attribute representing a reference to an integer set.
struct IntegerSetAttributeStorage : public AttributeStorage {
  using KeyTy = IntegerSet;

  IntegerSetAttributeStorage(IntegerSet value) : value(value) {}

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == value; }

  /// Construct a new storage instance.
  static IntegerSetAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    return new (allocator.allocate<IntegerSetAttributeStorage>())
        IntegerSetAttributeStorage(key);
  }

  IntegerSet value;
};

/// An attribute representing a reference to a type.
struct TypeAttributeStorage : public AttributeStorage {
  using KeyTy = Type;

  TypeAttributeStorage(Type value) : value(value) {}

  /// Key equality function.
  bool operator==(const KeyTy &key) const { return key == value; }

  /// Construct a new storage instance.
  static TypeAttributeStorage *construct(AttributeStorageAllocator &allocator,
                                         KeyTy key) {
    return new (allocator.allocate<TypeAttributeStorage>())
        TypeAttributeStorage(key);
  }

  Type value;
};

/// An attribute representing a reference to a vector or tensor constant,
/// inwhich all elements have the same value.
struct SplatElementsAttributeStorage : public AttributeStorage {
  using KeyTy = std::pair<Type, Attribute>;

  SplatElementsAttributeStorage(Type type, Attribute elt)
      : AttributeStorage(type), elt(elt) {}

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key == std::make_pair(getType(), elt);
  }

  /// Construct a new storage instance.
  static SplatElementsAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    return new (allocator.allocate<SplatElementsAttributeStorage>())
        SplatElementsAttributeStorage(key.first, key.second);
  }

  Attribute elt;
};

/// An attribute representing a reference to a dense vector or tensor object.
struct DenseElementsAttributeStorage : public AttributeStorage {
  using KeyTy = std::pair<Type, ArrayRef<char>>;

  DenseElementsAttributeStorage(Type ty, ArrayRef<char> data,
                                bool isSplat = false)
      : AttributeStorage(ty), data(data), isSplat(isSplat) {}

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key == KeyTy(getType(), data);
  }

  /// Construct a new storage instance.
  static DenseElementsAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    // If the data buffer is non-empty, we copy it into the allocator.
    ArrayRef<char> data = allocator.copyInto(key.second);
    return new (allocator.allocate<DenseElementsAttributeStorage>())
        DenseElementsAttributeStorage(key.first, data);
  }

  ArrayRef<char> data;
  bool isSplat;
};

/// An attribute representing a reference to a tensor constant with opaque
/// content.
struct OpaqueElementsAttributeStorage : public AttributeStorage {
  using KeyTy = std::tuple<Type, Dialect *, StringRef>;

  OpaqueElementsAttributeStorage(Type type, Dialect *dialect, StringRef bytes)
      : AttributeStorage(type), dialect(dialect), bytes(bytes) {}

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key == std::make_tuple(getType(), dialect, bytes);
  }
  static unsigned hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key), std::get<1>(key),
                              std::get<2>(key));
  }

  /// Construct a new storage instance.
  static OpaqueElementsAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    // TODO(b/131468830): Provide a way to avoid copying content of large opaque
    // tensors This will likely require a new reference attribute kind.
    return new (allocator.allocate<OpaqueElementsAttributeStorage>())
        OpaqueElementsAttributeStorage(std::get<0>(key), std::get<1>(key),
                                       allocator.copyInto(std::get<2>(key)));
  }

  Dialect *dialect;
  StringRef bytes;
};

/// An attribute representing a reference to a sparse vector or tensor object.
struct SparseElementsAttributeStorage : public AttributeStorage {
  using KeyTy = std::tuple<Type, DenseIntElementsAttr, DenseElementsAttr>;

  SparseElementsAttributeStorage(Type type, DenseIntElementsAttr indices,
                                 DenseElementsAttr values)
      : AttributeStorage(type), indices(indices), values(values) {}

  /// Key equality and hash functions.
  bool operator==(const KeyTy &key) const {
    return key == std::make_tuple(getType(), indices, values);
  }
  static unsigned hashKey(const KeyTy &key) {
    return llvm::hash_combine(std::get<0>(key), std::get<1>(key),
                              std::get<2>(key));
  }

  /// Construct a new storage instance.
  static SparseElementsAttributeStorage *
  construct(AttributeStorageAllocator &allocator, KeyTy key) {
    return new (allocator.allocate<SparseElementsAttributeStorage>())
        SparseElementsAttributeStorage(std::get<0>(key), std::get<1>(key),
                                       std::get<2>(key));
  }

  DenseIntElementsAttr indices;
  DenseElementsAttr values;
};
} // namespace detail
} // namespace mlir

#endif // ATTRIBUTEDETAIL_H_
