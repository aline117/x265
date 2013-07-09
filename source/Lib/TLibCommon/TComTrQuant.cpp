/* The copyright in this software is being made available under the BSD
 * License, included below. This software may be subject to other third party
 * and contributor rights, including patent rights, and no such rights are
 * granted under this license.
 *
 * Copyright (c) 2010-2013, ITU/ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *  * Neither the name of the ITU/ISO/IEC nor the names of its contributors may
 *    be used to endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/** \file     TComTrQuant.cpp
    \brief    transform and quantization class
*/

#include "TComTrQuant.h"
#include "TComPic.h"
#include "ContextTables.h"
#include "primitives.h"

#include <stdlib.h>
#include <math.h>
#include <memory.h>

typedef struct
{
    Int    nnzBeforePos0;
    Double codedLevelAndDist; // distortion and level cost only
    Double uncodedDist;  // all zero coded block distortion
    Double sigCost;
    Double sigCost0;
} coeffGroupRDStats;

//! \ingroup TLibCommon
//! \{

// ====================================================================================================================
// Constants
// ====================================================================================================================

#define RDOQ_CHROMA 1  ///< use of RDOQ in chroma

// ====================================================================================================================
// TComTrQuant class member functions
// ====================================================================================================================

TComTrQuant::TComTrQuant()
{
    m_cQP.clear();

    // allocate temporary buffers
    // OPT_ME: I may reduce this to short and output matched, bug I am not sure it is right.
    m_tmpCoeff  = (Int*)xMalloc(Int, MAX_CU_SIZE * MAX_CU_SIZE);

    // allocate bit estimation class  (for RDOQ)
    m_estBitsSbac = new estBitsSbacStruct;
    initScalingList();
}

TComTrQuant::~TComTrQuant()
{
    // delete temporary buffers
    if (m_tmpCoeff)
    {
        xFree(m_tmpCoeff);
        m_tmpCoeff = NULL;
    }

    // delete bit estimation class
    if (m_estBitsSbac)
    {
        delete m_estBitsSbac;
    }
    destroyScalingList();
}

Void TComTrQuant::storeSliceQpNext(TComSlice* slice)
{
    Int qpBase = slice->getSliceQpBase();
    Int sliceQpused = slice->getSliceQp();
    Int sliceQpnext;
    Double alpha = qpBase < 17 ? 0.5 : 1;

    Int cnt = 0;

    for (Int u = 1; u <= LEVEL_RANGE; u++)
    {
        cnt += m_sliceNsamples[u];
    }

    if (!m_useRDOQ)
    {
        sliceQpused = qpBase;
        alpha = 0.5;
    }

    if (cnt > 120)
    {
        Double sum = 0;
        Int k = 0;
        for (Int u = 1; u < LEVEL_RANGE; u++)
        {
            sum += u * m_sliceSumC[u];
            k += u * u * m_sliceNsamples[u];
        }

        Int v;
        Double q[MAX_QP + 1];
        for (v = 0; v <= MAX_QP; v++)
        {
            q[v] = (Double)(g_invQuantScales[v % 6] * (1 << (v / 6))) / 64;
        }

        Double qnext = sum / k * q[sliceQpused] / (1 << ARL_C_PRECISION);

        for (v = 0; v < MAX_QP; v++)
        {
            if (qnext < alpha * q[v] + (1 - alpha) * q[v + 1])
            {
                break;
            }
        }

        sliceQpnext = Clip3(sliceQpused - 3, sliceQpused + 3, v);
    }
    else
    {
        sliceQpnext = sliceQpused;
    }

    m_qpDelta[qpBase] = sliceQpnext - qpBase;
}

Void TComTrQuant::initSliceQpDelta()
{
    for (Int qp = 0; qp <= MAX_QP; qp++)
    {
        m_qpDelta[qp] = qp < 17 ? 0 : 1;
    }
}

Void TComTrQuant::clearSliceARLCnt()
{
    memset(m_sliceSumC, 0, sizeof(Double) * (LEVEL_RANGE + 1));
    memset(m_sliceNsamples, 0, sizeof(Int) * (LEVEL_RANGE + 1));
}

/** Set qP for Quantization.
 * \param qpy QPy
 * \param bLowpass
 * \param eSliceType
 * \param ttype
 * \param qpBdOffset
 * \param chromaQPOffset
 *
 * return void
 */
Void TComTrQuant::setQPforQuant(Int qpy, TextType ttype, Int qpBdOffset, Int chromaQPOffset)
{
    Int qpScaled;

    if (ttype == TEXT_LUMA)
    {
        qpScaled = qpy + qpBdOffset;
    }
    else
    {
        qpScaled = Clip3(-qpBdOffset, 57, qpy + chromaQPOffset);

        if (qpScaled < 0)
        {
            qpScaled = qpScaled + qpBdOffset;
        }
        else
        {
            qpScaled = g_chromaScale[qpScaled] + qpBdOffset;
        }
    }
    m_cQP.setQpParam(qpScaled);
}

// To minimize the distortion only. No rate is considered.
Void TComTrQuant::signBitHidingHDQ(TCoeff* qCoef, TCoeff* coef, UInt const *scan, Int* deltaU, Int width, Int height)
{
    Int lastCG = -1;
    Int absSum = 0;
    Int n;

    for (Int subSet = (width * height - 1) >> LOG2_SCAN_SET_SIZE; subSet >= 0; subSet--)
    {
        Int  subPos = subSet << LOG2_SCAN_SET_SIZE;
        Int  firstNZPosInCG = SCAN_SET_SIZE, lastNZPosInCG = -1;
        absSum = 0;

        for (n = SCAN_SET_SIZE - 1; n >= 0; --n)
        {
            if (qCoef[scan[n + subPos]])
            {
                lastNZPosInCG = n;
                break;
            }
        }

        for (n = 0; n < SCAN_SET_SIZE; n++)
        {
            if (qCoef[scan[n + subPos]])
            {
                firstNZPosInCG = n;
                break;
            }
        }

        for (n = firstNZPosInCG; n <= lastNZPosInCG; n++)
        {
            absSum += qCoef[scan[n + subPos]];
        }

        if (lastNZPosInCG >= 0 && lastCG == -1)
        {
            lastCG = 1;
        }

        if (lastNZPosInCG - firstNZPosInCG >= SBH_THRESHOLD)
        {
            UInt signbit = (qCoef[scan[subPos + firstNZPosInCG]] > 0 ? 0 : 1);
            if (signbit != (absSum & 0x1)) //compare signbit with sum_parity
            {
                Int minCostInc = MAX_INT,  minPos = -1, finalChange = 0, curCost = MAX_INT, curChange = 0;

                for (n = (lastCG == 1 ? lastNZPosInCG : SCAN_SET_SIZE - 1); n >= 0; --n)
                {
                    UInt blkPos   = scan[n + subPos];
                    if (qCoef[blkPos] != 0)
                    {
                        if (deltaU[blkPos] > 0)
                        {
                            curCost = -deltaU[blkPos];
                            curChange = 1;
                        }
                        else
                        {
                            //curChange =-1;
                            if (n == firstNZPosInCG && abs(qCoef[blkPos]) == 1)
                            {
                                curCost = MAX_INT;
                            }
                            else
                            {
                                curCost = deltaU[blkPos];
                                curChange = -1;
                            }
                        }
                    }
                    else
                    {
                        if (n < firstNZPosInCG)
                        {
                            UInt thisSignBit = (coef[blkPos] >= 0 ? 0 : 1);
                            if (thisSignBit != signbit)
                            {
                                curCost = MAX_INT;
                            }
                            else
                            {
                                curCost = -(deltaU[blkPos]);
                                curChange = 1;
                            }
                        }
                        else
                        {
                            curCost = -(deltaU[blkPos]);
                            curChange = 1;
                        }
                    }

                    if (curCost < minCostInc)
                    {
                        minCostInc = curCost;
                        finalChange = curChange;
                        minPos = blkPos;
                    }
                } //CG loop

                if (qCoef[minPos] == 32767 || qCoef[minPos] == -32768)
                {
                    finalChange = -1;
                }

                if (coef[minPos] >= 0)
                {
                    qCoef[minPos] += finalChange;
                }
                else
                {
                    qCoef[minPos] -= finalChange;
                }
            } // Hide
        }
        if (lastCG == 1)
        {
            lastCG = 0;
        }
    } // TU loop
}

