//===-- CGObjCJit.cpp - Interface to host environment Objective-C Runtime ===-//
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// This bit of ugliness is done to share all the type helpers without
// causing merge issues.
//
#ifdef INCLUDE_JIT_OBJC_RUNTIME

// To avoid clang having to be built with objc runtime libs
//
#include <dlfcn.h>

#include "llvm/Analysis/Verifier.h"

// Macro to clean up function dynamic load statements
//
#define LOAD_FN(fn, fn_name) *(void**)(&fn) = dlsym(RTLD_DEFAULT, fn_name)

namespace {

/// Class that lazily initialises the runtime function.  Avoids inserting the
/// types and the function declaration into a module if they're not used, and
/// avoids constructing the type more than once if it's used more than once.
class LazyRuntimeFunction {
  CodeGenModule *CGM;
  std::vector<llvm::Type*> ArgTys;
  const char *FunctionName;
  llvm::Constant *Function;
public:
  /// Constructor leaves this class uninitialized, because it is intended to
  /// be used as a field in another class and not all of the types that are
  /// used as arguments will necessarily be available at construction time.
  LazyRuntimeFunction() : CGM(0), FunctionName(0), Function(0) {}

  /// Initialises the lazy function with the name, return type, and the types
  /// of the arguments.
  END_WITH_NULL
  void init(CodeGenModule *Mod, const char *name,
            llvm::Type *RetTy, ...) {
    CGM = Mod;
    FunctionName = name;
    Function = 0;
    ArgTys.clear();
    va_list Args;
    va_start(Args, RetTy);
    while (llvm::Type *ArgTy = va_arg(Args, llvm::Type*)) {
      ArgTys.push_back(ArgTy);
    }
    va_end(Args);
    // Push the return type on at the end so we can pop it off easily
    ArgTys.push_back(RetTy);
  }
  bool isValid() const {
    return FunctionName != 0;
  }
  /// Overloaded cast operator, allows the class to be implicitly cast to an
  /// LLVM constant.
  operator llvm::Constant*() {
    if (!Function) {
      if (0 == FunctionName) {
        return 0;
      }
      // We put the return type on the end of the vector, so pop it back off
      llvm::Type *RetTy = ArgTys.back();
      ArgTys.pop_back();
      llvm::FunctionType *FTy = llvm::FunctionType::get(RetTy, ArgTys, false);
      Function =
        cast<llvm::Constant>(CGM->CreateRuntimeFunction(FTy, FunctionName));
      // We won't need to use the types again, so we may as well clean up the
      // vector now
      ArgTys.resize(0);
    }
    return Function;
  }
  operator llvm::Function*() {
    return cast<llvm::Function>((llvm::Constant*)*this);
  }

};

/// CGObjCJit
///
class CGObjCJit : public CGObjCRuntime {

protected: // types

  typedef DeclContext::specific_decl_iterator<ObjCIvarDecl> ivar_iterator;
  typedef void* (*_imp_t)(void*, void*, ...);

  /// Null pointer value.  Mainly used as a terminator in various arrays.
  llvm::Constant *NULLPtr;

private: // data

  bool isUsable;
  CodeGen::CodeGenModule &CGM;
  llvm::LLVMContext &VMContext;
  ObjCTypesHelper ObjCTypes;

  llvm::Type *ImpPtrTy;

  llvm::Function *JitInitFunction;
  llvm::BasicBlock *JitInitBlock;
  std::set<std::string> MethodTypeStrings;

  LazyRuntimeFunction fn_objc_getClass;
  LazyRuntimeFunction fn_objc_getMetaClass;
  LazyRuntimeFunction fn_objc_getProtocol;
  LazyRuntimeFunction fn_class_addMethod;
  LazyRuntimeFunction fn_class_replaceMethod;
  LazyRuntimeFunction fn_class_getMethodImplementation;
  LazyRuntimeFunction fn_object_getClass;

  // GNU runtime workaround
  LazyRuntimeFunction fn_objc_msg_lookup;

  // MethodDefinitions - map of methods which have been defined in
  // this translation unit.
  llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*> MethodDefinitions;

  // Protocols - map of protocols not registered with the runtime.
  llvm::StringMap<void*> DefinedProtocols;

  // Runtime-specific isa value for protocols, determined on init
  void *ProtocolIsaPointer;

  // "Constant" string support
  void  *stringClass;
  void  *allocSel;
  void  *stringInitSel;
  void  *wideStringInitSel;
  _imp_t stringAlloc_imp;
  void  *defaultCollector;
  void  *disableCollectorSel;
  _imp_t disableCollector_imp;

protected: // methods

  // Native ObjC runtine APIs, referenced dynamically. This avoid clang having
  // to be built with objective-c runtime frameworks/libs.
  //
  void* (*_objc_getClass)(const char *);
  void* (*_objc_allocateClassPair)(void*, const char *, size_t);
  void (*_objc_registerClassPair)(void*);
  void* (*_objc_getProtocol)(const char *);
  void* (*_object_getClass)(void *);
  void* (*_sel_registerName)(const char*);
  void* (*_class_getClassVariable)(void*, const char*);
  char(*_class_addIvar)(void*, const char *, size_t, uint8_t, const char *);
  void* (*_class_addProtocol)(void *, void *);
  _imp_t(*_class_getMethodImplementation)(void*, void*);
  ptrdiff_t (*_ivar_getOffset)(void*);
  const char* (*_NSGetSizeAndAlignment)
  (const char*, unsigned long *, unsigned long*);

