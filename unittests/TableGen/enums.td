//===-- enums.td - EnumsGen test definition file -----------*- tablegen -*-===//
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

include "mlir/IR/OpBase.td"

def CaseA: EnumAttrCase<"CaseA", 0>;
def CaseB: EnumAttrCase<"CaseB", 10>;

def MyEnum: EnumAttr<"MyEnum", "A test enum", [CaseA, CaseB]> {
  let cppNamespace = "Outer::Inner";
  let stringToSymbolFnName = "ConvertToEnum";
  let symbolToStringFnName = "ConvertToString";
}

def Uint64Enum : EnumAttr<"Uint64Enum", "A test enum", [CaseA, CaseB]> {
  let underlyingType = "uint64_t";
}