UInt TComTrQuant::xQuant(TComDataCU* cu,
                         Int*        coef,
                         TCoeff*     qCoef,
                         Int*        arlCCoef,
                         Int         width,
                         Int         height,
                         TextType    ttype,
                         UInt        absPartIdx)
{
    UInt acSum = 0;
    Int add = 0;
    Bool useRDOQ = cu->getTransformSkip(absPartIdx, ttype) ? m_useRDOQTS : m_useRDOQ;

    if (useRDOQ && (ttype == TEXT_LUMA || RDOQ_CHROMA))
    {
        acSum = xRateDistOptQuant(cu, coef, qCoef, arlCCoef, width, height, ttype, absPartIdx);
    }
    else
    {
        const UInt log2BlockSize = g_convertToBit[width] + 2;
        UInt scanIdx = cu->getCoefScanIdx(absPartIdx, width, ttype == TEXT_LUMA, cu->isIntra(absPartIdx));
        const UInt *scan = g_sigLastScan[scanIdx][log2BlockSize - 1];

        Int deltaU[32 * 32];

        QpParam cQpBase;
        Int qpbase = cu->getSlice()->getSliceQpBase();

        Int qpScaled;
        Int qpBDOffset = (ttype == TEXT_LUMA) ? cu->getSlice()->getSPS()->getQpBDOffsetY() : cu->getSlice()->getSPS()->getQpBDOffsetC();

        if (ttype == TEXT_LUMA)
        {
            qpScaled = qpbase + qpBDOffset;
        }
        else
        {
            Int chromaQPOffset;
            if (ttype == TEXT_CHROMA_U)
            {
                chromaQPOffset = cu->getSlice()->getPPS()->getChromaCbQpOffset() + cu->getSlice()->getSliceQpDeltaCb();
            }
            else
            {
                chromaQPOffset = cu->getSlice()->getPPS()->getChromaCrQpOffset() + cu->getSlice()->getSliceQpDeltaCr();
            }
            qpbase = qpbase + chromaQPOffset;

            qpScaled = Clip3(-qpBDOffset, 57, qpbase);

            if (qpScaled < 0)
            {
                qpScaled = qpScaled +  qpBDOffset;
            }
            else
            {
                qpScaled = g_chromaScale[qpScaled] + qpBDOffset;
            }
        }
        cQpBase.setQpParam(qpScaled);

        UInt log2TrSize = g_convertToBit[width] + 2;
        Int scalingListType = (cu->isIntra(absPartIdx) ? 0 : 3) + g_eTTable[(Int)ttype];
        assert(scalingListType < 6);
        Int *quantCoeff = 0;
        quantCoeff = getQuantCoeff(scalingListType, m_cQP.m_iRem, log2TrSize - 2);

        UInt bitDepth = ttype == TEXT_LUMA ? g_bitDepthY : g_bitDepthC;
        Int transformShift = MAX_TR_DYNAMIC_RANGE - bitDepth - log2TrSize; // Represents scaling through forward transform

        Int qbits = QUANT_SHIFT + cQpBase.m_iPer + transformShift;
        add = (cu->getSlice()->getSliceType() == I_SLICE ? 171 : 85) << (qbits - 9);
        Int qbitsC = QUANT_SHIFT + cQpBase.m_iPer + transformShift - ARL_C_PRECISION;

        Int numCoeff = width * height;
        if (m_bUseAdaptQpSelect)
        {
            acSum += x265::primitives.quantaq(coef, quantCoeff, deltaU, qCoef, arlCCoef, qbitsC, qbits, add, numCoeff);
        }
        else
        {
            acSum += x265::primitives.quant(coef, quantCoeff, deltaU, qCoef, qbits, add, numCoeff);
        }
        if (cu->getSlice()->getPPS()->getSignHideFlag() && acSum >= 2)
            signBitHidingHDQ(qCoef, coef, scan, deltaU, width, height);
    }

    return acSum;
}

Void TComTrQuant::xDeQuant(Int bitDepth, const TCoeff* qCoef, Int* coef, Int width, Int height, Int scalingListType)
{
    if (width > (Int)m_maxTrSize)
    {
        width  = m_maxTrSize;
        height = m_maxTrSize;
    }

    Int shift, add, coeffQ;
    UInt log2TrSize = g_convertToBit[width] + 2;

    Int transformShift = MAX_TR_DYNAMIC_RANGE - bitDepth - log2TrSize;

    shift = QUANT_IQUANT_SHIFT - QUANT_SHIFT - transformShift;

    TCoeff clipQCoef;

    if (getUseScalingList())
    {
        shift += 4;
        Int *dequantCoef = getDequantCoeff(scalingListType, m_cQP.m_iRem, log2TrSize - 2);

        if (shift > m_cQP.m_iPer)
        {
            add = 1 << (shift - m_cQP.m_iPer - 1);

            for (Int n = 0; n < width * height; n++)
            {
                clipQCoef = Clip3(-32768, 32767, qCoef[n]);
                coeffQ = ((clipQCoef * dequantCoef[n]) + add) >> (shift -  m_cQP.m_iPer);
                coef[n] = Clip3(-32768, 32767, coeffQ);
            }
        }
        else
        {
            for (Int n = 0; n < width * height; n++)
            {
                clipQCoef = Clip3(-32768, 32767, qCoef[n]);
                coeffQ   = Clip3(-32768, 32767, clipQCoef * dequantCoef[n]); // Clip to avoid possible overflow in following shift left operation
                coef[n] = Clip3(-32768, 32767, coeffQ << (m_cQP.m_iPer - shift));
            }
        }
    }
    else
    {
        add = 1 << (shift - 1);
        Int scale = g_invQuantScales[m_cQP.m_iRem] << m_cQP.m_iPer;

        for (Int n = 0; n < width * height; n++)
        {
            clipQCoef = Clip3(-32768, 32767, qCoef[n]);
            coeffQ = (clipQCoef * scale + add) >> shift;
            coef[n] = Clip3(-32768, 32767, coeffQ);
        }
    }
}

Void TComTrQuant::init(UInt maxTrSize, Bool useRDOQ, Bool useRDOQTS, Bool useTransformSkipFast, Bool useAdaptQpSelect)
{
    m_maxTrSize          = maxTrSize;
    m_useRDOQ              = useRDOQ;
    m_useRDOQTS            = useRDOQTS;
    m_useTransformSkipFast = useTransformSkipFast;
    m_bUseAdaptQpSelect    = useAdaptQpSelect;
}

UInt TComTrQuant::transformNxN(TComDataCU* cu,
                               Short*      residual,
                               UInt        stride,
                               TCoeff*     coeff,
                               Int*        arlCoeff,
                               UInt        width,
                               UInt        height,
                               TextType    ttype,
                               UInt        absPartIdx,
                               Bool        useTransformSkip)
{
    if (cu->getCUTransquantBypass(absPartIdx))
    {
        UInt absSum = 0;
        for (UInt k = 0; k < height; k++)
        {
            for (UInt j = 0; j < width; j++)
            {
                coeff[k * width + j] = ((Short)residual[k * stride + j]);
                absSum += abs(residual[k * stride + j]);
            }
        }
        return absSum;
    }

    UInt mode; //luma intra pred
    if (ttype == TEXT_LUMA && cu->getPredictionMode(absPartIdx) == MODE_INTRA)
    {
        mode = cu->getLumaIntraDir(absPartIdx);
    }
    else
    {
        mode = REG_DCT;
    }

    assert((cu->getSlice()->getSPS()->getMaxTrSize() >= width));
    Int bitDepth = ttype == TEXT_LUMA ? g_bitDepthY : g_bitDepthC;
    if (useTransformSkip)
    {
        xTransformSkip(bitDepth, residual, stride, m_tmpCoeff, width, height);
    }
    else
    {
        // TODO: this may need larger data types for bitDepth > 8
        const UInt log2BlockSize = g_convertToBit[width];
        x265::primitives.dct[x265::DCT_4x4 + log2BlockSize - ((width == 4) && (mode != REG_DCT))](residual, m_tmpCoeff, stride);
    }
    return xQuant(cu, m_tmpCoeff, coeff, arlCoeff, width, height, ttype, absPartIdx);
}