  // GNU runtime workaround
  _imp_t (*_objc_msg_lookup)(void *, void *);

  llvm::Value *GetMetaClass(CGBuilderTy &Builder,
                            const ObjCInterfaceDecl *ID);

  CodeGen::RValue EmitMessageSend(CodeGen::CodeGenFunction &CGF,
                                  ReturnValueSlot Return,
                                  QualType ResultType,
                                  Selector Sel,
                                  llvm::Value *Receiver,
                                  llvm::Value *ReceiverClass,
                                  const CallArgList &CallArgs,
                                  const ObjCMethodDecl *Method);

  void AddMethodsToClass(void *theClass);

  void AddIvarsToClass(void *theClass,
                       const ivar_iterator& ivar_begin,
                       const ivar_iterator& ivar_end);

  void InitConstantStringGenerator();

public:

  CGObjCJit(CodeGen::CodeGenModule &cgm);

  virtual llvm::Function *ModuleInitFunction();

  virtual llvm::Value *GetSelector(CodeGenFunction &CGF,
                                   Selector Sel, bool lval = false);

  virtual llvm::Value *GetSelector(CodeGenFunction &CGF,
                                   const ObjCMethodDecl *Method);

  virtual llvm::Constant *GetEHType(QualType T);

  virtual llvm::Constant *GenerateConstantString(const StringLiteral *);

  virtual void GenerateCategory(const ObjCCategoryImplDecl *CMD);

  virtual void GenerateClass(const ObjCImplementationDecl *ClassDecl);

  virtual void RegisterAlias(const ObjCCompatibleAliasDecl *OAD) {};

  virtual CodeGen::RValue GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                                              ReturnValueSlot Return,
                                              QualType ResultType,
                                              Selector Sel,
                                              llvm::Value *Receiver,
                                              const CallArgList &CallArgs,
                                              const ObjCInterfaceDecl *Class,
                                              const ObjCMethodDecl *Method);

  virtual CodeGen::RValue
  GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                           ReturnValueSlot Return,
                           QualType ResultType,
                           Selector Sel,
                           const ObjCInterfaceDecl *Class,
                           bool isCategoryImpl,
                           llvm::Value *Receiver,
                           bool IsClassMessage,
                           const CallArgList &CallArgs,
                           const ObjCMethodDecl *Method);

  virtual llvm::Value *GenerateProtocolRef(CodeGenFunction &CGF,
                                           const ObjCProtocolDecl *PD);

  virtual void GenerateProtocol(const ObjCProtocolDecl *OPD);

  virtual llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
                                         const ObjCContainerDecl *CD);

  virtual llvm::Constant *GetPropertyGetFunction();

  virtual llvm::Constant *GetPropertySetFunction();

  virtual llvm::Constant *GetOptimizedPropertySetFunction(bool atomic,
                                                          bool copy);
  virtual llvm::Constant *GetGetStructFunction();

  virtual llvm::Constant *GetSetStructFunction();

  virtual llvm::Constant *GetCppAtomicObjectGetFunction();

  virtual llvm::Constant *GetCppAtomicObjectSetFunction();

  virtual llvm::Value *GetClass(CodeGenFunction &CGF,
                                const ObjCInterfaceDecl *ID);

  virtual llvm::Constant *EnumerationMutationFunction();

  virtual llvm::Constant *GetCppAtomicObjectFunction();

  virtual void EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                    const ObjCAtSynchronizedStmt &S);

  virtual void EmitTryStmt(CodeGen::CodeGenFunction &CGF,
                           const ObjCAtTryStmt &S);

  virtual void EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                             const ObjCAtThrowStmt &S,
                             bool ClearInsertionPoint = true);

  virtual llvm::Value *EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *AddrWeakObj);

  virtual void EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dest);

  virtual void EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest,
                                    bool threadlocal = false);

  virtual void EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dest,
                                  llvm::Value *ivarOffset);

  virtual void EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *src, llvm::Value *dest);

  virtual LValue EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                      QualType ObjectTy,
                                      llvm::Value *BaseValue,
                                      const ObjCIvarDecl *Ivar,
                                      unsigned CVRQualifiers);

  virtual llvm::Value *EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                      const ObjCInterfaceDecl *Interface,
                                      const ObjCIvarDecl *Ivar);

  virtual void EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *DestPtr,
                                        llvm::Value *SrcPtr,
                                        llvm::Value *Size);

  virtual llvm::Constant *BuildGCBlockLayout(CodeGen::CodeGenModule &CGM,
                                         const CodeGen::CGBlockInfo &blockInfo);


  virtual llvm::Constant *BuildRCBlockLayout(CodeGenModule &CGM,
                                             const CGBlockInfo &blockInfo) {
    return NULLPtr;
  }

  virtual llvm::Constant *BuildByrefLayout(CodeGenModule &CGM,
                                           QualType T) {
    return NULLPtr;
  }

  /// GetClassGlobal - Return the global variable for the Objective-C
  /// class of the given name.
  virtual llvm::GlobalVariable *GetClassGlobal(const std::string &Name) {
    llvm_unreachable("CGObjCJit::GetClassGlobal");
  }
};


