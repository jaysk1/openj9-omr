/*******************************************************************************
 * Copyright (c) 2000, 2016 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
 *******************************************************************************/

#include <stdlib.h>
#include <string.h>
#include "env/CPU.hpp"
#include "env/JitConfig.hpp"
#include "env/ProcessorInfo.hpp"
#include "x/runtime/X86Runtime.hpp"

TR_X86CPUIDBuffer *
OMR::X86::CPU::queryX86TargetCPUID(TR::Compilation *comp)
   {
#ifdef J9_PROJECT_SPECIFIC
   return NULL;
#else
   static TR_X86CPUIDBuffer *buf = NULL;
   auto jitConfig = TR::JitConfig::instance();

   if (jitConfig && jitConfig->getProcessorInfo() == NULL)
      {
      buf = (TR_X86CPUIDBuffer *) malloc(sizeof(TR_X86CPUIDBuffer));
      if (!buf)
         return 0;
      jitGetCPUID(buf);
      jitConfig->setProcessorInfo(buf);
      }
   else
      {
      if (!buf)
         {
         if (jitConfig && jitConfig->getProcessorInfo())
            {
            buf = (TR_X86CPUIDBuffer *)jitConfig->getProcessorInfo();
            }
         else
            {
            buf = (TR_X86CPUIDBuffer *) malloc(sizeof(TR_X86CPUIDBuffer));

            if (!buf)
               memcpy(buf->_vendorId, "Poughkeepsie", 12); // 12 character dummy string (NIL term not used)

            buf->_processorSignature = 0;
            buf->_brandIdEtc = 0;
            buf->_featureFlags = 0x00000000;
            buf->_cacheDescription.l1instr = 0;
            buf->_cacheDescription.l1data  = 0;
            buf->_cacheDescription.l2      = 0;
            buf->_cacheDescription.l3      = 0;
            }
         }
      }

   return buf;
#endif
   }


const char *
OMR::X86::CPU::getX86ProcessorVendorId(TR::Compilation *comp)
   {
   static char buf[13];

   // Terminate the vendor ID with NULL before returning.
   //
   strncpy(buf, self()->queryX86TargetCPUID(comp)->_vendorId, 12);
   buf[12] = '\0';

   return buf;
   }

uint32_t
OMR::X86::CPU::getX86ProcessorSignature(TR::Compilation *comp)
   {
   return self()->queryX86TargetCPUID(comp)->_processorSignature;
   }

uint32_t
OMR::X86::CPU::getX86ProcessorFeatureFlags(TR::Compilation *comp)
   {
   return self()->queryX86TargetCPUID(comp)->_featureFlags;
   }

uint32_t
OMR::X86::CPU::getX86ProcessorFeatureFlags2(TR::Compilation *comp)
   {
   return self()->queryX86TargetCPUID(comp)->_featureFlags2;
   }

uint32_t
OMR::X86::CPU::getX86ProcessorFeatureFlags8(TR::Compilation *comp)
   {
   return self()->queryX86TargetCPUID(comp)->_featureFlags8;
   }

bool
OMR::X86::CPU::testOSForSSESupport(TR::Compilation *comp)
   {
   return false;
   }