Void TComTrQuant::invtransformNxN(Bool transQuantBypass, TextType eText, UInt mode, Short* residual, UInt stride, TCoeff* coeff, UInt width, UInt height,  Int scalingListType, Bool useTransformSkip)
{
    if (transQuantBypass)
    {
        for (UInt k = 0; k < height; k++)
        {
            for (UInt j = 0; j < width; j++)
            {
                residual[k * stride + j] = (Short)(coeff[k * width + j]);
            }
        }
        return;
    }

    Int bitDepth = eText == TEXT_LUMA ? g_bitDepthY : g_bitDepthC;

    // Values need to pass as input parameter in dequant
    Int per = m_cQP.m_iPer;
    Int rem = m_cQP.m_iRem;
    Bool useScalingList = getUseScalingList();
    UInt log2TrSize = g_convertToBit[width] + 2;
    Int *dequantCoef = getDequantCoeff(scalingListType, m_cQP.m_iRem, log2TrSize - 2);
    x265::primitives.dequant(bitDepth, coeff, m_tmpCoeff, width, height, per, rem, useScalingList, log2TrSize, dequantCoef);

    if (useTransformSkip == true)
    {
        xITransformSkip(bitDepth, m_tmpCoeff, residual, stride, width, height);
    }
    else
    {
        // TODO: this may need larger data types for bitDepth > 8
        const UInt log2BlockSize = g_convertToBit[width];
        x265::primitives.idct[x265::IDCT_4x4 + log2BlockSize - ((width == 4) && (mode != REG_DCT))](m_tmpCoeff, residual, stride);
    }
}

Void TComTrQuant::invRecurTransformNxN(TComDataCU* cu, UInt absPartIdx, TextType eTxt, Short* residual, UInt addr, UInt stride, UInt width, UInt height, UInt maxTrMode, UInt trMode, TCoeff* coeff)
{
    if (!cu->getCbf(absPartIdx, eTxt, trMode))
    {
        return;
    }
    const UInt stopTrMode = cu->getTransformIdx(absPartIdx);

    if (trMode == stopTrMode)
    {
        UInt depth      = cu->getDepth(absPartIdx) + trMode;
        UInt log2TrSize = g_convertToBit[cu->getSlice()->getSPS()->getMaxCUWidth() >> depth] + 2;
        if (eTxt != TEXT_LUMA && log2TrSize == 2)
        {
            UInt qpDiv = cu->getPic()->getNumPartInCU() >> ((depth - 1) << 1);
            if ((absPartIdx % qpDiv) != 0)
            {
                return;
            }
            width  <<= 1;
            height <<= 1;
        }
        Short* resi = residual + addr;
        Int scalingListType = (cu->isIntra(absPartIdx) ? 0 : 3) + g_eTTable[(Int)eTxt];
        assert(scalingListType < 6);
        invtransformNxN(cu->getCUTransquantBypass(absPartIdx), eTxt, REG_DCT, resi, stride, coeff, width, height, scalingListType, cu->getTransformSkip(absPartIdx, eTxt));
    }
    else
    {
        trMode++;
        width  >>= 1;
        height >>= 1;
        Int trWidth = width, trHeight = height;
        UInt addrOffset = trHeight * stride;
        UInt coefOffset = trWidth * trHeight;
        UInt partOffset = cu->getTotalNumPart() >> (trMode << 1);
        {
            invRecurTransformNxN(cu, absPartIdx, eTxt, residual, addr, stride, width, height, maxTrMode, trMode, coeff);
            coeff += coefOffset;
            absPartIdx += partOffset;
            invRecurTransformNxN(cu, absPartIdx, eTxt, residual, addr + trWidth, stride, width, height, maxTrMode, trMode, coeff);
            coeff += coefOffset;
            absPartIdx += partOffset;
            invRecurTransformNxN(cu, absPartIdx, eTxt, residual, addr + addrOffset, stride, width, height, maxTrMode, trMode, coeff);
            coeff += coefOffset;
            absPartIdx += partOffset;
            invRecurTransformNxN(cu, absPartIdx, eTxt, residual, addr + addrOffset + trWidth, stride, width, height, maxTrMode, trMode, coeff);
        }
    }
}

// ------------------------------------------------------------------------------------------------
// Logical transform
// ------------------------------------------------------------------------------------------------

/** Wrapper function between HM interface and core NxN inverse transform (2D)
 *  \param plCoef input data (transform coefficients)
 *  \param pResidual output data (residual)
 *  \param uiStride stride of input residual data
 *  \param iSize transform size (iSize x iSize)
 *  \param uiMode is Intra Prediction mode used in Mode-Dependent DCT/DST only
 */
Void TComTrQuant::xIT(Int bitDepth, UInt mode, Int* coef, Short* residual, UInt stride, Int width, Int height)
{
    // TODO: this may need larger data types for bitDepth > 8
    const UInt log2BlockSize = g_convertToBit[width];
    x265::primitives.idct[x265::IDCT_4x4 + log2BlockSize - ((width == 4) && (mode != REG_DCT))](coef, residual, stride);
}

/** Wrapper function between HM interface and core 4x4 transform skipping
 *  \param piBlkResi input data (residual)
 *  \param psCoeff output data (transform coefficients)
 *  \param uiStride stride of input residual data
 *  \param iSize transform size (iSize x iSize)
 */
Void TComTrQuant::xTransformSkip(Int bitDepth, Short* blkResi, UInt stride, Int* coeff, Int width, Int height)
{
    assert(width == height);
    UInt log2TrSize = g_convertToBit[width] + 2;
    Int  shift = MAX_TR_DYNAMIC_RANGE - bitDepth - log2TrSize;
    UInt transformSkipShift;
    Int  j, k;
    if (shift >= 0)
    {
        x265::primitives.cvt16to32_shl(coeff, blkResi, stride, shift, width);
    }
    else
    {
        //The case when uiBitDepth > 13
        Int offset;
        transformSkipShift = -shift;
        offset = (1 << (transformSkipShift - 1));
        for (j = 0; j < height; j++)
        {
            for (k = 0; k < width; k++)
            {
                coeff[j * height + k] = (blkResi[j * stride + k] + offset) >> transformSkipShift;
            }
        }
    }
}

/** Wrapper function between HM interface and core NxN transform skipping
 *  \param plCoef input data (coefficients)
 *  \param pResidual output data (residual)
 *  \param uiStride stride of input residual data
 *  \param iSize transform size (iSize x iSize)
 */
Void TComTrQuant::xITransformSkip(Int bitDepth, Int* coef, Short* residual, UInt stride, Int width, Int height)
{
    assert(width == height);
    UInt log2TrSize = g_convertToBit[width] + 2;
    Int  shift = MAX_TR_DYNAMIC_RANGE - bitDepth - log2TrSize;
    UInt transformSkipShift;
    Int  j, k;
    if (shift > 0)
    {
        transformSkipShift = shift;
        for (j = 0; j < height; j++)
        {
            x265::primitives.cvt32to16_shr(&residual[j * stride], &coef[j * width], shift, width);
        }
    }
    else
    {
        //The case when uiBitDepth >= 13
        transformSkipShift = -shift;
        for (j = 0; j < height; j++)
        {
            for (k = 0; k < width; k++)
            {
                residual[j * stride + k] =  coef[j * width + k] << transformSkipShift;
            }
        }
    }
}

/** RDOQ with CABAC
 * \param pcCU pointer to coding unit structure
 * \param plSrcCoeff pointer to input buffer
 * \param piDstCoeff reference to pointer to output buffer
 * \param uiWidth block width
 * \param uiHeight block height
 * \param uiAbsSum reference to absolute sum of quantized transform coefficient
 * \param ttype plane type / luminance or chrominance
 * \param uiAbsPartIdx absolute partition index
 * \returns Void
 * Rate distortion optimized quantization for entropy
 * coding engines using probability models like CABAC
 */