CGObjCJit::CGObjCJit(CodeGen::CodeGenModule &cgm)
  : CGObjCRuntime(cgm),
    isUsable(false),
    CGM(cgm),
    VMContext(cgm.getLLVMContext()),
    ObjCTypes(cgm),
    JitInitFunction(0) {

  //puts("Constructing CGObjCJit (host runtime proxy)");

  llvm::PointerType *PtrToInt8Ty =
      llvm::Type::getInt8Ty(VMContext)->getPointerTo();

  NULLPtr = llvm::ConstantPointerNull::get(PtrToInt8Ty);

  // TODO: For now, just searching for pre-loaded objc runtime
  LOAD_FN(_objc_getClass, "objc_getClass");

  // Don't bother to try the rest if the first one wasn't available
  if (_objc_getClass) {
    isUsable = true;
    LOAD_FN(_objc_allocateClassPair,        "objc_allocateClassPair");
    LOAD_FN(_objc_registerClassPair,        "objc_registerClassPair");
    LOAD_FN(_objc_getProtocol,              "objc_getProtocol");
    LOAD_FN(_object_getClass,               "object_getClass");
    LOAD_FN(_sel_registerName,              "sel_registerName");
    LOAD_FN(_class_getClassVariable,        "class_getClassVariable");
    LOAD_FN(_class_addIvar,                 "class_addIvar");
    LOAD_FN(_class_addProtocol,             "class_addProtocol");
    LOAD_FN(_class_getMethodImplementation, "class_getMethodImplementation");
    LOAD_FN(_ivar_getOffset,                "ivar_getOffset");
    LOAD_FN(_NSGetSizeAndAlignment,         "NSGetSizeAndAlignment");

    //puts("Creating .objc_jit_init()");
    llvm::FunctionType *initFuncType =
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), false);

    JitInitFunction =
        cast<llvm::Function>(CGM.getModule().getOrInsertFunction(
                                               llvm::StringRef(".objc_jit_init"),
                                               initFuncType));
    JitInitFunction->setLinkage(llvm::Function::PrivateLinkage);
    JitInitFunction->setCallingConv(llvm::CallingConv::C);
    JitInitBlock =
      llvm::BasicBlock::Create(VMContext, "entry", JitInitFunction);


    // Define VM objc_getClass function

    fn_objc_getClass.init(&CGM, "objc_getClass",
                          ObjCTypes.ClassPtrTy,
                          ObjCTypes.Int8PtrTy,
                          NULL);

    fn_objc_getMetaClass.init(&CGM, "objc_getMetaClass",
                              ObjCTypes.ClassPtrTy,
                              ObjCTypes.Int8PtrTy,
                              NULL);

    fn_objc_getProtocol.init(&CGM, "objc_getProtocol",
                             ObjCTypes.ProtocolPtrTy,
                             ObjCTypes.Int8PtrTy,
                             NULL);

    fn_object_getClass.init(&CGM, "object_getClass",
                            ObjCTypes.ClassPtrTy,
                            ObjCTypes.ObjectPtrTy,
                            NULL);

    // Define VM class_addMethod and class_replaceMethod function calls for
    // the init function

    llvm::Type *ImpParams[] = {ObjCTypes.ObjectPtrTy, ObjCTypes.SelectorPtrTy};
    ImpPtrTy = llvm::FunctionType::get(ObjCTypes.ObjectPtrTy,
                                       ImpParams,
                                       false)->getPointerTo();

    fn_class_addMethod.init(&CGM, "class_addMethod",
                            llvm::Type::getInt8Ty(VMContext),
                            ObjCTypes.ClassPtrTy,
                            ObjCTypes.SelectorPtrTy,
                            ImpPtrTy,
                            ObjCTypes.Int8PtrTy,
                            NULL);

    fn_class_replaceMethod.init(&CGM, "class_replaceMethod",
                                llvm::Type::getInt8Ty(VMContext),
                                ObjCTypes.ClassTy,
                                ObjCTypes.SelectorPtrTy,
                                ImpPtrTy,
                                ObjCTypes.Int8PtrTy,
                                NULL);

    fn_class_getMethodImplementation.init(&CGM, "class_getMethodImplementation",
                                          ImpPtrTy,
                                          ObjCTypes.ClassPtrTy,
                                          ObjCTypes.SelectorPtrTy,
                                          NULL);

    // Unfortunately, even though class_getMethodImplementation is provided by
    // the GNU runtime, it doesn't work on a class unless objc_msg_lookup was
    // called first.
    //
    LOAD_FN(_objc_msg_lookup, "objc_msg_lookup");

    if (_objc_msg_lookup) {
      fn_objc_msg_lookup.init(&CGM, "objc_msg_lookup",
                              ImpPtrTy,
                              ObjCTypes.ClassPtrTy,
                              ObjCTypes.SelectorPtrTy,
                              NULL);
    }

    // Save 'isa' pointer value used for locally-stored prototypes
    void *sampleProtocol = _objc_getProtocol("NSObject");
    ProtocolIsaPointer = *((void**)sampleProtocol);

    InitConstantStringGenerator();
  }
}


llvm::Function *CGObjCJit::ModuleInitFunction() {
  if (isUsable) {
    // Finalize the init function
    CGBuilderTy RetBuilder(JitInitBlock);
    RetBuilder.CreateRetVoid();
//    llvm::verifyFunction(*JitInitFunction);
  }
  return NULL;
}


