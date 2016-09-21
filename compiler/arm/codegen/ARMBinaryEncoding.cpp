/*******************************************************************************
 *
 * (c) Copyright IBM Corp. 2000, 2016
 *
 *  This program and the accompanying materials are made available
 *  under the terms of the Eclipse Public License v1.0 and
 *  Apache License v2.0 which accompanies this distribution.
 *
 *      The Eclipse Public License is available at
 *      http://www.eclipse.org/legal/epl-v10.html
 *
 *      The Apache License v2.0 is available at
 *      http://www.opensource.org/licenses/apache2.0.php
 *
 * Contributors:
 *    Multiple authors (IBM Corp.) - initial implementation and documentation
 *******************************************************************************/

#include <algorithm>                            // for std::find
#include "codegen/CodeGenerator.hpp"
#include "il/Node.hpp"
#include "il/Node_inlines.hpp"
#include "codegen/Relocation.hpp"
#include "codegen/Machine.hpp"
#include "arm/codegen/ARMInstruction.hpp"
#include "infra/Bit.hpp"
#ifdef J9_PROJECT_SPECIFIC
#include "env/CHTable.hpp"                     // for TR_VirtualGuardSite
#endif
#include "env/CompilerEnv.hpp"

void OMR::ARM::Instruction::generateConditionBinaryEncoding(uint8_t *instructionStart)
   {
   *((uint32_t *)instructionStart) |= ((uint32_t) self()->getConditionCode()) << 28;
   }

uint8_t *OMR::ARM::Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = self()->cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = self()->getOpCode().copyBinaryToBuffer(instructionStart);
   self()->generateConditionBinaryEncoding(instructionStart);
   cursor += 4;
   self()->setBinaryLength(cursor - instructionStart);
   self()->setBinaryEncoding(instructionStart);
   return cursor;
   }

int32_t OMR::ARM::Instruction::estimateBinaryLength(int32_t currentEstimate)
   {
   self()->setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH);
   return currentEstimate + self()->getEstimatedBinaryLength();
   }

uint32_t encodeBranchDistance(uint32_t from, uint32_t to)
   {
   // labels will always be 4 aligned, CPU shifts # left by 2
   // during execution (24 bits -> 26 bit branch reach)
   return ((to - (from + 8)) >> 2) & 0x00FFFFFF;
   }

uint32_t encodeHelperBranchAndLink(TR::SymbolReference *symRef, uint8_t *cursor, TR::Node *node, TR::CodeGenerator *cg)
   {
   return encodeHelperBranch(true, symRef, cursor, ARMConditionCodeAL, node, cg);
   }

uint32_t encodeHelperBranch(bool isBranchAndLink, TR::SymbolReference *symRef, uint8_t *cursor, TR_ARMConditionCode cc, TR::Node *node, TR::CodeGenerator *cg)
   {
   int32_t  distance;
   uint32_t target;

   target = (uintptr_t)symRef->getMethodAddress();
   distance = target - (uintptr_t) cursor;
   if (distance > BRANCH_FORWARD_LIMIT || distance < BRANCH_BACKWARD_LIMIT)
      {
      target = cg->fe()->indexedTrampolineLookup(symRef->getReferenceNumber(), (void *)cursor);
      TR_ASSERT(((int32_t)target - (int32_t)cursor) <= BRANCH_FORWARD_LIMIT &&
             ((int32_t)target - (int32_t)cursor) >=  BRANCH_BACKWARD_LIMIT,
             "CodeCache is more than 32MB.\n");
      }

   cg->addAOTRelocation(
      new (cg->trHeapMemory()) TR_ExternalRelocation(cursor,
                                     (uint8_t *)symRef,
                                     TR_HelperAddress, cg),
      __FILE__, __LINE__, node);

   return (isBranchAndLink ? 0x0B000000 : 0x0A000000) | encodeBranchDistance((uintptr_t) cursor, target) | (((uint32_t) cc) << 28);
   }

