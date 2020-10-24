#include <utils/branch.h>
#include <region/common/fatweaks/fatweaks.h>
#include <region/common/hgm/hgm.h>
#include <region/common/lgtweaks/lgtweaks.h>
#include <region/common/wrcweaks/wrcweaks.h>
#include <translatetype.h>
#include <region/common/common.h>
#include <region/common/heap.h>
#include "assist.h"

Assist::Assist(GlobalState* globalState_) :
    globalState(globalState_),
    immStructs(globalState, makeImmControlBlock(globalState)),
    mutNonWeakableStructs(globalState, makeAssistAndNaiveRCNonWeakableControlBlock(globalState)),
    mutWeakableStructs(
        globalState,
        makeAssistAndNaiveRCWeakableControlBlock(globalState),
        WrcWeaks::makeWeakRefHeaderStruct(globalState)),
    defaultImmutables(globalState, &immStructs),
    referendStructs(
        globalState,
        [this](Referend* referend) -> IReferendStructsSource* {
          if (globalState->program->getReferendMutability(referend) == Mutability::IMMUTABLE) {
            return &immStructs;
          } else {
            if (globalState->program->getReferendWeakability(referend) == Weakability::NON_WEAKABLE) {
              return &mutNonWeakableStructs;
            } else {
              return &mutWeakableStructs;
            }
          }
        }),
    weakRefStructs(
        [this](Referend* referend) -> IWeakRefStructsSource* {
          if (globalState->program->getReferendMutability(referend) == Mutability::IMMUTABLE) {
            assert(false);
          } else {
            if (globalState->program->getReferendWeakability(referend) == Weakability::NON_WEAKABLE) {
              assert(false);
            } else {
              return &mutWeakableStructs;
            }
          }
        }),
    fatWeaks(globalState_, &weakRefStructs),
    wrcWeaks(globalState_, &referendStructs, &weakRefStructs) {
}


void Assist::alias(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* sourceRef,
    Ref expr) {
  auto sourceRnd = sourceRef->referend;

  if (dynamic_cast<Int *>(sourceRnd) ||
      dynamic_cast<Bool *>(sourceRnd) ||
      dynamic_cast<Float *>(sourceRnd)) {
    // Do nothing for these, they're always inlined and copied.
  } else if (dynamic_cast<InterfaceReferend *>(sourceRnd) ||
      dynamic_cast<StructReferend *>(sourceRnd) ||
      dynamic_cast<KnownSizeArrayT *>(sourceRnd) ||
      dynamic_cast<UnknownSizeArrayT *>(sourceRnd) ||
      dynamic_cast<Str *>(sourceRnd)) {
    if (sourceRef->ownership == Ownership::OWN) {
      // We might be loading a member as an own if we're destructuring.
      // Don't adjust the RC, since we're only moving it.
    } else if (sourceRef->ownership == Ownership::BORROW) {
      adjustStrongRc(from, globalState, functionState, &referendStructs, builder, expr, sourceRef, 1);
    } else if (sourceRef->ownership == Ownership::WEAK) {
      aliasWeakRef(from, functionState, builder, sourceRef, expr);
    } else if (sourceRef->ownership == Ownership::SHARE) {
      if (sourceRef->location == Location::INLINE) {
        // Do nothing, we can just let inline structs disappear
      } else {
        adjustStrongRc(from, globalState, functionState, &referendStructs, builder, expr, sourceRef, 1);
      }
    } else
      assert(false);
  } else {
    std::cerr << "Unimplemented type in acquireReference: "
        << typeid(*sourceRef->referend).name() << std::endl;
    assert(false);
  }
}


void Assist::dealias(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    BlockState* blockState,
    LLVMBuilderRef builder,
    Reference* sourceMT,
    Ref sourceRef) {
  if (sourceMT->ownership == Ownership::SHARE) {
    defaultImmutables.discard(from, globalState, functionState, blockState, builder, sourceMT, sourceRef);
  } else if (sourceMT->ownership == Ownership::OWN) {
    // We can't discard owns, they must be destructured.
    assert(false);
  } else if (sourceMT->ownership == Ownership::BORROW) {
    adjustStrongRc(from, globalState, functionState, &referendStructs, builder, sourceRef, sourceMT, -1);
  } else if (sourceMT->ownership == Ownership::WEAK) {
    discardWeakRef(from, functionState, builder, sourceMT, sourceRef);
  } else assert(false);
}