llvm::Value *CGObjCJit::GetSelector(CodeGenFunction &CGF,
                                    Selector Sel, bool /*lval*/) {
  if (isUsable) {
    void *sel = _sel_registerName(Sel.getAsString().c_str());
    return llvm::Constant::getIntegerValue(ObjCTypes.SelectorPtrTy,
                                           llvm::APInt(sizeof(void*) * 8,
                                               (uint64_t)sel));
  }
  return 0;
}


llvm::Value *CGObjCJit::GetSelector(CodeGenFunction &CGF,
                                    const ObjCMethodDecl *Method) {
  return GetSelector(CGF, Method->getSelector());
}


llvm::Constant *CGObjCJit::GetEHType(QualType T) {
  llvm_unreachable("asking for catch type for ObjC type in JIT runtime");
  return 0;
}


/// Not really a constant string; can be explicitly released...
llvm::Constant *CGObjCJit::GenerateConstantString(const StringLiteral *SL) {
  if (isUsable && stringClass) {
    void *stringInstance = stringAlloc_imp(stringClass, allocSel);
    const llvm::StringRef& literalStringRef(SL->getString());

    // Need to get specific class of instance
    void *instanceStringClass = _object_getClass(stringInstance);
    _imp_t init_imp;

    if (!SL->isWide()) {
      init_imp = _class_getMethodImplementation(instanceStringClass,
                 stringInitSel);
      stringInstance = init_imp(stringInstance,
                                stringInitSel,
                                literalStringRef.data(),
                                literalStringRef.size(),
                                4); // NSUTF8StringEncoding = 4 (shouldn't change)

    } else { // shouldn't see this anyway; does StringRef support wide?
      init_imp = _class_getMethodImplementation(instanceStringClass,
                 wideStringInitSel);
      stringInstance = init_imp(stringInstance,
                                wideStringInitSel,
                                literalStringRef.data(),
                                literalStringRef.size());
    }

    // If applicable, make sure the string is not freed by the garbage collector
    if (defaultCollector) {
      disableCollector_imp(defaultCollector,
                           disableCollectorSel,
                           stringInstance);
    }

    // TODO: save string objects to a list?

    return llvm::Constant::getIntegerValue(ObjCTypes.ObjectPtrTy,
                                           llvm::APInt(sizeof(void*) * 8,
                                               (uint64_t)stringInstance));
  }
  return 0;
}


void CGObjCJit::GenerateCategory(const ObjCCategoryImplDecl *CMD) {
  if (isUsable) {
    // Just replace/add all the methods at runtime.
    void *theClass =
      _objc_getClass(CMD->getClassInterface()->getIdentifier()->getNameStart());
    AddMethodsToClass(theClass);
  }
}


void CGObjCJit::GenerateClass(const ObjCImplementationDecl *ClassDecl) {
  if (isUsable) {
    const char* ClassName = ClassDecl->getIdentifier()->getNameStart();

    void* Superclass = 0;
    ObjCInterfaceDecl *superClassDecl =
      ClassDecl->getClassInterface()->getSuperClass();

    if (superClassDecl) {
      const char* superClassName =
          superClassDecl->getIdentifier()->getNameStart();
      Superclass = _objc_getClass(superClassName);
    }

    void *theClass =
      _objc_allocateClassPair(Superclass, ClassName, 0); // TODO: always zero?

    // Add methods
    AddMethodsToClass(theClass);

    // Add interface ivars
    const ObjCInterfaceDecl *classInterfaceDecl = ClassDecl->getClassInterface();
    AddIvarsToClass(theClass,
                    classInterfaceDecl->ivar_begin(),
                    classInterfaceDecl->ivar_end());

    // Add implementation ivars
    AddIvarsToClass(theClass,
                    ClassDecl->ivar_begin(),
                    ClassDecl->ivar_end());

    // Add protocols
    ObjCInterfaceDecl::protocol_iterator protocol =
      classInterfaceDecl->protocol_begin();
    const ObjCInterfaceDecl::protocol_iterator protocol_end =
      classInterfaceDecl->protocol_end();
    while (protocol != protocol_end) {
      void *theProtocol = 0;
      // Search "locally" first, then from runtime
      llvm::StringMap<void*>::iterator proto_local =
        DefinedProtocols.find((*protocol)->getName());
      if (proto_local != DefinedProtocols.end()) {
        theProtocol = proto_local->second;
      } else {
        theProtocol = _objc_getProtocol((*protocol)->getNameAsString().c_str());
      }
      _class_addProtocol(theClass, theProtocol);
      protocol++;
    }

    // Finalize class (adding methods later, at runtime, in init function)
    _objc_registerClassPair(theClass);
  }
}


CodeGen::RValue
CGObjCJit::GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                               ReturnValueSlot Return,
                               QualType ResultType,
                               Selector Sel,
                               llvm::Value *Receiver,
                               const CallArgList &CallArgs,
                               const ObjCInterfaceDecl *Class,
                               const ObjCMethodDecl *Method) {
  llvm::Value *ReceiverObj =
      CGF.Builder.CreateBitCast(Receiver, ObjCTypes.ObjectPtrTy);
  llvm::Value *ReceiverClass;
  if (Class) {
    ReceiverClass = GetMetaClass(CGF.Builder, Class);
  } else {
    ReceiverClass = CGF.Builder.CreateCall(fn_object_getClass, ReceiverObj);
  }

  return EmitMessageSend(CGF, Return, ResultType, Sel,
                         ReceiverObj, ReceiverClass,
                         CallArgs, Method);
}