UInt TComTrQuant::xRateDistOptQuant(TComDataCU* cu,
                                    Int*        srcCoeff,
                                    TCoeff*     dstCoeff,
                                    Int*        arlDstCoeff,
                                    UInt        width,
                                    UInt        height,
                                    TextType    ttype,
                                    UInt        absPartIdx)
{
    UInt log2TrSize = g_convertToBit[width] + 2;
    UInt absSum = 0;
    UInt bitDepth = ttype == TEXT_LUMA ? g_bitDepthY : g_bitDepthC;
    Int transformShift = MAX_TR_DYNAMIC_RANGE - bitDepth - log2TrSize; // Represents scaling through forward transform
    UInt       goRiceParam      = 0;
    Double     blockUncodedCost = 0;
    const UInt log2BlkSize      = g_convertToBit[width] + 2;
    const UInt maxNumCoeff      = width * height;
    Int scalingListType = (cu->isIntra(absPartIdx) ? 0 : 3) + g_eTTable[(Int)ttype];

    assert(scalingListType < 6);

    Int qbits = QUANT_SHIFT + m_cQP.m_iPer + transformShift; // Right shift of non-RDOQ quantizer;  level = (coeff*Q + offset)>>q_bits
    Double *errScaleOrg = getErrScaleCoeff(scalingListType, log2TrSize - 2, m_cQP.m_iRem);
    Int *qCoefOrg = getQuantCoeff(scalingListType, m_cQP.m_iRem, log2TrSize - 2);
    Int *qCoef = qCoefOrg;
    Double *errScale = errScaleOrg;
    Int qbitsC = qbits - ARL_C_PRECISION;
    Int addc = 1 << (qbitsC - 1);
    UInt scanIdx = cu->getCoefScanIdx(absPartIdx, width, ttype == TEXT_LUMA, cu->isIntra(absPartIdx));

    Double costCoeff[32 * 32];
    Double costSig[32 * 32];
    Double costCoeff0[32 * 32];

    Int rateIncUp[32 * 32];
    Int rateIncDown[32 * 32];
    Int sigRateDelta[32 * 32];
    Int deltaU[32 * 32];

    const UInt * scanCG;
    scanCG = g_sigLastScan[scanIdx][log2BlkSize > 3 ? log2BlkSize - 2 - 1 : 0];
    if (log2BlkSize == 3)
    {
        scanCG = g_sigLastScan8x8[scanIdx];
    }
    else if (log2BlkSize == 5)
    {
        scanCG = g_sigLastScanCG32x32;
    }
    const UInt cgSize = (1 << MLS_CG_SIZE); // 16
    Double costCoeffGroupSig[MLS_GRP_NUM];
    UInt sigCoeffGroupFlag[MLS_GRP_NUM];

    UInt   numBlkSide  = width / MLS_CG_SIZE;
    UInt   ctxSet      = 0;
    Int    c1          = 1;
    Int    c2          = 0;
    Double baseCost    = 0;
    Int    lastScanPos = -1;
    UInt   c1Idx       = 0;
    UInt   c2Idx       = 0;
    Int    cgLastScanPos = -1;
    Int    baseLevel;

    const UInt *scan = g_sigLastScan[scanIdx][log2BlkSize - 1];

    ::memset(sigCoeffGroupFlag, 0, sizeof(UInt) * MLS_GRP_NUM);

    UInt cgNum = width * height >> MLS_CG_SIZE;
    Int scanPos;
    coeffGroupRDStats rdStats;

    for (Int cgScanPos = cgNum - 1; cgScanPos >= 0; cgScanPos--)
    {
        UInt cgBlkPos = scanCG[cgScanPos];
        UInt cgPosY   = cgBlkPos / numBlkSide;
        UInt cgPosX   = cgBlkPos - (cgPosY * numBlkSide);
        ::memset(&rdStats, 0, sizeof(coeffGroupRDStats));

        const Int patternSigCtx = TComTrQuant::calcPatternSigCtx(sigCoeffGroupFlag, cgPosX, cgPosY, width, height);
        for (Int scanPosinCG = cgSize - 1; scanPosinCG >= 0; scanPosinCG--)
        {
            scanPos = cgScanPos * cgSize + scanPosinCG;
            //===== quantization =====
            UInt blkPos = scan[scanPos];
            // set coeff
            Int Q = qCoef[blkPos];
            Double scaleFactor = errScale[blkPos];
            Int levelDouble    = srcCoeff[blkPos];
            levelDouble        = (Int)min<Int64>((Int64)abs((Int)levelDouble) * Q, MAX_INT - (1 << (qbits - 1)));

            if (m_bUseAdaptQpSelect)
            {
                arlDstCoeff[blkPos] = (Int)((levelDouble + addc) >> qbitsC);
            }
            UInt maxAbsLevel = (levelDouble + (1 << (qbits - 1))) >> qbits;

            Double err          = Double(levelDouble);
            costCoeff0[scanPos] = err * err * scaleFactor;
            blockUncodedCost   += costCoeff0[scanPos];
            dstCoeff[blkPos]    = maxAbsLevel;

            if (maxAbsLevel > 0 && lastScanPos < 0)
            {
                lastScanPos   = scanPos;
                ctxSet        = (scanPos < SCAN_SET_SIZE || ttype != TEXT_LUMA) ? 0 : 2;
                cgLastScanPos = cgScanPos;
            }

            if (lastScanPos >= 0)
            {
                rateIncUp[blkPos] = 0;
                rateIncDown[blkPos] = 0;
                deltaU[blkPos] = 0;
                sigRateDelta[blkPos] = 0;

                //===== coefficient level estimation =====
                UInt level;
                UInt oneCtx = 4 * ctxSet + c1;
                UInt absCtx = ctxSet + c2;

                if (scanPos == lastScanPos)
                {
                    level = xGetCodedLevel(costCoeff[scanPos], costCoeff0[scanPos], costSig[scanPos],
                                           levelDouble, maxAbsLevel, 0, oneCtx, absCtx, goRiceParam,
                                           c1Idx, c2Idx, qbits, scaleFactor, 1);
                }
                else
                {
                    UInt   posY   = blkPos >> log2BlkSize;
                    UInt   posX   = blkPos - (posY << log2BlkSize);
                    UShort ctxSig = getSigCtxInc(patternSigCtx, scanIdx, posX, posY, log2BlkSize, ttype);
                    level         = xGetCodedLevel(costCoeff[scanPos], costCoeff0[scanPos], costSig[scanPos],
                                                   levelDouble, maxAbsLevel, ctxSig, oneCtx, absCtx, goRiceParam,
                                                   c1Idx, c2Idx, qbits, scaleFactor, 0);
                    sigRateDelta[blkPos] = m_estBitsSbac->significantBits[ctxSig][1] - m_estBitsSbac->significantBits[ctxSig][0];
                }
                deltaU[blkPos] = (levelDouble - ((Int)level << qbits)) >> (qbits - 8);
                if (level > 0)
                {
                    Int rateNow = xGetICRate(level, oneCtx, absCtx, goRiceParam, c1Idx, c2Idx);
                    rateIncUp[blkPos] = xGetICRate(level + 1, oneCtx, absCtx, goRiceParam, c1Idx, c2Idx) - rateNow;
                    rateIncDown[blkPos] = xGetICRate(level - 1, oneCtx, absCtx, goRiceParam, c1Idx, c2Idx) - rateNow;
                }
                else // level == 0
                {
                    rateIncUp[blkPos] = m_estBitsSbac->greaterOneBits[oneCtx][0];
                }
                dstCoeff[blkPos] = level;
                baseCost           += costCoeff[scanPos];

                baseLevel = (c1Idx < C1FLAG_NUMBER) ? (2 + (c2Idx < C2FLAG_NUMBER)) : 1;
                if (level >= baseLevel)
                {
                    if (level  > 3 * (1 << goRiceParam))
                    {
                        goRiceParam = min<UInt>(goRiceParam + 1, 4);
                    }
                }
                if (level >= 1)
                {
                    c1Idx++;
                }

                //===== update bin model =====
                if (level > 1)
                {
                    c1 = 0;
                    c2 += (c2 < 2);
                    c2Idx++;
                }
                else if ((c1 < 3) && (c1 > 0) && level)
                {
                    c1++;
                }

                //===== context set update =====
                if ((scanPos % SCAN_SET_SIZE == 0) && (scanPos > 0))
                {
                    c2                = 0;
                    goRiceParam     = 0;

                    c1Idx   = 0;
                    c2Idx   = 0;
                    ctxSet = (scanPos == SCAN_SET_SIZE || ttype != TEXT_LUMA) ? 0 : 2;
                    if (c1 == 0)
                    {
                        ctxSet++;
                    }
                    c1 = 1;
                }
            }
            else
            {
                costCoeff[scanPos] = 0;
                baseCost += costCoeff0[scanPos];
            }
            rdStats.sigCost += costSig[scanPos];
            if (scanPosinCG == 0)
            {
                rdStats.sigCost0 = costSig[scanPos];
            }
            if (dstCoeff[blkPos])
            {
                sigCoeffGroupFlag[cgBlkPos] = 1;
                rdStats.codedLevelAndDist += costCoeff[scanPos] - costSig[scanPos];
                rdStats.uncodedDist += costCoeff0[scanPos];
                if (scanPosinCG != 0)
                {
                    rdStats.nnzBeforePos0++;
                }
            }
        } //end for (scanPosinCG)

        if (cgLastScanPos >= 0)
        {
            costCoeffGroupSig[cgScanPos] = 0;
            if (cgScanPos)
            {
                if (sigCoeffGroupFlag[cgBlkPos] == 0)
                {
                    UInt  ctxSig = getSigCoeffGroupCtxInc(sigCoeffGroupFlag, cgPosX, cgPosY, width, height);
                    baseCost += xGetRateSigCoeffGroup(0, ctxSig) - rdStats.sigCost;
                    costCoeffGroupSig[cgScanPos] = xGetRateSigCoeffGroup(0, ctxSig);
                }
                else
                {
                    if (cgScanPos < cgLastScanPos) //skip the last coefficient group, which will be handled together with last position below.
                    {
                        if (rdStats.nnzBeforePos0 == 0)
                        {
                            baseCost -= rdStats.sigCost0;
                            rdStats.sigCost -= rdStats.sigCost0;
                        }
                        // rd-cost if SigCoeffGroupFlag = 0, initialization
                        Double costZeroCG = baseCost;

                        // add SigCoeffGroupFlag cost to total cost
                        UInt  ctxSig = getSigCoeffGroupCtxInc(sigCoeffGroupFlag, cgPosX, cgPosY, width, height);
                        if (cgScanPos < cgLastScanPos)
                        {
                            baseCost  += xGetRateSigCoeffGroup(1, ctxSig);
                            costZeroCG += xGetRateSigCoeffGroup(0, ctxSig);
                            costCoeffGroupSig[cgScanPos] = xGetRateSigCoeffGroup(1, ctxSig);
                        }

                        // try to convert the current coeff group from non-zero to all-zero
                        costZeroCG += rdStats.uncodedDist; // distortion for resetting non-zero levels to zero levels
                        costZeroCG -= rdStats.codedLevelAndDist; // distortion and level cost for keeping all non-zero levels
                        costZeroCG -= rdStats.sigCost; // sig cost for all coeffs, including zero levels and non-zerl levels

                        // if we can save cost, change this block to all-zero block
                        if (costZeroCG < baseCost)
                        {
                            sigCoeffGroupFlag[cgBlkPos] = 0;
                            baseCost = costZeroCG;
                            if (cgScanPos < cgLastScanPos)
                            {
                                costCoeffGroupSig[cgScanPos] = xGetRateSigCoeffGroup(0, ctxSig);
                            }
                            // reset coeffs to 0 in this block
                            for (Int scanPosinCG = cgSize - 1; scanPosinCG >= 0; scanPosinCG--)
                            {
                                scanPos      = cgScanPos * cgSize + scanPosinCG;
                                UInt blkPos = scan[scanPos];

                                if (dstCoeff[blkPos])
                                {
                                    dstCoeff[blkPos] = 0;
                                    costCoeff[scanPos] = costCoeff0[scanPos];
                                    costSig[scanPos] = 0;
                                }
                            }
                        } // end if ( d64CostAllZeros < baseCost )
                    }
                } // end if if (sigCoeffGroupFlag[ cgBlkPos ] == 0)
            }
            else
            {
                sigCoeffGroupFlag[cgBlkPos] = 1;
            }
        }
    } //end for (cgScanPos)

    //===== estimate last position =====
    if (lastScanPos < 0)
    {
        return absSum;
    }

    Double bestCost = 0;
    Int    ctxCbf = 0;
    Int    bestLastIdxp1 = 0;
    if (!cu->isIntra(absPartIdx) && ttype == TEXT_LUMA && cu->getTransformIdx(absPartIdx) == 0)
    {
        ctxCbf    = 0;
        bestCost  = blockUncodedCost + xGetICost(m_estBitsSbac->blockRootCbpBits[ctxCbf][0]);
        baseCost += xGetICost(m_estBitsSbac->blockRootCbpBits[ctxCbf][1]);
    }
    else
    {
        ctxCbf    = cu->getCtxQtCbf(ttype, cu->getTransformIdx(absPartIdx));
        ctxCbf    = (ttype ? TEXT_CHROMA : ttype) * NUM_QT_CBF_CTX + ctxCbf;
        bestCost  = blockUncodedCost + xGetICost(m_estBitsSbac->blockCbpBits[ctxCbf][0]);
        baseCost += xGetICost(m_estBitsSbac->blockCbpBits[ctxCbf][1]);
    }

    Bool foundLast = false;
    for (Int cgScanPos = cgLastScanPos; cgScanPos >= 0; cgScanPos--)
    {
        UInt cgBlkPos = scanCG[cgScanPos];

        baseCost -= costCoeffGroupSig[cgScanPos];
        if (sigCoeffGroupFlag[cgBlkPos])
        {
            for (Int scanPosinCG = cgSize - 1; scanPosinCG >= 0; scanPosinCG--)
            {
                scanPos = cgScanPos * cgSize + scanPosinCG;
                if (scanPos > lastScanPos) continue;

                UInt blkPos = scan[scanPos];
                if (dstCoeff[blkPos])
                {
                    UInt posY = blkPos >> log2BlkSize;
                    UInt posX = blkPos - (posY << log2BlkSize);

                    Double costLast = scanIdx == SCAN_VER ? xGetRateLast(posY, posX) : xGetRateLast(posX, posY);
                    Double totalCost = baseCost + costLast - costSig[scanPos];

                    if (totalCost < bestCost)
                    {
                        bestLastIdxp1  = scanPos + 1;
                        bestCost     = totalCost;
                    }
                    if (dstCoeff[blkPos] > 1)
                    {
                        foundLast = true;
                        break;
                    }
                    baseCost -= costCoeff[scanPos];
                    baseCost += costCoeff0[scanPos];
                }
                else
                {
                    baseCost -= costSig[scanPos];
                }
            } //end for

            if (foundLast)
            {
                break;
            }
        } // end if (sigCoeffGroupFlag[ cgBlkPos ])
    } // end for

    for (Int scanPos = 0; scanPos < bestLastIdxp1; scanPos++)
    {
        Int blkPos = scan[scanPos];
        Int level  = dstCoeff[blkPos];
        absSum += level;
        dstCoeff[blkPos] = (srcCoeff[blkPos] < 0) ? -level : level;
    }

    //===== clean uncoded coefficients =====
    for (Int scanPos = bestLastIdxp1; scanPos <= lastScanPos; scanPos++)
    {
        dstCoeff[scan[scanPos]] = 0;
    }

    if (cu->getSlice()->getPPS()->getSignHideFlag() && absSum >= 2)
    {
        Int64 rdFactor = (Int64)(
                g_invQuantScales[m_cQP.rem()] * g_invQuantScales[m_cQP.rem()] * (1 << (2 * m_cQP.m_iPer))
                / m_lambda / 16 / (1 << DISTORTION_PRECISION_ADJUSTMENT(2 * (bitDepth - 8)))
                + 0.5);
        Int lastCG = -1;
        Int absSum = 0;
        Int n;

        for (Int subSet = (width * height - 1) >> LOG2_SCAN_SET_SIZE; subSet >= 0; subSet--)
        {
            Int subPos = subSet << LOG2_SCAN_SET_SIZE;
            Int firstNZPosInCG = SCAN_SET_SIZE, lastNZPosInCG = -1;
            absSum = 0;

            for (n = SCAN_SET_SIZE - 1; n >= 0; --n)
            {
                if (dstCoeff[scan[n + subPos]])
                {
                    lastNZPosInCG = n;
                    break;
                }
            }

            for (n = 0; n < SCAN_SET_SIZE; n++)
            {
                if (dstCoeff[scan[n + subPos]])
                {
                    firstNZPosInCG = n;
                    break;
                }
            }

            for (n = firstNZPosInCG; n <= lastNZPosInCG; n++)
            {
                absSum += dstCoeff[scan[n + subPos]];
            }

            if (lastNZPosInCG >= 0 && lastCG == -1)
            {
                lastCG = 1;
            }

            if (lastNZPosInCG - firstNZPosInCG >= SBH_THRESHOLD)
            {
                UInt signbit = (dstCoeff[scan[subPos + firstNZPosInCG]] > 0 ? 0 : 1);
                if (signbit != (absSum & 0x1)) // hide but need tune
                {
                    // calculate the cost
                    Int64 minCostInc = MAX_INT64, curCost = MAX_INT64;
                    Int minPos = -1, finalChange = 0, curChange = 0;

                    for (n = (lastCG == 1 ? lastNZPosInCG : SCAN_SET_SIZE - 1); n >= 0; --n)
                    {
                        UInt blkPos   = scan[n + subPos];
                        if (dstCoeff[blkPos] != 0)
                        {
                            Int64 costUp   = rdFactor * (-deltaU[blkPos]) + rateIncUp[blkPos];
                            Int64 costDown = rdFactor * (deltaU[blkPos]) + rateIncDown[blkPos] -
                                             (abs(dstCoeff[blkPos]) == 1 ? ((1 << 15) + sigRateDelta[blkPos]) : 0);

                            if (lastCG == 1 && lastNZPosInCG == n && abs(dstCoeff[blkPos]) == 1)
                            {
                                costDown -= (4 << 15);
                            }

                            if (costUp < costDown)
                            {
                                curCost = costUp;
                                curChange =  1;
                            }
                            else
                            {
                                curChange = -1;
                                if (n == firstNZPosInCG && abs(dstCoeff[blkPos]) == 1)
                                {
                                    curCost = MAX_INT64;
                                }
                                else
                                {
                                    curCost = costDown;
                                }
                            }
                        }
                        else
                        {
                            curCost = rdFactor * (-(abs(deltaU[blkPos]))) + (1 << 15) + rateIncUp[blkPos] + sigRateDelta[blkPos];
                            curChange = 1;

                            if (n < firstNZPosInCG)
                            {
                                UInt thissignbit = (srcCoeff[blkPos] >= 0 ? 0 : 1);
                                if (thissignbit != signbit)
                                {
                                    curCost = MAX_INT64;
                                }
                            }
                        }

                        if (curCost < minCostInc)
                        {
                            minCostInc = curCost;
                            finalChange = curChange;
                            minPos = blkPos;
                        }
                    }

                    if (dstCoeff[minPos] == 32767 || dstCoeff[minPos] == -32768)
                    {
                        finalChange = -1;
                    }

                    if (srcCoeff[minPos] >= 0)
                    {
                        dstCoeff[minPos] += finalChange;
                    }
                    else
                    {
                        dstCoeff[minPos] -= finalChange;
                    }
                }
            }

            if (lastCG == 1)
            {
                lastCG = 0;
            }
        }
    }

    return absSum;
}