Ref Assist::lockWeak(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    bool thenResultIsNever,
    bool elseResultIsNever,
    Reference* resultOptTypeM,
    Reference* constraintRefM,
    Reference* sourceWeakRefMT,
    Ref sourceWeakRef,
    bool sourceKnownLive,
    std::function<Ref(LLVMBuilderRef, Ref)> buildThen,
    std::function<Ref(LLVMBuilderRef)> buildElse) {
  assert(sourceWeakRefMT->ownership == Ownership::WEAK);
  auto isAliveLE =
      getIsAliveFromWeakRef(
          functionState, builder, sourceWeakRefMT, sourceWeakRef, sourceKnownLive);
  auto resultOptTypeLE = translateType(resultOptTypeM);
  return regularInnerLockWeak(
      globalState, functionState, builder, thenResultIsNever, elseResultIsNever, resultOptTypeM,
      constraintRefM, sourceWeakRefMT, sourceWeakRef, buildThen, buildElse,
      isAliveLE, resultOptTypeLE, &weakRefStructs, &fatWeaks);
}

LLVMTypeRef Assist::translateType(Reference* referenceM) {
  switch (referenceM->ownership) {
    case Ownership::SHARE:
      return defaultImmutables.translateType(globalState, referenceM);
    case Ownership::OWN:
    case Ownership::BORROW:
      assert(referenceM->location != Location::INLINE);
      return translateReferenceSimple(globalState, referenceM->referend);
    case Ownership::WEAK:
      assert(referenceM->location != Location::INLINE);
      return translateWeakReference(globalState, referenceM->referend);
    default:
      assert(false);
  }
}

Ref Assist::upcastWeak(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    WeakFatPtrLE sourceRefLE,
    StructReferend* sourceStructReferendM,
    Reference* sourceStructTypeM,
    InterfaceReferend* targetInterfaceReferendM,
    Reference* targetInterfaceTypeM) {
  return wrap(
      this,
      targetInterfaceTypeM,
      wrcWeaks.weakStructPtrToWrciWeakInterfacePtr(
          globalState, functionState, builder, sourceRefLE, sourceStructReferendM,
          sourceStructTypeM, targetInterfaceReferendM, targetInterfaceTypeM));
}

void Assist::declareKnownSizeArray(
    KnownSizeArrayT* knownSizeArrayMT) {
  referendStructs.declareKnownSizeArray(knownSizeArrayMT);
}

void Assist::declareUnknownSizeArray(
    UnknownSizeArrayT* unknownSizeArrayMT) {
  referendStructs.declareUnknownSizeArray(unknownSizeArrayMT);
}

void Assist::translateUnknownSizeArray(
    UnknownSizeArrayT* unknownSizeArrayMT) {
  auto elementLT =
      translateType(
          unknownSizeArrayMT->rawArray->elementType);
  referendStructs.translateUnknownSizeArray(unknownSizeArrayMT, elementLT);
}

void Assist::translateKnownSizeArray(
    KnownSizeArrayT* knownSizeArrayMT) {
  auto elementLT =
      translateType(
          knownSizeArrayMT->rawArray->elementType);
  referendStructs.translateKnownSizeArray(knownSizeArrayMT, elementLT);
}

void Assist::declareStruct(
    StructDefinition* structM) {
  referendStructs.declareStruct(structM);
}

void Assist::translateStruct(
    StructDefinition* structM) {
  std::vector<LLVMTypeRef> innerStructMemberTypesL;
  for (int i = 0; i < structM->members.size(); i++) {
    innerStructMemberTypesL.push_back(
        translateType(
            structM->members[i]->type));
  }
  referendStructs.translateStruct(
      structM,
      innerStructMemberTypesL);
}

