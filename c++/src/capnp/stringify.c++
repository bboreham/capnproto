// Copyright (c) 2013-2014 Sandstorm Development Group, Inc. and contributors
// Licensed under the MIT License:
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "dynamic.h"
#include <kj/debug.h>
#include <kj/vector.h>
#if _MSC_VER
#include <malloc.h>		// for _alloca()
#endif

namespace capnp {

namespace {

static const char HEXDIGITS[] = "0123456789abcdef";

enum PrintMode {
  BARE,
  // The value is planned to be printed on its own line, unless it is very short and contains
  // no inner newlines.

  PREFIXED,
  // The value is planned to be printed with a prefix, like "memberName = " (a struct field).

  PARENTHESIZED
  // The value is printed in parenthesized (a union value).
};

enum class PrintKind {
  LIST,
  RECORD
};

class Indent {
public:
  explicit Indent(bool enable): amount(enable ? 1 : 0) {}

  Indent next() {
    return Indent(amount == 0 ? 0 : amount + 1);
  }

  kj::StringTree delimit(kj::Array<kj::StringTree> items, PrintMode mode, PrintKind kind) {
    if (amount == 0 || canPrintAllInline(items, kind)) {
      return kj::StringTree(kj::mv(items), ", ");
    } else {
#if _MSC_VER
      char *delim = static_cast<char*>(_alloca(amount * 2 + 3));
#else
      char delim[amount * 2 + 3];
#endif
      delim[0] = ',';
      delim[1] = '\n';
      memset(delim + 2, ' ', amount * 2);
      delim[amount * 2 + 2] = '\0';

      // If the outer value isn't being printed on its own line, we need to add a newline/indent
      // before the first item, otherwise we only add a space on the assumption that it is preceded
      // by an open bracket or parenthesis.
      return kj::strTree(mode == BARE ? " " : delim + 1,
          kj::StringTree(kj::mv(items), kj::StringPtr(delim, amount * 2 + 2)), ' ');
    }
  }

private:
  uint amount;

  explicit Indent(uint amount): amount(amount) {}

  static constexpr size_t maxInlineValueSize = 24;
  static constexpr size_t maxInlineRecordSize = 64;

  static bool canPrintInline(const kj::StringTree& text) {
    if (text.size() > maxInlineValueSize) {
      return false;
    }

    char flat[maxInlineValueSize + 1];
    text.flattenTo(flat);
    flat[text.size()] = '\0';
    if (strchr(flat, '\n') != nullptr) {
      return false;
    }

    return true;
  }