/** Pattern decision for context derivation process of significant_coeff_flag
 * \param sigCoeffGroupFlag pointer to prior coded significant coeff group
 * \param posXCG column of current coefficient group
 * \param posYCG row of current coefficient group
 * \param width width of the block
 * \param height height of the block
 * \returns pattern for current coefficient group
 */
Int TComTrQuant::calcPatternSigCtx(const UInt* sigCoeffGroupFlag, UInt posXCG, UInt posYCG, Int width, Int height)
{
    if (width == 4 && height == 4) return -1;

    UInt sigRight = 0;
    UInt sigLower = 0;

    width >>= 2;
    height >>= 2;
    if (posXCG < width - 1)
    {
        sigRight = (sigCoeffGroupFlag[posYCG * width + posXCG + 1] != 0);
    }
    if (posYCG < height - 1)
    {
        sigLower = (sigCoeffGroupFlag[(posYCG  + 1) * width + posXCG] != 0);
    }
    return sigRight + (sigLower << 1);
}

/** Context derivation process of coeff_abs_significant_flag
 * \param patternSigCtx pattern for current coefficient group
 * \param posX column of current scan position
 * \param posY row of current scan position
 * \param log2BlockSize log2 value of block size (square block)
 * \param width width of the block
 * \param height height of the block
 * \param textureType texture type (TEXT_LUMA...)
 * \returns ctxInc for current scan position
 */