void Assist::declareEdge(
    Edge* edge) {
  referendStructs.declareEdge(edge);
}

void Assist::translateEdge(
    Edge* edge) {
  auto interfaceM = globalState->program->getInterface(edge->interfaceName->fullName);

  std::vector<LLVMTypeRef> interfaceFunctionsLT;
  std::vector<LLVMValueRef> edgeFunctionsL;
  for (int i = 0; i < edge->structPrototypesByInterfaceMethod.size(); i++) {
    auto interfaceFunctionLT =
        translateInterfaceMethodToFunctionType(interfaceM->methods[i]);
    interfaceFunctionsLT.push_back(interfaceFunctionLT);

    auto funcName = edge->structPrototypesByInterfaceMethod[i].second->name;
    auto edgeFunctionL = globalState->getFunction(funcName);
    edgeFunctionsL.push_back(edgeFunctionL);
  }
  referendStructs.translateEdge(edge, interfaceFunctionsLT, edgeFunctionsL);
}

void Assist::declareInterface(
    InterfaceDefinition* interfaceM) {
  referendStructs.declareInterface(interfaceM);
}

void Assist::translateInterface(
    InterfaceDefinition* interfaceM) {
  std::vector<LLVMTypeRef> interfaceMethodTypesL;
  for (int i = 0; i < interfaceM->methods.size(); i++) {
    interfaceMethodTypesL.push_back(
        LLVMPointerType(
            translateInterfaceMethodToFunctionType(interfaceM->methods[i]),
            0));
  }
  referendStructs.translateInterface(
      interfaceM,
      interfaceMethodTypesL);
}

LLVMTypeRef Assist::translateInterfaceMethodToFunctionType(
    InterfaceMethod* method) {
  auto returnMT = method->prototype->returnType;
  auto paramsMT = method->prototype->params;
  auto returnLT = translateType(returnMT);
  auto paramsLT = translateTypes(globalState, this, paramsMT);

  switch (paramsMT[method->virtualParamIndex]->ownership) {
    case Ownership::BORROW:
    case Ownership::OWN:
    case Ownership::SHARE:
      paramsLT[method->virtualParamIndex] = LLVMPointerType(LLVMInt8TypeInContext(globalState->context), 0);
      break;
    case Ownership::WEAK:
      paramsLT[method->virtualParamIndex] = mutWeakableStructs.weakVoidRefStructL;
      break;
  }

  return LLVMFunctionType(returnLT, paramsLT.data(), paramsLT.size(), false);
}

Ref Assist::weakAlias(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* sourceRefMT,
    Reference* targetRefMT,
    Ref sourceRef) {
  return regularWeakAlias(globalState, functionState, &referendStructs, &wrcWeaks, builder, sourceRefMT, targetRefMT, sourceRef);
}

void Assist::discardOwningRef(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    BlockState* blockState,
    LLVMBuilderRef builder,
    Reference* sourceMT,
    Ref sourceRef) {
  auto exprWrapperPtrLE =
      referendStructs.makeWrapperPtr(
          FL(), functionState, builder, sourceMT,
          checkValidReference(FL(), functionState, builder, sourceMT, sourceRef));

  adjustStrongRc(
      AFL("Destroy decrementing the owning ref"),
      globalState, functionState, &referendStructs, builder, sourceRef, sourceMT, -1);
  // No need to check the RC, we know we're freeing right now.

  // Free it!
  deallocate(AFL("discardOwningRef"), functionState, builder, sourceMT, sourceRef);
}

void Assist::noteWeakableDestroyed(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* refM,
    ControlBlockPtrLE controlBlockPtrLE) {
  auto rcIsZeroLE = strongRcIsZero(globalState, builder, refM, controlBlockPtrLE);
  buildAssert(globalState, functionState, builder, rcIsZeroLE,
      "Tried to free concrete that had nonzero RC!");

  if (auto structReferendM = dynamic_cast<StructReferend*>(refM->referend)) {
    auto structM = globalState->program->getStruct(structReferendM->fullName);
    if (structM->weakability == Weakability::WEAKABLE) {
      wrcWeaks.innerNoteWeakableDestroyed(functionState, builder, refM, controlBlockPtrLE);
    }
  } else if (auto interfaceReferendM = dynamic_cast<InterfaceReferend*>(refM->referend)) {
    assert(false); // Do we ever deallocate an interface?
  } else {
    // Do nothing, only structs and interfaces are weakable in assist mode.
  }
}