  static bool canPrintAllInline(const kj::Array<kj::StringTree>& items, PrintKind kind) {
    size_t totalSize = 0;
    for (auto& item: items) {
      if (!canPrintInline(item)) return false;
      if (kind == PrintKind::RECORD) {
        totalSize += item.size();
        if (totalSize > maxInlineRecordSize) return false;
      }
    }
    return true;
  }
};

static schema::Type::Which whichFieldType(const StructSchema::Field& field) {
  auto proto = field.getProto();
  switch (proto.which()) {
    case schema::Field::SLOT:
      return proto.getSlot().getType().which();
    case schema::Field::GROUP:
      return schema::Type::STRUCT;
  }
  KJ_UNREACHABLE;
}

static kj::StringTree print(const DynamicValue::Reader& value,
                            schema::Type::Which which, Indent indent,
                            PrintMode mode) {
  switch (value.getType()) {
    case DynamicValue::UNKNOWN:
      return kj::strTree("?");
    case DynamicValue::VOID:
      return kj::strTree("void");
    case DynamicValue::BOOL:
      return kj::strTree(value.as<bool>() ? "true" : "false");
    case DynamicValue::INT:
      return kj::strTree(value.as<int64_t>());
    case DynamicValue::UINT:
      return kj::strTree(value.as<uint64_t>());
    case DynamicValue::FLOAT:
      if (which == schema::Type::FLOAT32) {
        return kj::strTree(value.as<float>());
      } else {
        return kj::strTree(value.as<double>());
      }
    case DynamicValue::TEXT:
    case DynamicValue::DATA: {
      // TODO(someday):  Data probably shouldn't be printed as a string.
      kj::ArrayPtr<const char> chars;
      if (value.getType() == DynamicValue::DATA) {
        auto reader = value.as<Data>();
        chars = kj::arrayPtr(reinterpret_cast<const char*>(reader.begin()), reader.size());
      } else {
        chars = value.as<Text>();
      }

      kj::Vector<char> escaped(chars.size());

      for (char c: chars) {
        switch (c) {
          case '\a': escaped.addAll(kj::StringPtr("\\a")); break;
          case '\b': escaped.addAll(kj::StringPtr("\\b")); break;
          case '\f': escaped.addAll(kj::StringPtr("\\f")); break;
          case '\n': escaped.addAll(kj::StringPtr("\\n")); break;
          case '\r': escaped.addAll(kj::StringPtr("\\r")); break;
          case '\t': escaped.addAll(kj::StringPtr("\\t")); break;
          case '\v': escaped.addAll(kj::StringPtr("\\v")); break;
          case '\'': escaped.addAll(kj::StringPtr("\\\'")); break;
          case '\"': escaped.addAll(kj::StringPtr("\\\"")); break;
          case '\\': escaped.addAll(kj::StringPtr("\\\\")); break;
          default:
            if (c < 0x20) {
              escaped.add('\\');
              escaped.add('x');
              uint8_t c2 = c;
              escaped.add(HEXDIGITS[c2 / 16]);
              escaped.add(HEXDIGITS[c2 % 16]);
            } else {
              escaped.add(c);
            }
            break;
        }
      }
      return kj::strTree('"', escaped, '"');
    }
    case DynamicValue::LIST: {
      auto listValue = value.as<DynamicList>();
	  auto which = listValue.getSchema().whichElementType();
#ifdef MSVC_HACKS
	  // Open-code what KJ_MAP does because MSVC can't instantiate the template
	  auto builder = kj::heapArrayBuilder<kj::StringTree>(listValue.size());
	  for (auto iter = listValue.begin(); iter != listValue.end(); ++iter) {
		  builder.add(print(*iter, which, indent.next(), BARE));
	  }
	  kj::Array<kj::StringTree> elements = builder.finish();
#else
      kj::Array<kj::StringTree> elements = KJ_MAP(element, listValue) {
        return print(element, which, indent.next(), BARE);
      };
#endif
      return kj::strTree('[', indent.delimit(kj::mv(elements), mode, PrintKind::LIST), ']');
    }
    case DynamicValue::ENUM: {
      auto enumValue = value.as<DynamicEnum>();
      KJ_IF_MAYBE(enumerant, enumValue.getEnumerant()) {
        return kj::strTree(enumerant->getProto().getName());
      } else {
        // Unknown enum value; output raw number.
        return kj::strTree('(', enumValue.getRaw(), ')');
      }
      break;
    }
    case DynamicValue::STRUCT: {
      auto structValue = value.as<DynamicStruct>();
      auto unionFields = structValue.getSchema().getUnionFields();
      auto nonUnionFields = structValue.getSchema().getNonUnionFields();

      kj::Vector<kj::StringTree> printedFields(nonUnionFields.size() + (unionFields.size() != 0));

      // We try to write the union field, if any, in proper order with the rest.
      auto which = structValue.which();

      kj::StringTree unionValue;
      KJ_IF_MAYBE(field, which) {
        // Even if the union field has its default value, if it is not the default field of the
        // union then we have to print it anyway.
        auto fieldProto = field->getProto();
        if (fieldProto.getDiscriminantValue() != 0 || structValue.has(*field)) {
          unionValue = kj::strTree(
              fieldProto.getName(), " = ",
              print(structValue.get(*field), whichFieldType(*field), indent.next(), PREFIXED));
        } else {
          which = nullptr;
        }
      }

      for (auto field: nonUnionFields) {
        KJ_IF_MAYBE(unionField, which) {
          if (unionField->getIndex() < field.getIndex()) {
            printedFields.add(kj::mv(unionValue));
            which = nullptr;
          }
        }
        if (structValue.has(field)) {
          printedFields.add(kj::strTree(
              field.getProto().getName(), " = ",
              print(structValue.get(field), whichFieldType(field), indent.next(), PREFIXED)));
        }
      }
      if (which != nullptr) {
        // Union value is last.
        printedFields.add(kj::mv(unionValue));
      }

      if (mode == PARENTHESIZED) {
        return indent.delimit(printedFields.releaseAsArray(), mode, PrintKind::RECORD);
      } else {
        return kj::strTree(
            '(', indent.delimit(printedFields.releaseAsArray(), mode, PrintKind::RECORD), ')');
      }
    }
    case DynamicValue::CAPABILITY:
      return kj::strTree("<external capability>");
    case DynamicValue::ANY_POINTER:
      return kj::strTree("<opaque pointer>");
  }

  KJ_UNREACHABLE;
}

kj::StringTree stringify(DynamicValue::Reader value) {
  return print(value, schema::Type::STRUCT, Indent(false), BARE);
}

}  // namespace

kj::StringTree prettyPrint(DynamicStruct::Reader value) {
  return print(value, schema::Type::STRUCT, Indent(true), BARE);
}

kj::StringTree prettyPrint(DynamicList::Reader value) {
  return print(value, schema::Type::LIST, Indent(true), BARE);
}

kj::StringTree prettyPrint(DynamicStruct::Builder value) { return prettyPrint(value.asReader()); }
kj::StringTree prettyPrint(DynamicList::Builder value) { return prettyPrint(value.asReader()); }

kj::StringTree KJ_STRINGIFY(const DynamicValue::Reader& value) { return stringify(value); }
kj::StringTree KJ_STRINGIFY(const DynamicValue::Builder& value) { return stringify(value.asReader()); }
kj::StringTree KJ_STRINGIFY(DynamicEnum value) { return stringify(value); }
kj::StringTree KJ_STRINGIFY(const DynamicStruct::Reader& value) { return stringify(value); }
kj::StringTree KJ_STRINGIFY(const DynamicStruct::Builder& value) { return stringify(value.asReader()); }
kj::StringTree KJ_STRINGIFY(const DynamicList::Reader& value) { return stringify(value); }
kj::StringTree KJ_STRINGIFY(const DynamicList::Builder& value) { return stringify(value.asReader()); }

namespace _ {  // private

kj::StringTree structString(StructReader reader, const RawSchema& schema) {
  return stringify(DynamicStruct::Reader(StructSchema(&schema), reader));
}

}  // namespace _ (private)

}  // namespace capnp
