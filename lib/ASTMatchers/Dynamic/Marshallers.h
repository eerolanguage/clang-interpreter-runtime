//===--- Marshallers.h - Generic matcher function marshallers -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Functions templates and classes to wrap matcher construct functions.
///
/// A collection of template function and classes that provide a generic
/// marshalling layer on top of matcher construct functions.
/// These are used by the registry to export all marshaller constructors with
/// the same generic interface.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_MATCHERS_DYNAMIC_MARSHALLERS_H
#define LLVM_CLANG_AST_MATCHERS_DYNAMIC_MARSHALLERS_H

#include <string>

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/Dynamic/Diagnostics.h"
#include "clang/ASTMatchers/Dynamic/VariantValue.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/type_traits.h"

namespace clang {
namespace ast_matchers {
namespace dynamic {

namespace internal {

/// \brief Helper template class to just from argument type to the right is/get
///   functions in VariantValue.
/// Used to verify and extract the matcher arguments below.
template <class T> struct ArgTypeTraits;
template <class T> struct ArgTypeTraits<const T &> : public ArgTypeTraits<T> {
};

template <> struct ArgTypeTraits<std::string> {
  static StringRef asString() { return "String"; }
  static bool is(const VariantValue &Value) { return Value.isString(); }
  static const std::string &get(const VariantValue &Value) {
    return Value.getString();
  }
};

template <>
struct ArgTypeTraits<StringRef> : public ArgTypeTraits<std::string> {
};

template <class T> struct ArgTypeTraits<ast_matchers::internal::Matcher<T> > {
  static std::string asString() {
    return (Twine("Matcher<") +
            ast_type_traits::ASTNodeKind::getFromNodeKind<T>().asStringRef() +
            ">").str();
  }
  static bool is(const VariantValue &Value) {
    return Value.hasTypedMatcher<T>();
  }
  static ast_matchers::internal::Matcher<T> get(const VariantValue &Value) {
    return Value.getTypedMatcher<T>();
  }
};

template <> struct ArgTypeTraits<unsigned> {
  static std::string asString() { return "Unsigned"; }
  static bool is(const VariantValue &Value) { return Value.isUnsigned(); }
  static unsigned get(const VariantValue &Value) {
    return Value.getUnsigned();
  }
};

/// \brief Generic MatcherCreate interface.
///
/// Provides a \c run() method that constructs the matcher from the provided
/// arguments.
class MatcherCreateCallback {
public:
  virtual ~MatcherCreateCallback() {}
  virtual VariantMatcher run(const SourceRange &NameRange,
                             ArrayRef<ParserValue> Args,
                             Diagnostics *Error) const = 0;
};

/// \brief Simple callback implementation. Marshaller and function are provided.
///
/// This class wraps a function of arbitrary signature and a marshaller
/// function into a MatcherCreateCallback.
/// The marshaller is in charge of taking the VariantValue arguments, checking
/// their types, unpacking them and calling the underlying function.
template <typename FuncType>
class FixedArgCountMatcherCreateCallback : public MatcherCreateCallback {
public:
  /// FIXME: Use void(*)() as FuncType on this interface to remove the template
  /// argument of this class. The marshaller can cast the function pointer back
  /// to the original type.
  typedef VariantMatcher (*MarshallerType)(FuncType, StringRef,
                                           const SourceRange &,
                                           ArrayRef<ParserValue>,
                                           Diagnostics *);

  /// \param Marshaller Function to unpack the arguments and call \c Func
  /// \param Func Matcher construct function. This is the function that
  ///   compile-time matcher expressions would use to create the matcher.
  FixedArgCountMatcherCreateCallback(MarshallerType Marshaller, FuncType Func,
                                     StringRef MatcherName)
      : Marshaller(Marshaller), Func(Func), MatcherName(MatcherName.str()) {}