uint8_t *TR_ARMLabelInstruction::generateBinaryEncoding()
   {
   uint8_t        *instructionStart = cg()->getBinaryBufferCursor();
   TR::LabelSymbol *label            = getLabelSymbol();
   uint8_t        *cursor           = instructionStart;

   if (getOpCode().isLabel()) // LABEL
      {
      label->setCodeLocation(instructionStart);
      }
   else
      {
      cursor = getOpCode().copyBinaryToBuffer(instructionStart);
      generateConditionBinaryEncoding(instructionStart);
      uintptr_t destination = (uintptr_t)label->getCodeLocation();
      if (getOpCode().isBranchOp())
         {
         if (destination != 0)
            *(int32_t *)cursor |= encodeBranchDistance((uintptr_t)cursor, destination);
         else
            cg()->addRelocation(new (cg()->trHeapMemory()) TR_24BitLabelRelativeRelocation(cursor, label));
         }
      else
         {
         // TrgSRc1Imm instruction,
         // where Imm == label address - cursor - 8 and is a positive 8-bit value
         insertTargetRegister(toARMCursor(cursor));
         insertSource1Register(toARMCursor(cursor));
         *((uint32_t *)cursor) |= 1 << 25;    // set the I bit
         cg()->addRelocation(new (cg()->trHeapMemory()) TR_8BitLabelRelativeRelocation(cursor, label));
         }
      cursor += ARM_INSTRUCTION_LENGTH;
      }

   setBinaryLength(cursor - instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   setBinaryEncoding(instructionStart);
   return cursor;
   }

int32_t TR_ARMLabelInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   if (getOpCode().isLabel()) // LABEL
      {
      setEstimatedBinaryLength(0);
      getLabelSymbol()->setEstimatedCodeLocation(currentEstimate);
      }
   else
      {
      setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH);
      }
   return currentEstimate + getEstimatedBinaryLength();
   }

// Conditional branches are just like label instructions but have their
// CC set - for now, simply forward - TODO remove entirely.

uint8_t *TR_ARMConditionalBranchInstruction::generateBinaryEncoding()
   {
   return TR_ARMLabelInstruction::generateBinaryEncoding();
   }

int32_t TR_ARMConditionalBranchInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   return TR_ARMLabelInstruction::estimateBinaryLength(currentEstimate);
   }

uint8_t *TR_ARMAdminInstruction::generateBinaryEncoding()
   {
   uint8_t  *instructionStart = cg()->getBinaryBufferCursor();
   uint32_t i;

   if (getOpCodeValue() == ARMOp_fence)
      {
      TR::Node  *fenceNode = getFenceNode();
      uint32_t  rtype     = fenceNode->getRelocationType();
      if (rtype == TR_AbsoluteAddress)
         {
         for (i = 0; i < fenceNode->getNumRelocations(); ++i)
            {
            *(uint8_t **)(fenceNode->getRelocationDestination(i)) = instructionStart;
            }
         }
      else if (rtype == TR_EntryRelative16Bit)
         {
         for (i = 0; i < fenceNode->getNumRelocations(); ++i)
            {
            *(uint16_t *)(fenceNode->getRelocationDestination(i)) = (uint16_t)cg()->getCodeLength();
            }
         }
      else if (rtype == TR_EntryRelative32Bit)
         {
         for (i = 0; i < fenceNode->getNumRelocations(); ++i)
            {
            *(uint32_t *)(fenceNode->getRelocationDestination(i)) = cg()->getCodeLength();
            }
         }
      else // TR_ExternalAbsoluteAddress
         {
         TR_ASSERT(0, "external absolute relocations unimplemented");
         }
      }
   setBinaryLength(0);
   setBinaryEncoding(instructionStart);

   return instructionStart;
   }

int32_t TR_ARMAdminInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   setEstimatedBinaryLength(0);
   return currentEstimate;
   }