Ref Assist::loadMember(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* structRefMT,
    Ref structRef,
    bool structKnownLive,
    int memberIndex,
    Reference* expectedMemberType,
    Reference* targetType,
    const std::string& memberName) {
  if (structRefMT->ownership == Ownership::SHARE) {
    auto memberLE =
        defaultImmutables.loadMember(
            functionState, builder, structRefMT, structRef, memberIndex, expectedMemberType,
            targetType, memberName);
    auto resultRef =
        upgradeLoadResultToRefWithTargetOwnership(
            functionState, builder, expectedMemberType, targetType, memberLE);
    return resultRef;
  } else {
    auto unupgradedMemberLE =
        regularLoadMember(
            globalState, functionState, builder, &referendStructs, structRefMT, structRef,
            memberIndex, expectedMemberType, targetType, memberName);
    return upgradeLoadResultToRefWithTargetOwnership(
        functionState, builder, expectedMemberType, targetType, unupgradedMemberLE);
  }
}

void Assist::storeMember(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* structRefMT,
    Ref structRef,
    bool structKnownLive,
    int memberIndex,
    const std::string& memberName,
    LLVMValueRef newValueLE) {
  switch (structRefMT->ownership) {
    case Ownership::OWN:
    case Ownership::SHARE:
    case Ownership::BORROW: {
      storeMemberStrong(
          globalState, functionState, builder, &referendStructs, structRefMT, structRef,
          structKnownLive, memberIndex, memberName, newValueLE);
      break;
    }
    case Ownership::WEAK: {
      storeMemberWeak(
          globalState, functionState, builder, &referendStructs, structRefMT, structRef,
          structKnownLive, memberIndex, memberName, newValueLE);
      break;
    }
    default:
      assert(false);
  }
}


// Gets the itable PTR and the new value that we should put into the virtual param's slot
// (such as a void* or a weak void ref)
std::tuple<LLVMValueRef, LLVMValueRef> Assist::explodeInterfaceRef(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* virtualParamMT,
    Ref virtualArgRef) {
  switch (virtualParamMT->ownership) {
    case Ownership::OWN:
    case Ownership::BORROW:
    case Ownership::SHARE: {
      return explodeStrongInterfaceRef(
          globalState, functionState, builder, &referendStructs, virtualParamMT, virtualArgRef);
    }
    case Ownership::WEAK: {
      return explodeWeakInterfaceRef(
          globalState, functionState, builder, &referendStructs, &fatWeaks, &weakRefStructs,
          virtualParamMT, virtualArgRef,
          [this, functionState, builder, virtualParamMT](WeakFatPtrLE weakFatPtrLE) {
            return wrcWeaks.weakInterfaceRefToWeakStructRef(
                functionState, builder, virtualParamMT, weakFatPtrLE);
          });
    }
    default:
      assert(false);
  }
}

void Assist::aliasWeakRef(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* weakRefMT,
    Ref weakRef) {
  return wrcWeaks.aliasWeakRef(from, functionState, builder, weakRefMT, weakRef);
}

void Assist::discardWeakRef(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* weakRefMT,
    Ref weakRef) {
  return wrcWeaks.discardWeakRef(from, functionState, builder, weakRefMT, weakRef);
}

Ref Assist::getIsAliveFromWeakRef(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* weakRefM,
    Ref weakRef,
    bool knownLive) {
  return wrcWeaks.getIsAliveFromWeakRef(functionState, builder, weakRefM, weakRef);
}

LLVMValueRef Assist::getStringBytesPtr(FunctionState* functionState, LLVMBuilderRef builder, Ref ref) {
  return referendStructs.getStringBytesPtr(functionState, builder, ref);
}

