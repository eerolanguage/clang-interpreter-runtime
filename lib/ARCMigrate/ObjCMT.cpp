//===--- ObjCMT.cpp - ObjC Migrate Tool -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "Transforms.h"
#include "clang/ARCMigrate/ARCMTActions.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/NSAPI.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/FileManager.h"
#include "clang/Edit/Commit.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Edit/EditsReceiver.h"
#include "clang/Edit/Rewriters.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/PPConditionalDirectiveRecord.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/AST/Attr.h"
#include "llvm/ADT/SmallString.h"

using namespace clang;
using namespace arcmt;

namespace {

class ObjCMigrateASTConsumer : public ASTConsumer {
  void migrateDecl(Decl *D);
  void migrateObjCInterfaceDecl(ASTContext &Ctx, ObjCInterfaceDecl *D);
  void migrateProtocolConformance(ASTContext &Ctx,
                                  const ObjCImplementationDecl *ImpDecl);
  void migrateNSEnumDecl(ASTContext &Ctx, const EnumDecl *EnumDcl,
                     const TypedefDecl *TypedefDcl);
  void migrateInstanceType(ASTContext &Ctx, ObjCContainerDecl *CDecl);
  void migrateMethodInstanceType(ASTContext &Ctx, ObjCContainerDecl *CDecl,
                                 ObjCMethodDecl *OM);
  void migrateFactoryMethod(ASTContext &Ctx, ObjCContainerDecl *CDecl,
                            ObjCMethodDecl *OM,
                            ObjCInstanceTypeFamily OIT_Family = OIT_None);
  
  void migrateFunctionDeclAnnotation(ASTContext &Ctx,
                                     const FunctionDecl *FuncDecl);
  
  void migrateObjCMethodDeclAnnotation(ASTContext &Ctx,
                                       const ObjCMethodDecl *MethodDecl);
public:
  std::string MigrateDir;
  bool MigrateLiterals;
  bool MigrateSubscripting;
  bool MigrateProperty;
  OwningPtr<NSAPI> NSAPIObj;
  OwningPtr<edit::EditedSource> Editor;
  FileRemapper &Remapper;
  FileManager &FileMgr;
  const PPConditionalDirectiveRecord *PPRec;
  Preprocessor &PP;
  bool IsOutputFile;
  llvm::SmallPtrSet<ObjCProtocolDecl *, 32> ObjCProtocolDecls;
  
  ObjCMigrateASTConsumer(StringRef migrateDir,
                         bool migrateLiterals,
                         bool migrateSubscripting,
                         bool migrateProperty,
                         FileRemapper &remapper,
                         FileManager &fileMgr,
                         const PPConditionalDirectiveRecord *PPRec,
                         Preprocessor &PP,
                         bool isOutputFile = false)
  : MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals),
    MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty),
    Remapper(remapper), FileMgr(fileMgr), PPRec(PPRec), PP(PP),
    IsOutputFile(isOutputFile) { }

protected:
  virtual void Initialize(ASTContext &Context) {
    NSAPIObj.reset(new NSAPI(Context));
    Editor.reset(new edit::EditedSource(Context.getSourceManager(),
                                        Context.getLangOpts(),
                                        PPRec));
  }

  virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
    for (DeclGroupRef::iterator I = DG.begin(), E = DG.end(); I != E; ++I)
      migrateDecl(*I);
    return true;
  }
  virtual void HandleInterestingDecl(DeclGroupRef DG) {
    // Ignore decls from the PCH.
  }
  virtual void HandleTopLevelDeclInObjCContainer(DeclGroupRef DG) {
    ObjCMigrateASTConsumer::HandleTopLevelDecl(DG);
  }

  virtual void HandleTranslationUnit(ASTContext &Ctx);
};

}

ObjCMigrateAction::ObjCMigrateAction(FrontendAction *WrappedAction,
                             StringRef migrateDir,
                             bool migrateLiterals,
                             bool migrateSubscripting,
                             bool migrateProperty)
  : WrapperFrontendAction(WrappedAction), MigrateDir(migrateDir),
    MigrateLiterals(migrateLiterals), MigrateSubscripting(migrateSubscripting),
    MigrateProperty(migrateProperty),
    CompInst(0) {
  if (MigrateDir.empty())
    MigrateDir = "."; // user current directory if none is given.
}