CodeGen::RValue
CGObjCJit::GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                                    ReturnValueSlot Return,
                                    QualType ResultType,
                                    Selector Sel,
                                    const ObjCInterfaceDecl *Class,
                                    bool isCategoryImpl,
                                    llvm::Value *Receiver,
                                    bool IsClassMessage,
                                    const CodeGen::CallArgList &CallArgs,
                                    const ObjCMethodDecl *Method) {
  llvm::Value *ReceiverClass;
  if (IsClassMessage)
    if (isCategoryImpl) {
      ReceiverClass = GetClass(CGF, Class);
    } else {
      ReceiverClass = GetMetaClass(CGF.Builder, Class);
    }
  else {
    ReceiverClass = GetClass(CGF, Class->getSuperClass());
  }

  return EmitMessageSend(CGF, Return, ResultType, Sel,
                         Receiver, ReceiverClass,
                         CallArgs, Method);
}


llvm::Value *CGObjCJit::GenerateProtocolRef(CodeGenFunction &CGF,
                                            const ObjCProtocolDecl *PD) {
  if (isUsable) {
    llvm::Value *theProtocol;  
    // If not locally defined, retrieve from runtime
    llvm::StringMap<void*>::iterator it =
      DefinedProtocols.find(PD->getIdentifier()->getNameStart());
    if (it != DefinedProtocols.end()) {
      theProtocol =
          llvm::Constant::getIntegerValue(ObjCTypes.ProtocolPtrTy,
                                          llvm::APInt(sizeof(void*) * 8,
                                                      (uint64_t)it->second));
    } else {
      llvm::Value *ProtocolName =
          CGF.Builder.CreateGlobalStringPtr(PD->getNameAsString());
      theProtocol = CGF.Builder.CreateCall(fn_objc_getProtocol, ProtocolName);
    }
    return CGF.Builder.CreateBitCast(theProtocol, ObjCTypes.getExternalProtocolPtrTy());
  }
  return 0;
}


void CGObjCJit::GenerateProtocol(const ObjCProtocolDecl *OPD) {
  if (isUsable) {
    // if it doesn't already exist in the runtime...
    if (!_objc_getProtocol(OPD->getIdentifier()->getNameStart())) {
      // Create a dummy protocol
      struct _objc_protocol {
        void *isa;
        const char *protocol_name;
        struct _objc_protocol **_objc_protocol_list;
        void *instance_methods;
        void *class_methods;
      };
      _objc_protocol *theProto = new _objc_protocol;

      theProto->isa = ProtocolIsaPointer;
      theProto->protocol_name = OPD->getIdentifier()->getNameStart();
      theProto->_objc_protocol_list = 0; // TODO: populate
      theProto->instance_methods = 0; // unused
      theProto->class_methods = 0;    // unused

      DefinedProtocols[OPD->getIdentifier()->getNameStart()] = theProto;
    }
  }
}


llvm::Function *CGObjCJit::GenerateMethod(const ObjCMethodDecl *OMD,
                                          const ObjCContainerDecl *CD) {

  assert(CD && "Missing container decl in GetNameForMethod");

  llvm::SmallString<256> Name;
  llvm::raw_svector_ostream OS(Name);

  OS << '\01' << (OMD->isInstanceMethod() ? '-' : '+')
     << '[' << CD->getName();
  if (const ObjCCategoryImplDecl *CID =
        dyn_cast<ObjCCategoryImplDecl>(OMD->getDeclContext())) {
    OS << '(' << CID << ')';
  }
  OS << ' ' << OMD->getSelector().getAsString() << ']';

  CodeGenTypes &Types = CGM.getTypes();
  llvm::FunctionType *MethodTy =
    Types.GetFunctionType(Types.arrangeObjCMethodDeclaration(OMD));
  llvm::Function *Method =
    llvm::Function::Create(MethodTy,
                           llvm::GlobalValue::InternalLinkage,
                           Name.str(),
                           &CGM.getModule());
  MethodDefinitions.insert(std::make_pair(OMD, Method));

  return Method;
}


llvm::Constant *CGObjCJit::GetPropertyGetFunction() {
  return ObjCTypes.getGetPropertyFn();
//  return GetPropertyFn;
}


llvm::Constant *CGObjCJit::GetPropertySetFunction() {
  return ObjCTypes.getSetPropertyFn();
}

llvm::Constant *CGObjCJit::GetOptimizedPropertySetFunction(bool atomic,
    bool copy) {
  return ObjCTypes.getOptimizedSetPropertyFn(atomic, copy);
}

llvm::Constant *CGObjCJit::GetGetStructFunction() {
  return ObjCTypes.getCopyStructFn();
}


llvm::Constant *CGObjCJit::GetSetStructFunction() {
  return ObjCTypes.getCopyStructFn();
}


llvm::Value *CGObjCJit::GetClass(CodeGenFunction &CGF,
                                 const ObjCInterfaceDecl *ID) {
  if (isUsable) {
    llvm::Value *ClassName =
        CGF.Builder.CreateGlobalStringPtr(ID->getNameAsString());
    return CGF.Builder.CreateCall(fn_objc_getClass, ClassName);
  }
  return 0;
}