uint8_t *TR_ARMImmInstruction::generateBinaryEncoding()
   {
   TR::Compilation *comp = cg()->comp();
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   *(int32_t *)cursor = (int32_t)getSourceImmediate();

   if (needsAOTRelocation())
      {
      switch(getReloKind())
         {
         case TR_AbsoluteHelperAddress:
            cg()->addAOTRelocation(new (cg()->trHeapMemory()) TR_ExternalRelocation(cursor, (uint8_t *)getSymbolReference(), TR_AbsoluteHelperAddress, cg()), __FILE__, __LINE__, getNode());
            break;
         case TR_RamMethod:
            cg()->addAOTRelocation(new (cg()->trHeapMemory()) TR_ExternalRelocation(cursor, NULL, TR_RamMethod, cg()), __FILE__, __LINE__, getNode());
            break;
         case TR_BodyInfoAddress:
            cg()->addAOTRelocation(new (cg()->trHeapMemory()) TR_ExternalRelocation(cursor, 0, TR_BodyInfoAddress, cg()), __FILE__, __LINE__, getNode());
            break;
         default:
            TR_ASSERT(false, "Unsupported AOT relocation type specified.");
         }
      }
   if (std::find(comp->getStaticHCRPICSites()->begin(), comp->getStaticHCRPICSites()->end(), this) != comp->getStaticHCRPICSites()->end())
      {
      // HCR: whole pointer replacement.
      //
      void **locationToPatch = (void**)cursor;
      cg()->jitAddPicToPatchOnClassRedefinition(*locationToPatch, locationToPatch);
      cg()->addAOTRelocation(new (cg()->trHeapMemory()) TR_ExternalRelocation((uint8_t *)locationToPatch, (uint8_t *)*locationToPatch, TR_HCR, cg()), __FILE__,__LINE__, getNode());
      }

   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

uint8_t *TR_ARMImmSymInstruction::generateBinaryEncoding()
   {
   TR::Compilation *comp = TR::comp();
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   int32_t imm = getSourceImmediate();
   int32_t distance = imm - (intptr_t)cursor;
   TR::LabelSymbol *label;
   void *trmpln = NULL;

   if (getOpCodeValue() == ARMOp_bl)
      {
      TR::ResolvedMethodSymbol *sym = getSymbolReference()->getSymbol()->getResolvedMethodSymbol();
      TR_ResolvedMethod *resolvedMethod = (sym==NULL)?NULL:sym->getResolvedMethod();

      label = getSymbolReference()->getSymbol()->getLabelSymbol();

      if (comp->getCodeCacheSwitched())
         {
         TR::SymbolReference *calleeSymRef = NULL;

         if (label == NULL)
            calleeSymRef = getSymbolReference();
         else if (getNode() != NULL)
            calleeSymRef = getNode()->getSymbolReference();

         if (calleeSymRef != NULL)
            {
            if (calleeSymRef->getReferenceNumber()>=TR_ARMnumRuntimeHelpers)
               cg()->fe()->reserveTrampolineIfNecessary(comp, calleeSymRef, true);
            }
         else
            {
            TR_ASSERT(0, "Missing possible re-reservation for trampolines.\n");
            }
         }

      if (resolvedMethod != NULL && resolvedMethod->isSameMethod(comp->getCurrentMethod()) && !comp->isDLT())
         {
         uint32_t jitTojitStart = (uintptr_t) cg()->getCodeStart();

         // reach for how many interp->jit argument loads to skip
         jitTojitStart += ((*(int32_t *)(jitTojitStart - 4)) >> 16) & 0xFFFF;
         *(int32_t *) cursor = (*(int32_t *)cursor) | encodeBranchDistance((uintptr_t)cursor, jitTojitStart);
         }
      else if (label != NULL)
         {
         cg()->addRelocation(new (cg()->trHeapMemory()) TR_24BitLabelRelativeRelocation(cursor, label));
         ((TR_ARMCallSnippet *)getCallSnippet())->setCallRA(cursor+4);
         }
      else
         {
         TR::MethodSymbol *method;
         if(((method = getSymbolReference()->getSymbol()->getMethodSymbol()) != NULL) && method->isHelper())
            {
            *(int32_t *) cursor = encodeHelperBranch(true, getSymbolReference(), cursor, getConditionCode(), getNode(), cg());
            }
         else
            {
            if (distance<=0x01fffffc && distance>=0xfe000000)
               {
               *(int32_t *)cursor |= encodeBranchDistance((uintptr_t)cursor, (uint32_t) imm);
               }
            else
               {
               TR::Node *node = getNode();
               if (sym->isJNI() && node && node->isPreparedForDirectJNI() && getConditionCode() == ARMConditionCodeAL)
                  {
                  // A trampoline is not created.
                  // Generate a long jump.
                  //
                  *(int32_t *)cursor = 0xe28fe004;  // add LR, PC, #4
                  cursor += 4;
                  *(int32_t *)cursor = 0xe51ff004;  // ldr PC, [PC, #-4]
                  cursor += 4;
                  *(int32_t *)cursor = imm;
                  }
               else
                  {
                  // have to use the trampoline as the target and not the label
                  trmpln = (void *)cg()->fe()->methodTrampolineLookup(comp, getSymbolReference(), (void *)cursor);
                  distance = (intptr_t) trmpln - (intptr_t)cursor;
                  TR_ASSERT(distance <= BRANCH_FORWARD_LIMIT && distance >= BRANCH_BACKWARD_LIMIT,
                         "CodeCache is more than 32MB.\n");
                  *(int32_t *)cursor |= encodeBranchDistance((uintptr_t)cursor, (uintptr_t) trmpln);
                  }

               // no need to add 4 to cursor, it is done below in the
               // common path
               cg()->addAOTRelocation(new (cg()->trHeapMemory()) TR_ExternalRelocation(
                  cursor,
                  NULL,
                  TR_AbsoluteMethodAddress,
                  cg()),
                  __FILE__, __LINE__, getNode());
               }
            }
         }
      }
   else
      {
      // Place holder only: non-ARMOp_bl usage of this instruction doesn't
      // exist at this moment.
      TR_ASSERT(0, "non bl encoding");
      *(int32_t *)cursor |= distance & 0x03fffffc;
      }
   cursor += 4;
   setBinaryLength(cursor - instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

int32_t TR_ARMImmSymInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   // *this   swipeable for debugging purposes
   int32_t length;
   TR::ResolvedMethodSymbol *sym = getSymbolReference()->getSymbol()->getResolvedMethodSymbol();
   TR::Node *node = getNode();

   if (sym && sym->isJNI() && node && node->isPreparedForDirectJNI() && getConditionCode() == ARMConditionCodeAL)
      length = 3 * ARM_INSTRUCTION_LENGTH;
   else
      length = ARM_INSTRUCTION_LENGTH;

   setEstimatedBinaryLength(length);
   return(currentEstimate + length);
   }

uint8_t *TR_ARMTrg1Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTargetRegister(toARMCursor(cursor));
   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   return cursor;
   }

uint8_t *TR_ARMTrg1Src2Instruction::generateBinaryEncoding()
   {
   TR::Compilation *comp = cg()->comp();
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTargetRegister(toARMCursor(cursor), cg()->trMemory());
   insertSource1Register(toARMCursor(cursor));
   insertSource2Operand(toARMCursor(cursor));

   if (std::find(comp->getStaticPICSites()->begin(), comp->getStaticPICSites()->end(), this) != comp->getStaticPICSites()->end())
      {
      TR::Node *node = getNode();
      cg()->jitAddPicToPatchOnClassUnload((void *)(TR::Compiler->target.is64Bit()?node->getLongInt():node->getInt()), (void *)cursor);
      }
   if (std::find(comp->getStaticMethodPICSites()->begin(), comp->getStaticMethodPICSites()->end(), this) != comp->getStaticMethodPICSites()->end())
      {
      TR::Node *node = getNode();
      cg()->jitAddPicToPatchOnClassUnload((void *) (cg()->fe()->createResolvedMethod(cg()->trMemory(), (TR_OpaqueMethodBlock *) (TR::Compiler->target.is64Bit()?node->getLongInt():node->getInt()), comp->getCurrentMethod())->classOfMethod()), (void *)cursor);
      }

   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   return cursor;
   }

uint8_t *TR_ARMLoadStartPCInstruction::generateBinaryEncoding()
   {
   /* Calculates the offset from the current instruction to startPCAddr, then set it to the operand2 */
   uint8_t *startPCAddr = (uint8_t *)getSymbolReference()->getSymbol()->getStaticSymbol()->getStaticAddress();
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   uint32_t offset = (instructionStart + 8) - startPCAddr;
   uint32_t base, rotate;

   if (constantIsImmed8r(offset, &base, &rotate))
      {
      TR_ARMOperand2 *op2 = new (cg()->trHeapMemory()) TR_ARMOperand2(base, rotate);
      setSource2Operand(op2);
      /* Remove the following 3 Trg1Src2Instructions */
      getNext()->remove();
      getNext()->remove();
      getNext()->remove();

      return TR_ARMTrg1Src2Instruction::generateBinaryEncoding();
      }
   else
      {
      uint32_t bitValue    = offset;
      uint32_t bitTrailing = trailingZeroes(bitValue) & ~1;
      uint32_t base        = bitValue>> bitTrailing;

      if ((base & 0xFFFF0000) == 0)
         {
         TR_ARMOperand2 *op2_1 = new (cg()->trHeapMemory()) TR_ARMOperand2((base >> 8) & 0x000000FF, 8 + bitTrailing);
         TR_ARMOperand2 *op2_0 = new (cg()->trHeapMemory()) TR_ARMOperand2(base & 0x000000FF, bitTrailing);
         setSource2Operand(op2_1);
         TR_ARMTrg1Src2Instruction *secondInstruction = (TR_ARMTrg1Src2Instruction *)getNext();
         secondInstruction->setSource2Operand(op2_0);

         /* Remove the trailing 2 Trg1Src2Instructions */
         secondInstruction->getNext()->remove();
         secondInstruction->getNext()->remove();

         return TR_ARMTrg1Src2Instruction::generateBinaryEncoding();
         }
      else
         {
         intParts localVal(offset);
         TR_ARMOperand2 *op2_3 = new (cg()->trHeapMemory()) TR_ARMOperand2(localVal.getByte3(), 24);
         TR_ARMOperand2 *op2_2 = new (cg()->trHeapMemory()) TR_ARMOperand2(localVal.getByte2(), 16);
         TR_ARMOperand2 *op2_1 = new (cg()->trHeapMemory()) TR_ARMOperand2(localVal.getByte1(), 8);
         TR_ARMOperand2 *op2_0 = new (cg()->trHeapMemory()) TR_ARMOperand2(localVal.getByte0(), 0);

         setSource2Operand(op2_3);
         TR_ARMTrg1Src2Instruction *secondInstruction = (TR_ARMTrg1Src2Instruction *)getNext();
         secondInstruction->setSource2Operand(op2_2);

         TR_ARMTrg1Src2Instruction *thirdInstruction = (TR_ARMTrg1Src2Instruction *)secondInstruction->getNext();
         thirdInstruction->setSource2Operand(op2_1);

         TR_ARMTrg1Src2Instruction *fourthInstruction = (TR_ARMTrg1Src2Instruction *)thirdInstruction->getNext();
         fourthInstruction->setSource2Operand(op2_0);

         return TR_ARMTrg1Src2Instruction::generateBinaryEncoding();
         }
      }
   }

#if defined(__VFP_FP__) && !defined(__SOFTFP__)
uint8_t *TR_ARMTrg2Src1Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTarget1Register(toARMCursor(cursor));
   insertTarget2Register(toARMCursor(cursor));
   insertSourceRegister(toARMCursor(cursor));
   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   return cursor;
   }