ASTConsumer *ObjCMigrateAction::CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) {
  PPConditionalDirectiveRecord *
    PPRec = new PPConditionalDirectiveRecord(CompInst->getSourceManager());
  CompInst->getPreprocessor().addPPCallbacks(PPRec);
  ASTConsumer *
    WrappedConsumer = WrapperFrontendAction::CreateASTConsumer(CI, InFile);
  ASTConsumer *MTConsumer = new ObjCMigrateASTConsumer(MigrateDir,
                                                       MigrateLiterals,
                                                       MigrateSubscripting,
                                                       MigrateProperty,
                                                       Remapper,
                                                    CompInst->getFileManager(),
                                                       PPRec,
                                                       CompInst->getPreprocessor());
  ASTConsumer *Consumers[] = { MTConsumer, WrappedConsumer };
  return new MultiplexConsumer(Consumers);
}

bool ObjCMigrateAction::BeginInvocation(CompilerInstance &CI) {
  Remapper.initFromDisk(MigrateDir, CI.getDiagnostics(),
                        /*ignoreIfFilesChanges=*/true);
  CompInst = &CI;
  CI.getDiagnostics().setIgnoreAllWarnings(true);
  return true;
}

namespace {
class ObjCMigrator : public RecursiveASTVisitor<ObjCMigrator> {
  ObjCMigrateASTConsumer &Consumer;
  ParentMap &PMap;

public:
  ObjCMigrator(ObjCMigrateASTConsumer &consumer, ParentMap &PMap)
    : Consumer(consumer), PMap(PMap) { }

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  bool VisitObjCMessageExpr(ObjCMessageExpr *E) {
    if (Consumer.MigrateLiterals) {
      edit::Commit commit(*Consumer.Editor);
      edit::rewriteToObjCLiteralSyntax(E, *Consumer.NSAPIObj, commit, &PMap);
      Consumer.Editor->commit(commit);
    }

    if (Consumer.MigrateSubscripting) {
      edit::Commit commit(*Consumer.Editor);
      edit::rewriteToObjCSubscriptSyntax(E, *Consumer.NSAPIObj, commit);
      Consumer.Editor->commit(commit);
    }

    return true;
  }

  bool TraverseObjCMessageExpr(ObjCMessageExpr *E) {
    // Do depth first; we want to rewrite the subexpressions first so that if
    // we have to move expressions we will move them already rewritten.
    for (Stmt::child_range range = E->children(); range; ++range)
      if (!TraverseStmt(*range))
        return false;

    return WalkUpFromObjCMessageExpr(E);
  }
};

class BodyMigrator : public RecursiveASTVisitor<BodyMigrator> {
  ObjCMigrateASTConsumer &Consumer;
  OwningPtr<ParentMap> PMap;

public:
  BodyMigrator(ObjCMigrateASTConsumer &consumer) : Consumer(consumer) { }

  bool shouldVisitTemplateInstantiations() const { return false; }
  bool shouldWalkTypesOfTypeLocs() const { return false; }

  bool TraverseStmt(Stmt *S) {
    PMap.reset(new ParentMap(S));
    ObjCMigrator(Consumer, *PMap).TraverseStmt(S);
    return true;
  }
};
}

void ObjCMigrateASTConsumer::migrateDecl(Decl *D) {
  if (!D)
    return;
  if (isa<ObjCMethodDecl>(D))
    return; // Wait for the ObjC container declaration.

  BodyMigrator(*this).TraverseDecl(D);
}

static void append_attr(std::string &PropertyString, const char *attr,
                        bool GetterHasIsPrefix) {
  PropertyString += (GetterHasIsPrefix ? ", " : "(");
  PropertyString += attr;
  PropertyString += ')';
}