llvm::Constant *CGObjCJit::GetCppAtomicObjectSetFunction() {
  return ObjCTypes.getCppAtomicObjectFunction();
}

llvm::Constant *CGObjCJit::GetCppAtomicObjectGetFunction() {
  return ObjCTypes.getCppAtomicObjectFunction();
}

llvm::Constant *CGObjCJit::EnumerationMutationFunction() {
  return ObjCTypes.getEnumerationMutationFn();
}

llvm::Constant *CGObjCJit::GetCppAtomicObjectFunction() {
  return ObjCTypes.getCppAtomicObjectFunction();
}

void
CGObjCJit::EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                const ObjCAtSynchronizedStmt &S) {
  // TODO: review this

  EmitAtSynchronizedStmt(CGF, S,
                         cast<llvm::Function>(ObjCTypes.getSyncEnterFn()),
                         cast<llvm::Function>(ObjCTypes.getSyncExitFn()));
}

void
CGObjCJit::EmitTryStmt(CodeGenFunction &CGF, const ObjCAtTryStmt &S) {

// TODO: does this work on non-Macs?

  llvm::Type *params[] = { ObjCTypes.Int8PtrTy };
  llvm::Constant *BeginCatchFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjCTypes.Int8PtrTy,
                              params, false),
                              "objc_begin_catch");
  llvm::Constant *EndCatchFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(CGM.VoidTy, false),
                              "objc_end_catch");
  EmitTryCatchStmt(CGF, S,
                   cast<llvm::Function>(BeginCatchFn),
                   cast<llvm::Function>(EndCatchFn),
                   cast<llvm::Function>(ObjCTypes.getExceptionRethrowFn()));
}


void CGObjCJit::EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                              const ObjCAtThrowStmt &S,
                              bool ClearInsertionPoint) {
// TODO: does this work on non-Macs?

  if (const Expr *ThrowExpr = S.getThrowExpr()) {
    llvm::Value *Exception = CGF.EmitObjCThrowOperand(ThrowExpr);
    Exception = CGF.Builder.CreateBitCast(Exception, ObjCTypes.ObjectPtrTy);
    CGF.EmitCallOrInvoke(ObjCTypes.getExceptionThrowFn(), Exception)
    .setDoesNotReturn();
  } else {
    CGF.EmitCallOrInvoke(ObjCTypes.getExceptionRethrowFn())
    .setDoesNotReturn();
  }

//  CGF.Builder.CreateUnreachable();

  // Clear the insertion point to indicate we are in unreachable code.
  if (ClearInsertionPoint) {
    CGF.Builder.ClearInsertionPoint();
  }
}


llvm::Value * CGObjCJit::EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                          llvm::Value *AddrWeakObj) {
  llvm::Type* DestTy =
    cast<llvm::PointerType>(AddrWeakObj->getType())->getElementType();
  AddrWeakObj = CGF.Builder.CreateBitCast(AddrWeakObj, ObjCTypes.PtrObjectPtrTy);
  llvm::Value *read_weak = CGF.Builder.CreateCall(ObjCTypes.getGcReadWeakFn(),
                           AddrWeakObj, "weakread");
  read_weak = CGF.Builder.CreateBitCast(read_weak, DestTy);
  return read_weak;
}


void CGObjCJit::EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, llvm::Value *dst) {
  llvm::Type * SrcTy = src->getType();
  if (!isa<llvm::PointerType>(SrcTy)) {
    unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
    assert(Size <= 8 && "does not support size > 8");
    src = (Size == 4 ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
           : CGF.Builder.CreateBitCast(src, ObjCTypes.LongTy));
    src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
  }
  src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
  dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
  CGF.Builder.CreateCall2(ObjCTypes.getGcAssignWeakFn(),
                          src, dst, "weakassign");
  return;
}


void CGObjCJit::EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                     llvm::Value *src, llvm::Value *dst,
                                     bool threadlocal) {
  llvm::Type * SrcTy = src->getType();
  if (!isa<llvm::PointerType>(SrcTy)) {
    unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
    assert(Size <= 8 && "does not support size > 8");
    src = (Size == 4 ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
           : CGF.Builder.CreateBitCast(src, ObjCTypes.LongTy));
    src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
  }
  src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
  dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
  if (!threadlocal)
    CGF.Builder.CreateCall2(ObjCTypes.getGcAssignGlobalFn(),
                            src, dst, "globalassign");
  else
    CGF.Builder.CreateCall2(ObjCTypes.getGcAssignThreadLocalFn(),
                            src, dst, "threadlocalassign");
  return;
}


void CGObjCJit::EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src,
                                   llvm::Value *dst,
                                   llvm::Value *ivarOffset) {
  llvm::Type * SrcTy = src->getType();
  if (!isa<llvm::PointerType>(SrcTy)) {
    unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
    assert(Size <= 8 && "does not support size > 8");
    src = (Size == 4 ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
           : CGF.Builder.CreateBitCast(src, ObjCTypes.LongTy));
    src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
  }
  src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
  dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
  CGF.Builder.CreateCall3(ObjCTypes.getGcAssignIvarFn(),
                          src, dst, ivarOffset);
  return;
}


