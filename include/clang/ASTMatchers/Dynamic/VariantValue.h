//===--- VariantValue.h - Polymorphic value type -*- C++ -*-===/
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Polymorphic value type.
///
/// Supports all the types required for dynamic Matcher construction.
///  Used by the registry to construct matchers in a generic way.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_MATCHERS_DYNAMIC_VARIANT_VALUE_H
#define LLVM_CLANG_AST_MATCHERS_DYNAMIC_VARIANT_VALUE_H

#include <vector>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersInternal.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/type_traits.h"

namespace clang {
namespace ast_matchers {
namespace dynamic {

using ast_matchers::internal::DynTypedMatcher;

/// \brief A variant matcher object.
///
/// The purpose of this object is to abstract simple and polymorphic matchers
/// into a single object type.
/// Polymorphic matchers might be implemented as a list of all the possible
/// overloads of the matcher. \c VariantMatcher knows how to select the
/// appropriate overload when needed.
/// To get a real matcher object out of a \c VariantMatcher you can do:
///  - getSingleMatcher() which returns a matcher, only if it is not ambiguous
///    to decide which matcher to return. Eg. it contains only a single
///    matcher, or a polymorphic one with only one overload.
///  - hasTypedMatcher<T>()/getTypedMatcher<T>(): These calls will determine if
///    the underlying matcher(s) can unambiguously return a Matcher<T>.
class VariantMatcher {
public:
  /// \brief A null matcher.
  VariantMatcher();

  /// \brief Clones the matcher objects.
  VariantMatcher(const VariantMatcher &Other);

  /// \brief Clones the provided matcher.
  static VariantMatcher SingleMatcher(const DynTypedMatcher &Matcher);

  /// \brief Clones the provided matchers.
  ///
  /// They should be the result of a polymorphic matcher.
  static VariantMatcher
  PolymorphicMatcher(ArrayRef<const DynTypedMatcher *> Matchers);

  ~VariantMatcher();

  /// \brief Copy the \c VariantMatcher, by making a copy of its representation.
  VariantMatcher &operator=(const VariantMatcher &Other);

  /// \brief Makes the matcher the "null" matcher.
  void reset();

  /// \brief Whether the matcher is null.
  bool isNull() const { return List.empty(); }

  /// \brief Return a single matcher, if there is no ambiguity.
  ///
  /// \returns True, and set Out to the matcher, if there is only one matcher.
  /// False, if the underlying matcher is a polymorphic matcher with
  /// more than one representation.
  bool getSingleMatcher(const DynTypedMatcher *&Out) const;

  /// \brief Determines if any of the contained matchers can be converted
  ///   to \c Matcher<T>.
  ///
  /// Returns true if one, and only one, of the contained matchers can be
  /// converted to \c Matcher<T>. If there are more than one that can, the
  /// result would be ambigous and false is returned.
  template <class T>
  bool hasTypedMatcher() const {
    return getTypedMatcher(
        &ast_matchers::internal::Matcher<T>::canConstructFrom) != NULL;
  }

  /// \brief Wrap the correct matcher as a \c Matcher<T>.
  ///
  /// Selects the appropriate matcher from the wrapped matchers and returns it
  /// as a \c Matcher<T>.
  /// Asserts that \c hasTypedMatcher<T>() is true.
  template <class T>
  ast_matchers::internal::Matcher<T> getTypedMatcher() const {
    assert(hasTypedMatcher<T>());
    return ast_matchers::internal::Matcher<T>::constructFrom(*getTypedMatcher(
        &ast_matchers::internal::Matcher<T>::canConstructFrom));
  }

  /// \brief String representation of the type of the value.
  ///
  /// If the underlying matcher is a polymorphic one, the string will show all
  /// the types.
  std::string getTypeAsString() const;

private:
  /// \brief Returns the matcher that passes the callback.
  ///
  /// Returns NULL if no matcher passes the test, or if more than one do.
  const DynTypedMatcher *
  getTypedMatcher(bool (*CanConstructCallback)(const DynTypedMatcher &)) const;

  std::vector<const DynTypedMatcher *> List;
};

/// \brief Variant value class.
///
/// Basically, a tagged union with value type semantics.
/// It is used by the registry as the return value and argument type for the
/// matcher factory methods.
/// It can be constructed from any of the supported types. It supports
/// copy/assignment.
///
/// Supported types:
///  - \c unsigned
///  - \c std::string
///  - \c VariantMatcher (\c DynTypedMatcher / \c Matcher<T>)
class VariantValue {
public:
  VariantValue() : Type(VT_Nothing) {}

  VariantValue(const VariantValue &Other);
  ~VariantValue();
  VariantValue &operator=(const VariantValue &Other);

  /// \brief Specific constructors for each supported type.
  VariantValue(unsigned Unsigned);
  VariantValue(const std::string &String);
  VariantValue(const VariantMatcher &Matchers);

  /// \brief Unsigned value functions.
  bool isUnsigned() const;
  unsigned getUnsigned() const;
  void setUnsigned(unsigned Unsigned);

  /// \brief String value functions.
  bool isString() const;
  const std::string &getString() const;
  void setString(const std::string &String);

  /// \brief Matcher value functions.
  bool isMatcher() const;
  const VariantMatcher &getMatcher() const;
  void setMatcher(const VariantMatcher &Matcher);

  /// \brief Shortcut functions.
  template <class T>
  bool hasTypedMatcher() const {
    return isMatcher() && getMatcher().hasTypedMatcher<T>();
  }

  template <class T>
  ast_matchers::internal::Matcher<T> getTypedMatcher() const {
    return getMatcher().getTypedMatcher<T>();
  }

  /// \brief String representation of the type of the value.
  std::string getTypeAsString() const;

private:
  void reset();

  /// \brief All supported value types.
  enum ValueType {
    VT_Nothing,
    VT_Unsigned,
    VT_String,
    VT_Matcher
  };

  /// \brief All supported value types.
  union AllValues {
    unsigned Unsigned;
    std::string *String;
    VariantMatcher *Matcher;
  };

  ValueType Type;
  AllValues Value;
};

} // end namespace dynamic
} // end namespace ast_matchers
} // end namespace clang

#endif  // LLVM_CLANG_AST_MATCHERS_DYNAMIC_VARIANT_VALUE_H
