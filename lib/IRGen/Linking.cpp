//===--- Linking.cpp - Name mangling for IRGen entities -------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements name mangling for IRGen entities with linkage.
//
//===----------------------------------------------------------------------===//

#include "swift/IRGen/Linking.h"
#include "IRGenMangler.h"
#include "IRGenModule.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/raw_ostream.h"

using namespace swift;
using namespace irgen;

bool swift::irgen::useDllStorage(const llvm::Triple &triple) {
  return triple.isOSBinFormatCOFF() && !triple.isOSCygMing();
}

UniversalLinkageInfo::UniversalLinkageInfo(IRGenModule &IGM)
    : UniversalLinkageInfo(IGM.Triple, IGM.IRGen.hasMultipleIGMs(),
                           IGM.getSILModule().isWholeModule()) {}

UniversalLinkageInfo::UniversalLinkageInfo(const llvm::Triple &triple,
                                           bool hasMultipleIGMs,
                                           bool isWholeModule)
    : IsELFObject(triple.isOSBinFormatELF()),
      UseDLLStorage(useDllStorage(triple)), HasMultipleIGMs(hasMultipleIGMs),
      IsWholeModule(isWholeModule) {}

/// Mangle this entity into the given buffer.
void LinkEntity::mangle(SmallVectorImpl<char> &buffer) const {
  llvm::raw_svector_ostream stream(buffer);
  mangle(stream);
}

/// Use the Clang importer to mangle a Clang declaration.
static void mangleClangDecl(raw_ostream &buffer,
                            const clang::NamedDecl *clangDecl,
                            ASTContext &ctx) {
  auto *importer = static_cast<ClangImporter *>(ctx.getClangModuleLoader());
  importer->getMangledName(buffer, clangDecl);
}

/// Mangle this entity into the given stream.
void LinkEntity::mangle(raw_ostream &buffer) const {
  std::string Result = mangleAsString();
  buffer.write(Result.data(), Result.size());
}