Ref Assist::constructKnownSizeArray(FunctionState *functionState, LLVMBuilderRef builder, Reference *referenceM, KnownSizeArrayT *referendM, const std::vector<Ref> &membersLE) {
  return ::constructKnownSizeArray(
      globalState, functionState, builder, referenceM, referendM, membersLE, &referendStructs,
      [this, functionState, referenceM, referendM](LLVMBuilderRef innerBuilder, ControlBlockPtrLE controlBlockPtrLE) {
        fillControlBlock(
            FL(),
            functionState,
            innerBuilder,
            referenceM->referend,
            referendM->rawArray->mutability,
            controlBlockPtrLE,
            referendM->name->name);
      });
}

WrapperPtrLE Assist::mallocStr(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    LLVMValueRef lengthLE) {
  return ::mallocStr(
      globalState, functionState, builder, lengthLE, &referendStructs,
      [this, functionState](LLVMBuilderRef innerBuilder, ControlBlockPtrLE controlBlockPtrLE) {
        fillControlBlock(
            FL(), functionState, innerBuilder, globalState->metalCache.str,
            Mutability::IMMUTABLE, controlBlockPtrLE, "Str");
      });
}

Ref Assist::allocate(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* desiredReference,
    const std::vector<Ref>& membersLE) {
  auto structReferend = dynamic_cast<StructReferend*>(desiredReference->referend);
  auto structM = globalState->program->getStruct(structReferend->fullName);
  return innerAllocate(
      FL(), globalState, functionState, builder, desiredReference, &referendStructs, membersLE, Weakability::WEAKABLE,
      [this, functionState, desiredReference, structM](LLVMBuilderRef innerBuilder, ControlBlockPtrLE controlBlockPtrLE) {
        fillControlBlock(
            FL(), functionState, innerBuilder, desiredReference->referend, structM->mutability,
            controlBlockPtrLE, structM->name->name);
      });
}

// Doesn't return a constraint ref, returns a raw ref to the wrapper struct.
WrapperPtrLE Assist::lockWeakRef(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* refM,
    Ref weakRefLE,
    bool weakRefKnownLive) {
  switch (refM->ownership) {
    case Ownership::OWN:
    case Ownership::SHARE:
    case Ownership::BORROW:
      assert(false);
      break;
    case Ownership::WEAK: {
      auto weakFatPtrLE =
          weakRefStructs.makeWeakFatPtr(
              refM,
              checkValidReference(FL(), functionState, builder, refM, weakRefLE));
      return referendStructs.makeWrapperPtr(
          FL(), functionState, builder, refM,
          wrcWeaks.lockWrciFatPtr(from, functionState, builder, refM, weakFatPtrLE));
    }
    default:
      assert(false);
      break;
  }
}

Ref Assist::getUnknownSizeArrayLength(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* usaRefMT,
    Ref arrayRef,
    bool arrayKnownLive) {
  return getUnknownSizeArrayLengthStrong(globalState, functionState, builder, &referendStructs, usaRefMT, arrayRef);
}

LLVMValueRef Assist::checkValidReference(
    AreaAndFileAndLine checkerAFL,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* refM,
    Ref ref) {
  Reference *actualRefM = nullptr;
  LLVMValueRef refLE = nullptr;
  std::tie(actualRefM, refLE) = megaGetRefInnardsForChecking(ref);
  assert(actualRefM == refM);
  assert(refLE != nullptr);
  assert(LLVMTypeOf(refLE) == functionState->defaultRegion->translateType(refM));

  if (globalState->opt->census) {
    if (refM->ownership == Ownership::OWN) {
      regularCheckValidReference(checkerAFL, globalState, functionState, builder, &referendStructs, refM, refLE);
    } else if (refM->ownership == Ownership::SHARE) {
      defaultImmutables.checkValidReference(checkerAFL, functionState, builder, &referendStructs, refM, refLE);
    } else {
      if (refM->ownership == Ownership::BORROW) {
        regularCheckValidReference(checkerAFL, globalState, functionState, builder,
            &referendStructs, refM, refLE);
      } else if (refM->ownership == Ownership::WEAK) {
        wrcWeaks.buildCheckWeakRef(checkerAFL, functionState, builder, refM, ref);
      } else
        assert(false);
    }
    return refLE;
  } else {
    return refLE;
  }
}