static bool rewriteToObjCProperty(const ObjCMethodDecl *Getter,
                                  const ObjCMethodDecl *Setter,
                                  const NSAPI &NS, edit::Commit &commit,
                                  bool GetterHasIsPrefix) {
  ASTContext &Context = NS.getASTContext();
  std::string PropertyString = "@property";
  std::string PropertyNameString = Getter->getNameAsString();
  StringRef PropertyName(PropertyNameString);
  if (GetterHasIsPrefix) {
    PropertyString += "(getter=";
    PropertyString += PropertyNameString;
  }
  // Short circuit properties that contain the name "delegate" or "dataSource",
  // or have exact name "target" to have unsafe_unretained attribute.
  if (PropertyName.equals("target") ||
      (PropertyName.find("delegate") != StringRef::npos) ||
      (PropertyName.find("dataSource") != StringRef::npos))
    append_attr(PropertyString, "unsafe_unretained", GetterHasIsPrefix);
  else {
    const ParmVarDecl *argDecl = *Setter->param_begin();
    QualType ArgType = Context.getCanonicalType(argDecl->getType());
    Qualifiers::ObjCLifetime propertyLifetime = ArgType.getObjCLifetime();
    bool RetainableObject = ArgType->isObjCRetainableType();
    if (RetainableObject && propertyLifetime == Qualifiers::OCL_Strong) {
      if (const ObjCObjectPointerType *ObjPtrTy =
          ArgType->getAs<ObjCObjectPointerType>()) {
        ObjCInterfaceDecl *IDecl = ObjPtrTy->getObjectType()->getInterface();
        if (IDecl &&
            IDecl->lookupNestedProtocol(&Context.Idents.get("NSCopying")))
          append_attr(PropertyString, "copy", GetterHasIsPrefix);
        else
          append_attr(PropertyString, "retain", GetterHasIsPrefix);
      } else if (GetterHasIsPrefix)
          PropertyString += ')';
    } else if (propertyLifetime == Qualifiers::OCL_Weak)
      // TODO. More precise determination of 'weak' attribute requires
      // looking into setter's implementation for backing weak ivar.
      append_attr(PropertyString, "weak", GetterHasIsPrefix);
    else if (RetainableObject)
      append_attr(PropertyString, "retain", GetterHasIsPrefix);
    else if (GetterHasIsPrefix)
      PropertyString += ')';
  }
  
  QualType RT = Getter->getResultType();
  if (!isa<TypedefType>(RT)) {
    // strip off any ARC lifetime qualifier.
    QualType CanResultTy = Context.getCanonicalType(RT);
    if (CanResultTy.getQualifiers().hasObjCLifetime()) {
      Qualifiers Qs = CanResultTy.getQualifiers();
      Qs.removeObjCLifetime();
      RT = Context.getQualifiedType(CanResultTy.getUnqualifiedType(), Qs);
    }
  }
  PropertyString += " ";
  PropertyString += RT.getAsString(Context.getPrintingPolicy());
  PropertyString += " ";
  if (GetterHasIsPrefix) {
    // property name must strip off "is" and lower case the first character
    // after that; e.g. isContinuous will become continuous.
    StringRef PropertyNameStringRef(PropertyNameString);
    PropertyNameStringRef = PropertyNameStringRef.drop_front(2);
    PropertyNameString = PropertyNameStringRef;
    std::string NewPropertyNameString = PropertyNameString;
    NewPropertyNameString[0] = toLowercase(NewPropertyNameString[0]);
    PropertyString += NewPropertyNameString;
  }
  else
    PropertyString += PropertyNameString;
  commit.replace(CharSourceRange::getCharRange(Getter->getLocStart(),
                                               Getter->getDeclaratorEndLoc()),
                 PropertyString);
  SourceLocation EndLoc = Setter->getDeclaratorEndLoc();
  // Get location past ';'
  EndLoc = EndLoc.getLocWithOffset(1);
  commit.remove(CharSourceRange::getCharRange(Setter->getLocStart(), EndLoc));
  return true;
}

void ObjCMigrateASTConsumer::migrateObjCInterfaceDecl(ASTContext &Ctx,
                                                      ObjCInterfaceDecl *D) {
  for (ObjCContainerDecl::method_iterator M = D->meth_begin(), MEnd = D->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    if (Method->isPropertyAccessor() ||  Method->param_size() != 0)
      continue;
    // Is this method candidate to be a getter?
    QualType GRT = Method->getResultType();
    if (GRT->isVoidType())
      continue;
    // FIXME. Don't know what todo with attributes, skip for now.
    if (Method->hasAttrs())
      continue;
    
    Selector GetterSelector = Method->getSelector();
    IdentifierInfo *getterName = GetterSelector.getIdentifierInfoForSlot(0);
    Selector SetterSelector =
      SelectorTable::constructSetterSelector(PP.getIdentifierTable(),
                                             PP.getSelectorTable(),
                                             getterName);
    ObjCMethodDecl *SetterMethod = D->lookupMethod(SetterSelector, true);
    bool GetterHasIsPrefix = false;
    if (!SetterMethod) {
      // try a different naming convention for getter: isXxxxx
      StringRef getterNameString = getterName->getName();
      if (getterNameString.startswith("is") && !GRT->isObjCRetainableType()) {
        GetterHasIsPrefix = true;
        const char *CGetterName = getterNameString.data() + 2;
        if (CGetterName[0] && isUppercase(CGetterName[0])) {
          getterName = &Ctx.Idents.get(CGetterName);
          SetterSelector =
            SelectorTable::constructSetterSelector(PP.getIdentifierTable(),
                                                   PP.getSelectorTable(),
                                                   getterName);
          SetterMethod = D->lookupMethod(SetterSelector, true);
        }
      }
    }
    if (SetterMethod) {
      // Is this a valid setter, matching the target getter?
      QualType SRT = SetterMethod->getResultType();
      if (!SRT->isVoidType())
        continue;
      const ParmVarDecl *argDecl = *SetterMethod->param_begin();
      QualType ArgType = argDecl->getType();
      if (!Ctx.hasSameUnqualifiedType(ArgType, GRT) ||
          SetterMethod->hasAttrs())
          continue;
        edit::Commit commit(*Editor);
        rewriteToObjCProperty(Method, SetterMethod, *NSAPIObj, commit,
                              GetterHasIsPrefix);
        Editor->commit(commit);
      }
  }
}

