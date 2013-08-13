//===--- Format.h - Format C++ code -----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Various functions to configurably format source code.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_FORMAT_FORMAT_H
#define LLVM_CLANG_FORMAT_FORMAT_H

#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/Refactoring.h"
#include "llvm/Support/system_error.h"

namespace clang {

class Lexer;
class SourceManager;
class DiagnosticConsumer;

namespace format {

/// \brief The \c FormatStyle is used to configure the formatting to follow
/// specific guidelines.
struct FormatStyle {
  /// \brief The column limit.
  ///
  /// A column limit of \c 0 means that there is no column limit. In this case,
  /// clang-format will respect the input's line breaking decisions within
  /// statements.
  unsigned ColumnLimit;

  /// \brief The maximum number of consecutive empty lines to keep.
  unsigned MaxEmptyLinesToKeep;

  /// \brief The penalty for each line break introduced inside a comment.
  unsigned PenaltyBreakComment;

  /// \brief The penalty for each line break introduced inside a string literal.
  unsigned PenaltyBreakString;

  /// \brief The penalty for each character outside of the column limit.
  unsigned PenaltyExcessCharacter;

  /// \brief The penalty for breaking before the first "<<".
  unsigned PenaltyBreakFirstLessLess;

  /// \brief Set whether & and * bind to the type as opposed to the variable.
  bool PointerBindsToType;

  /// \brief If \c true, analyze the formatted file for the most common binding.
  bool DerivePointerBinding;

  /// \brief The extra indent or outdent of access modifiers (e.g.: public:).
  int AccessModifierOffset;

  enum LanguageStandard {
    LS_Cpp03,
    LS_Cpp11,
    LS_Auto
  };

  /// \brief Format compatible with this standard, e.g. use \c A<A<int> >
  /// instead of \c A<A<int>> for LS_Cpp03.
  LanguageStandard Standard;

  /// \brief Indent case labels one level from the switch statement.
  ///
  /// When false, use the same indentation level as for the switch statement.
  /// Switch statement body is always indented one level more than case labels.
  bool IndentCaseLabels;

  enum NamespaceIndentationKind {
    NI_None,  // Don't indent in namespaces.
    NI_Inner, // Indent only in inner namespaces (nested in other namespaces).
    NI_All    // Indent in all namespaces.
  };

  /// \brief The indentation used for namespaces.
  NamespaceIndentationKind NamespaceIndentation;

  /// \brief The number of spaces to before trailing line comments.
  unsigned SpacesBeforeTrailingComments;

  /// \brief If false, a function call's or function definition's parameters
  /// will either all be on the same line or will have one line each.
  bool BinPackParameters;

  /// \brief If true, clang-format detects whether function calls and
  /// definitions are formatted with one parameter per line.
  ///
  /// Each call can be bin-packed, one-per-line or inconclusive. If it is
  /// inconclusive, e.g. completely on one line, but a decision needs to be
  /// made, clang-format analyzes whether there are other bin-packed cases in
  /// the input file and act accordingly.
  ///
  /// NOTE: This is an experimental flag, that might go away or be renamed. Do
  /// not use this in config files, etc. Use at your own risk.
  bool ExperimentalAutoDetectBinPacking;

  /// \brief Allow putting all parameters of a function declaration onto
  /// the next line even if \c BinPackParameters is \c false.
  bool AllowAllParametersOfDeclarationOnNextLine;

  /// \brief Penalty for putting the return type of a function onto its own
  /// line.
  unsigned PenaltyReturnTypeOnItsOwnLine;

  /// \brief If the constructor initializers don't fit on a line, put each
  /// initializer on its own line.
  bool ConstructorInitializerAllOnOneLineOrOnePerLine;

  /// \brief Always break constructor initializers before commas and align
  /// the commas with the colon.
  bool BreakConstructorInitializersBeforeComma;