void CGObjCJit::EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *src, llvm::Value *dst) {
  llvm::Type * SrcTy = src->getType();
  if (!isa<llvm::PointerType>(SrcTy)) {
    unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
    assert(Size <= 8 && "does not support size > 8");
    src = (Size == 4 ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
           : CGF.Builder.CreateBitCast(src, ObjCTypes.LongTy));
    src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
  }
  src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
  dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
  CGF.Builder.CreateCall2(ObjCTypes.getGcAssignStrongCastFn(),
                          src, dst, "weakassign");
  return;
}


LValue CGObjCJit::EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                       QualType ObjectTy,
                                       llvm::Value *BaseValue,
                                       const ObjCIvarDecl *Ivar,
                                       unsigned CVRQualifiers) {
  const ObjCInterfaceDecl *ID =
    ObjectTy->getAs<ObjCObjectType>()->getInterface();
  return EmitValueForIvarAtOffset(CGF, ID, BaseValue, Ivar, CVRQualifiers,
                                  EmitIvarOffset(CGF, ID, Ivar));
}


llvm::Value *CGObjCJit::EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                       const ObjCInterfaceDecl *Interface,
                                       const ObjCIvarDecl *Ivar) {
  uint64_t Offset = ComputeIvarBaseOffset(CGM, Interface, Ivar);
  return llvm::ConstantInt::get(
           CGM.getTypes().ConvertType(CGM.getContext().LongTy),
           Offset);

//  if (isUsable) {
//    void *theClass =
//        _objc_getClass(Interface->getIdentifier()->getNameStart());
//    void *ivar = _class_getClassVariable(theClass,
//                                         Ivar->getIdentifier()->getNameStart());
//    ptrdiff_t offset = _ivar_getOffset(ivar);
//    llvm::Constant *offsetValue =
//        llvm::Constant::getIntegerValue(ObjCTypes.LongTy,
//                                        llvm::APInt(sizeof(ptrdiff_t)*8, (uint64_t)offset));
////    return CGF.Builder.CreateLoad(offsetValue,"ivar");
//    return offsetValue;
//  }
//  return 0;
}


void
CGObjCJit::EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *DestPtr,
                                    llvm::Value *SrcPtr,
                                    llvm::Value *Size) {
  SrcPtr = CGF.Builder.CreateBitCast(SrcPtr, ObjCTypes.Int8PtrTy);
  DestPtr = CGF.Builder.CreateBitCast(DestPtr, ObjCTypes.Int8PtrTy);
  CGF.Builder.CreateCall3(ObjCTypes.GcMemmoveCollectableFn(),
                          DestPtr, SrcPtr, Size);
  return;
}


llvm::Constant *CGObjCJit::BuildGCBlockLayout(CodeGenModule &CGM,
    const CGBlockInfo &blockInfo) {
  //puts("Calling CGObjCJit::BuildGCBlockLayout");
  return llvm::Constant::getNullValue(llvm::Type::getInt8PtrTy(VMContext));
}


// Protected methods


llvm::Value *CGObjCJit::GetMetaClass(CGBuilderTy &Builder,
                                     const ObjCInterfaceDecl *ID) {
  if (isUsable) {
    llvm::Value *ClassName =
        Builder.CreateGlobalStringPtr(ID->getNameAsString());
    return Builder.CreateCall(fn_objc_getMetaClass, ClassName);
  }
  return 0;
}


//llvm::Value *CGObjCJit::EmitSuperClassRef(const ObjCInterfaceDecl *ID) {
//  if (isUsable) {
//    ObjCInterfaceDecl *superClassDecl =
//        ID->getSuperClass();
//    if (superClassDecl) {
//      void* Superclass = _objc_getClass(superClassDecl->getIdentifier()->getNameStart());
//      return llvm::Constant::getIntegerValue(ObjCTypes.ClassTy,
//                                             llvm::APInt(sizeof(void*)*8, (uint64_t)Superclass));
//    }
//  }
//  return 0;
//}


CodeGen::RValue
CGObjCJit::EmitMessageSend(CodeGen::CodeGenFunction &CGF,
                           ReturnValueSlot Return,
                           QualType ResultType,
                           Selector Sel,
                           llvm::Value *Arg0,
                           llvm::Value *Arg0Class,
                           const CallArgList &CallArgs,
                           const ObjCMethodDecl *Method) {

  llvm::Value *Arg1 = GetSelector(CGF, Sel);

  CallArgList ActualArgs;
  ActualArgs.add(RValue::get(Arg0), CGF.getContext().getObjCIdType());
  ActualArgs.add(RValue::get(Arg1), CGF.getContext().getObjCSelType());
  ActualArgs.addFrom(CallArgs);

  if (Method)
    assert(CGM.getContext().getCanonicalType(Method->getResultType()) ==
           CGM.getContext().getCanonicalType(ResultType) &&
           "Result type mismatch!");

  // Perform the following:
  //   imp = class_getMethodImplementation( Arg0Class, Arg1 );
  //   (*imp)( Arg0, Arg1, CallArgs );
  llvm::CallInst *getImp;

  // Unfortunately, using the GNU runtime version of
  // class_getMethodImplementation and then calling the resulting
  // IMP doesn't work unless objc_msg_lookup was already
  // called first. TODO: avoid doing this every time
  //
  if (fn_objc_msg_lookup.isValid()) {
    getImp = CGF.Builder.CreateCall2(fn_objc_msg_lookup,
                                     Arg0,
                                     Arg1);
  } else { // use the universal way
    getImp = CGF.Builder.CreateCall2(fn_class_getMethodImplementation,
                                     Arg0Class,
                                     Arg1);
  }

  MessageSendInfo MSI = getMessageSendInfo(Method, ResultType, ActualArgs);
  llvm::Value *theImp = CGF.Builder.CreateBitCast(getImp, MSI.MessengerType);
  return CGF.EmitCall(MSI.CallInfo, theImp, Return, ActualArgs);
}