static bool 
ClassImplementsAllMethodsAndProperties(ASTContext &Ctx,
                                      const ObjCImplementationDecl *ImpDecl,
                                       const ObjCInterfaceDecl *IDecl,
                                      ObjCProtocolDecl *Protocol) {
  // In auto-synthesis, protocol properties are not synthesized. So,
  // a conforming protocol must have its required properties declared
  // in class interface.
  bool HasAtleastOneRequiredProperty = false;
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition())
    for (ObjCProtocolDecl::prop_iterator P = PDecl->prop_begin(),
         E = PDecl->prop_end(); P != E; ++P) {
      ObjCPropertyDecl *Property = *P;
      if (Property->getPropertyImplementation() == ObjCPropertyDecl::Optional)
        continue;
      HasAtleastOneRequiredProperty = true;
      DeclContext::lookup_const_result R = IDecl->lookup(Property->getDeclName());
      if (R.size() == 0) {
        // Relax the rule and look into class's implementation for a synthesize
        // or dynamic declaration. Class is implementing a property coming from
        // another protocol. This still makes the target protocol as conforming.
        if (!ImpDecl->FindPropertyImplDecl(
                                  Property->getDeclName().getAsIdentifierInfo()))
          return false;
      }
      else if (ObjCPropertyDecl *ClassProperty = dyn_cast<ObjCPropertyDecl>(R[0])) {
          if ((ClassProperty->getPropertyAttributes()
              != Property->getPropertyAttributes()) ||
              !Ctx.hasSameType(ClassProperty->getType(), Property->getType()))
            return false;
      }
      else
        return false;
    }
  
  // At this point, all required properties in this protocol conform to those
  // declared in the class.
  // Check that class implements the required methods of the protocol too.
  bool HasAtleastOneRequiredMethod = false;
  if (const ObjCProtocolDecl *PDecl = Protocol->getDefinition()) {
    if (PDecl->meth_begin() == PDecl->meth_end())
      return HasAtleastOneRequiredProperty;
    for (ObjCContainerDecl::method_iterator M = PDecl->meth_begin(),
         MEnd = PDecl->meth_end(); M != MEnd; ++M) {
      ObjCMethodDecl *MD = (*M);
      if (MD->isImplicit())
        continue;
      if (MD->getImplementationControl() == ObjCMethodDecl::Optional)
        continue;
      DeclContext::lookup_const_result R = ImpDecl->lookup(MD->getDeclName());
      if (R.size() == 0)
        return false;
      bool match = false;
      HasAtleastOneRequiredMethod = true;
      for (unsigned I = 0, N = R.size(); I != N; ++I)
        if (ObjCMethodDecl *ImpMD = dyn_cast<ObjCMethodDecl>(R[0]))
          if (Ctx.ObjCMethodsAreEqual(MD, ImpMD)) {
            match = true;
            break;
          }
      if (!match)
        return false;
    }
  }
  if (HasAtleastOneRequiredProperty || HasAtleastOneRequiredMethod)
    return true;
  return false;
}

static bool rewriteToObjCInterfaceDecl(const ObjCInterfaceDecl *IDecl,
                    llvm::SmallVectorImpl<ObjCProtocolDecl*> &ConformingProtocols,
                    const NSAPI &NS, edit::Commit &commit) {
  const ObjCList<ObjCProtocolDecl> &Protocols = IDecl->getReferencedProtocols();
  std::string ClassString;
  SourceLocation EndLoc =
  IDecl->getSuperClass() ? IDecl->getSuperClassLoc() : IDecl->getLocation();
  
  if (Protocols.empty()) {
    ClassString = '<';
    for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
      ClassString += ConformingProtocols[i]->getNameAsString();
      if (i != (e-1))
        ClassString += ", ";
    }
    ClassString += "> ";
  }
  else {
    ClassString = ", ";
    for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
      ClassString += ConformingProtocols[i]->getNameAsString();
      if (i != (e-1))
        ClassString += ", ";
    }
    ObjCInterfaceDecl::protocol_loc_iterator PL = IDecl->protocol_loc_end() - 1;
    EndLoc = *PL;
  }
  
  commit.insertAfterToken(EndLoc, ClassString);
  return true;
}