  /// \brief If true, "if (a) return;" can be put on a single line.
  bool AllowShortIfStatementsOnASingleLine;

  /// \brief If true, "while (true) continue;" can be put on a single line.
  bool AllowShortLoopsOnASingleLine;

  /// \brief Add a space in front of an Objective-C protocol list, i.e. use
  /// Foo <Protocol> instead of Foo<Protocol>.
  bool ObjCSpaceBeforeProtocolList;

  /// \brief If \c true, aligns trailing comments.
  bool AlignTrailingComments;

  /// \brief If \c true, aligns escaped newlines as far left as possible.
  /// Otherwise puts them into the right-most column.
  bool AlignEscapedNewlinesLeft;

  /// \brief The number of characters to use for indentation.
  unsigned IndentWidth;

  /// \brief The number of characters to use for indentation of constructor
  /// initializer lists.
  unsigned ConstructorInitializerIndentWidth;

  /// \brief If \c true, always break after the \c template<...> of a template
  /// declaration.
  bool AlwaysBreakTemplateDeclarations;

  /// \brief If \c true, always break before multiline string literals.
  bool AlwaysBreakBeforeMultilineStrings;

  /// \brief If true, \c IndentWidth consecutive spaces will be replaced with
  /// tab characters.
  bool UseTab;

  /// \brief If \c true, binary operators will be placed after line breaks.
  bool BreakBeforeBinaryOperators;

  /// \brief Different ways to attach braces to their surrounding context.
  enum BraceBreakingStyle {
    /// Always attach braces to surrounding context.
    BS_Attach,
    /// Like \c Attach, but break before braces on function, namespace and
    /// class definitions.
    BS_Linux,
    /// Like \c Attach, but break before function definitions.
    BS_Stroustrup,
    /// Always break before braces
    BS_Allman
  };

  /// \brief The brace breaking style to use.
  BraceBreakingStyle BreakBeforeBraces;

  /// \brief If \c true, format braced lists as best suited for C++11 braced
  /// lists.
  ///
  /// Important differences:
  /// - No spaces inside the braced list.
  /// - No line break before the closing brace.
  /// - Indentation with the continuation indent, not with the block indent.
  ///
  /// Fundamentally, C++11 braced lists are formatted exactly like function
  /// calls would be formatted in their place. If the braced list follows a name
  /// (e.g. a type or variable name), clang-format formats as if the "{}" were
  /// the parentheses of a function call with that name. If there is no name,
  /// a zero-length name is assumed.
  bool Cpp11BracedListStyle;

  /// \brief If \c true, indent when breaking function declarations which
  /// are not also definitions after the type.
  bool IndentFunctionDeclarationAfterType;