Int TComTrQuant::getSigCtxInc(Int  patternSigCtx,
                              UInt scanIdx,
                              Int  posX,
                              Int  posY,
                              Int  log2BlockSize,
                              TextType ttype)
{
    const Int ctxIndMap[16] =
    {
        0, 1, 4, 5,
        2, 3, 4, 5,
        6, 6, 8, 8,
        7, 7, 8, 8
    };

    if (posX + posY == 0)
    {
        return 0;
    }

    if (log2BlockSize == 2)
    {
        return ctxIndMap[4 * posY + posX];
    }

    Int offset = log2BlockSize == 3 ? (scanIdx == SCAN_DIAG ? 9 : 15) : (ttype == TEXT_LUMA ? 21 : 12);

    Int posXinSubset = posX - ((posX >> 2) << 2);
    Int posYinSubset = posY - ((posY >> 2) << 2);
    Int cnt = 0;
    if (patternSigCtx == 0)
    {
        cnt = posXinSubset + posYinSubset <= 2 ? (posXinSubset + posYinSubset == 0 ? 2 : 1) : 0;
    }
    else if (patternSigCtx == 1)
    {
        cnt = posYinSubset <= 1 ? (posYinSubset == 0 ? 2 : 1) : 0;
    }
    else if (patternSigCtx == 2)
    {
        cnt = posXinSubset <= 1 ? (posXinSubset == 0 ? 2 : 1) : 0;
    }
    else
    {
        cnt = 2;
    }

    return ((ttype == TEXT_LUMA && ((posX >> 2) + (posY >> 2)) > 0) ? 3 : 0) + offset + cnt;
}

/** Get the best level in RD sense
 * \param codedCost reference to coded cost
 * \param codedCost0 reference to cost when coefficient is 0
 * \param codedCostSig reference to cost of significant coefficient
 * \param levelDouble reference to unscaled quantized level
 * \param maxAbsLevel scaled quantized level
 * \param ctxNumSig current ctxInc for coeff_abs_significant_flag
 * \param ctxNumOne current ctxInc for coeff_abs_level_greater1 (1st bin of coeff_abs_level_minus1 in AVC)
 * \param ctxNumAbs current ctxInc for coeff_abs_level_greater2 (remaining bins of coeff_abs_level_minus1 in AVC)
 * \param absGoRice current Rice parameter for coeff_abs_level_minus3
 * \param qbits quantization step size
 * \param scaleFactor correction factor
 * \param bLast indicates if the coefficient is the last significant
 * \returns best quantized transform level for given scan position
 * This method calculates the best quantized transform level for a given scan position.
 */