static bool rewriteToNSEnumDecl(const EnumDecl *EnumDcl,
                                const TypedefDecl *TypedefDcl,
                                const NSAPI &NS, edit::Commit &commit,
                                bool IsNSIntegerType) {
  std::string ClassString =
    IsNSIntegerType ? "typedef NS_ENUM(NSInteger, " : "typedef NS_OPTIONS(NSUInteger, ";
  ClassString += TypedefDcl->getIdentifier()->getName();
  ClassString += ')';
  SourceRange R(EnumDcl->getLocStart(), EnumDcl->getLocStart());
  commit.replace(R, ClassString);
  SourceLocation EndOfTypedefLoc = TypedefDcl->getLocEnd();
  EndOfTypedefLoc = trans::findLocationAfterSemi(EndOfTypedefLoc, NS.getASTContext());
  if (!EndOfTypedefLoc.isInvalid()) {
    commit.remove(SourceRange(TypedefDcl->getLocStart(), EndOfTypedefLoc));
    return true;
  }
  return false;
}

static bool rewriteToNSMacroDecl(const EnumDecl *EnumDcl,
                                const TypedefDecl *TypedefDcl,
                                const NSAPI &NS, edit::Commit &commit,
                                 bool IsNSIntegerType) {
  std::string ClassString =
    IsNSIntegerType ? "NS_ENUM(NSInteger, " : "NS_OPTIONS(NSUInteger, ";
  ClassString += TypedefDcl->getIdentifier()->getName();
  ClassString += ')';
  SourceRange R(EnumDcl->getLocStart(), EnumDcl->getLocStart());
  commit.replace(R, ClassString);
  SourceLocation TypedefLoc = TypedefDcl->getLocEnd();
  commit.remove(SourceRange(TypedefLoc, TypedefLoc));
  return true;
}

static bool UseNSOptionsMacro(ASTContext &Ctx,
                              const EnumDecl *EnumDcl) {
  bool PowerOfTwo = true;
  for (EnumDecl::enumerator_iterator EI = EnumDcl->enumerator_begin(),
       EE = EnumDcl->enumerator_end(); EI != EE; ++EI) {
    EnumConstantDecl *Enumerator = (*EI);
    const Expr *InitExpr = Enumerator->getInitExpr();
    if (!InitExpr) {
      PowerOfTwo = false;
      continue;
    }
    InitExpr = InitExpr->IgnoreImpCasts();
    if (const BinaryOperator *BO = dyn_cast<BinaryOperator>(InitExpr))
      if (BO->isShiftOp() || BO->isBitwiseOp())
        return true;
    
    uint64_t EnumVal = Enumerator->getInitVal().getZExtValue();
    if (PowerOfTwo && EnumVal && !llvm::isPowerOf2_64(EnumVal))
      PowerOfTwo = false;
  }
  return PowerOfTwo;
}

void ObjCMigrateASTConsumer::migrateProtocolConformance(ASTContext &Ctx,   
                                            const ObjCImplementationDecl *ImpDecl) {
  const ObjCInterfaceDecl *IDecl = ImpDecl->getClassInterface();
  if (!IDecl || ObjCProtocolDecls.empty())
    return;
  // Find all implicit conforming protocols for this class
  // and make them explicit.
  llvm::SmallPtrSet<ObjCProtocolDecl *, 8> ExplicitProtocols;
  Ctx.CollectInheritedProtocols(IDecl, ExplicitProtocols);
  llvm::SmallVector<ObjCProtocolDecl *, 8> PotentialImplicitProtocols;
  
  for (llvm::SmallPtrSet<ObjCProtocolDecl*, 32>::iterator I =
       ObjCProtocolDecls.begin(),
       E = ObjCProtocolDecls.end(); I != E; ++I)
    if (!ExplicitProtocols.count(*I))
      PotentialImplicitProtocols.push_back(*I);
  
  if (PotentialImplicitProtocols.empty())
    return;

  // go through list of non-optional methods and properties in each protocol
  // in the PotentialImplicitProtocols list. If class implements every one of the
  // methods and properties, then this class conforms to this protocol.
  llvm::SmallVector<ObjCProtocolDecl*, 8> ConformingProtocols;
  for (unsigned i = 0, e = PotentialImplicitProtocols.size(); i != e; i++)
    if (ClassImplementsAllMethodsAndProperties(Ctx, ImpDecl, IDecl,
                                              PotentialImplicitProtocols[i]))
      ConformingProtocols.push_back(PotentialImplicitProtocols[i]);
  
  if (ConformingProtocols.empty())
    return;
  
  // Further reduce number of conforming protocols. If protocol P1 is in the list
  // protocol P2 (P2<P1>), No need to include P1.
  llvm::SmallVector<ObjCProtocolDecl*, 8> MinimalConformingProtocols;
  for (unsigned i = 0, e = ConformingProtocols.size(); i != e; i++) {
    bool DropIt = false;
    ObjCProtocolDecl *TargetPDecl = ConformingProtocols[i];
    for (unsigned i1 = 0, e1 = ConformingProtocols.size(); i1 != e1; i1++) {
      ObjCProtocolDecl *PDecl = ConformingProtocols[i1];
      if (PDecl == TargetPDecl)
        continue;
      if (PDecl->lookupProtocolNamed(
            TargetPDecl->getDeclName().getAsIdentifierInfo())) {
        DropIt = true;
        break;
      }
    }
    if (!DropIt)
      MinimalConformingProtocols.push_back(TargetPDecl);
  }
  edit::Commit commit(*Editor);
  rewriteToObjCInterfaceDecl(IDecl, MinimalConformingProtocols,
                             *NSAPIObj, commit);
  Editor->commit(commit);
}