  bool operator==(const FormatStyle &R) const {
    return AccessModifierOffset == R.AccessModifierOffset &&
           ConstructorInitializerIndentWidth ==
               R.ConstructorInitializerIndentWidth &&
           AlignEscapedNewlinesLeft == R.AlignEscapedNewlinesLeft &&
           AlignTrailingComments == R.AlignTrailingComments &&
           AllowAllParametersOfDeclarationOnNextLine ==
               R.AllowAllParametersOfDeclarationOnNextLine &&
           AllowShortIfStatementsOnASingleLine ==
               R.AllowShortIfStatementsOnASingleLine &&
           AlwaysBreakTemplateDeclarations ==
               R.AlwaysBreakTemplateDeclarations &&
           AlwaysBreakBeforeMultilineStrings ==
               R.AlwaysBreakBeforeMultilineStrings &&
           BinPackParameters == R.BinPackParameters &&
           BreakBeforeBraces == R.BreakBeforeBraces &&
           ColumnLimit == R.ColumnLimit &&
           ConstructorInitializerAllOnOneLineOrOnePerLine ==
               R.ConstructorInitializerAllOnOneLineOrOnePerLine &&
           DerivePointerBinding == R.DerivePointerBinding &&
           ExperimentalAutoDetectBinPacking ==
               R.ExperimentalAutoDetectBinPacking &&
           IndentCaseLabels == R.IndentCaseLabels &&
           IndentWidth == R.IndentWidth &&
           MaxEmptyLinesToKeep == R.MaxEmptyLinesToKeep &&
           NamespaceIndentation == R.NamespaceIndentation &&
           ObjCSpaceBeforeProtocolList == R.ObjCSpaceBeforeProtocolList &&
           PenaltyBreakComment == R.PenaltyBreakComment &&
           PenaltyBreakFirstLessLess == R.PenaltyBreakFirstLessLess &&
           PenaltyBreakString == R.PenaltyBreakString &&
           PenaltyExcessCharacter == R.PenaltyExcessCharacter &&
           PenaltyReturnTypeOnItsOwnLine == R.PenaltyReturnTypeOnItsOwnLine &&
           PointerBindsToType == R.PointerBindsToType &&
           SpacesBeforeTrailingComments == R.SpacesBeforeTrailingComments &&
           Cpp11BracedListStyle == R.Cpp11BracedListStyle &&
           Standard == R.Standard && UseTab == R.UseTab &&
           IndentFunctionDeclarationAfterType ==
               R.IndentFunctionDeclarationAfterType;
  }
};

/// \brief Returns a format style complying with the LLVM coding standards:
/// http://llvm.org/docs/CodingStandards.html.
FormatStyle getLLVMStyle();

/// \brief Returns a format style complying with Google's C++ style guide:
/// http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml.
FormatStyle getGoogleStyle();

/// \brief Returns a format style complying with Chromium's style guide:
/// http://www.chromium.org/developers/coding-style.
FormatStyle getChromiumStyle();

/// \brief Returns a format style complying with Mozilla's style guide:
/// https://developer.mozilla.org/en-US/docs/Developer_Guide/Coding_Style.
FormatStyle getMozillaStyle();

/// \brief Returns a format style complying with Webkit's style guide:
/// http://www.webkit.org/coding/coding-style.html
FormatStyle getWebKitStyle();

/// \brief Gets a predefined style by name.
///
/// Currently supported names: LLVM, Google, Chromium, Mozilla. Names are
/// compared case-insensitively.
///
/// Returns true if the Style has been set.
bool getPredefinedStyle(StringRef Name, FormatStyle *Style);

/// \brief Parse configuration from YAML-formatted text.
llvm::error_code parseConfiguration(StringRef Text, FormatStyle *Style);

/// \brief Gets configuration in a YAML string.
std::string configurationAsText(const FormatStyle &Style);

/// \brief Reformats the given \p Ranges in the token stream coming out of
/// \c Lex.
///
/// Each range is extended on either end to its next bigger logic unit, i.e.
/// everything that might influence its formatting or might be influenced by its
/// formatting.
///
/// Returns the \c Replacements necessary to make all \p Ranges comply with
/// \p Style.
tooling::Replacements reformat(const FormatStyle &Style, Lexer &Lex,
                               SourceManager &SourceMgr,
                               std::vector<CharSourceRange> Ranges);

/// \brief Reformats the given \p Ranges in \p Code.
///
/// Otherwise identical to the reformat() function consuming a \c Lexer.
tooling::Replacements reformat(const FormatStyle &Style, StringRef Code,
                               std::vector<tooling::Range> Ranges,
                               StringRef FileName = "<stdin>");

/// \brief Returns the \c LangOpts that the formatter expects you to set.
///
/// \param Standard determines lexing mode: LC_Cpp11 and LS_Auto turn on C++11
/// lexing mode, LS_Cpp03 - C++03 mode.
LangOptions getFormattingLangOpts(FormatStyle::LanguageStandard Standard =
                                      FormatStyle::LS_Cpp11);

} // end namespace format
} // end namespace clang

#endif // LLVM_CLANG_FORMAT_FORMAT_H