__inline UInt TComTrQuant::xGetCodedLevel(Double& codedCost,
                                          Double& codedCost0,
                                          Double& codedCostSig,
                                          Int     levelDouble,
                                          UInt    maxAbsLevel,
                                          UShort  ctxNumSig,
                                          UShort  ctxNumOne,
                                          UShort  ctxNumAbs,
                                          UShort  absGoRice,
                                          UInt    c1Idx,
                                          UInt    c2Idx,
                                          Int     qbits,
                                          Double  scaleFactor,
                                          Bool    bLast) const
{
    Double curCostSig   = 0;
    UInt   bestAbsLevel = 0;

    if (!bLast && maxAbsLevel < 3)
    {
        codedCostSig = xGetRateSigCoef(0, ctxNumSig);
        codedCost    = codedCost0 + codedCostSig;
        if (maxAbsLevel == 0)
        {
            return bestAbsLevel;
        }
    }
    else
    {
        codedCost = MAX_DOUBLE;
    }

    if (!bLast)
    {
        curCostSig = xGetRateSigCoef(1, ctxNumSig);
    }

    UInt minAbsLevel = (maxAbsLevel > 1 ? maxAbsLevel - 1 : 1);
    for (Int absLevel = maxAbsLevel; absLevel >= minAbsLevel; absLevel--)
    {
        Double err     = Double(levelDouble  - (absLevel << qbits));
        Double curCost = err * err * scaleFactor + xGetICRateCost(absLevel, ctxNumOne, ctxNumAbs, absGoRice, c1Idx, c2Idx);
        curCost       += curCostSig;

        if (curCost < codedCost)
        {
            bestAbsLevel = absLevel;
            codedCost    = curCost;
            codedCostSig = curCostSig;
        }
    }

    return bestAbsLevel;
}

/** Calculates the cost for specific absolute transform level
 * \param absLevel scaled quantized level
 * \param ctxNumOne current ctxInc for coeff_abs_level_greater1 (1st bin of coeff_abs_level_minus1 in AVC)
 * \param ctxNumAbs current ctxInc for coeff_abs_level_greater2 (remaining bins of coeff_abs_level_minus1 in AVC)
 * \param absGoRice Rice parameter for coeff_abs_level_minus3
 * \returns cost of given absolute transform level
 */
__inline Double TComTrQuant::xGetICRateCost(UInt   absLevel,
                                            UShort ctxNumOne,
                                            UShort ctxNumAbs,
                                            UShort absGoRice,
                                            UInt   c1Idx,
                                            UInt   c2Idx) const
{
    Double iRate = xGetIEPRate();
    UInt baseLevel = (c1Idx < C1FLAG_NUMBER) ? (2 + (c2Idx < C2FLAG_NUMBER)) : 1;

    if (absLevel >= baseLevel)
    {
        UInt symbol = absLevel - baseLevel;
        UInt length;
        if (symbol < (COEF_REMAIN_BIN_REDUCTION << absGoRice))
        {
            length = symbol >> absGoRice;
            iRate += (length + 1 + absGoRice) << 15;
        }
        else
        {
            length = absGoRice;
            symbol  = symbol - (COEF_REMAIN_BIN_REDUCTION << absGoRice);
            while (symbol >= (1 << length))
            {
                symbol -=  (1 << (length++));
            }

            iRate += (COEF_REMAIN_BIN_REDUCTION + length + 1 - absGoRice + length) << 15;
        }
        if (c1Idx < C1FLAG_NUMBER)
        {
            iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][1];

            if (c2Idx < C2FLAG_NUMBER)
            {
                iRate += m_estBitsSbac->levelAbsBits[ctxNumAbs][1];
            }
        }
    }
    else if (absLevel == 1)
    {
        iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][0];
    }
    else if (absLevel == 2)
    {
        iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][1];
        iRate += m_estBitsSbac->levelAbsBits[ctxNumAbs][0];
    }
    else
    {
        assert(0);
    }
    return xGetICost(iRate);
}

__inline Int TComTrQuant::xGetICRate(UInt   absLevel,
                                     UShort ctxNumOne,
                                     UShort ctxNumAbs,
                                     UShort absGoRice,
                                     UInt   c1Idx,
                                     UInt   c2Idx) const
{
    Int iRate = 0;
    UInt baseLevel = (c1Idx < C1FLAG_NUMBER) ? (2 + (c2Idx < C2FLAG_NUMBER)) : 1;

    if (absLevel >= baseLevel)
    {
        UInt uiSymbol   = absLevel - baseLevel;
        UInt uiMaxVlc   = g_goRiceRange[absGoRice];
        Bool bExpGolomb = (uiSymbol > uiMaxVlc);

        if (bExpGolomb)
        {
            absLevel = uiSymbol - uiMaxVlc;
            Int iEGS = 1;
            for (UInt uiMax = 2; absLevel >= uiMax; uiMax <<= 1, iEGS += 2)
            {}

            iRate   += iEGS << 15;
            uiSymbol = min<UInt>(uiSymbol, (uiMaxVlc + 1));
        }

        UShort ui16PrefLen = UShort(uiSymbol >> absGoRice) + 1;
        UShort ui16NumBins = min<UInt>(ui16PrefLen, g_goRicePrefixLen[absGoRice]) + absGoRice;

        iRate += ui16NumBins << 15;

        if (c1Idx < C1FLAG_NUMBER)
        {
            iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][1];

            if (c2Idx < C2FLAG_NUMBER)
            {
                iRate += m_estBitsSbac->levelAbsBits[ctxNumAbs][1];
            }
        }
    }
    else if (absLevel == 0)
    {
        return 0;
    }
    else if (absLevel == 1)
    {
        iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][0];
    }
    else if (absLevel == 2)
    {
        iRate += m_estBitsSbac->greaterOneBits[ctxNumOne][1];
        iRate += m_estBitsSbac->levelAbsBits[ctxNumAbs][0];
    }
    else
    {
        assert(0);
    }
    return iRate;
}

/** Calculates the cost of signaling the last significant coefficient in the block
 * \param posx X coordinate of the last significant coefficient
 * \param posy Y coordinate of the last significant coefficient
 * \returns cost of last significant coefficient
 */
__inline Double TComTrQuant::xGetRateLast(UInt posx, UInt posy) const
{
    UInt ctxX = g_groupIdx[posx];
    UInt ctxY = g_groupIdx[posy];
    Double cost = m_estBitsSbac->lastXBits[ctxX] + m_estBitsSbac->lastYBits[ctxY];

    if (ctxX > 3)
    {
        cost += xGetIEPRate() * ((ctxX - 2) >> 1);
    }
    if (ctxY > 3)
    {
        cost += xGetIEPRate() * ((ctxY - 2) >> 1);
    }
    return xGetICost(cost);
}

/** Context derivation process of coeff_abs_significant_flag
 * \param sigCoeffGroupFlag significance map of L1
 * \param uiBlkX column of current scan position
 * \param uiBlkY row of current scan position
 * \param uiLog2BlkSize log2 value of block size
 * \returns ctxInc for current scan position
 */
UInt TComTrQuant::getSigCoeffGroupCtxInc(const UInt* sigCoeffGroupFlag, UInt cgPosX, UInt cgPosY,
                                         Int width, Int height)
{
    UInt right = 0;
    UInt lower = 0;

    width >>= 2;
    height >>= 2;
    if (cgPosX < width - 1)
    {
        right = (sigCoeffGroupFlag[cgPosY * width + cgPosX + 1] != 0);
    }
    if (cgPosY < height - 1)
    {
        lower = (sigCoeffGroupFlag[(cgPosY  + 1) * width + cgPosX] != 0);
    }
    return right || lower;
}

/** set quantized matrix coefficient for encode
 * \param scalingList quantized matrix address
 */