void ObjCMigrateASTConsumer::migrateNSEnumDecl(ASTContext &Ctx,
                                           const EnumDecl *EnumDcl,
                                           const TypedefDecl *TypedefDcl) {
  if (!EnumDcl->isCompleteDefinition() || EnumDcl->getIdentifier() ||
      !TypedefDcl->getIdentifier())
    return;
  
  QualType qt = TypedefDcl->getTypeSourceInfo()->getType();
  bool IsNSIntegerType = NSAPIObj->isObjCNSIntegerType(qt);
  bool IsNSUIntegerType = !IsNSIntegerType && NSAPIObj->isObjCNSUIntegerType(qt);
  
  if (!IsNSIntegerType && !IsNSUIntegerType) {
    // Also check for typedef enum {...} TD;
    if (const EnumType *EnumTy = qt->getAs<EnumType>()) {
      if (EnumTy->getDecl() == EnumDcl) {
        bool NSOptions = UseNSOptionsMacro(Ctx, EnumDcl);
        if (NSOptions) {
          if (!Ctx.Idents.get("NS_OPTIONS").hasMacroDefinition())
            return;
        }
        else if (!Ctx.Idents.get("NS_ENUM").hasMacroDefinition())
          return;
        edit::Commit commit(*Editor);
        rewriteToNSMacroDecl(EnumDcl, TypedefDcl, *NSAPIObj, commit, !NSOptions);
        Editor->commit(commit);
      }
    }
    return;
  }
  if (IsNSIntegerType && UseNSOptionsMacro(Ctx, EnumDcl)) {
    // We may still use NS_OPTIONS based on what we find in the enumertor list.
    IsNSIntegerType = false;
    IsNSUIntegerType = true;
  }
  
  // NS_ENUM must be available.
  if (IsNSIntegerType && !Ctx.Idents.get("NS_ENUM").hasMacroDefinition())
    return;
  // NS_OPTIONS must be available.
  if (IsNSUIntegerType && !Ctx.Idents.get("NS_OPTIONS").hasMacroDefinition())
    return;
  edit::Commit commit(*Editor);
  rewriteToNSEnumDecl(EnumDcl, TypedefDcl, *NSAPIObj, commit, IsNSIntegerType);
  Editor->commit(commit);
}

static void ReplaceWithInstancetype(const ObjCMigrateASTConsumer &ASTC,
                                    ObjCMethodDecl *OM) {
  SourceRange R;
  std::string ClassString;
  if (TypeSourceInfo *TSInfo =  OM->getResultTypeSourceInfo()) {
    TypeLoc TL = TSInfo->getTypeLoc();
    R = SourceRange(TL.getBeginLoc(), TL.getEndLoc());
    ClassString = "instancetype";
  }
  else {
    R = SourceRange(OM->getLocStart(), OM->getLocStart());
    ClassString = OM->isInstanceMethod() ? '-' : '+';
    ClassString += " (instancetype)";
  }
  edit::Commit commit(*ASTC.Editor);
  commit.replace(R, ClassString);
  ASTC.Editor->commit(commit);
}

void ObjCMigrateASTConsumer::migrateMethodInstanceType(ASTContext &Ctx,
                                                       ObjCContainerDecl *CDecl,
                                                       ObjCMethodDecl *OM) {
  ObjCInstanceTypeFamily OIT_Family =
    Selector::getInstTypeMethodFamily(OM->getSelector());
  
  std::string ClassName;
  switch (OIT_Family) {
    case OIT_None:
      migrateFactoryMethod(Ctx, CDecl, OM);
      return;
    case OIT_Array:
      ClassName = "NSArray";
      break;
    case OIT_Dictionary:
      ClassName = "NSDictionary";
      break;
    case OIT_MemManage:
      ClassName = "NSObject";
      break;
    case OIT_Singleton:
      migrateFactoryMethod(Ctx, CDecl, OM, OIT_Singleton);
      return;
  }
  if (!OM->getResultType()->isObjCIdType())
    return;
  
  ObjCInterfaceDecl *IDecl = dyn_cast<ObjCInterfaceDecl>(CDecl);
  if (!IDecl) {
    if (ObjCCategoryDecl *CatDecl = dyn_cast<ObjCCategoryDecl>(CDecl))
      IDecl = CatDecl->getClassInterface();
    else if (ObjCImplDecl *ImpDecl = dyn_cast<ObjCImplDecl>(CDecl))
      IDecl = ImpDecl->getClassInterface();
  }
  if (!IDecl ||
      !IDecl->lookupInheritedClass(&Ctx.Idents.get(ClassName))) {
    migrateFactoryMethod(Ctx, CDecl, OM);
    return;
  }
  ReplaceWithInstancetype(*this, OM);
}