#endif

uint8_t *TR_ARMMulInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTargetRegister(toARMCursor(cursor));
   insertSource1Register(toARMCursor(cursor));
   insertSource2Register(toARMCursor(cursor));
   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   return cursor;
   }

uint8_t *TR_ARMMemInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTargetRegister(toARMCursor(cursor));
   cursor = getMemoryReference()->generateBinaryEncoding(this, cursor, cg());
   setBinaryLength(cursor-instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

int32_t TR_ARMMemInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   setEstimatedBinaryLength(getMemoryReference()->estimateBinaryLength(getOpCodeValue()));
   return(currentEstimate + getEstimatedBinaryLength());
   }

uint8_t *TR_ARMTrg1MemSrc1Instruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertTargetRegister(toARMCursor(cursor));
   insertSourceRegister(toARMCursor(cursor));
   cursor = getMemoryReference()->generateBinaryEncoding(this, cursor, cg());
   setBinaryLength(cursor-instructionStart);
   setBinaryEncoding(instructionStart);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor;
   }

uint8_t *TR_ARMControlFlowInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   setBinaryLength(0);
   return cursor;
   }

int32_t TR_ARMControlFlowInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   switch(getOpCodeValue())
      {
      case ARMOp_iflong:
      case ARMOp_setbool:
         setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH * 4);
         break;
      case ARMOp_idiv:
      case ARMOp_setbflt:
         setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH * 5);
         break;
      case ARMOp_setblong:
      case ARMOp_flcmpg:
      case ARMOp_flcmpl:
      case ARMOp_irem:
         setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH * 6);
         break;
      case ARMOp_lcmp:
         setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH * 5);
         break;
      }
   return currentEstimate + getEstimatedBinaryLength();
   }