  VariantMatcher run(const SourceRange &NameRange, ArrayRef<ParserValue> Args,
                     Diagnostics *Error) const {
    return Marshaller(Func, MatcherName, NameRange, Args, Error);
  }

private:
  const MarshallerType Marshaller;
  const FuncType Func;
  const std::string MatcherName;
};

/// \brief Simple callback implementation. Free function is wrapped.
///
/// This class simply wraps a free function with the right signature to export
/// it as a MatcherCreateCallback.
/// This allows us to have one implementation of the interface for as many free
/// functions as we want, reducing the number of symbols and size of the
/// object file.
class FreeFuncMatcherCreateCallback : public MatcherCreateCallback {
public:
  typedef VariantMatcher (*RunFunc)(StringRef MatcherName,
                                    const SourceRange &NameRange,
                                    ArrayRef<ParserValue> Args,
                                    Diagnostics *Error);

  FreeFuncMatcherCreateCallback(RunFunc Func, StringRef MatcherName)
      : Func(Func), MatcherName(MatcherName.str()) {}

  VariantMatcher run(const SourceRange &NameRange, ArrayRef<ParserValue> Args,
                     Diagnostics *Error) const {
    return Func(MatcherName, NameRange, Args, Error);
  }

private:
  const RunFunc Func;
  const std::string MatcherName;
};

/// \brief Helper macros to check the arguments on all marshaller functions.
#define CHECK_ARG_COUNT(count)                                                 \
  if (Args.size() != count) {                                                  \
    Error->addError(NameRange, Error->ET_RegistryWrongArgCount)                \
        << count << Args.size();                                               \
    return VariantMatcher();                                                   \
  }

#define CHECK_ARG_TYPE(index, type)                                            \
  if (!ArgTypeTraits<type>::is(Args[index].Value)) {                           \
    Error->addError(Args[index].Range, Error->ET_RegistryWrongArgType)         \
        << (index + 1) << ArgTypeTraits<type>::asString()                      \
        << Args[index].Value.getTypeAsString();                                \
    return VariantMatcher();                                                   \
  }

/// \brief Helper methods to extract and merge all possible typed matchers
/// out of the polymorphic object.
template <class PolyMatcher>
static void mergePolyMatchers(const PolyMatcher &Poly,
                              std::vector<const DynTypedMatcher *> &Out,
                              ast_matchers::internal::EmptyTypeList) {}

template <class PolyMatcher, class TypeList>
static void mergePolyMatchers(const PolyMatcher &Poly,
                              std::vector<const DynTypedMatcher *> &Out,
                              TypeList) {
  Out.push_back(ast_matchers::internal::Matcher<typename TypeList::head>(Poly)
                    .clone());
  mergePolyMatchers(Poly, Out, typename TypeList::tail());
}

/// \brief Convert the return values of the functions into a VariantMatcher.
///
/// There are 2 cases right now: The return value is a Matcher<T> or is a
/// polymorphic matcher. For the former, we just construct the VariantMatcher.
/// For the latter, we instantiate all the possible Matcher<T> of the poly
/// matcher.
template <typename T>
static VariantMatcher
outvalueToVariantMatcher(const ast_matchers::internal::Matcher<T> &Matcher) {
  return VariantMatcher::SingleMatcher(Matcher);
}

template <typename T>
static VariantMatcher outvalueToVariantMatcher(const T &PolyMatcher,
                                               typename T::ReturnTypes * =
                                                   NULL) {
  std::vector<const DynTypedMatcher *> Matchers;
  mergePolyMatchers(PolyMatcher, Matchers, typename T::ReturnTypes());
  VariantMatcher Out = VariantMatcher::PolymorphicMatcher(Matchers);
  llvm::DeleteContainerPointers(Matchers);
  return Out;
}

/// \brief 0-arg marshaller function.
template <typename ReturnType>
static VariantMatcher
matcherMarshall0(ReturnType (*Func)(), StringRef MatcherName,
                 const SourceRange &NameRange, ArrayRef<ParserValue> Args,
                 Diagnostics *Error) {
  CHECK_ARG_COUNT(0);
  return outvalueToVariantMatcher(Func());
}

/// \brief 1-arg marshaller function.
template <typename ReturnType, typename ArgType1>
static VariantMatcher
matcherMarshall1(ReturnType (*Func)(ArgType1), StringRef MatcherName,
                 const SourceRange &NameRange, ArrayRef<ParserValue> Args,
                 Diagnostics *Error) {
  CHECK_ARG_COUNT(1);
  CHECK_ARG_TYPE(0, ArgType1);
  return outvalueToVariantMatcher(
      Func(ArgTypeTraits<ArgType1>::get(Args[0].Value)));
}

/// \brief 2-arg marshaller function.
template <typename ReturnType, typename ArgType1, typename ArgType2>
static VariantMatcher
matcherMarshall2(ReturnType (*Func)(ArgType1, ArgType2), StringRef MatcherName,
                 const SourceRange &NameRange, ArrayRef<ParserValue> Args,
                 Diagnostics *Error) {
  CHECK_ARG_COUNT(2);
  CHECK_ARG_TYPE(0, ArgType1);
  CHECK_ARG_TYPE(1, ArgType2);
  return outvalueToVariantMatcher(
      Func(ArgTypeTraits<ArgType1>::get(Args[0].Value),
           ArgTypeTraits<ArgType2>::get(Args[1].Value)));
}

#undef CHECK_ARG_COUNT
#undef CHECK_ARG_TYPE

/// \brief Variadic marshaller function.
template <typename ResultT, typename ArgT,
          ResultT (*Func)(ArrayRef<const ArgT *>)>
VariantMatcher
variadicMatcherCreateCallback(StringRef MatcherName,
                              const SourceRange &NameRange,
                              ArrayRef<ParserValue> Args, Diagnostics *Error) {
  ArgT **InnerArgs = new ArgT *[Args.size()]();

  bool HasError = false;
  for (size_t i = 0, e = Args.size(); i != e; ++i) {
    typedef ArgTypeTraits<ArgT> ArgTraits;
    const ParserValue &Arg = Args[i];
    const VariantValue &Value = Arg.Value;
    if (!ArgTraits::is(Value)) {
      Error->addError(Arg.Range, Error->ET_RegistryWrongArgType)
          << (i + 1) << ArgTraits::asString() << Value.getTypeAsString();
      HasError = true;
      break;
    }
    InnerArgs[i] = new ArgT(ArgTraits::get(Value));
  }

  VariantMatcher Out;
  if (!HasError) {
    Out = outvalueToVariantMatcher(
        Func(ArrayRef<const ArgT *>(InnerArgs, Args.size())));
  }

  for (size_t i = 0, e = Args.size(); i != e; ++i) {
    delete InnerArgs[i];
  }
  delete[] InnerArgs;
  return Out;
}

/// Helper functions to select the appropriate marshaller functions.
/// They detect the number of arguments, arguments types and return type.

/// \brief 0-arg overload
template <typename ReturnType>
MatcherCreateCallback *makeMatcherAutoMarshall(ReturnType (*Func)(),
                                               StringRef MatcherName) {
  return new FixedArgCountMatcherCreateCallback<ReturnType (*)()>(
      matcherMarshall0, Func, MatcherName);
}

/// \brief 1-arg overload
template <typename ReturnType, typename ArgType1>
MatcherCreateCallback *makeMatcherAutoMarshall(ReturnType (*Func)(ArgType1),
                                               StringRef MatcherName) {
  return new FixedArgCountMatcherCreateCallback<ReturnType (*)(ArgType1)>(
      matcherMarshall1, Func, MatcherName);
}

/// \brief 2-arg overload
template <typename ReturnType, typename ArgType1, typename ArgType2>
MatcherCreateCallback *makeMatcherAutoMarshall(ReturnType (*Func)(ArgType1,
                                                                  ArgType2),
                                               StringRef MatcherName) {
  return new FixedArgCountMatcherCreateCallback<
      ReturnType (*)(ArgType1, ArgType2)>(matcherMarshall2, Func, MatcherName);
}

/// \brief Variadic overload.
template <typename ResultT, typename ArgT,
          ResultT (*Func)(ArrayRef<const ArgT *>)>
MatcherCreateCallback *
makeMatcherAutoMarshall(llvm::VariadicFunction<ResultT, ArgT, Func> VarFunc,
                        StringRef MatcherName) {
  return new FreeFuncMatcherCreateCallback(
      &variadicMatcherCreateCallback<ResultT, ArgT, Func>, MatcherName);
}

}  // namespace internal
}  // namespace dynamic
}  // namespace ast_matchers
}  // namespace clang

#endif  // LLVM_CLANG_AST_MATCHERS_DYNAMIC_MARSHALLERS_H