Void TComTrQuant::setScalingList(TComScalingList *scalingList)
{
    UInt size, list;
    UInt qp;

    for (size = 0; size < SCALING_LIST_SIZE_NUM; size++)
    {
        for (list = 0; list < g_scalingListNum[size]; list++)
        {
            for (qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
            {
                xSetScalingListEnc(scalingList, list, size, qp);
                xSetScalingListDec(scalingList, list, size, qp);
                setErrScaleCoeff(list, size, qp);
            }
        }
    }
}

/** set quantized matrix coefficient for decode
 * \param scalingList quantized matrix address
 */
Void TComTrQuant::setScalingListDec(TComScalingList *scalingList)
{
    UInt size, list;
    UInt qp;

    for (size = 0; size < SCALING_LIST_SIZE_NUM; size++)
    {
        for (list = 0; list < g_scalingListNum[size]; list++)
        {
            for (qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
            {
                xSetScalingListDec(scalingList, list, size, qp);
            }
        }
    }
}

/** set error scale coefficients
 * \param list List ID
 * \param uiSize Size
 * \param uiQP Quantization parameter
 */
Void TComTrQuant::setErrScaleCoeff(UInt list, UInt size, UInt qp)
{
    UInt uiLog2TrSize = g_convertToBit[g_scalingListSizeX[size]] + 2;
    Int bitDepth = (size < SCALING_LIST_32x32 && list != 0 && list != 3) ? g_bitDepthC : g_bitDepthY;
    Int iTransformShift = MAX_TR_DYNAMIC_RANGE - bitDepth - uiLog2TrSize; // Represents scaling through forward transform

    UInt i, uiMaxNumCoeff = g_scalingListSize[size];
    Int *piQuantcoeff;
    Double *pdErrScale;

    piQuantcoeff   = getQuantCoeff(list, qp, size);
    pdErrScale     = getErrScaleCoeff(list, size, qp);

    Double dErrScale = (Double)(1 << SCALE_BITS);                          // Compensate for scaling of bitcount in Lagrange cost function
    dErrScale = dErrScale * pow(2.0, -2.0 * iTransformShift);              // Compensate for scaling through forward transform
    for (i = 0; i < uiMaxNumCoeff; i++)
    {
        pdErrScale[i] = dErrScale / piQuantcoeff[i] / piQuantcoeff[i] / (1 << DISTORTION_PRECISION_ADJUSTMENT(2 * (bitDepth - 8)));
    }
}

/** set quantized matrix coefficient for encode
 * \param scalingList quantized matrix address
 * \param listId List index
 * \param sizeId size index
 * \param uiQP Quantization parameter
 */
Void TComTrQuant::xSetScalingListEnc(TComScalingList *scalingList, UInt listId, UInt sizeId, UInt qp)
{
    UInt width = g_scalingListSizeX[sizeId];
    UInt height = g_scalingListSizeX[sizeId];
    UInt ratio = g_scalingListSizeX[sizeId] / min(MAX_MATRIX_SIZE_NUM, (Int)g_scalingListSizeX[sizeId]);
    Int *quantcoeff;
    Int *coeff = scalingList->getScalingListAddress(sizeId, listId);

    quantcoeff   = getQuantCoeff(listId, qp, sizeId);
    processScalingListEnc(coeff, quantcoeff, g_quantScales[qp] << 4, height, width, ratio, min(MAX_MATRIX_SIZE_NUM, (Int)g_scalingListSizeX[sizeId]), scalingList->getScalingListDC(sizeId, listId));
}

/** set quantized matrix coefficient for decode
 * \param scalingList quantized matrix address
 * \param list List index
 * \param size size index
 * \param uiQP Quantization parameter
 */
Void TComTrQuant::xSetScalingListDec(TComScalingList *scalingList, UInt listId, UInt sizeId, UInt qp)
{
    UInt width = g_scalingListSizeX[sizeId];
    UInt height = g_scalingListSizeX[sizeId];
    UInt ratio = g_scalingListSizeX[sizeId] / min(MAX_MATRIX_SIZE_NUM, (Int)g_scalingListSizeX[sizeId]);
    Int *dequantcoeff;
    Int *coeff = scalingList->getScalingListAddress(sizeId, listId);

    dequantcoeff = getDequantCoeff(listId, qp, sizeId);
    processScalingListDec(coeff, dequantcoeff, g_invQuantScales[qp], height, width, ratio, min(MAX_MATRIX_SIZE_NUM, (Int)g_scalingListSizeX[sizeId]), scalingList->getScalingListDC(sizeId, listId));
}

/** set flat matrix value to quantized coefficient
 */
Void TComTrQuant::setFlatScalingList()
{
    UInt size, list;
    UInt qp;

    for (size = 0; size < SCALING_LIST_SIZE_NUM; size++)
    {
        for (list = 0; list < g_scalingListNum[size]; list++)
        {
            for (qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
            {
                xsetFlatScalingList(list, size, qp);
                setErrScaleCoeff(list, size, qp);
            }
        }
    }
}

/** set flat matrix value to quantized coefficient
 * \param list List ID
 * \param uiQP Quantization parameter
 * \param uiSize Size
 */
Void TComTrQuant::xsetFlatScalingList(UInt list, UInt size, UInt qp)
{
    UInt i, num = g_scalingListSize[size];
    Int *quantcoeff;
    Int *dequantcoeff;
    Int quantScales = g_quantScales[qp];
    Int invQuantScales = g_invQuantScales[qp] << 4;

    quantcoeff   = getQuantCoeff(list, qp, size);
    dequantcoeff = getDequantCoeff(list, qp, size);

    for (i = 0; i < num; i++)
    {
        *quantcoeff++ = quantScales;
        *dequantcoeff++ = invQuantScales;
    }
}

/** set quantized matrix coefficient for encode
 * \param coeff quantized matrix address
 * \param quantcoeff quantized matrix address
 * \param quantScales Q(QP%6)
 * \param height height
 * \param width width
 * \param ratio ratio for upscale
 * \param sizuNum matrix size
 * \param dc dc parameter
 */
Void TComTrQuant::processScalingListEnc(Int *coeff, Int *quantcoeff, Int quantScales, UInt height, UInt width, UInt ratio, Int sizuNum, UInt dc)
{
    Int nsqth = (height < width) ? 4 : 1; //height ratio for NSQT
    Int nsqtw = (width < height) ? 4 : 1; //width ratio for NSQT

    for (UInt j = 0; j < height; j++)
    {
        for (UInt i = 0; i < width; i++)
        {
            quantcoeff[j * width + i] = quantScales / coeff[sizuNum * (j * nsqth / ratio) + i * nsqtw / ratio];
        }
    }

    if (ratio > 1)
    {
        quantcoeff[0] = quantScales / dc;
    }
}

/** set quantized matrix coefficient for decode
 * \param coeff quantized matrix address
 * \param dequantcoeff quantized matrix address
 * \param invQuantScales IQ(QP%6))
 * \param height height
 * \param width width
 * \param ratio ratio for upscale
 * \param sizuNum matrix size
 * \param dc dc parameter
 */
Void TComTrQuant::processScalingListDec(Int *coeff, Int *dequantcoeff, Int invQuantScales, UInt height, UInt width, UInt ratio, Int sizuNum, UInt dc)
{
    for (UInt j = 0; j < height; j++)
    {
        for (UInt i = 0; i < width; i++)
        {
            dequantcoeff[j * width + i] = invQuantScales * coeff[sizuNum * (j / ratio) + i / ratio];
        }
    }

    if (ratio > 1)
    {
        dequantcoeff[0] = invQuantScales * dc;
    }
}

/** initialization process of scaling list array
 */
Void TComTrQuant::initScalingList()
{
    for (UInt sizeId = 0; sizeId < SCALING_LIST_SIZE_NUM; sizeId++)
    {
        for (UInt listId = 0; listId < g_scalingListNum[sizeId]; listId++)
        {
            for (UInt qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
            {
                m_quantCoef[sizeId][listId][qp] = new Int[g_scalingListSize[sizeId]];
                m_dequantCoef[sizeId][listId][qp] = new Int[g_scalingListSize[sizeId]];
                m_errScale[sizeId][listId][qp] = new Double[g_scalingListSize[sizeId]];
            }
        }
    }

    // alias list [1] as [3].
    for (UInt qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
    {
        m_quantCoef[SCALING_LIST_32x32][3][qp] = m_quantCoef[SCALING_LIST_32x32][1][qp];
        m_dequantCoef[SCALING_LIST_32x32][3][qp] = m_dequantCoef[SCALING_LIST_32x32][1][qp];
        m_errScale[SCALING_LIST_32x32][3][qp] = m_errScale[SCALING_LIST_32x32][1][qp];
    }
}

/** destroy quantization matrix array
 */
Void TComTrQuant::destroyScalingList()
{
    for (UInt sizeId = 0; sizeId < SCALING_LIST_SIZE_NUM; sizeId++)
    {
        for (UInt listId = 0; listId < g_scalingListNum[sizeId]; listId++)
        {
            for (UInt qp = 0; qp < SCALING_LIST_REM_NUM; qp++)
            {
                if (m_quantCoef[sizeId][listId][qp]) delete [] m_quantCoef[sizeId][listId][qp];
                if (m_dequantCoef[sizeId][listId][qp]) delete [] m_dequantCoef[sizeId][listId][qp];
                if (m_errScale[sizeId][listId][qp]) delete [] m_errScale[sizeId][listId][qp];
            }
        }
    }
}

//! \}