void CGObjCJit::AddMethodsToClass(void *theClass) {

  // Methods need to be added at runtime. Method function pointers (IMP)
  // are not available until then.

  CGBuilderTy Builder(JitInitBlock);
  CodeGen::CodeGenFunction CGF(CGM);

  void *theMetaclass = _object_getClass(theClass);

  llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*>::iterator I =
      MethodDefinitions.begin();

  while (I != MethodDefinitions.end()) {
    const ObjCMethodDecl *D = I->first;
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForMethodDecl(const_cast<ObjCMethodDecl*>(D),
        TypeStr);
    const char* TypeCStr = // keep in a set
      MethodTypeStrings.insert(MethodTypeStrings.begin(), TypeStr)->c_str();
    void *ClassObject = D->isClassMethod() ? theMetaclass : theClass;
    llvm::Value *ClassArg =
      llvm::Constant::getIntegerValue(ObjCTypes.ClassPtrTy,
                                      llvm::APInt(sizeof(void*) * 8,
                                          (uint64_t)ClassObject));
    llvm::Value *SelectorArg = GetSelector(CGF, D->getSelector());
    llvm::Value *TypeArg =
      llvm::Constant::getIntegerValue(ObjCTypes.Int8PtrTy,
                                      llvm::APInt(sizeof(void*) * 8,
                                          (uint64_t)TypeCStr));

    llvm::Value *MethodArg = Builder.CreateBitCast(I->second, ImpPtrTy);

    Builder.CreateCall4(fn_class_addMethod,
                        ClassArg,
                        SelectorArg,
                        MethodArg,
                        TypeArg);
    I++;
  }

  // Done with list for this implementation, so clear it
  MethodDefinitions.clear();
}


void
CGObjCJit::AddIvarsToClass(void *theClass,
                           const ivar_iterator& ivar_begin,
                           const ivar_iterator& ivar_end) {

  for (ivar_iterator ivar = ivar_begin; ivar != ivar_end; ivar++) {
    std::string TypeStr;
    CGM.getContext().getObjCEncodingForType(ivar->getType(), TypeStr);
    unsigned long size;  // TODO: 32bit support?
    unsigned long align; //
    _NSGetSizeAndAlignment(TypeStr.c_str(), &size, &align);

    //char addResult =
    _class_addIvar(theClass,
                   ivar->getIdentifier()->getNameStart(),
                   size,
                   align,
                   TypeStr.c_str());
  }
}


void
CGObjCJit::InitConstantStringGenerator() {
  if (isUsable) {
    stringClass = _objc_getClass("NSString");
    defaultCollector = 0;

    if (stringClass) {
      allocSel = _sel_registerName("alloc");

      stringInitSel = _sel_registerName("initWithBytes:length:encoding:");
      wideStringInitSel = _sel_registerName("initWithCharacters:length");

      // Unfortunately, using the GNU runtime version of
      // class_getMethodImplementation and then calling the resulting IMP
      // doesn't work unless objc_msg_lookup was already called first.
      //
      if (_objc_msg_lookup) {
        stringAlloc_imp = _objc_msg_lookup(stringClass, allocSel);
      } else { // use universal way
        void *stringMetaClass = _object_getClass(stringClass);
        stringAlloc_imp = _class_getMethodImplementation(stringMetaClass,
                                                         allocSel);
      }

      // If applicable, make sure the string is not freed by the gc

      void *garbageCollectorClass = _objc_getClass("NSGarbageCollector");

      if (garbageCollectorClass) {
        void *defaultCollectorSel = _sel_registerName("defaultCollector");
        _imp_t _imp;

        if (_objc_msg_lookup) { // GNU runtime workaround
          _imp = _objc_msg_lookup(garbageCollectorClass, defaultCollectorSel);
        } else {
          void *garbageCollectorMetaClass =
            _object_getClass(garbageCollectorClass);

          _imp = _class_getMethodImplementation(garbageCollectorMetaClass,
                                                defaultCollectorSel);
        }

        defaultCollector = _imp(garbageCollectorClass, defaultCollectorSel);

        if (defaultCollector) {
          disableCollectorSel = _sel_registerName("disableCollectorForPointer:");
          void *defaultCollectorClass = _object_getClass(defaultCollector);
          disableCollector_imp =
            _class_getMethodImplementation(defaultCollectorClass,
                                           disableCollectorSel);
        }
      }
    }
  }
}
} // namespace

CodeGen::CGObjCRuntime *
CodeGen::CreateJitObjCRuntime(CodeGen::CodeGenModule &CGM) {
  return new CGObjCJit(CGM);
}

#else // INCLUDE_JIT_OBJC_RUNTIME not defined

void ThisIsJustToRemoveTheLinkerWarning() {
}

#endif // INCLUDE_JIT_OBJC_RUNTIME defined



