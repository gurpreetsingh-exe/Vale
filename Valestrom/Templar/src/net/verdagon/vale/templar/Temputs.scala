package net.verdagon.vale.templar

import net.verdagon.vale.scout.{CodeLocationS, RangeS}
import net.verdagon.vale.templar.env.{FunctionEnvironment, PackageEnvironment}
import net.verdagon.vale.templar.templata.{Prototype2, Signature2}
import net.verdagon.vale.templar.types.{CitizenDefinition2, CitizenRef2, Coord, Immutable, InterfaceDefinition2, InterfaceRef2, Kind, StaticSizedArrayT2, Mutability, Never2, RawArrayT2, Share, StructDefinition2, StructRef2, RuntimeSizedArrayT2}
import net.verdagon.vale.{PackageCoordinate, vassert, vassertSome, vfail}

import scala.collection.immutable.{List, Map}
import scala.collection.mutable

case class Temputs() {
  // Signatures that have already started to be compiled.
  // The value is a location for checking where a given function came from, which is useful
  // for detecting when the user makes two functions with identical signatures.
  private val declaredSignatures: mutable.HashMap[Signature2, RangeS] = mutable.HashMap()

  // Not all signatures/banners will have a return type here, it might not have been processed yet.
  private val returnTypesBySignature: mutable.HashMap[Signature2, Coord] = mutable.HashMap()

  // Not all signatures/banners or even return types will have a function here, it might not have
  // been processed yet.
  private val functions: mutable.ArrayBuffer[Function2] = mutable.ArrayBuffer()
  private val envByFunctionSignature: mutable.HashMap[Signature2, FunctionEnvironment] = mutable.HashMap()

  // Prototypes for extern functions
  private val moduleNameToExternNameToExtern: mutable.HashMap[String, mutable.HashMap[String, (PackageCoordinate, Prototype2)]] = mutable.HashMap()

  // One must fill this in when putting things into declaredStructs/Interfaces.
  private val mutabilitiesByCitizenRef: mutable.HashMap[CitizenRef2, Mutability] = mutable.HashMap()

  // declaredStructs is the structs that we're currently in the process of defining
  // Things will appear here before they appear in structDefsByRef
  // This is to prevent infinite recursion / stack overflow when templaring recursive types
  // Not too sure about the type of declaredStructs, we might need something else
  private val declaredStructs: mutable.HashSet[StructRef2] = mutable.HashSet()
  private val structDefsByRef: mutable.HashMap[StructRef2, StructDefinition2] = mutable.HashMap()
  private val envByStructRef: mutable.HashMap[StructRef2, PackageEnvironment[IName2]] = mutable.HashMap()
  // declaredInterfaces is the interfaces that we're currently in the process of defining
  // Things will appear here before they appear in interfaceDefsByRef
  // This is to prevent infinite recursion / stack overflow when templaring recursive types
  // Not too sure about the type of declaredInterfaces, we might need something else
  private val declaredInterfaces: mutable.HashSet[InterfaceRef2] = mutable.HashSet()
  private val interfaceDefsByRef: mutable.HashMap[InterfaceRef2, InterfaceDefinition2] = mutable.HashMap()
  private val envByInterfaceRef: mutable.HashMap[InterfaceRef2, PackageEnvironment[IName2]] = mutable.HashMap()

  private val impls: mutable.ArrayBuffer[Impl2] = mutable.ArrayBuffer()

  private val exports: mutable.ArrayBuffer[ExportAs2] = mutable.ArrayBuffer()

  // Only PackTemplar can make a PackT2.
  private val packTypes: mutable.HashMap[List[Coord], StructRef2] = mutable.HashMap()
  // Only ArrayTemplar can make an RawArrayT2.
  private val staticSizedArrayTypes: mutable.HashMap[(Int, RawArrayT2), StaticSizedArrayT2] = mutable.HashMap()
  // Only ArrayTemplar can make an RawArrayT2.
  private val runtimeSizedArrayTypes: mutable.HashMap[RawArrayT2, RuntimeSizedArrayT2] = mutable.HashMap()

  private val kindToDestructor: mutable.HashMap[Kind, Prototype2] = mutable.HashMap()

  def lookupFunction(signature2: Signature2): Option[Function2] = {
    functions.find(_.header.toSignature == signature2)
  }

  // This means we've at least started to evaluate this function's body.
  // We use this to cut short any infinite looping that might happen when,
  // for example, there's a recursive function call.
  def declareFunctionSignature(range: RangeS, signature: Signature2, maybeEnv: Option[FunctionEnvironment]): Unit = {
    // The only difference between this and declareNonGlobalFunctionSignature is
    // that we put an environment in here.

    // This should have been checked outside
    vassert(!declaredSignatures.contains(signature))

    declaredSignatures += signature -> range
    envByFunctionSignature ++= maybeEnv.map(env => Map(signature -> env)).getOrElse(Map())
    this
  }

  def declareFunctionReturnType(signature: Signature2, returnType2: Coord): Unit = {
    returnTypesBySignature.get(signature) match {
      case None =>
      case Some(existingReturnType2) => vassert(existingReturnType2 == returnType2)
    }
    if (!declaredSignatures.contains(signature)) {
      vfail("wot")
    }
    returnTypesBySignature += (signature -> returnType2)
  }

  def addFunction(function: Function2): Unit = {
    vassert(declaredSignatures.contains(function.header.toSignature))
    vassert(
      function.body.resultRegister.reference.referend == Never2() ||
      function.body.resultRegister.reference == function.header.returnType)
    function.all({
      case Return2(innerExpr) => {
        vassert(
          innerExpr.resultRegister.reference.referend == Never2() ||
          innerExpr.resultRegister.reference == function.header.returnType)
      }
    })

    if (functions.exists(_.header == function.header)) {
      vfail("wot")
    }

    functions += function
  }

  // We can't declare the struct at the same time as we declare its mutability or environment,
  // see MFDBRE.
  def declareStruct(
    structRef: StructRef2
  ): Unit = {
    vassert(!declaredStructs.contains(structRef))
    declaredStructs += structRef
  }

  def declareStructMutability(
    structRef: StructRef2,
    mutability: Mutability
  ): Unit = {
    vassert(declaredStructs.contains(structRef))
    vassert(!mutabilitiesByCitizenRef.contains(structRef))
    mutabilitiesByCitizenRef += (structRef -> mutability)
  }

  def declareStructEnv(
    structRef: StructRef2,
    env: PackageEnvironment[ICitizenName2],
  ): Unit = {
    vassert(declaredStructs.contains(structRef))
    vassert(!envByStructRef.contains(structRef))
    envByStructRef += (structRef -> env)
  }

  def declareInterface(interfaceRef: InterfaceRef2): Unit = {
    vassert(!declaredInterfaces.contains(interfaceRef))
    declaredInterfaces += interfaceRef
  }

  def declareInterfaceMutability(
    interfaceRef: InterfaceRef2,
    mutability: Mutability
  ): Unit = {
    vassert(declaredInterfaces.contains(interfaceRef))
    vassert(!mutabilitiesByCitizenRef.contains(interfaceRef))
    mutabilitiesByCitizenRef += (interfaceRef -> mutability)
  }

  def declareInterfaceEnv(
    interfaceRef: InterfaceRef2,
    env: PackageEnvironment[CitizenName2]
  ): Unit = {
    vassert(declaredInterfaces.contains(interfaceRef))
    vassert(!envByInterfaceRef.contains(interfaceRef))
    envByInterfaceRef += (interfaceRef -> env)
  }

  def declarePack(members: List[Coord], understructRef2: StructRef2): Unit = {
    packTypes += (members -> understructRef2)
  }

  def add(structDef: StructDefinition2): Unit = {
    if (structDef.mutability == Immutable) {
      if (structDef.members.exists(_.tyype.reference.ownership != Share)) {
        vfail("ImmutableP contains a non-immutable!")
      }
    }
    vassert(!structDefsByRef.contains(structDef.getRef))
    structDefsByRef += (structDef.getRef -> structDef)
  }

  def add(interfaceDef: InterfaceDefinition2): Unit = {
    vassert(!interfaceDefsByRef.contains(interfaceDef.getRef))
    interfaceDefsByRef += (interfaceDef.getRef -> interfaceDef)
  }

  def addStaticSizedArray(array2: StaticSizedArrayT2): Unit = {
    staticSizedArrayTypes += ((array2.size, array2.array) -> array2)
  }

  def addRuntimeSizedArray(array2: RuntimeSizedArrayT2): Unit = {
    runtimeSizedArrayTypes += (array2.array -> array2)
  }

  def addImpl(structRef2: StructRef2, interfaceRef2: InterfaceRef2): Unit = {
    impls += Impl2(structRef2, interfaceRef2)
  }

  def addExport(kind: Kind, packageCoord: PackageCoordinate, exportedName: String): Unit = {
    exports += ExportAs2(kind, packageCoord, exportedName)
  }

  def addExternPrototype(prototype2: Prototype2): Unit = {
    val externName =
      prototype2.fullName.last match {
        case ExternFunctionName2(externName, _) => externName
      }
    moduleNameToExternNameToExtern.get(prototype2.fullName.packageCoord.module) match {
      case None => {
        moduleNameToExternNameToExtern.put(
          prototype2.fullName.packageCoord.module,
          mutable.HashMap(externName -> (prototype2.fullName.packageCoord, prototype2)))
      }
      case Some(externNameToExtern) => {
        externNameToExtern.get(externName) match {
          case None => externNameToExtern.put(externName, (prototype2.fullName.packageCoord, prototype2))
          case Some(_) => vfail("Extern already exists: " + externName)
        }
      }
    }
  }

  def addDestructor(kind: Kind, destructor: Prototype2): Unit = {
    vassert(!kindToDestructor.contains(kind))
    vassert(prototypeDeclared(destructor.fullName).nonEmpty)
    kindToDestructor.put(kind, destructor)
  }

  def getDestructor(kind: Kind): Prototype2 = {
    vassertSome(kindToDestructor.get(kind))
  }

  def structDeclared(fullName: FullName2[ICitizenName2]): Option[StructRef2] = {
    // This is the only place besides StructDefinition2 and declareStruct thats allowed to make one of these
    val structRef = StructRef2(fullName)
    if (declaredStructs.contains(structRef)) {
      Some(structRef)
    } else {
      None
    }
  }

  def prototypeDeclared(fullName: FullName2[IFunctionName2]): Option[Prototype2] = {
    declaredSignatures.find(_._1.fullName == fullName) match {
      case None => None
      case Some((sig, _)) => {
        returnTypesBySignature.get(sig) match {
          case None => None
          case Some(ret) => Some(Prototype2(sig.fullName, ret))
        }
      }
    }
  }

  def lookupMutability(citizenRef2: CitizenRef2): Mutability = {
    // If it has a structRef, then we've at least started to evaluate this citizen
    mutabilitiesByCitizenRef.get(citizenRef2) match {
      case None => vfail("Still figuring out mutability for struct: " + citizenRef2) // See MFDBRE
      case Some(m) => m
    }
  }

  def lookupStruct(structRef: StructRef2): StructDefinition2 = {
    // If it has a structRef, then we're done (or at least have started) stamping it
    // If this throws an error, then you should not use this function, you should
    // do structDefsByRef.get(structRef) yourself and handle the None case
    vassertSome(structDefsByRef.get(structRef))
  }

  def lookupCitizen(citizenRef: CitizenRef2): CitizenDefinition2 = {
    citizenRef match {
      case s @ StructRef2(_) => lookupStruct(s)
      case i @ InterfaceRef2(_) => lookupInterface(i)
    }
  }

  def interfaceDeclared(fullName: FullName2[CitizenName2]): Option[InterfaceRef2] = {
    // This is the only place besides InterfaceDefinition2 and declareInterface thats allowed to make one of these
    val interfaceRef = InterfaceRef2(fullName)
    if (declaredInterfaces.contains(interfaceRef)) {
      Some(interfaceRef)
    } else {
      None
    }
  }

  def lookupInterface(interfaceRef: InterfaceRef2): InterfaceDefinition2 = {
    // If it has a interfaceRef, then we're done (or at least have started) stamping it.
    // If this throws an error, then you should not use this function, you should
    // do interfaceDefsByRef.get(interfaceRef) yourself and handle the None case
    interfaceDefsByRef(interfaceRef)
  }
//
//  def functionAlreadyDeclared(rangeS: RangeS, fullName: FullName2[IFunctionName2]): Boolean = {
//    declaredSignatures.get(Signature2(fullName)) == Some(rangeS)
//  }
//
//  def functionAlreadyDeclared(rangeS: RangeS, signature: Signature2): Boolean = {
//    declaredSignatures.contains(signature) == Some(rangeS)
//  }

  //  def findFunction(name: String, paramTypes: List[Coord]): Option[FunctionHeader2] = {
  //    val matchingFunctions = functions.find(this, name, paramTypes)
  //    vassert(matchingFunctions.size < 2)
  //    matchingFunctions.headOption
  //  }

  def getAllStructs(): Iterable[StructDefinition2] = structDefsByRef.values
  def getAllInterfaces(): Iterable[InterfaceDefinition2] = interfaceDefsByRef.values
  def getAllFunctions(): Iterable[Function2] = functions
  def getAllImpls(): Iterable[Impl2] = impls
  def getAllStaticSizedArrays(): Iterable[StaticSizedArrayT2] = staticSizedArrayTypes.values
  def getAllRuntimeSizedArrays(): Iterable[RuntimeSizedArrayT2] = runtimeSizedArrayTypes.values
  def getKindToDestructorMap(): Map[Kind, Prototype2] = kindToDestructor.toMap

  def getStaticSizedArrayType(size: Int, array: RawArrayT2): Option[StaticSizedArrayT2] = {
    staticSizedArrayTypes.get((size, array))
  }
  def getEnvForFunctionSignature(sig: Signature2): FunctionEnvironment = {
    envByFunctionSignature(sig)
  }
  def getEnvForInterfaceRef(sr: InterfaceRef2): PackageEnvironment[IName2] = {
    envByInterfaceRef(sr)
  }
  def getEnvForStructRef(sr: StructRef2): PackageEnvironment[IName2] = {
    envByStructRef(sr)
  }
  def getInterfaceDefForRef(ir: InterfaceRef2): InterfaceDefinition2 = {
    interfaceDefsByRef(ir)
  }
  def getPackType(coords: List[Coord]): Option[StructRef2] = {
    packTypes.get(coords)
  }
  def getReturnTypeForSignature(sig: Signature2): Option[Coord] = {
    returnTypesBySignature.get(sig)
  }
  def getDeclaredSignatureOrigin(sig: Signature2): Option[RangeS] = {
    declaredSignatures.get(sig)
  }
  def getDeclaredSignatureOrigin(name: FullName2[IFunctionName2]): Option[RangeS] = {
    declaredSignatures.get(Signature2(name))
  }
  def getStructDefForRef(sr: StructRef2): StructDefinition2 = {
    structDefsByRef(sr)
  }
  def getRuntimeSizedArray(array: RawArrayT2): Option[RuntimeSizedArrayT2] = {
    runtimeSizedArrayTypes.get(array)
  }
  def getExternPrototypes: Map[String, Map[String, (PackageCoordinate, Prototype2)]] = {
    moduleNameToExternNameToExtern.map({ case (moduleName, externNameToExtern) =>
      (moduleName -> externNameToExtern.toMap)
    }).toMap
  }
  def getExports: List[ExportAs2] = {
    exports.toList
  }
}