/// Mangle this entity as a std::string.
std::string LinkEntity::mangleAsString() const {
  IRGenMangler mangler;
  switch (getKind()) {
    case Kind::ValueWitness:
      return mangler.mangleValueWitness(getType(), getValueWitness());

    case Kind::ValueWitnessTable:
      return mangler.mangleValueWitnessTable(getType());

    case Kind::TypeMetadataAccessFunction:
      return mangler.mangleTypeMetadataAccessFunction(getType());

    case Kind::TypeMetadataLazyCacheVariable:
      return mangler.mangleTypeMetadataLazyCacheVariable(getType());

    case Kind::TypeMetadata:
      switch (getMetadataAddress()) {
        case TypeMetadataAddress::FullMetadata:
          return mangler.mangleTypeFullMetadataFull(getType());
        case TypeMetadataAddress::AddressPoint:
          return mangler.mangleTypeMetadataFull(getType(), isMetadataPattern());
      }
      llvm_unreachable("invalid metadata address");

    case Kind::ForeignTypeMetadataCandidate:
      return mangler.mangleTypeMetadataFull(getType(), /*isPattern=*/false);

    case Kind::SwiftMetaclassStub:
      return mangler.mangleClassMetaClass(cast<ClassDecl>(getDecl()));

    case Kind::ClassMetadataBaseOffset:               // class metadata base offset
      return mangler.mangleClassMetadataBaseOffset(cast<ClassDecl>(getDecl()));

    case Kind::NominalTypeDescriptor:
      return mangler.mangleNominalTypeDescriptor(
                                          cast<NominalTypeDecl>(getDecl()));

    case Kind::ProtocolDescriptor:
      return mangler.mangleProtocolDescriptor(cast<ProtocolDecl>(getDecl()));

    case Kind::FieldOffset:
      return mangler.mangleFieldOffsetFull(getDecl(), isOffsetIndirect());

    case Kind::DirectProtocolWitnessTable:
      return mangler.mangleDirectProtocolWitnessTable(getProtocolConformance());

    case Kind::GenericProtocolWitnessTableCache:
      return mangler.mangleGenericProtocolWitnessTableCache(
                                                      getProtocolConformance());

    case Kind::GenericProtocolWitnessTableInstantiationFunction:
      return mangler.mangleGenericProtocolWitnessTableInstantiationFunction(
                                                      getProtocolConformance());

    case Kind::ProtocolWitnessTableAccessFunction:
      return mangler.mangleProtocolWitnessTableAccessFunction(
                                                      getProtocolConformance());

    case Kind::ProtocolWitnessTableLazyAccessFunction:
      return mangler.mangleProtocolWitnessTableLazyAccessFunction(getType(),
                                                      getProtocolConformance());

    case Kind::ProtocolWitnessTableLazyCacheVariable:
      return mangler.mangleProtocolWitnessTableLazyCacheVariable(getType(),
                                                      getProtocolConformance());

    case Kind::AssociatedTypeMetadataAccessFunction:
      return mangler.mangleAssociatedTypeMetadataAccessFunction(
                  getProtocolConformance(), getAssociatedType()->getNameStr());

    case Kind::AssociatedTypeWitnessTableAccessFunction: {
      auto assocConf = getAssociatedConformance();
      return mangler.mangleAssociatedTypeWitnessTableAccessFunction(
                  getProtocolConformance(), assocConf.first, assocConf.second);
    }

    case Kind::Function:
      // As a special case, functions can have manually mangled names.
      if (auto AsmA = getDecl()->getAttrs().getAttribute<SILGenNameAttr>())
        if (!AsmA->Name.empty())
          return AsmA->Name;

      // Otherwise, fall through into the 'other decl' case.
      LLVM_FALLTHROUGH;

    case Kind::Other:
      // As a special case, Clang functions and globals don't get mangled at all.
      if (auto clangDecl = getDecl()->getClangDecl()) {
        if (auto namedClangDecl = dyn_cast<clang::DeclaratorDecl>(clangDecl)) {
          if (auto asmLabel = namedClangDecl->getAttr<clang::AsmLabelAttr>()) {
            std::string Name(1, '\01');
            Name.append(asmLabel->getLabel());
            return Name;
          }
          if (namedClangDecl->hasAttr<clang::OverloadableAttr>()) {
            // FIXME: When we can import C++, use Clang's mangler all the time.
            std::string storage;
            llvm::raw_string_ostream SS(storage);
            mangleClangDecl(SS, namedClangDecl, getDecl()->getASTContext());
            return SS.str();
          }
          return namedClangDecl->getName();
        }
      }

      if (auto type = dyn_cast<NominalTypeDecl>(getDecl())) {
        return mangler.mangleNominalType(type);
      }
      if (auto ctor = dyn_cast<ConstructorDecl>(getDecl())) {
        // FIXME: Hack. LinkInfo should be able to refer to the allocating
        // constructor rather than inferring it here.
        return mangler.mangleConstructorEntity(ctor, /*isAllocating=*/true,
                                               /*isCurried=*/false);
      }
      return mangler.mangleEntity(getDecl(), /*isCurried=*/false);

      // An Objective-C class reference reference. The symbol is private, so
      // the mangling is unimportant; it should just be readable in LLVM IR.
    case Kind::ObjCClassRef: {
      llvm::SmallString<64> tempBuffer;
      StringRef name = cast<ClassDecl>(getDecl())->getObjCRuntimeName(tempBuffer);
      std::string Result("OBJC_CLASS_REF_$_");
      Result.append(name.data(), name.size());
      return Result;
    }

      // An Objective-C class reference;  not a swift mangling.
    case Kind::ObjCClass: {
      llvm::SmallString<64> TempBuffer;
      StringRef Name = cast<ClassDecl>(getDecl())->getObjCRuntimeName(TempBuffer);
      std::string Result("OBJC_CLASS_$_");
      Result.append(Name.data(), Name.size());
      return Result;
    }

      // An Objective-C metaclass reference;  not a swift mangling.
    case Kind::ObjCMetaclass: {
      llvm::SmallString<64> TempBuffer;
      StringRef Name = cast<ClassDecl>(getDecl())->getObjCRuntimeName(TempBuffer);
      std::string Result("OBJC_METACLASS_$_");
      Result.append(Name.data(), Name.size());
      return Result;
    }

    case Kind::SILFunction:
      return getSILFunction()->getName();
    case Kind::SILGlobalVariable:
      return getSILGlobalVariable()->getName();

    case Kind::ReflectionBuiltinDescriptor:
      return mangler.mangleReflectionBuiltinDescriptor(getType());
    case Kind::ReflectionFieldDescriptor:
      return mangler.mangleReflectionFieldDescriptor(getType());
    case Kind::ReflectionAssociatedTypeDescriptor:
      return mangler.mangleReflectionAssociatedTypeDescriptor(
                                                      getProtocolConformance());
  }
  llvm_unreachable("bad entity kind!");
}