uint8_t *TR_ARMMultipleMoveInstruction::generateBinaryEncoding()
   {
   uint8_t *instructionStart = cg()->getBinaryBufferCursor();
   uint8_t *cursor           = instructionStart;
   cursor = getOpCode().copyBinaryToBuffer(instructionStart);
   generateConditionBinaryEncoding(instructionStart);
   insertMemoryBaseRegister(toARMCursor(cursor), cg()->trMemory());
   insertRegisterList(toARMCursor(cursor));
   if(isWriteBack())
      *(int32_t*)cursor |= 1 << 21;
   if (isPreIndex())
      *(int32_t*)cursor |= 1 << 24;
   if (isIncrement())
      *(int32_t*)cursor |= 1 << 23;
   cursor += ARM_INSTRUCTION_LENGTH;
   setBinaryLength(ARM_INSTRUCTION_LENGTH);
   setBinaryEncoding(instructionStart);
   return cursor;
   }

#ifdef J9_PROJECT_SPECIFIC
uint8_t *TR_ARMVirtualGuardNOPInstruction::generateBinaryEncoding()
   {
   uint8_t    *cursor           = cg()->getBinaryBufferCursor();
   TR::LabelSymbol *label        = getLabelSymbol();
   int32_t     length = 0;

   _site->setLocation(cursor);
   if (label->getCodeLocation() == NULL)
      {
      _site->setDestination(cursor);
      cg()->addRelocation(new (cg()->trHeapMemory()) TR_LabelAbsoluteRelocation((uint8_t *) (&_site->getDestination()), label));

#ifdef DEBUG
   if (debug("traceVGNOP"))
      printf("####> virtual location = %p, label (relocation) = %p\n", cursor, label);
#endif
      }
   else
      {
       _site->setDestination(label->getCodeLocation());
#ifdef DEBUG
   if (debug("traceVGNOP"))
      printf("####> virtual location = %p, label location = %p\n", cursor, label->getCodeLocation());
#endif
      }

   setBinaryEncoding(cursor);
   if (cg()->sizeOfInstructionToBePatched(this) == 0 ||
       // AOT needs an explicit nop, even if there are patchable instructions at this site because
       // 1) Those instructions might have AOT data relocations (and therefore will be incorrectly patched again)
       // 2) We might want to re-enable the code path and unpatch, in which case we would have to know what the old instruction was
         cg()->comp()->compileRelocatableCode())
      {
      TR_ARMOpCode opCode(ARMOp_nop);
      opCode.copyBinaryToBuffer(cursor);
      length = ARM_INSTRUCTION_LENGTH;
      }

   setBinaryLength(length);
   cg()->addAccumulatedInstructionLengthError(getEstimatedBinaryLength() - getBinaryLength());
   return cursor+length;
   }

int32_t TR_ARMVirtualGuardNOPInstruction::estimateBinaryLength(int32_t currentEstimate)
   {
   // This is a conservative estimation for reserving NOP space.
   setEstimatedBinaryLength(ARM_INSTRUCTION_LENGTH);
   return currentEstimate+ARM_INSTRUCTION_LENGTH;
   }
#endif