void ObjCMigrateASTConsumer::migrateInstanceType(ASTContext &Ctx,
                                                 ObjCContainerDecl *CDecl) {
  // migrate methods which can have instancetype as their result type.
  for (ObjCContainerDecl::method_iterator M = CDecl->meth_begin(),
       MEnd = CDecl->meth_end();
       M != MEnd; ++M) {
    ObjCMethodDecl *Method = (*M);
    migrateMethodInstanceType(Ctx, CDecl, Method);
  }
}

void ObjCMigrateASTConsumer::migrateFactoryMethod(ASTContext &Ctx,
                                                  ObjCContainerDecl *CDecl,
                                                  ObjCMethodDecl *OM,
                                                  ObjCInstanceTypeFamily OIT_Family) {
  if (OM->isInstanceMethod() ||
      OM->getResultType() == Ctx.getObjCInstanceType() ||
      !OM->getResultType()->isObjCIdType())
    return;
  
  // Candidate factory methods are + (id) NaMeXXX : ... which belong to a class
  // NSYYYNamE with matching names be at least 3 characters long.
  ObjCInterfaceDecl *IDecl = dyn_cast<ObjCInterfaceDecl>(CDecl);
  if (!IDecl) {
    if (ObjCCategoryDecl *CatDecl = dyn_cast<ObjCCategoryDecl>(CDecl))
      IDecl = CatDecl->getClassInterface();
    else if (ObjCImplDecl *ImpDecl = dyn_cast<ObjCImplDecl>(CDecl))
      IDecl = ImpDecl->getClassInterface();
  }
  if (!IDecl)
    return;
  
  std::string StringClassName = IDecl->getName();
  StringRef LoweredClassName(StringClassName);
  std::string StringLoweredClassName = LoweredClassName.lower();
  LoweredClassName = StringLoweredClassName;
  
  IdentifierInfo *MethodIdName = OM->getSelector().getIdentifierInfoForSlot(0);
  // Handle method with no name at its first selector slot; e.g. + (id):(int)x.
  if (!MethodIdName)
    return;
  
  std::string MethodName = MethodIdName->getName();
  if (OIT_Family == OIT_Singleton) {
    StringRef STRefMethodName(MethodName);
    size_t len = 0;
    if (STRefMethodName.startswith("standard"))
      len = strlen("standard");
    else if (STRefMethodName.startswith("shared"))
      len = strlen("shared");
    else if (STRefMethodName.startswith("default"))
      len = strlen("default");
    else
      return;
    MethodName = STRefMethodName.substr(len);
  }
  std::string MethodNameSubStr = MethodName.substr(0, 3);
  StringRef MethodNamePrefix(MethodNameSubStr);
  std::string StringLoweredMethodNamePrefix = MethodNamePrefix.lower();
  MethodNamePrefix = StringLoweredMethodNamePrefix;
  size_t Ix = LoweredClassName.rfind(MethodNamePrefix);
  if (Ix == StringRef::npos)
    return;
  std::string ClassNamePostfix = LoweredClassName.substr(Ix);
  StringRef LoweredMethodName(MethodName);
  std::string StringLoweredMethodName = LoweredMethodName.lower();
  LoweredMethodName = StringLoweredMethodName;
  if (!LoweredMethodName.startswith(ClassNamePostfix))
    return;
  ReplaceWithInstancetype(*this, OM);
}

void ObjCMigrateASTConsumer::migrateFunctionDeclAnnotation(
                                                ASTContext &Ctx,
                                                const FunctionDecl *FuncDecl) {
  if (FuncDecl->hasAttr<CFAuditedTransferAttr>() ||
      FuncDecl->getAttr<CFReturnsRetainedAttr>() ||
      FuncDecl->getAttr<CFReturnsNotRetainedAttr>() ||
      FuncDecl->hasBody())
    return;
}