// TODO maybe combine with alias/acquireReference?
// After we load from a local, member, or element, we can feed the result through this
// function to turn it into a desired ownership.
// Example:
// - Can load from an owning ref member to get a constraint ref.
// - Can load from a constraint ref member to get a weak ref.
Ref Assist::upgradeLoadResultToRefWithTargetOwnership(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* sourceType,
    Reference* targetType,
    Ref sourceRef) {
  auto sourceOwnership = sourceType->ownership;
  auto sourceLocation = sourceType->location;
  auto targetOwnership = targetType->ownership;
  auto targetLocation = targetType->location;
//  assert(sourceLocation == targetLocation); // unimplemented

  if (sourceOwnership == Ownership::SHARE) {
    if (sourceLocation == Location::INLINE) {
      return sourceRef;
    } else {
      return sourceRef;
    }
  } else if (sourceOwnership == Ownership::OWN) {
    if (targetOwnership == Ownership::OWN) {
      // We can never "load" an owning ref from any of these:
      // - We can only get owning refs from locals by unstackifying
      // - We can only get owning refs from structs by destroying
      // - We can only get owning refs from elements by destroying
      // However, we CAN load owning refs by:
      // - Swapping from a local
      // - Swapping from an element
      // - Swapping from a member
      return sourceRef;
    } else if (targetOwnership == Ownership::BORROW) {
      auto resultRef = transmutePtr(functionState, builder, sourceType, targetType, sourceRef);
      checkValidReference(FL(),
          functionState, builder, targetType, resultRef);
      return resultRef;
    } else if (targetOwnership == Ownership::WEAK) {
      return wrcWeaks.assembleWeakRef(functionState, builder, sourceType, targetType, sourceRef);
    } else {
      assert(false);
    }
  } else if (sourceOwnership == Ownership::BORROW) {
    buildFlare(FL(), globalState, functionState, builder);

    if (targetOwnership == Ownership::OWN) {
      assert(false); // Cant load an owning reference from a constraint ref local.
    } else if (targetOwnership == Ownership::BORROW) {
      return sourceRef;
    } else if (targetOwnership == Ownership::WEAK) {
      // Making a weak ref from a constraint ref local.
      assert(dynamic_cast<StructReferend *>(sourceType->referend) ||
          dynamic_cast<InterfaceReferend *>(sourceType->referend));
      return wrcWeaks.assembleWeakRef(functionState, builder, sourceType, targetType, sourceRef);
    } else {
      assert(false);
    }
  } else if (sourceOwnership == Ownership::WEAK) {
    assert(targetOwnership == Ownership::WEAK);
    return sourceRef;
  } else {
    assert(false);
  }
}

// Returns object ID
void Assist::fillControlBlock(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Referend* referendM,
    Mutability mutability,
    ControlBlockPtrLE controlBlockPtrLE,
    const std::string& typeName) {
  regularFillControlBlock(
      from, globalState, functionState, builder, referendM, mutability, controlBlockPtrLE,
      typeName, &wrcWeaks);
}

Ref Assist::loadElementFromKSAWithUpgrade(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* ksaRefMT,
    KnownSizeArrayT* ksaMT,
    Ref arrayRef,
    bool arrayKnownLive,
    Ref indexRef,
    Reference* targetType) {
  Ref memberRef =
      loadElementFromKSAWithoutUpgrade(
          functionState, builder, ksaRefMT, ksaMT, arrayRef, arrayKnownLive, indexRef);
  return upgradeLoadResultToRefWithTargetOwnership(
      functionState, builder, ksaMT->rawArray->elementType, targetType, memberRef);
}