void ObjCMigrateASTConsumer::migrateObjCMethodDeclAnnotation(
                                            ASTContext &Ctx,
                                            const ObjCMethodDecl *MethodDecl) {
  if (MethodDecl->hasAttr<CFAuditedTransferAttr>() ||
      MethodDecl->getAttr<CFReturnsRetainedAttr>() ||
      MethodDecl->getAttr<CFReturnsNotRetainedAttr>() ||
      MethodDecl->hasBody())
    return;
}

namespace {

class RewritesReceiver : public edit::EditsReceiver {
  Rewriter &Rewrite;

public:
  RewritesReceiver(Rewriter &Rewrite) : Rewrite(Rewrite) { }

  virtual void insert(SourceLocation loc, StringRef text) {
    Rewrite.InsertText(loc, text);
  }
  virtual void replace(CharSourceRange range, StringRef text) {
    Rewrite.ReplaceText(range.getBegin(), Rewrite.getRangeSize(range), text);
  }
};

}

void ObjCMigrateASTConsumer::HandleTranslationUnit(ASTContext &Ctx) {
  
  TranslationUnitDecl *TU = Ctx.getTranslationUnitDecl();
  if (MigrateProperty)
    for (DeclContext::decl_iterator D = TU->decls_begin(), DEnd = TU->decls_end();
         D != DEnd; ++D) {
      if (ObjCInterfaceDecl *CDecl = dyn_cast<ObjCInterfaceDecl>(*D))
        migrateObjCInterfaceDecl(Ctx, CDecl);
      else if (ObjCProtocolDecl *PDecl = dyn_cast<ObjCProtocolDecl>(*D))
        ObjCProtocolDecls.insert(PDecl);
      else if (const ObjCImplementationDecl *ImpDecl =
               dyn_cast<ObjCImplementationDecl>(*D))
        migrateProtocolConformance(Ctx, ImpDecl);
      else if (const EnumDecl *ED = dyn_cast<EnumDecl>(*D)) {
        DeclContext::decl_iterator N = D;
        ++N;
        if (N != DEnd)
          if (const TypedefDecl *TD = dyn_cast<TypedefDecl>(*N))
            migrateNSEnumDecl(Ctx, ED, TD);
      }
      else if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(*D))
        migrateFunctionDeclAnnotation(Ctx, FD);
      else if (const ObjCMethodDecl *MD = dyn_cast<ObjCMethodDecl>(*D))
        migrateObjCMethodDeclAnnotation(Ctx, MD);
      
      // migrate methods which can have instancetype as their result type.
      if (ObjCContainerDecl *CDecl = dyn_cast<ObjCContainerDecl>(*D))
        migrateInstanceType(Ctx, CDecl);
    }
  
  Rewriter rewriter(Ctx.getSourceManager(), Ctx.getLangOpts());
  RewritesReceiver Rec(rewriter);
  Editor->applyRewrites(Rec);

  for (Rewriter::buffer_iterator
        I = rewriter.buffer_begin(), E = rewriter.buffer_end(); I != E; ++I) {
    FileID FID = I->first;
    RewriteBuffer &buf = I->second;
    const FileEntry *file = Ctx.getSourceManager().getFileEntryForID(FID);
    assert(file);
    SmallString<512> newText;
    llvm::raw_svector_ostream vecOS(newText);
    buf.write(vecOS);
    vecOS.flush();
    llvm::MemoryBuffer *memBuf = llvm::MemoryBuffer::getMemBufferCopy(
                   StringRef(newText.data(), newText.size()), file->getName());
    SmallString<64> filePath(file->getName());
    FileMgr.FixupRelativePath(filePath);
    Remapper.remap(filePath.str(), memBuf);
  }

  if (IsOutputFile) {
    Remapper.flushToFile(MigrateDir, Ctx.getDiagnostics());
  } else {
    Remapper.flushToDisk(MigrateDir, Ctx.getDiagnostics());
  }
}

bool MigrateSourceAction::BeginInvocation(CompilerInstance &CI) {
  CI.getDiagnostics().setIgnoreAllWarnings(true);
  return true;
}

ASTConsumer *MigrateSourceAction::CreateASTConsumer(CompilerInstance &CI,
                                                  StringRef InFile) {
  PPConditionalDirectiveRecord *
    PPRec = new PPConditionalDirectiveRecord(CI.getSourceManager());
  CI.getPreprocessor().addPPCallbacks(PPRec);
  return new ObjCMigrateASTConsumer(CI.getFrontendOpts().OutputFile,
                                    /*MigrateLiterals=*/true,
                                    /*MigrateSubscripting=*/true,
                                    /*MigrateProperty*/true,
                                    Remapper,
                                    CI.getFileManager(),
                                    PPRec,
                                    CI.getPreprocessor(),
                                    /*isOutputFile=*/true); 
}