Ref Assist::loadElementFromKSAWithoutUpgrade(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* ksaRefMT,
    KnownSizeArrayT* ksaMT,
    Ref arrayRef,
    bool arrayKnownLive,
    Ref indexRef) {
  return regularLoadElementFromKSAWithoutUpgrade(
      globalState, functionState, builder, ksaRefMT, ksaMT, arrayRef, arrayKnownLive, indexRef, &referendStructs);
}

Ref Assist::loadElementFromUSAWithUpgrade(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* usaRefMT,
    UnknownSizeArrayT* usaMT,
    Ref arrayRef,
    bool arrayKnownLive,
    Ref indexRef,
    Reference* targetType) {
  Ref memberRef = loadElementFromUSAWithoutUpgrade(functionState, builder, usaRefMT,
      usaMT, arrayRef, arrayKnownLive, indexRef);
  return upgradeLoadResultToRefWithTargetOwnership(
      functionState, builder, usaMT->rawArray->elementType, targetType, memberRef);
}

Ref Assist::loadElementFromUSAWithoutUpgrade(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* usaRefMT,
    UnknownSizeArrayT* usaMT,
    Ref arrayRef,
    bool arrayKnownLive,
    Ref indexRef) {
  return regularLoadElementFromUSAWithoutUpgrade(
      globalState, functionState, builder, &referendStructs, usaRefMT, usaMT, arrayRef,
      arrayKnownLive, indexRef);
}

Ref Assist::storeElementInUSA(
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* usaRefMT,
    UnknownSizeArrayT* usaMT,
    Ref arrayRef,
    bool arrayKnownLive,
    Ref indexRef,
    Ref elementRef) {
  return regularStoreElementInUSA(
      globalState, functionState, builder, &referendStructs, usaRefMT, usaMT, arrayRef,
      indexRef, elementRef);
}

Ref Assist::upcast(
    FunctionState* functionState,
    LLVMBuilderRef builder,

    Reference* sourceStructMT,
    StructReferend* sourceStructReferendM,
    Ref sourceRefLE,

    Reference* targetInterfaceTypeM,
    InterfaceReferend* targetInterfaceReferendM) {
  switch (sourceStructMT->ownership) {
    case Ownership::SHARE:
    case Ownership::OWN:
    case Ownership::BORROW: {
      return upcastStrong(globalState, functionState, builder, &referendStructs, sourceStructMT, sourceStructReferendM, sourceRefLE, targetInterfaceTypeM, targetInterfaceReferendM);
    }
    case Ownership::WEAK: {
      return ::upcastWeak(globalState, functionState, builder, &weakRefStructs, sourceStructMT, sourceStructReferendM, sourceRefLE, targetInterfaceTypeM, targetInterfaceReferendM);
    }
    default:
      assert(false);
  }
}


void Assist::deallocate(
    AreaAndFileAndLine from,
    FunctionState* functionState,
    LLVMBuilderRef builder,
    Reference* refMT,
    Ref refLE) {
  innerDeallocate(from, globalState, functionState, &referendStructs, builder, refMT, refLE);
}

Ref Assist::constructUnknownSizeArrayCountedStruct(
    FunctionState* functionState,
    BlockState* blockState,
    LLVMBuilderRef builder,
    Reference* usaMT,
    UnknownSizeArrayT* unknownSizeArrayT,
    Reference* generatorType,
    Prototype* generatorMethod,
    Ref generatorRef,
    LLVMTypeRef usaWrapperPtrLT,
    LLVMTypeRef usaElementLT,
    Ref sizeRef,
    const std::string& typeName) {
  return ::constructUnknownSizeArrayCountedStruct(
      globalState, functionState, blockState, builder, &referendStructs, usaMT, unknownSizeArrayT, generatorType, generatorMethod,
      generatorRef, usaWrapperPtrLT, usaElementLT, sizeRef, typeName,
      [this, functionState, unknownSizeArrayT, usaMT, typeName](
          LLVMBuilderRef innerBuilder, ControlBlockPtrLE controlBlockPtrLE) {
        fillControlBlock(
            FL(),
            functionState,
            innerBuilder,
            unknownSizeArrayT,
            unknownSizeArrayT->rawArray->mutability,
            controlBlockPtrLE,
            typeName);
      });
}
