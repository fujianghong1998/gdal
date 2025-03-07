/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements FileGDB OGR layer.
 * Author:   Ragi Yaser Burhum, ragi@burhum.com
 *           Paul Ramsey, pramsey at cleverelephant.ca
 *
 ******************************************************************************
 * Copyright (c) 2010, Ragi Yaser Burhum
 * Copyright (c) 2011, Paul Ramsey <pramsey at cleverelephant.ca>
 * Copyright (c) 2011-2014, Even Rouault <even dot rouault at spatialys.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include <cassert>
#include "ogr_fgdb.h"
#include "ogrpgeogeometry.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "FGdbUtils.h"
#include "cpl_minixml.h"  // the only way right now to extract schema information
#include "filegdb_gdbtoogrfieldtype.h"
#include "filegdb_fielddomain.h"

// See https://github.com/Esri/file-geodatabase-api/issues/46
// On certain FileGDB datasets with binary fields, iterating over a result set
// where the binary field is requested crashes in EnumRows::Next() at the
// second iteration.
// The workaround consists in iterating only over OBJECTID in the main loop,
// and requesting each feature in a separate request.
#define WORKAROUND_CRASH_ON_CDF_WITH_BINARY_FIELD

using std::string;
using std::wstring;

/************************************************************************/
/*                           FGdbBaseLayer()                            */
/************************************************************************/
FGdbBaseLayer::FGdbBaseLayer()
    : m_pFeatureDefn(nullptr), m_pSRS(nullptr), m_pEnumRows(nullptr),
      m_suppressColumnMappingError(false), m_forceMulti(false)
{
}

/************************************************************************/
/*                          ~FGdbBaseLayer()                            */
/************************************************************************/
FGdbBaseLayer::~FGdbBaseLayer()
{
    if (m_pFeatureDefn)
    {
        m_pFeatureDefn->Release();
        m_pFeatureDefn = nullptr;
    }

    FGdbBaseLayer::CloseGDBObjects();

    if (m_pSRS)
    {
        m_pSRS->Release();
        m_pSRS = nullptr;
    }
}

/************************************************************************/
/*                          CloseGDBObjects()                           */
/************************************************************************/

void FGdbBaseLayer::CloseGDBObjects()
{
    if (m_pEnumRows)
    {
        delete m_pEnumRows;
        m_pEnumRows = nullptr;
    }
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *FGdbBaseLayer::GetNextFeature()
{
    while (true)  // want to skip errors
    {
        if (m_pEnumRows == nullptr)
            return nullptr;

        long hr;

        Row row;

        if (FAILED(hr = m_pEnumRows->Next(row)))
        {
            GDBErr(hr, "Failed fetching features");
            return nullptr;
        }

        if (hr != S_OK)
        {
            // It's OK, we are done fetching - failure is caught by FAILED macro
            return nullptr;
        }

        OGRFeature *pOGRFeature = nullptr;

        if (!OGRFeatureFromGdbRow(&row, &pOGRFeature))
        {
            int32 oid = -1;
            CPL_IGNORE_RET_VAL(row.GetOID(oid));

            GDBErr(hr,
                   CPLSPrintf("Failed translating FGDB row [%d] to OGR Feature",
                              oid));

            // return NULL;
            continue;  // skip feature
        }

        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(pOGRFeature->GetGeometryRef())))
        {
            return pOGRFeature;
        }
        delete pOGRFeature;
    }
}

/************************************************************************/
/*                              FGdbLayer()                             */
/************************************************************************/
FGdbLayer::FGdbLayer()
    : m_pDS(nullptr), m_pTable(nullptr), m_wstrSubfields(L"*"),
      m_bFilterDirty(true), m_bLaunderReservedKeywords(true)
{
    m_bBulkLoadAllowed = -1; /* uninitialized */
    m_bBulkLoadInProgress = FALSE;
    m_pEnumRows = new EnumRows;

#ifdef EXTENT_WORKAROUND
    m_bLayerEnvelopeValid = false;
    m_bLayerJustCreated = false;
#endif
    m_papszOptions = nullptr;
    m_bCreateMultipatch = FALSE;
    m_nResyncThreshold =
        atoi(CPLGetConfigOption("FGDB_RESYNC_THRESHOLD", "1000000"));
    m_bSymlinkFlag = FALSE;
}

/************************************************************************/
/*                            ~FGdbLayer()                              */
/************************************************************************/

FGdbLayer::~FGdbLayer()
{
    FGdbLayer::CloseGDBObjects();

    for (size_t i = 0; i < m_apoByteArrays.size(); i++)
        delete m_apoByteArrays[i];
    m_apoByteArrays.resize(0);

    CSLDestroy(m_papszOptions);
    m_papszOptions = nullptr;
}

/************************************************************************/
/*                        CloseGDBObjects()                             */
/************************************************************************/

void FGdbLayer::CloseGDBObjects()
{
    EndBulkLoad();

#ifdef EXTENT_WORKAROUND
    WorkAroundExtentProblem();
#endif

    if (m_pTable)
    {
        delete m_pTable;
        m_pTable = nullptr;
    }

    FGdbBaseLayer::CloseGDBObjects();
}

/************************************************************************/
/*                     EditIndexesForFIDHack()                          */
/************************************************************************/

int FGdbLayer::EditIndexesForFIDHack(const char *pszRadixTablename)
{
    // Fix FIDs in .gdbtablx, .spx and .atx's

    CPLString osGDBTablX = CPLResetExtension(pszRadixTablename, "gdbtablx");
    CPLString osNewGDBTablX =
        CPLResetExtension(pszRadixTablename, "gdbtablx.new");

    if (!EditGDBTablX(osGDBTablX, osNewGDBTablX))
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Error occurred when editing %s",
                 osNewGDBTablX.c_str());
        VSIUnlink(osNewGDBTablX);
        return FALSE;
    }

    CPLString osDirectory(CPLGetPath(pszRadixTablename));
    char **papszFiles = VSIReadDir(osDirectory);
    CPLString osBasename(CPLGetBasename(pszRadixTablename));
    int bRet = TRUE;
    for (char **papszIter = papszFiles; papszIter && *papszIter; papszIter++)
    {
        if (strncmp(*papszIter, osBasename.c_str(), osBasename.size()) == 0 &&
            (EQUAL(CPLGetExtension(*papszIter), "atx") ||
             EQUAL(CPLGetExtension(*papszIter), "spx")))
        {
            CPLString osIndex(
                CPLFormFilename(osDirectory, *papszIter, nullptr));
            if (!EditATXOrSPX(osIndex))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Error occurred when editing %s", osIndex.c_str());
                bRet = FALSE;
            }
        }
    }
    CSLDestroy(papszFiles);

    CPLString osGDBTablXTmp(CPLSPrintf("%s.tmp", osGDBTablX.c_str()));
    int bRet2 = (VSIRename(osGDBTablX, osGDBTablXTmp) == 0 &&
                 VSIRename(osNewGDBTablX, osGDBTablX) == 0);
    VSIUnlink(osGDBTablXTmp);
    if (!bRet2)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot rename %s to %s",
                 osNewGDBTablX.c_str(), osGDBTablX.c_str());
        bRet = FALSE;
    }

    return bRet;
}

/************************************************************************/
/*                           EditATXOrSPX()                             */
/************************************************************************/

/* See https://github.com/rouault/dump_gdbtable/wiki/FGDB-Spec */
int FGdbLayer::EditATXOrSPX(const CPLString &osIndex)
{
    VSILFILE *fp = VSIFOpenL(osIndex, "rb+");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Cannot open %s", osIndex.c_str());
        return FALSE;
    }
    VSIFSeekL(fp, 0, SEEK_END);
    vsi_l_offset nPos = VSIFTellL(fp);
    int bRet = FALSE;
    int bInvalidateIndex = FALSE;
    if (nPos > 22)
    {
        VSIFSeekL(fp, nPos - 22, SEEK_SET);
        GByte nSizeIndexedValue;
        if (VSIFReadL(&nSizeIndexedValue, 1, 1, fp) == 1 &&
            nSizeIndexedValue > 0)
        {
            GByte abyIndexedValue[255];
            VSIFSeekL(fp, nPos - 22 + 6, SEEK_SET);
            int nDepth;
            if (VSIFReadL(&nDepth, 1, 4, fp) == 4)
            {
                CPL_LSBPTR32(&nDepth);

                int bIndexedValueIsValid = FALSE;
                int nFirstIndexAtThisValue = -1;
                std::vector<int> anPagesAtThisValue;
                int bSortThisValue = FALSE;
                int nLastPageVisited = 0;
                bRet = EditATXOrSPX(fp, 1, nLastPageVisited, nDepth,
                                    nSizeIndexedValue, abyIndexedValue,
                                    bIndexedValueIsValid,
                                    nFirstIndexAtThisValue, anPagesAtThisValue,
                                    bSortThisValue, bInvalidateIndex);
            }
        }
    }
    VSIFCloseL(fp);
    if (bInvalidateIndex)
    {
        // CPLDebug("FGDB", "Invalidate %s", osIndex.c_str());
        CPLError(CE_Warning, CPLE_AppDefined, "Invalidate %s", osIndex.c_str());
        VSIUnlink(osIndex);
    }
    return bRet;
}

static int FGdbLayerSortATX(const void *_pa, const void *_pb)
{
    int a = CPL_LSBWORD32(*(int *)_pa);
    int b = CPL_LSBWORD32(*(int *)_pb);
    if (a < b)
        return -1;
    else if (a > b)
        return 1;
    CPLAssert(false);
    return 0;
}

int FGdbLayer::EditATXOrSPX(VSILFILE *fp, int nThisPage, int &nLastPageVisited,
                            int nDepth, int nSizeIndexedValue,
                            GByte *pabyLastIndexedValue,
                            int &bIndexedValueIsValid,
                            int &nFirstIndexAtThisValue,
                            std::vector<int> &anPagesAtThisValue,
                            int &bSortThisValue, int &bInvalidateIndex)
{
    GByte abyBuffer[4096];

    VSIFSeekL(fp, (nThisPage - 1) * 4096, SEEK_SET);

    if (nDepth == 1)
    {
        if (nThisPage == nLastPageVisited)
            return TRUE;

        /* This page directly references features */
        int bRewritePage = FALSE;
        if (VSIFReadL(abyBuffer, 1, 4096, fp) != 4096)
            return FALSE;
        int nNextPageID;
        memcpy(&nNextPageID, abyBuffer, 4);
        int nFeatures;
        memcpy(&nFeatures, abyBuffer + 4, 4);
        CPL_LSBPTR32(&nFeatures);

        // if( nLastPageVisited == 0 )
        //     printf("nFeatures = %d\n", nFeatures);

        const int nMaxPerPages = (4096 - 12) / (4 + nSizeIndexedValue);
        const int nOffsetFirstValInPage = 12 + nMaxPerPages * 4;
        if (nFeatures > nMaxPerPages)
            return FALSE;
        for (int i = 0; i < nFeatures; i++)
        {
            int bNewVal = (!bIndexedValueIsValid ||
                           memcmp(pabyLastIndexedValue,
                                  abyBuffer + nOffsetFirstValInPage +
                                      i * nSizeIndexedValue,
                                  nSizeIndexedValue) != 0);

            int nFID;
            memcpy(&nFID, abyBuffer + 12 + 4 * i, 4);
            CPL_LSBPTR32(&nFID);
            int nOGRFID = m_oMapFGDBFIDToOGRFID[nFID];
            if (nOGRFID)
            {
                nFID = nOGRFID;
                CPL_LSBPTR32(&nOGRFID);
                memcpy(abyBuffer + 12 + 4 * i, &nOGRFID, 4);
                bRewritePage = TRUE;

                if (bIndexedValueIsValid && i == nFeatures - 1 &&
                    nNextPageID == 0)
                    bSortThisValue = TRUE;
            }

            // We must make sure that features with same indexed values are
            // sorted by increasing FID, even when that spans over several
            // pages
            if (bSortThisValue &&
                (bNewVal || (i == nFeatures - 1 && nNextPageID == 0)))
            {
                if (anPagesAtThisValue[0] == nThisPage)
                {
                    CPLAssert(anPagesAtThisValue.size() == 1);
                    int nFeaturesToSortThisPage = i - nFirstIndexAtThisValue;
                    if (!bNewVal && i == nFeatures - 1 && nNextPageID == 0)
                        nFeaturesToSortThisPage++;
                    CPLAssert(nFeaturesToSortThisPage > 0);

                    bRewritePage = TRUE;
                    qsort(abyBuffer + 12 + 4 * nFirstIndexAtThisValue,
                          nFeaturesToSortThisPage, 4, FGdbLayerSortATX);
                }
                else
                {
                    std::vector<int> anValues;
                    int nFeaturesToSort = 0;
                    anValues.resize(anPagesAtThisValue.size() * nMaxPerPages);

                    int nFeaturesToSortLastPage = i;
                    if (!bNewVal && i == nFeatures - 1 && nNextPageID == 0)
                        nFeaturesToSortLastPage++;

                    for (size_t j = 0; j < anPagesAtThisValue.size(); j++)
                    {
                        int nFeaturesPrevPage;
                        VSIFSeekL(fp, (anPagesAtThisValue[j] - 1) * 4096 + 4,
                                  SEEK_SET);
                        VSIFReadL(&nFeaturesPrevPage, 1, 4, fp);
                        CPL_LSBPTR32(&nFeaturesPrevPage);
                        if (j == 0)
                        {
                            VSIFSeekL(fp,
                                      (anPagesAtThisValue[j] - 1) * 4096 + 12 +
                                          4 * nFirstIndexAtThisValue,
                                      SEEK_SET);
                            VSIFReadL(
                                &anValues[nFeaturesToSort], 4,
                                nFeaturesPrevPage - nFirstIndexAtThisValue, fp);
                            nFeaturesToSort +=
                                nFeaturesPrevPage - nFirstIndexAtThisValue;
                        }
                        else if (j == anPagesAtThisValue.size() - 1 &&
                                 anPagesAtThisValue[j] == nThisPage)
                        {
                            bRewritePage = TRUE;
                            memcpy(&anValues[nFeaturesToSort], abyBuffer + 12,
                                   nFeaturesToSortLastPage * 4);
                            nFeaturesToSort += nFeaturesToSortLastPage;
                        }
                        else
                        {
                            VSIFSeekL(fp,
                                      (anPagesAtThisValue[j] - 1) * 4096 + 12,
                                      SEEK_SET);
                            VSIFReadL(&anValues[nFeaturesToSort], 4,
                                      nFeaturesPrevPage, fp);
                            nFeaturesToSort += nFeaturesPrevPage;
                        }
                    }

                    qsort(&anValues[0], nFeaturesToSort, 4, FGdbLayerSortATX);

                    nFeaturesToSort = 0;
                    for (size_t j = 0; j < anPagesAtThisValue.size(); j++)
                    {
                        int nFeaturesPrevPage;
                        VSIFSeekL(fp, (anPagesAtThisValue[j] - 1) * 4096 + 4,
                                  SEEK_SET);
                        VSIFReadL(&nFeaturesPrevPage, 1, 4, fp);
                        CPL_LSBPTR32(&nFeaturesPrevPage);
                        if (j == 0)
                        {
                            VSIFSeekL(fp,
                                      (anPagesAtThisValue[j] - 1) * 4096 + 12 +
                                          4 * nFirstIndexAtThisValue,
                                      SEEK_SET);
                            VSIFWriteL(
                                &anValues[nFeaturesToSort], 4,
                                nFeaturesPrevPage - nFirstIndexAtThisValue, fp);
                            nFeaturesToSort +=
                                nFeaturesPrevPage - nFirstIndexAtThisValue;
                        }
                        else if (j == anPagesAtThisValue.size() - 1 &&
                                 anPagesAtThisValue[j] == nThisPage)
                        {
                            memcpy(abyBuffer + 12, &anValues[nFeaturesToSort],
                                   nFeaturesToSortLastPage * 4);
                            nFeaturesToSort += nFeaturesToSortLastPage;
                        }
                        else
                        {
                            VSIFSeekL(fp,
                                      (anPagesAtThisValue[j] - 1) * 4096 + 12,
                                      SEEK_SET);
                            VSIFWriteL(&anValues[nFeaturesToSort], 4,
                                       nFeaturesPrevPage, fp);
                            nFeaturesToSort += nFeaturesPrevPage;
                        }
                    }
                }
            }

            if (bNewVal)
            {
                nFirstIndexAtThisValue = i;
                anPagesAtThisValue.clear();
                anPagesAtThisValue.push_back(nThisPage);

                memcpy(pabyLastIndexedValue,
                       abyBuffer + nOffsetFirstValInPage +
                           i * nSizeIndexedValue,
                       nSizeIndexedValue);
                bSortThisValue = FALSE;
            }
            else if (i == 0)
            {
                if (anPagesAtThisValue.size() > 100000)
                {
                    bInvalidateIndex = TRUE;
                    return FALSE;
                }
                else
                {
                    anPagesAtThisValue.push_back(nThisPage);
                }
            }

            if (nOGRFID)
                bSortThisValue = TRUE;

            bIndexedValueIsValid = TRUE;
        }

        if (bRewritePage)
        {
            VSIFSeekL(fp, (nThisPage - 1) * 4096, SEEK_SET);
            if (VSIFWriteL(abyBuffer, 1, 4096, fp) != 4096)
                return FALSE;
        }

        nLastPageVisited = nThisPage;

        return TRUE;
    }
    else
    {
        /* This page references other pages */
        if (VSIFReadL(abyBuffer, 1, 4096, fp) != 4096)
            return FALSE;
        int nSubPages;
        memcpy(&nSubPages, abyBuffer + 4, 4);
        CPL_LSBPTR32(&nSubPages);
        nSubPages++;
        if (nSubPages > (4096 - 8) / 4)
            return FALSE;
        for (int i = 0; i < nSubPages; i++)
        {
            int nSubPageID;
            memcpy(&nSubPageID, abyBuffer + 8 + 4 * i, 4);
            CPL_LSBPTR32(&nSubPageID);
            if (nSubPageID < 1)
                return FALSE;
            if (!EditATXOrSPX(fp, nSubPageID, nLastPageVisited, nDepth - 1,
                              nSizeIndexedValue, pabyLastIndexedValue,
                              bIndexedValueIsValid, nFirstIndexAtThisValue,
                              anPagesAtThisValue, bSortThisValue,
                              bInvalidateIndex))
            {
                return FALSE;
            }
        }

        return TRUE;
    }
}

/************************************************************************/
/*                              GetInt32()                              */
/************************************************************************/

static GInt32 GetInt32(const GByte *pBaseAddr, int iOffset)
{
    GInt32 nVal;
    memcpy(&nVal, pBaseAddr + sizeof(nVal) * iOffset, sizeof(nVal));
    CPL_LSBPTR32(&nVal);
    return nVal;
}

/************************************************************************/
/*                     UpdateNextOGRFIDAndFGDBFID()                     */
/************************************************************************/

static CPL_INLINE void UpdateNextOGRFIDAndFGDBFID(
    int i, std::map<int, int> &oMapOGRFIDToFGDBFID,
    std::map<int, int>::iterator &oIterO2F, int &nNextOGRFID,
    std::map<int, int> &oMapFGDBFIDToOGRFID,
    std::map<int, int>::iterator &oIterF2O, int &nNextFGDBFID)
{
    while (nNextOGRFID > 0 && i > nNextOGRFID)
    {
        ++oIterO2F;
        if (oIterO2F == oMapOGRFIDToFGDBFID.end())
            nNextOGRFID = -1;
        else
            nNextOGRFID = oIterO2F->first;
    }

    while (nNextFGDBFID > 0 && i > nNextFGDBFID)
    {
        ++oIterF2O;
        if (oIterF2O == oMapFGDBFIDToOGRFID.end())
            nNextFGDBFID = -1;
        else
            nNextFGDBFID = oIterF2O->first;
    }
}

/************************************************************************/
/*                          EditGDBTablX()                              */
/************************************************************************/

#define TEST_BIT(ar, bit) (ar[(bit) / 8] & (1 << ((bit) % 8)))
#define SET_BIT(ar, bit) ar[(bit) / 8] |= (1 << ((bit) % 8))
#define BIT_ARRAY_SIZE_IN_BYTES(bitsize) (((bitsize) + 7) / 8)

/* See https://github.com/rouault/dump_gdbtable/wiki/FGDB-Spec */
int FGdbLayer::EditGDBTablX(const CPLString &osGDBTablX,
                            const CPLString &osNewGDBTablX)
{
    VSILFILE *fp = VSIFOpenL(osGDBTablX, "rb");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Cannot open %s", osGDBTablX.c_str());
        return FALSE;
    }
    VSILFILE *fpNew = VSIFOpenL(osNewGDBTablX, "wb");
    if (fpNew == nullptr)
    {
        CPLError(CE_Failure, CPLE_FileIO, "Cannot create %s",
                 osNewGDBTablX.c_str());
        VSIFCloseL(fp);
        return FALSE;
    }
    GByte abyBuffer[16];
    VSIFReadL(abyBuffer, 1, 16, fp);
    int n1024Blocks = GetInt32(abyBuffer, 1);
    int nInMaxFID = GetInt32(abyBuffer, 2);
#ifdef DEBUG
    const int nInMaxFIDOri = nInMaxFID;
#endif
    int nRecordSize = GetInt32(abyBuffer, 3);
    CPLAssert(nRecordSize >= 4 && nRecordSize <= 6);

    std::map<int, int>::iterator oIterO2F = m_oMapOGRFIDToFGDBFID.begin();
    int nMaxOGRFID = 0;
    for (; oIterO2F != m_oMapOGRFIDToFGDBFID.end(); ++oIterO2F)
        nMaxOGRFID = oIterO2F->first;
    // printf("nInMaxFID = %d\n", nInMaxFID);
    // printf("nMaxOGRFID = %d\n", nMaxOGRFID);
    int nOutMaxFID = MAX(nInMaxFID, nMaxOGRFID);

    // Optimization: If the feature ids at the end of the file all map to a OGR
    // fid then they don't need to be included in the final file
    for (int i = nInMaxFID; i > nMaxOGRFID; i--)
    {
        if (m_oMapFGDBFIDToOGRFID.find(i) != m_oMapFGDBFIDToOGRFID.end())
        {
            nOutMaxFID--;
            nInMaxFID--;
        }
        else
            break;
    }

    // printf("nInMaxFID = %d\n", nInMaxFID);
    // printf("nOutMaxFID = %d\n", nOutMaxFID);

    int n1024BlocksOut = (int)(((GIntBig)nOutMaxFID + 1023) / 1024);
    int nTmp;

    nTmp = CPL_LSBWORD32(n1024BlocksOut);
    memcpy(abyBuffer + 4, &nTmp, 4);

    nTmp = CPL_LSBWORD32(nOutMaxFID);
    memcpy(abyBuffer + 8, &nTmp, 4);
    VSIFWriteL(abyBuffer, 1, 16, fpNew);

    VSIFSeekL(fp, 1024 * n1024Blocks * nRecordSize, SEEK_CUR);
    VSIFReadL(abyBuffer, 1, 16, fp);
    int nBitmapInt32Words = GetInt32(abyBuffer, 0);
    int n1024BlocksTotal = GetInt32(abyBuffer, 1);
    CPLAssert(n1024BlocksTotal == (int)(((GIntBig)nInMaxFIDOri + 1023) / 1024));
    GByte *pabyBlockMap = nullptr;
    if (nBitmapInt32Words != 0)
    {
        int nSizeInBytes = BIT_ARRAY_SIZE_IN_BYTES(n1024BlocksTotal);
        pabyBlockMap = (GByte *)CPLMalloc(nSizeInBytes);
        VSIFReadL(pabyBlockMap, nSizeInBytes, 1, fp);
    }
    int nSizeInBytesOut = BIT_ARRAY_SIZE_IN_BYTES(n1024BlocksOut);
    /* Round to the next multiple of 128 bytes (32 int4 words) */
    nSizeInBytesOut = ((nSizeInBytesOut + 127) / 128) * 128;
    GByte *pabyBlockMapOut = (GByte *)VSI_CALLOC_VERBOSE(1, nSizeInBytesOut);
    GByte *pabyPage = (GByte *)VSI_MALLOC_VERBOSE(1024 * nRecordSize);
    if (pabyBlockMapOut == nullptr || pabyPage == nullptr)
    {
        VSIFree(pabyBlockMapOut);
        VSIFree(pabyPage);
        VSIFCloseL(fp);
        return FALSE;
    }
    GByte abyEmptyOffset[6];
    memset(abyEmptyOffset, 0, 6);
    int nNonEmptyPages = 0;
    int nOffsetInPage = 0, nLastWrittenOffset = 0;
    int bDisableSparsePages =
        CPLTestBool(CPLGetConfigOption("FILEGDB_DISABLE_SPARSE_PAGES", "NO"));

    oIterO2F = m_oMapOGRFIDToFGDBFID.begin();
    int nNextOGRFID = oIterO2F->first;
    std::map<int, int>::iterator oIterF2O = m_oMapFGDBFIDToOGRFID.begin();
    int nNextFGDBFID = oIterF2O->first;

    int nCountBlocksBeforeIBlockIdx = 0;
    int nCountBlocksBeforeIBlockValue = 0;

    int bRet = TRUE;
    int i = 0;
    for (unsigned iUnsigned = 1; iUnsigned <= static_cast<unsigned>(nOutMaxFID);
         iUnsigned = static_cast<unsigned>(i) + 1, nOffsetInPage += nRecordSize)
    {
        i = static_cast<int>(iUnsigned);
        if (nOffsetInPage == 1024 * nRecordSize)
        {
            if (nLastWrittenOffset > 0 || bDisableSparsePages)
            {
                SET_BIT(pabyBlockMapOut, (i - 2) / 1024);
                nNonEmptyPages++;
                if (nLastWrittenOffset < nOffsetInPage)
                    memset(pabyPage + nLastWrittenOffset, 0,
                           nOffsetInPage - nLastWrittenOffset);
                if (VSIFWriteL(pabyPage, 1024 * nRecordSize, 1, fpNew) != 1)
                {
                    bRet = FALSE;
                    goto end;
                }
            }
            nOffsetInPage = 0;
            nLastWrittenOffset = 0;

            // A few optimizations :
            if (!bDisableSparsePages && i > nInMaxFID && nNextOGRFID > 0 &&
                i < nNextOGRFID - 1024)
            {
                // If we created a OGR FID far away from the latest FGDB FID
                // then skip to it
                i = ((nNextOGRFID - 1) / 1024) * 1024 + 1;
            }
            // coverity[negative_shift]
            else if (!bDisableSparsePages && pabyBlockMap != nullptr &&
                     i <= nInMaxFID &&
                     TEST_BIT(pabyBlockMap, (i - 1) / 1024) == 0)
            {
                // Skip empty pages
                UpdateNextOGRFIDAndFGDBFID(i, m_oMapOGRFIDToFGDBFID, oIterO2F,
                                           nNextOGRFID, m_oMapFGDBFIDToOGRFID,
                                           oIterF2O, nNextFGDBFID);
                if ((nNextOGRFID < 0 || i < nNextOGRFID - 1024) &&
                    (nNextFGDBFID < 0 || i < nNextFGDBFID - 1024))
                {
                    if (i > INT_MAX - 1024)
                        break;
                    i += 1023;
                    nOffsetInPage += 1023 * nRecordSize;
                    continue;
                }
            }
        }

        UpdateNextOGRFIDAndFGDBFID(i, m_oMapOGRFIDToFGDBFID, oIterO2F,
                                   nNextOGRFID, m_oMapFGDBFIDToOGRFID, oIterF2O,
                                   nNextFGDBFID);

        int nSrcFID;
        if (i == nNextOGRFID)
        {
            // This FID matches a user defined OGR FID, then find the
            // corresponding FGDB record
            nSrcFID = oIterO2F->second;
            // printf("(1) i = %d, nSrcFID = %d\n", i, nSrcFID);
        }
        else if (i == nNextFGDBFID || i > nInMaxFID)
        {
            // This record is a temporary one (will be moved to a user-define
            // FID) or we are out of the validity zone of input records
            // printf("(2) i = %d, nNextFGDBFID = %d, nInMaxFID = %d\n", i,
            // nNextFGDBFID, nInMaxFID);
            continue;
        }
        else
        {
            // Regular record, not overloaded by user defined FID
            nSrcFID = i;
            // printf("(3) i = %d, nSrcFID = %d\n", i, nSrcFID);
        }

        if (pabyBlockMap != nullptr)
        {
            int iBlock = (nSrcFID - 1) / 1024;

            // Check if the block is not empty
            // coverity[negative_shift]
            if (TEST_BIT(pabyBlockMap, iBlock))
            {
                int nCountBlocksBefore;
                if (iBlock >= nCountBlocksBeforeIBlockIdx)
                {
                    nCountBlocksBefore = nCountBlocksBeforeIBlockValue;
                    for (int j = nCountBlocksBeforeIBlockIdx; j < iBlock; j++)
                    {
                        // coverity[negative_shift]
                        nCountBlocksBefore += TEST_BIT(pabyBlockMap, j) != 0;
                    }
                }
                else
                {
                    nCountBlocksBefore = 0;
                    for (int j = 0; j < iBlock; j++)
                    {
                        // coverity[negative_shift]
                        nCountBlocksBefore += TEST_BIT(pabyBlockMap, j) != 0;
                    }
                }
                nCountBlocksBeforeIBlockIdx = iBlock;
                nCountBlocksBeforeIBlockValue = nCountBlocksBefore;
                int iCorrectedRow =
                    nCountBlocksBefore * 1024 + ((nSrcFID - 1) % 1024);
                VSIFSeekL(fp, 16 + nRecordSize * iCorrectedRow, SEEK_SET);
                VSIFReadL(abyBuffer, 1, nRecordSize, fp);
                if (memcmp(abyBuffer, abyEmptyOffset, nRecordSize) != 0)
                {
                    if (nLastWrittenOffset < nOffsetInPage)
                        memset(pabyPage + nLastWrittenOffset, 0,
                               nOffsetInPage - nLastWrittenOffset);
                    memcpy(pabyPage + nOffsetInPage, abyBuffer, nRecordSize);
                    nLastWrittenOffset = nOffsetInPage + nRecordSize;
                }
            }
        }
        else
        {
            VSIFSeekL(fp, 16 + nRecordSize * (nSrcFID - 1), SEEK_SET);
            VSIFReadL(abyBuffer, 1, nRecordSize, fp);
            if (memcmp(abyBuffer, abyEmptyOffset, nRecordSize) != 0)
            {
                if (nLastWrittenOffset < nOffsetInPage)
                    memset(pabyPage + nLastWrittenOffset, 0,
                           nOffsetInPage - nLastWrittenOffset);
                memcpy(pabyPage + nOffsetInPage, abyBuffer, nRecordSize);
                nLastWrittenOffset = nOffsetInPage + nRecordSize;
            }
        }
    }
    // printf("nLastWrittenOffset = %d\n", nLastWrittenOffset);
    if (nLastWrittenOffset > 0 || bDisableSparsePages)
    {
        assert(nOutMaxFID >= 1);
        SET_BIT(pabyBlockMapOut, (nOutMaxFID - 1) / 1024);
        nNonEmptyPages++;
        if (nLastWrittenOffset < 1024 * nRecordSize)
            memset(pabyPage + nLastWrittenOffset, 0,
                   1024 * nRecordSize - nLastWrittenOffset);
        if (VSIFWriteL(pabyPage, 1024 * nRecordSize, 1, fpNew) != 1)
        {
            bRet = FALSE;
            goto end;
        }
    }

    memset(abyBuffer, 0, 16);

    /* Number of total blocks, including omitted ones */
    nTmp = CPL_LSBWORD32(n1024BlocksOut);
    memcpy(abyBuffer + 4, &nTmp, 4);

    nTmp = CPL_LSBWORD32(nNonEmptyPages);
    memcpy(abyBuffer + 8, &nTmp, 4);

    if (nNonEmptyPages < n1024BlocksOut)
    {
        /* Number of int4 words for the bitmap (rounded to the next multiple of
         * 32) */
        nTmp = CPL_LSBWORD32(nSizeInBytesOut / 4);
        memcpy(abyBuffer + 0, &nTmp, 4);

        /* Number of int4 words in the bitmap where there's at least a non-zero
         * bit */
        /* Seems to be unused */
        nTmp = CPL_LSBWORD32(((nOutMaxFID - 1) / 1024 + 31) / 32);
        memcpy(abyBuffer + 12, &nTmp, 4);
    }

    if (VSIFWriteL(abyBuffer, 1, 16, fpNew) != 16)
    {
        bRet = FALSE;
        goto end;
    }

    if (nNonEmptyPages < n1024BlocksOut)
    {
        VSIFWriteL(pabyBlockMapOut, 1, nSizeInBytesOut, fpNew);

        VSIFSeekL(fpNew, 4, SEEK_SET);
        nTmp = CPL_LSBWORD32(nNonEmptyPages);
        VSIFWriteL(&nTmp, 1, 4, fpNew);
    }

end:
    CPLFree(pabyBlockMap);
    CPLFree(pabyBlockMapOut);
    CPLFree(pabyPage);
    VSIFCloseL(fpNew);
    VSIFCloseL(fp);

    return bRet;
}

#ifdef EXTENT_WORKAROUND

/************************************************************************/
/*                     UpdateRowWithGeometry()                          */
/************************************************************************/

bool FGdbLayer::UpdateRowWithGeometry(Row &row, OGRGeometry *poGeom)
{
    ShapeBuffer shape;
    long hr;

    /* Write geometry to a buffer */
    GByte *pabyShape = nullptr;
    int nShapeSize = 0;
    if (OGRWriteToShapeBin(poGeom, &pabyShape, &nShapeSize) != OGRERR_NONE)
    {
        CPLFree(pabyShape);
        return false;
    }

    /* Copy it into a ShapeBuffer */
    if (nShapeSize > 0)
    {
        shape.Allocate(nShapeSize);
        memcpy(shape.shapeBuffer, pabyShape, nShapeSize);
        shape.inUseLength = nShapeSize;
    }

    /* Free the shape buffer */
    CPLFree(pabyShape);

    /* Write ShapeBuffer into the Row */
    hr = row.SetGeometry(shape);
    if (FAILED(hr))
    {
        return false;
    }

    /* Update row */
    hr = m_pTable->Update(row);
    if (FAILED(hr))
    {
        return false;
    }

    return true;
}

/************************************************************************/
/*                    WorkAroundExtentProblem()                         */
/*                                                                      */
/* Work-around problem with FileGDB API 1.1 on Linux 64bit. See #4455   */
/************************************************************************/

void FGdbLayer::WorkAroundExtentProblem()
{
    if (!m_bLayerJustCreated || !m_bLayerEnvelopeValid)
        return;
    m_bLayerJustCreated = FALSE;

    OGREnvelope sEnvelope;
    if (FGdbLayer::GetExtent(&sEnvelope, TRUE) != OGRERR_NONE)
        return;

    /* The characteristic of the bug is that the reported extent */
    /* is the real extent truncated incorrectly to integer values */
    /* We work around that by temporary updating one feature with a geometry */
    /* whose coordinates are integer values but ceil'ed and floor'ed */
    /* such that they include the real layer extent. */
    if (((double)(int)sEnvelope.MinX == sEnvelope.MinX &&
         (double)(int)sEnvelope.MinY == sEnvelope.MinY &&
         (double)(int)sEnvelope.MaxX == sEnvelope.MaxX &&
         (double)(int)sEnvelope.MaxY == sEnvelope.MaxY) &&
        (fabs(sEnvelope.MinX - sLayerEnvelope.MinX) > 1e-5 ||
         fabs(sEnvelope.MinY - sLayerEnvelope.MinY) > 1e-5 ||
         fabs(sEnvelope.MaxX - sLayerEnvelope.MaxX) > 1e-5 ||
         fabs(sEnvelope.MaxY - sLayerEnvelope.MaxY) > 1e-5))
    {
        long hr;
        Row row;
        EnumRows enumRows;

        if (FAILED(hr = m_pTable->Search(StringToWString("*"),
                                         StringToWString(""), true, enumRows)))
            return;

        if (FAILED(hr = enumRows.Next(row)))
            return;

        if (hr != S_OK)
            return;

        /* Backup original shape buffer */
        ShapeBuffer originalGdbGeometry;
        if (FAILED(hr = row.GetGeometry(originalGdbGeometry)))
            return;

        OGRGeometry *pOGRGeo = nullptr;
        if ((!GDBGeometryToOGRGeometry(m_forceMulti, &originalGdbGeometry,
                                       m_pSRS, &pOGRGeo)) ||
            pOGRGeo == nullptr)
        {
            delete pOGRGeo;
            return;
        }

        OGRwkbGeometryType eType = wkbFlatten(pOGRGeo->getGeometryType());

        delete pOGRGeo;
        pOGRGeo = nullptr;

        OGRPoint oP1(floor(sLayerEnvelope.MinX), floor(sLayerEnvelope.MinY));
        OGRPoint oP2(ceil(sLayerEnvelope.MaxX), ceil(sLayerEnvelope.MaxY));

        OGRLinearRing oLR;
        oLR.addPoint(&oP1);
        oLR.addPoint(&oP2);
        oLR.addPoint(&oP1);

        if (eType == wkbPoint)
        {
            UpdateRowWithGeometry(row, &oP1);
            UpdateRowWithGeometry(row, &oP2);
        }
        else if (eType == wkbLineString)
        {
            UpdateRowWithGeometry(row, &oLR);
        }
        else if (eType == wkbPolygon)
        {
            OGRPolygon oPoly;
            oPoly.addRing(&oLR);

            UpdateRowWithGeometry(row, &oPoly);
        }
        else if (eType == wkbMultiPoint)
        {
            OGRMultiPoint oColl;
            oColl.addGeometry(&oP1);
            oColl.addGeometry(&oP2);

            UpdateRowWithGeometry(row, &oColl);
        }
        else if (eType == wkbMultiLineString)
        {
            OGRMultiLineString oColl;
            oColl.addGeometry(&oLR);

            UpdateRowWithGeometry(row, &oColl);
        }
        else if (eType == wkbMultiPolygon)
        {
            OGRMultiPolygon oColl;
            OGRPolygon oPoly;
            oPoly.addRing(&oLR);
            oColl.addGeometry(&oPoly);

            UpdateRowWithGeometry(row, &oColl);
        }
        else
            return;

        /* Restore original ShapeBuffer */
        hr = row.SetGeometry(originalGdbGeometry);
        if (FAILED(hr))
            return;

        /* Update Row */
        hr = m_pTable->Update(row);
        if (FAILED(hr))
            return;

        CPLDebug("FGDB",
                 "Workaround extent problem with Linux 64bit FGDB SDK 1.1");
    }
}
#endif  // EXTENT_WORKAROUND

/************************************************************************/
/*                            ICreateFeature()                           */
/* Create an FGDB Row and populate it from an OGRFeature.               */
/*                                                                      */
/************************************************************************/

OGRErr FGdbLayer::ICreateFeature(OGRFeature *poFeature)
{
    Row fgdb_row;
    fgdbError hr;

    if (!m_pDS->GetUpdate() || m_pTable == nullptr)
        return OGRERR_FAILURE;

    GIntBig nFID = poFeature->GetFID();
    if (nFID < -1 || nFID == 0 || !CPL_INT64_FITS_ON_INT32(nFID))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Only 32 bit positive integers FID supported by FileGDB");
        return OGRERR_FAILURE;
    }

    if (nFID > 0)
    {
        if (m_pDS->GetOpenFileGDBDrv() == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot call CreateFeature() with a set FID when "
                     "OpenFileGDB driver not available");
            return OGRERR_FAILURE;
        }

        if (m_pDS->HasSelectLayers())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot call CreateFeature() with a set FID when a layer "
                     "resulting from ExecuteSQL() is still opened");
            return OGRERR_FAILURE;
        }

        if (m_pDS->GetConnection()->GetRefCount() > 1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot call CreateFeature() with a set FID when a "
                     "dataset is opened more than once");
            return OGRERR_FAILURE;
        }

        if (m_oMapOGRFIDToFGDBFID.find((int)poFeature->GetFID()) !=
            m_oMapOGRFIDToFGDBFID.end())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "A feature with same FID already exists");
            return OGRERR_FAILURE;
        }

        if (m_oMapFGDBFIDToOGRFID.find((int)poFeature->GetFID()) ==
            m_oMapFGDBFIDToOGRFID.end())
        {
            EnumRows enumRows;
            Row row;
            if (GetRow(enumRows, row, (int)poFeature->GetFID()) == OGRERR_NONE)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "A feature with same FID already exists");
                return OGRERR_FAILURE;
            }
        }

        if ((int)m_oMapOGRFIDToFGDBFID.size() == m_nResyncThreshold)
            ResyncIDs();
    }

    if (m_bSymlinkFlag && !CreateRealCopy())
        return OGRERR_FAILURE;

    if (m_bBulkLoadAllowed < 0)
        m_bBulkLoadAllowed =
            CPLTestBool(CPLGetConfigOption("FGDB_BULK_LOAD", "NO"));

    if (m_bBulkLoadAllowed && !m_bBulkLoadInProgress)
        StartBulkLoad();

    hr = m_pTable->CreateRowObject(fgdb_row);

    /* Check the status of the Row create */
    if (FAILED(hr))
    {
        GDBErr(hr, "Failed at creating Row in CreateFeature.");
        return OGRERR_FAILURE;
    }

    /* As we have issues with fixed values for dates, or CURRENT_xxxx isn't */
    /* handled anyway, let's fill ourselves all unset fields with their default
     */
    poFeature->FillUnsetWithDefault(FALSE, nullptr);

    /* Populate the row with the feature content */
    if (PopulateRowWithFeature(fgdb_row, poFeature) != OGRERR_NONE)
        return OGRERR_FAILURE;

    /* Cannot write to FID field - it is managed by GDB*/
    // std::wstring wfield_name = StringToWString(m_strOIDFieldName);
    // hr = fgdb_row.SetInteger(wfield_name, poFeature->GetFID());

    /* Write the row to the table */
    hr = m_pTable->Insert(fgdb_row);
    if (FAILED(hr))
    {
        GDBErr(hr, "Failed at writing Row to Table in CreateFeature.");
        return OGRERR_FAILURE;
    }

    int32 oid = -1;
    if (!FAILED(hr = fgdb_row.GetOID(oid)))
    {
        if (poFeature->GetFID() < 0)
        {
            // Avoid colliding with a user set FID
            while (m_oMapOGRFIDToFGDBFID.find(oid) !=
                   m_oMapOGRFIDToFGDBFID.end())
            {
                EndBulkLoad();

                CPLDebug("FGDB", "Collision with user set FID %d", oid);
                if (FAILED(hr = m_pTable->Delete(fgdb_row)))
                {
                    GDBErr(hr, "Failed deleting row ");
                    return OGRERR_FAILURE;
                }
                hr = m_pTable->Insert(fgdb_row);
                if (FAILED(hr))
                {
                    GDBErr(hr,
                           "Failed at writing Row to Table in CreateFeature.");
                    return OGRERR_FAILURE;
                }
                if (FAILED(hr = fgdb_row.GetOID(oid)))
                {
                    return OGRERR_FAILURE;
                }
            }
            poFeature->SetFID(oid);
        }
        else if ((int)poFeature->GetFID() != oid)
        {
            m_pDS->GetConnection()->SetFIDHackInProgress(TRUE);
            m_oMapOGRFIDToFGDBFID[(int)poFeature->GetFID()] = oid;
            m_oMapFGDBFIDToOGRFID[oid] = (int)poFeature->GetFID();
        }
    }

#ifdef EXTENT_WORKAROUND
    /* For WorkAroundExtentProblem() needs */
    OGRGeometry *poGeom = poFeature->GetGeometryRef();
    if (m_bLayerJustCreated && poGeom != nullptr && !poGeom->IsEmpty())
    {
        OGREnvelope sFeatureGeomEnvelope;
        poGeom->getEnvelope(&sFeatureGeomEnvelope);
        if (!m_bLayerEnvelopeValid)
        {
            sLayerEnvelope = sFeatureGeomEnvelope;
            m_bLayerEnvelopeValid = true;
        }
        else
        {
            sLayerEnvelope.Merge(sFeatureGeomEnvelope);
        }
    }
#endif

    return OGRERR_NONE;
}

/************************************************************************/
/*                    PopulateRowWithFeature()                          */
/*                                                                      */
/************************************************************************/

OGRErr FGdbLayer::PopulateRowWithFeature(Row &fgdb_row, OGRFeature *poFeature)
{
    ShapeBuffer shape;
    fgdbError hr;

    OGRFeatureDefn *poFeatureDefn = m_pFeatureDefn;
    int nFieldCount = poFeatureDefn->GetFieldCount();

    /* Copy the OGR visible fields (everything except geometry and FID) */
    int nCountBinaryField = 0;
    for (int i = 0; i < nFieldCount; i++)
    {
        std::string field_name = poFeatureDefn->GetFieldDefn(i)->GetNameRef();
        std::wstring wfield_name = StringToWString(field_name);
        const std::string &strFieldType = m_vOGRFieldToESRIFieldType[i];

        /* Set empty fields to NULL */
        if (!poFeature->IsFieldSetAndNotNull(i))
        {
            if (strFieldType == "esriFieldTypeGlobalID")
                continue;

            if (FAILED(hr = fgdb_row.SetNull(wfield_name)))
            {
                GDBErr(hr, "Failed setting field to NULL.");
                return OGRERR_FAILURE;
            }
            continue;
        }

        /* Set the information using the appropriate FGDB function */
        int nOGRFieldType = poFeatureDefn->GetFieldDefn(i)->GetType();

        if (nOGRFieldType == OFTInteger)
        {
            int fldvalue = poFeature->GetFieldAsInteger(i);
            if (strFieldType == "esriFieldTypeInteger")
                hr = fgdb_row.SetInteger(wfield_name, fldvalue);
            else
            {
                if (fldvalue < -32768 || fldvalue > 32767)
                {
                    static int bHasWarned = FALSE;
                    if (!bHasWarned)
                    {
                        bHasWarned = TRUE;
                        CPLError(CE_Warning, CPLE_NotSupported,
                                 "Value %d for field %s does not fit into a "
                                 "short and will be clamped. "
                                 "This warning will not be emitted any more",
                                 fldvalue, field_name.c_str());
                    }
                    if (fldvalue < -32768)
                        fldvalue = -32768;
                    else
                        fldvalue = 32767;
                }
                hr = fgdb_row.SetShort(wfield_name, (short)fldvalue);
            }
        }
        else if (nOGRFieldType == OFTReal || nOGRFieldType == OFTInteger64)
        {
            /* Doubles (we don't handle FGDB Floats) */
            double fldvalue = poFeature->GetFieldAsDouble(i);
            if (strFieldType == "esriFieldTypeDouble")
                hr = fgdb_row.SetDouble(wfield_name, fldvalue);
            else
                hr = fgdb_row.SetFloat(wfield_name, (float)fldvalue);
        }
        else if (nOGRFieldType == OFTString)
        {
            /* Strings we convert to wstring */
            std::string fldvalue = poFeature->GetFieldAsString(i);
            if (strFieldType == "esriFieldTypeString")
            {
                std::wstring wfldvalue = StringToWString(fldvalue);
                hr = fgdb_row.SetString(wfield_name, wfldvalue);
            }
            // Apparently, esriFieldTypeGlobalID can not be set, but is
            // initialized by the FileGDB SDK itself.
            else if( strFieldType == "esriFieldTypeGUID" /*||
                     strFieldType == "esriFieldTypeGlobalID" */ )
            {
                Guid guid;
                std::wstring wfldvalue = StringToWString(fldvalue);
                if (FAILED(hr = guid.FromString(wfldvalue)))
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot parse GUID value %s for field %s.",
                             fldvalue.c_str(), field_name.c_str());
                }
                else
                {
                    hr = fgdb_row.SetGUID(wfield_name, guid);
                }
            }
            else if (strFieldType == "esriFieldTypeXML")
            {
                hr = fgdb_row.SetXML(wfield_name, fldvalue);
            }
            else
                hr = 0;
        }
        else if (nOGRFieldType == OFTDateTime || nOGRFieldType == OFTDate)
        {
            /* Dates we need to coerce a little */
            struct tm val;
            poFeature->GetFieldAsDateTime(
                i, &(val.tm_year), &(val.tm_mon), &(val.tm_mday),
                &(val.tm_hour), &(val.tm_min), &(val.tm_sec), nullptr);
            val.tm_year -= 1900;
            val.tm_mon = val.tm_mon - 1; /* OGR months go 1-12, FGDB go 0-11 */
            hr = fgdb_row.SetDate(wfield_name, val);
        }
        else if (nOGRFieldType == OFTBinary)
        {
            /* Binary data */
            int bytesize;
            GByte *bytes = poFeature->GetFieldAsBinary(i, &bytesize);
            if (bytesize)
            {
                /* This is annoying but SetBinary() doesn't keep the binary */
                /* content. The ByteArray object must still be alive at */
                /* the time Insert() is called */
                m_apoByteArrays[nCountBinaryField]->Allocate(bytesize);
                memcpy(m_apoByteArrays[nCountBinaryField]->byteArray, bytes,
                       bytesize);
                m_apoByteArrays[nCountBinaryField]->inUseLength = bytesize;
                hr = fgdb_row.SetBinary(wfield_name,
                                        *(m_apoByteArrays[nCountBinaryField]));
            }
            else
            {
                hr = fgdb_row.SetNull(wfield_name);
            }
            nCountBinaryField++;
        }
        else
        {
            /* We can't handle this type */
            CPLError(CE_Failure, CPLE_AppDefined,
                     "FGDB driver does not support OGR type.");
            return OGRERR_FAILURE;
        }

        if (FAILED(hr))
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Cannot set value for field %s", field_name.c_str());
        }
    }

    const auto eFlatLayerGeomType = wkbFlatten(m_pFeatureDefn->GetGeomType());
    if (eFlatLayerGeomType != wkbNone)
    {
        /* Done with attribute fields, now do geometry */
        OGRGeometry *poGeom = poFeature->GetGeometryRef();

        if (poGeom == nullptr || poGeom->IsEmpty())
        {
            /* EMPTY geometries should be treated as NULL, see #4832 */
            hr = fgdb_row.SetNull(StringToWString(m_strShapeFieldName));
            if (FAILED(hr))
            {
                GDBErr(hr, "Failed at writing EMPTY Geometry to Row in "
                           "CreateFeature.");
                return OGRERR_FAILURE;
            }
        }
        else
        {
            /* Write geometry to a buffer */
            GByte *pabyShape = nullptr;
            int nShapeSize = 0;
            OGRErr err;

            const OGRwkbGeometryType eType =
                wkbFlatten(poGeom->getGeometryType());
            if (m_bCreateMultipatch &&
                (eType == wkbMultiPolygon || eType == wkbMultiSurface ||
                 eType == wkbTIN || eType == wkbPolyhedralSurface ||
                 eType == wkbGeometryCollection))
            {
                err = OGRWriteMultiPatchToShapeBin(poGeom, &pabyShape,
                                                   &nShapeSize);
                if (err == OGRERR_UNSUPPORTED_OPERATION)
                    err = OGRWriteToShapeBin(poGeom, &pabyShape, &nShapeSize);
            }
            else
            {
                if (((eFlatLayerGeomType == wkbLineString ||
                      eFlatLayerGeomType == wkbMultiLineString) &&
                     (eType != wkbLineString && eType != wkbMultiLineString)) ||
                    ((eFlatLayerGeomType == wkbPolygon ||
                      eFlatLayerGeomType == wkbMultiPolygon) &&
                     (eType != wkbPolygon && eType != wkbMultiPolygon)) ||
                    ((eFlatLayerGeomType == wkbPoint ||
                      eFlatLayerGeomType == wkbMultiPoint) &&
                     eType != eFlatLayerGeomType))
                {
                    // Otherwise crash in the SDK...
                    CPLError(
                        CE_Failure, CPLE_NotSupported,
                        "Geometry type %s not supported in layer of type %s",
                        OGRToOGCGeomType(eType),
                        OGRToOGCGeomType(eFlatLayerGeomType));
                    return OGRERR_FAILURE;
                }

                err = OGRWriteToShapeBin(poGeom, &pabyShape, &nShapeSize);
            }
            if (err != OGRERR_NONE)
            {
                CPLFree(pabyShape);
                return err;
            }

            /* Copy it into a ShapeBuffer */
            if (nShapeSize > 0)
            {
                shape.Allocate(nShapeSize);
                memcpy(shape.shapeBuffer, pabyShape, nShapeSize);
                shape.inUseLength = nShapeSize;
            }

            /* Free the shape buffer */
            CPLFree(pabyShape);

            /* Write ShapeBuffer into the Row */
            hr = fgdb_row.SetGeometry(shape);
            if (FAILED(hr))
            {
                GDBErr(hr,
                       "Failed at writing Geometry to Row in CreateFeature.");
                return OGRERR_FAILURE;
            }
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                             GetRow()                                 */
/************************************************************************/

OGRErr FGdbLayer::GetRow(EnumRows &enumRows, Row &row, GIntBig nFID)
{
    long hr;
    CPLString osQuery;

    /* Querying a 64bit FID causes a runtime exception in FileGDB... */
    if (!CPL_INT64_FITS_ON_INT32(nFID))
    {
        return OGRERR_FAILURE;
    }

    osQuery.Printf("%s = " CPL_FRMT_GIB, m_strOIDFieldName.c_str(), nFID);

    if (FAILED(hr = m_pTable->Search(m_wstrSubfields,
                                     StringToWString(osQuery.c_str()), true,
                                     enumRows)))
    {
        GDBErr(hr, "Failed fetching row ");
        return OGRERR_FAILURE;
    }

    if (FAILED(hr = enumRows.Next(row)))
    {
        GDBErr(hr, "Failed fetching row ");
        return OGRERR_FAILURE;
    }

    if (hr != S_OK)
        return OGRERR_NON_EXISTING_FEATURE;  // none found - but no failure

    return OGRERR_NONE;
}

/************************************************************************/
/*                           DeleteFeature()                            */
/************************************************************************/

OGRErr FGdbLayer::DeleteFeature(GIntBig nFID)

{
    long hr;
    EnumRows enumRows;
    Row row;

    if (!m_pDS->GetUpdate() || m_pTable == nullptr)
        return OGRERR_FAILURE;
    if (!CPL_INT64_FITS_ON_INT32(nFID))
        return OGRERR_NON_EXISTING_FEATURE;

    if (m_bSymlinkFlag && !CreateRealCopy())
        return OGRERR_FAILURE;

    int nFID32 = (int)nFID;
    std::map<int, int>::iterator oIter = m_oMapOGRFIDToFGDBFID.find(nFID32);
    if (oIter != m_oMapOGRFIDToFGDBFID.end())
    {
        nFID32 = oIter->second;
        m_oMapFGDBFIDToOGRFID.erase(nFID32);
        m_oMapOGRFIDToFGDBFID.erase(oIter);
    }
    else if (m_oMapFGDBFIDToOGRFID.find(nFID32) != m_oMapFGDBFIDToOGRFID.end())
        return OGRERR_NON_EXISTING_FEATURE;

    EndBulkLoad();

    OGRErr eErr = GetRow(enumRows, row, nFID32);
    if (eErr != OGRERR_NONE)
        return eErr;

    if (FAILED(hr = m_pTable->Delete(row)))
    {
        GDBErr(hr, "Failed deleting row ");
        return OGRERR_FAILURE;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                            ISetFeature()                              */
/************************************************************************/

OGRErr FGdbLayer::ISetFeature(OGRFeature *poFeature)

{
    long hr;
    EnumRows enumRows;
    Row row;

    if (!m_pDS->GetUpdate() || m_pTable == nullptr)
        return OGRERR_FAILURE;

    GIntBig nFID64 = poFeature->GetFID();
    if (nFID64 == OGRNullFID)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "SetFeature() with unset FID fails.");
        return OGRERR_FAILURE;
    }
    if (!CPL_INT64_FITS_ON_INT32(nFID64))
        return OGRERR_NON_EXISTING_FEATURE;

    EndBulkLoad();

    if (m_bSymlinkFlag && !CreateRealCopy())
        return OGRERR_FAILURE;

    int nFID = (int)nFID64;
    std::map<int, int>::iterator oIter = m_oMapOGRFIDToFGDBFID.find(nFID);
    if (oIter != m_oMapOGRFIDToFGDBFID.end())
        nFID = oIter->second;
    else if (m_oMapFGDBFIDToOGRFID.find((int)poFeature->GetFID()) !=
             m_oMapFGDBFIDToOGRFID.end())
        return OGRERR_NON_EXISTING_FEATURE;

    OGRErr eErr = GetRow(enumRows, row, nFID);
    if (eErr != OGRERR_NONE)
        return eErr;

    /* Populate the row with the feature content */
    if (PopulateRowWithFeature(row, poFeature) != OGRERR_NONE)
        return OGRERR_FAILURE;

    if (FAILED(hr = m_pTable->Update(row)))
    {
        GDBErr(hr, "Failed updating row ");
        return OGRERR_FAILURE;
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                          CreateFieldDefn()                           */
/************************************************************************/

char *FGdbLayer::CreateFieldDefn(OGRFieldDefn &oField, int bApproxOK,
                                 std::string &fieldname_clean,
                                 std::string &gdbFieldType)
{
    std::string fieldname = oField.GetNameRef();
    // std::string fidname = std::string(GetFIDColumn());
    std::string nullable = (oField.IsNullable()) ? "true" : "false";

    /* Try to map the OGR type to an ESRI type */
    OGRFieldType fldtype = oField.GetType();
    if (!OGRToGDBFieldType(fldtype, oField.GetSubType(), &gdbFieldType))
    {
        GDBErr(-1, "Failed converting field type.");
        return nullptr;
    }

    if (oField.GetType() == OFTInteger64 && !bApproxOK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Integer64 not supported in FileGDB");
        return nullptr;
    }

    const char *pszColumnTypes =
        CSLFetchNameValue(m_papszOptions, "COLUMN_TYPES");
    if (pszColumnTypes != nullptr)
    {
        char **papszTokens = CSLTokenizeString2(pszColumnTypes, ",", 0);
        const char *pszFieldType =
            CSLFetchNameValue(papszTokens, fieldname.c_str());
        if (pszFieldType != nullptr)
        {
            OGRFieldType fldtypeCheck;
            OGRFieldSubType eSubType;
            if (GDBToOGRFieldType(pszFieldType, &fldtypeCheck, &eSubType))
            {
                if (fldtypeCheck != fldtype)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Ignoring COLUMN_TYPES=%s=%s : %s not consistent "
                             "with OGR data type",
                             fieldname.c_str(), pszFieldType, pszFieldType);
                }
                else
                    gdbFieldType = pszFieldType;
            }
            else
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Ignoring COLUMN_TYPES=%s=%s : %s not recognized",
                         fieldname.c_str(), pszFieldType, pszFieldType);
        }
        CSLDestroy(papszTokens);
    }

    if (!fieldname_clean.empty())
    {
        oField.SetName(fieldname_clean.c_str());
    }
    else
    {
        /* Clean field names */
        std::wstring wfieldname_clean =
            FGDBLaunderName(StringToWString(fieldname));

        if (m_bLaunderReservedKeywords)
            wfieldname_clean = FGDBEscapeReservedKeywords(wfieldname_clean);

        /* Truncate to 64 characters */
        constexpr size_t FIELD_NAME_MAX_SIZE = 64;
        if (wfieldname_clean.size() > FIELD_NAME_MAX_SIZE)
            wfieldname_clean.resize(FIELD_NAME_MAX_SIZE);

        /* Ensures uniqueness of field name */
        int numRenames = 1;
        while ((m_pFeatureDefn->GetFieldIndex(
                    WStringToString(wfieldname_clean).c_str()) >= 0) &&
               (numRenames < 10))
        {
            wfieldname_clean = StringToWString(
                CPLSPrintf("%s_%d",
                           WStringToString(wfieldname_clean.substr(
                                               0, FIELD_NAME_MAX_SIZE - 2))
                               .c_str(),
                           numRenames));
            numRenames++;
        }
        while ((m_pFeatureDefn->GetFieldIndex(
                    WStringToString(wfieldname_clean).c_str()) >= 0) &&
               (numRenames < 100))
        {
            wfieldname_clean = StringToWString(
                CPLSPrintf("%s_%d",
                           WStringToString(wfieldname_clean.substr(
                                               0, FIELD_NAME_MAX_SIZE - 3))
                               .c_str(),
                           numRenames));
            numRenames++;
        }

        fieldname_clean = WStringToString(wfieldname_clean);
        if (fieldname_clean != fieldname)
        {
            if (!bApproxOK ||
                (m_pFeatureDefn->GetFieldIndex(fieldname_clean.c_str()) >= 0))
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Failed to add field named '%s'", fieldname.c_str());
                return nullptr;
            }
            CPLError(CE_Warning, CPLE_NotSupported,
                     "Normalized/laundered field name: '%s' to '%s'",
                     fieldname.c_str(), fieldname_clean.c_str());

            oField.SetName(fieldname_clean.c_str());
        }
    }

    /* Then the Field definition */
    CPLXMLNode *defn_xml = CPLCreateXMLNode(nullptr, CXT_Element, "esri:Field");

    /* Add the XML attributes to the Field node */
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xsi",
                            "http://www.w3.org/2001/XMLSchema-instance");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xs",
                            "http://www.w3.org/2001/XMLSchema");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:esri",
                            "http://www.esri.com/schemas/ArcGIS/10.1");
    FGDB_CPLAddXMLAttribute(defn_xml, "xsi:type", "esri:Field");

    /* Basic field information */
    CPLCreateXMLElementAndValue(defn_xml, "Name", fieldname_clean.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "Type", gdbFieldType.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "IsNullable", nullable.c_str());

    /* Get the Width and Precision if we know them */
    int width = oField.GetWidth();
    int precision = oField.GetPrecision();
    if (width <= 0)
        GDBFieldTypeToWidthPrecision(gdbFieldType, &width, &precision);

    /* Write out the Width and Precision */
    char buf[100];
    snprintf(buf, 100, "%d", width);
    CPLCreateXMLElementAndValue(defn_xml, "Length", buf);
    snprintf(buf, 100, "%d", precision);
    CPLCreateXMLElementAndValue(defn_xml, "Precision", buf);

    /* We know nothing about Scale, so zero it out */
    CPLCreateXMLElementAndValue(defn_xml, "Scale", "0");

    const char *pszAlternativeName = oField.GetAlternativeNameRef();
    if (pszAlternativeName != nullptr && pszAlternativeName[0])
    {
        CPLCreateXMLElementAndValue(defn_xml, "AliasName", pszAlternativeName);
    }
    else if (fieldname != fieldname_clean)
    {
        /*  Attempt to preserve the original fieldname */
        CPLCreateXMLElementAndValue(defn_xml, "AliasName", fieldname.c_str());
    }

    if (oField.GetDefault() != nullptr)
    {
        const char *pszDefault = oField.GetDefault();
        /*int nYear, nMonth, nDay, nHour, nMinute;
        float fSecond;*/
        if (oField.GetType() == OFTString)
        {
            CPLString osVal = pszDefault;
            if (osVal[0] == '\'' && osVal.back() == '\'')
            {
                osVal = osVal.substr(1);
                osVal.resize(osVal.size() - 1);
                char *pszTmp = CPLUnescapeString(osVal, nullptr, CPLES_SQL);
                osVal = pszTmp;
                CPLFree(pszTmp);
            }
            CPLXMLNode *psDefaultValue =
                CPLCreateXMLElementAndValue(defn_xml, "DefaultValue", osVal);
            FGDB_CPLAddXMLAttribute(psDefaultValue, "xsi:type", "xs:string");
        }
        else if (oField.GetType() == OFTInteger &&
                 !EQUAL(gdbFieldType.c_str(), "esriFieldTypeSmallInteger") &&
                 CPLGetValueType(pszDefault) == CPL_VALUE_INTEGER)
        {
            CPLXMLNode *psDefaultValue = CPLCreateXMLElementAndValue(
                defn_xml, "DefaultValue", pszDefault);
            FGDB_CPLAddXMLAttribute(psDefaultValue, "xsi:type", "xs:int");
        }
        else if (oField.GetType() == OFTReal &&
                 !EQUAL(gdbFieldType.c_str(), "esriFieldTypeSingle") &&
                 CPLGetValueType(pszDefault) != CPL_VALUE_STRING)
        {
            CPLXMLNode *psDefaultValue = CPLCreateXMLElementAndValue(
                defn_xml, "DefaultValue", pszDefault);
            FGDB_CPLAddXMLAttribute(psDefaultValue, "xsi:type", "xs:double");
        }
        /*else if( oField.GetType() == OFTDateTime &&
                 sscanf(pszDefault, "'%d/%d/%d %d:%d:%f'", &nYear, &nMonth,
        &nDay, &nHour, &nMinute, &fSecond) == 6 )
        {
            CPLXMLNode* psDefaultValue =
                CPLCreateXMLElementAndValue(defn_xml, "DefaultValue",
                    CPLSPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                               nYear, nMonth, nDay, nHour, nMinute,
        (int)(fSecond + 0.5))); FGDB_CPLAddXMLAttribute(psDefaultValue,
        "xsi:type", "xs:dateTime");
        }*/
    }
    /* <DefaultValue xsi:type="xs:string">afternoon</DefaultValue> */

    const auto &osDomainName = oField.GetDomainName();
    if (!osDomainName.empty())
    {
        const auto poDomain = m_pDS->GetFieldDomain(osDomainName);
        if (poDomain)
        {
            std::string failureReason;
            std::string osXML =
                BuildXMLFieldDomainDef(poDomain, true, failureReason);
            if (!osXML.empty())
            {
                auto psDomain = CPLParseXMLString(osXML.c_str());
                if (psDomain)
                {
                    CPLFree(psDomain->pszValue);
                    psDomain->pszValue = CPLStrdup("Domain");
                    CPLAddXMLChild(defn_xml, psDomain);
                }
            }
        }
    }

    /* Convert our XML tree into a string for FGDB */
    char *defn_str = CPLSerializeXMLTree(defn_xml);
    CPLDebug("FGDB", "CreateField() generated XML for FGDB\n%s", defn_str);

    /* Free the XML */
    CPLDestroyXMLNode(defn_xml);

    return defn_str;
}

/************************************************************************/
/*                            CreateField()                             */
/*  Build up an FGDB XML field definition and use it to create a Field  */
/*  Update the OGRFeatureDefn to reflect the new field.                 */
/*                                                                      */
/************************************************************************/

OGRErr FGdbLayer::CreateField(OGRFieldDefn *poField, int bApproxOK)
{
    OGRFieldDefn oField(poField);
    std::string fieldname_clean;
    std::string gdbFieldType;

    if (!m_pDS->GetUpdate() || m_pTable == nullptr)
        return OGRERR_FAILURE;

    char *defn_str =
        CreateFieldDefn(oField, bApproxOK, fieldname_clean, gdbFieldType);
    if (defn_str == nullptr)
        return OGRERR_FAILURE;

    /* Add the FGDB Field to the FGDB Table. */
    fgdbError hr = m_pTable->AddField(defn_str);

    CPLFree(defn_str);

    /* Check the status of the Field add */
    if (FAILED(hr))
    {
        GDBErr(hr, "Failed at creating Field for " +
                       std::string(oField.GetNameRef()));
        return OGRERR_FAILURE;
    }

    /* Now add the OGRFieldDefn to the OGRFeatureDefn */
    m_pFeatureDefn->AddFieldDefn(&oField);

    m_vOGRFieldToESRIField.push_back(StringToWString(fieldname_clean));
    m_vOGRFieldToESRIFieldType.push_back(gdbFieldType);

    if (oField.GetType() == OFTBinary)
        m_apoByteArrays.push_back(new ByteArray());

    /* All done and happy */
    return OGRERR_NONE;
}

/************************************************************************/
/*                             DeleteField()                            */
/************************************************************************/

OGRErr FGdbLayer::DeleteField(int iFieldToDelete)
{

    if (!m_pDS->GetUpdate() || m_pTable == nullptr)
        return OGRERR_FAILURE;

    if (iFieldToDelete < 0 || iFieldToDelete >= m_pFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    ResetReading();

    const char *pszFieldName =
        m_pFeatureDefn->GetFieldDefn(iFieldToDelete)->GetNameRef();

    fgdbError hr;
    if (FAILED(hr = m_pTable->DeleteField(StringToWString(pszFieldName))))
    {
        GDBErr(hr, "Failed deleting field " + std::string(pszFieldName));
        return OGRERR_FAILURE;
    }

    m_vOGRFieldToESRIField.erase(m_vOGRFieldToESRIField.begin() +
                                 iFieldToDelete);
    m_vOGRFieldToESRIFieldType.erase(m_vOGRFieldToESRIFieldType.begin() +
                                     iFieldToDelete);

    return m_pFeatureDefn->DeleteFieldDefn(iFieldToDelete);
}

#ifdef AlterFieldDefn_implemented_but_not_working

/************************************************************************/
/*                           AlterFieldDefn()                           */
/************************************************************************/

OGRErr FGdbLayer::AlterFieldDefn(int iFieldToAlter,
                                 OGRFieldDefn *poNewFieldDefn, int nFlags)
{

    if (!m_pDS->GetUpdate() || m_pTable == NULL)
        return OGRERR_FAILURE;

    if (iFieldToAlter < 0 || iFieldToAlter >= m_pFeatureDefn->GetFieldCount())
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Invalid field index");
        return OGRERR_FAILURE;
    }

    OGRFieldDefn *poFieldDefn = m_pFeatureDefn->GetFieldDefn(iFieldToAlter);
    OGRFieldDefn oField(poFieldDefn);

    if (nFlags & ALTER_TYPE_FLAG)
    {
        oField.SetSubType(OFSTNone);
        oField.SetType(poNewFieldDefn->GetType());
        oField.SetSubType(poNewFieldDefn->GetSubType());
    }
    if (nFlags & ALTER_NAME_FLAG)
    {
        if (strcmp(poNewFieldDefn->GetNameRef(), oField.GetNameRef()) != 0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Altering field name is not supported");
            return OGRERR_FAILURE;
        }
        oField.SetName(poNewFieldDefn->GetNameRef());
    }
    if (nFlags & ALTER_WIDTH_PRECISION_FLAG)
    {
        oField.SetWidth(poNewFieldDefn->GetWidth());
        oField.SetPrecision(poNewFieldDefn->GetPrecision());
    }

    std::string fieldname_clean =
        WStringToString(m_vOGRFieldToESRIField[iFieldToAlter]);
    std::string gdbFieldType;

    char *defn_str =
        CreateFieldDefn(oField, TRUE, fieldname_clean, gdbFieldType);
    if (defn_str == NULL)
        return OGRERR_FAILURE;

    ResetReading();

    /* Add the FGDB Field to the FGDB Table. */
    fgdbError hr = m_pTable->AlterField(defn_str);

    CPLFree(defn_str);

    /* Check the status of the AlterField */
    if (FAILED(hr))
    {
        GDBErr(hr,
               "Failed at altering field " + std::string(oField.GetNameRef()));
        return OGRERR_FAILURE;
    }

    m_vOGRFieldToESRIFieldType[iFieldToAlter] = gdbFieldType;

    poFieldDefn->SetSubType(OFSTNone);
    poFieldDefn->SetType(oField.GetType());
    poFieldDefn->SetType(oField.GetSubType());
    poFieldDefn->SetWidth(oField.GetWidth());
    poFieldDefn->SetPrecision(oField.GetPrecision());

    return OGRERR_NONE;
}
#endif  // AlterFieldDefn_implemented_but_not_working

/************************************************************************/
/*                      XMLSpatialReference()                           */
/*  Build up an XML representation of an OGRSpatialReference.           */
/*  Used in layer creation.                                             */
/*                                                                      */
/************************************************************************/

static CPLXMLNode *XMLSpatialReference(OGRSpatialReference *poSRS,
                                       char **papszOptions)
{
    /* We always need a SpatialReference */
    CPLXMLNode *srs_xml =
        CPLCreateXMLNode(nullptr, CXT_Element, "SpatialReference");

    /* Extract the WKID before morphing */
    int nSRID = 0;
    if (poSRS && poSRS->GetAuthorityCode(nullptr))
    {
        nSRID = atoi(poSRS->GetAuthorityCode(nullptr));
    }

    /* NULL poSRS => UnknownCoordinateSystem */
    if (!poSRS)
    {
        FGDB_CPLAddXMLAttribute(srs_xml, "xsi:type",
                                "esri:UnknownCoordinateSystem");
    }
    else
    {
        /* Set the SpatialReference type attribute correctly for GEOGCS/PROJCS
         */
        if (poSRS->IsProjected())
            FGDB_CPLAddXMLAttribute(srs_xml, "xsi:type",
                                    "esri:ProjectedCoordinateSystem");
        else
            FGDB_CPLAddXMLAttribute(srs_xml, "xsi:type",
                                    "esri:GeographicCoordinateSystem");

        /* Add the WKT to the XML */
        SpatialReferenceInfo oESRI_SRS;

        /* Do we have a known SRID ? If so, directly query the ESRI SRS DB */
        if (nSRID &&
            SpatialReferences::FindSpatialReferenceBySRID(nSRID, oESRI_SRS))
        {
            CPLDebug("FGDB",
                     "Layer SRS has a SRID (%d). Using WKT from ESRI SRS "
                     "DBFound perfect match. ",
                     nSRID);
            CPLCreateXMLElementAndValue(
                srs_xml, "WKT", WStringToString(oESRI_SRS.srtext).c_str());
        }
        else
        {
            /* Make a clone so we can morph it without morphing the original */
            OGRSpatialReference *poSRSClone = poSRS->Clone();

            /* Flip the WKT to ESRI form, return UnknownCoordinateSystem if we
             * can't */
            if (poSRSClone->morphToESRI() != OGRERR_NONE)
            {
                delete poSRSClone;
                FGDB_CPLAddXMLAttribute(srs_xml, "xsi:type",
                                        "esri:UnknownCoordinateSystem");
                return srs_xml;
            }

            char *wkt = nullptr;
            poSRSClone->exportToWkt(&wkt);
            if (wkt)
            {
                std::vector<int> oaiCandidateSRS;
                nSRID = 0;

                // Ask PROJ which known SRS matches poSRS
                int nEntries = 0;
                int *panMatchConfidence = nullptr;
                auto pahSRS =
                    poSRS->FindMatches(nullptr, &nEntries, &panMatchConfidence);
                for (int i = 0; i < nEntries; ++i)
                {
                    if (panMatchConfidence[i] >= 70)
                    {
                        // Look for candidates in the EPSG/ESRI namespace,
                        // and find the correspond ESRI SRS from the code
                        const char *pszAuthName =
                            OSRGetAuthorityName(pahSRS[i], nullptr);
                        const char *pszAuthCode =
                            OSRGetAuthorityCode(pahSRS[i], nullptr);
                        if (pszAuthName &&
                            (EQUAL(pszAuthName, "EPSG") ||
                             EQUAL(pszAuthName, "ESRI")) &&
                            pszAuthCode &&
                            SpatialReferences::FindSpatialReferenceBySRID(
                                atoi(pszAuthCode), oESRI_SRS))
                        {
                            const std::string osESRI_WKT =
                                WStringToString(oESRI_SRS.srtext);
                            OGRSpatialReference oSRS_FromESRI;
                            oSRS_FromESRI.SetAxisMappingStrategy(
                                OAMS_TRADITIONAL_GIS_ORDER);
                            if (oSRS_FromESRI.importFromWkt(
                                    osESRI_WKT.c_str()) == OGRERR_NONE &&
                                poSRSClone->IsSame(&oSRS_FromESRI))
                            {
                                if (panMatchConfidence[i] == 100)
                                {
                                    /* Exact match found (not sure this case
                                     * happens) */
                                    nSRID = oESRI_SRS.auth_srid;
                                    break;
                                }
                                oaiCandidateSRS.push_back(oESRI_SRS.auth_srid);
                            }
                        }
                    }
                }
                OSRFreeSRSArray(pahSRS);
                CPLFree(panMatchConfidence);

                if (nSRID != 0)
                {
                    CPLDebug("FGDB",
                             "Found perfect match in ESRI SRS DB "
                             "for layer SRS. SRID is %d",
                             nSRID);
                }
                else if (oaiCandidateSRS.empty())
                {
                    CPLDebug(
                        "FGDB",
                        "Did not found a match in ESRI SRS DB for layer SRS. "
                        "Using morphed SRS WKT. Failure is to be expected");
                }
                else if (oaiCandidateSRS.size() == 1)
                {
                    nSRID = oaiCandidateSRS[0];
                    if (SpatialReferences::FindSpatialReferenceBySRID(
                            nSRID, oESRI_SRS))
                    {
                        CPLDebug("FGDB",
                                 "Found a single match in ESRI SRS DB "
                                 "for layer SRS. SRID is %d",
                                 nSRID);
                        nSRID = oESRI_SRS.auth_srid;
                        CPLFree(wkt);
                        wkt = CPLStrdup(
                            WStringToString(oESRI_SRS.srtext).c_str());
                    }
                }
                else
                {
                    /* Not sure this case can happen */

                    CPLString osCandidateSRS;
                    for (int i = 0; i < (int)oaiCandidateSRS.size() && i < 10;
                         i++)
                    {
                        if (!osCandidateSRS.empty())
                            osCandidateSRS += ", ";
                        osCandidateSRS += CPLSPrintf("%d", oaiCandidateSRS[i]);
                    }
                    if (oaiCandidateSRS.size() > 10)
                        osCandidateSRS += "...";

                    CPLDebug(
                        "FGDB",
                        "As several candidates (%s) have been found in "
                        "ESRI SRS DB for layer SRS, none has been selected. "
                        "Using morphed SRS WKT. Failure is to be expected",
                        osCandidateSRS.c_str());
                }

                CPLCreateXMLElementAndValue(srs_xml, "WKT", wkt);
                CPLFree(wkt);
            }

            /* Dispose of our close */
            delete poSRSClone;
        }
    }

    /* Handle Origin/Scale/Tolerance */
    const char *grid[10] = {"XOrigin",    "YOrigin",   "XYScale", "ZOrigin",
                            "ZScale",     "MOrigin",   "MScale",  "XYTolerance",
                            "ZTolerance", "MTolerance"};
    const char *gridvalues[10];

    /*
    Need different default parameters for geographic and projected coordinate
    systems. Try and use ArcGIS 10 default values.
    */
    // default tolerance is 1mm in the units of the coordinate system
    double ztol =
        0.001 * (poSRS ? poSRS->GetTargetLinearUnits("VERT_CS") : 1.0);
    // default scale is 10x the tolerance
    long zscale = (long)(1 / ztol * 10);

    double mtol = 0.001;
    long mscale = (long)(1 / mtol * 10);

    char s_xyscale[50], s_xytol[50], s_zscale[50], s_ztol[50], s_mscale[50],
        s_mtol[50];
    CPLsnprintf(s_ztol, 50, "%f", ztol);
    snprintf(s_zscale, 50, "%ld", zscale);

    CPLsnprintf(s_mtol, 50, "%f", mtol);
    snprintf(s_mscale, 50, "%ld", mscale);

    if (poSRS == nullptr || poSRS->IsProjected())
    {
        // default tolerance is 1mm in the units of the coordinate system
        double xytol =
            0.001 * (poSRS ? poSRS->GetTargetLinearUnits("PROJCS") : 1.0);
        // default scale is 10x the tolerance
        long xyscale = (long)(1 / xytol * 10);

        CPLsnprintf(s_xytol, 50, "%f", xytol);
        snprintf(s_xyscale, 50, "%ld", xyscale);

        // Ideally we would use the same X/Y origins as ArcGIS, but we need the
        // algorithm they use.
        gridvalues[0] = "-2147483647";
        gridvalues[1] = "-2147483647";
        gridvalues[2] = s_xyscale;
        gridvalues[3] = "-100000";
        gridvalues[4] = s_zscale;
        gridvalues[5] = "-100000";
        gridvalues[6] = s_mscale;
        gridvalues[7] = s_xytol;
        gridvalues[8] = s_ztol;
        gridvalues[9] = s_mtol;
    }
    else
    {
        gridvalues[0] = "-400";
        gridvalues[1] = "-400";
        gridvalues[2] = "1000000000";
        gridvalues[3] = "-100000";
        gridvalues[4] = s_zscale;
        gridvalues[5] = "-100000";
        gridvalues[6] = s_mscale;
        gridvalues[7] = "0.000000008983153";
        gridvalues[8] = s_ztol;
        gridvalues[9] = s_mtol;
    }

    /* Convert any layer creation options available, use defaults otherwise */
    for (int i = 0; i < 10; i++)
    {
        if (CSLFetchNameValue(papszOptions, grid[i]) != nullptr)
            gridvalues[i] = CSLFetchNameValue(papszOptions, grid[i]);

        CPLCreateXMLElementAndValue(srs_xml, grid[i], gridvalues[i]);
    }

    /* FGDB is always High Precision */
    CPLCreateXMLElementAndValue(srs_xml, "HighPrecision", "true");

    /* Add the WKID to the XML */
    const char *pszWKID = CSLFetchNameValue(papszOptions, "WKID");
    if (pszWKID)
        nSRID = atoi(pszWKID);
    if (nSRID)
    {
        CPLCreateXMLElementAndValue(srs_xml, "WKID", CPLSPrintf("%d", nSRID));
    }

    return srs_xml;
}

/************************************************************************/
/*                    CreateFeatureDataset()                            */
/************************************************************************/

bool FGdbLayer::CreateFeatureDataset(FGdbDataSource *pParentDataSource,
                                     const std::string &feature_dataset_name,
                                     OGRSpatialReference *poSRS,
                                     char **papszOptions)
{
    /* XML node */
    CPLXMLNode *xml_xml = CPLCreateXMLNode(nullptr, CXT_Element, "?xml");
    FGDB_CPLAddXMLAttribute(xml_xml, "version", "1.0");
    FGDB_CPLAddXMLAttribute(xml_xml, "encoding", "UTF-8");

    /* First build up a bare-bones feature definition */
    CPLXMLNode *defn_xml =
        CPLCreateXMLNode(nullptr, CXT_Element, "esri:DataElement");
    CPLAddXMLSibling(xml_xml, defn_xml);

    /* Add the attributes to the DataElement */
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xsi",
                            "http://www.w3.org/2001/XMLSchema-instance");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xs",
                            "http://www.w3.org/2001/XMLSchema");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:esri",
                            "http://www.esri.com/schemas/ArcGIS/10.1");

    /* Need to set this to esri:DEFeatureDataset or esri:DETable */
    FGDB_CPLAddXMLAttribute(defn_xml, "xsi:type", "esri:DEFeatureDataset");

    /* Add in more children */
    std::string catalog_page = "\\" + feature_dataset_name;
    CPLCreateXMLElementAndValue(defn_xml, "CatalogPath", catalog_page.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "Name", feature_dataset_name.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "ChildrenExpanded", "false");
    CPLCreateXMLElementAndValue(defn_xml, "DatasetType",
                                "esriDTFeatureDataset");
    CPLCreateXMLElementAndValue(defn_xml, "Versioned", "false");
    CPLCreateXMLElementAndValue(defn_xml, "CanVersion", "false");

    /* Add in empty extent */
    CPLXMLNode *extent_xml = CPLCreateXMLNode(nullptr, CXT_Element, "Extent");
    FGDB_CPLAddXMLAttribute(extent_xml, "xsi:nil", "true");
    CPLAddXMLChild(defn_xml, extent_xml);

    /* Add the SRS */
    if (true)  // TODO: conditional on existence of SRS
    {
        CPLXMLNode *srs_xml = XMLSpatialReference(poSRS, papszOptions);
        if (srs_xml)
            CPLAddXMLChild(defn_xml, srs_xml);
    }

    /* Convert our XML tree into a string for FGDB */
    char *defn_str = CPLSerializeXMLTree(xml_xml);
    CPLDestroyXMLNode(xml_xml);

    /* TODO, tie this to debugging levels */
    CPLDebug("FGDB", "%s", defn_str);

    /* Create the FeatureDataset. */
    Geodatabase *gdb = pParentDataSource->GetGDB();
    fgdbError hr = gdb->CreateFeatureDataset(defn_str);

    /* Free the XML */
    CPLFree(defn_str);

    /* Check table create status */
    if (FAILED(hr))
    {
        return GDBErr(hr, "Failed at creating FeatureDataset " +
                              feature_dataset_name);
    }

    return true;
}

/************************************************************************/
/*                            Create()                                  */
/* Build up an FGDB XML layer definition and use it to create a Table   */
/* or Feature Class to work from.                                       */
/*                                                                      */
/* Layer creation options:                                              */
/*   FEATURE_DATASET, nest layer inside a FeatureDataset folder         */
/*   GEOMETRY_NAME, user-selected name for the geometry column          */
/*   FID/OID_NAME, user-selected name for the FID column                */
/*   XORIGIN, YORIGIN, ZORIGIN, origin of the snapping grid             */
/*   XYSCALE, ZSCALE, inverse resolution of the snapping grid           */
/*   XYTOLERANCE, ZTOLERANCE, snapping tolerance for topology/networks  */
/*                                                                      */
/************************************************************************/

bool FGdbLayer::Create(FGdbDataSource *pParentDataSource,
                       const char *pszLayerNameIn, OGRSpatialReference *poSRS,
                       OGRwkbGeometryType eType, char **papszOptions)
{
    std::string parent_path = "";
    std::wstring wtable_path, wparent_path;
    std::string geometry_name = FGDB_GEOMETRY_NAME;
    std::string fid_name = FGDB_OID_NAME;
    std::string esri_type;
    bool has_z = false;
    bool has_m = false;

#ifdef EXTENT_WORKAROUND
    m_bLayerJustCreated = true;
#endif

    /* Launder the Layer name */
    std::wstring wlayerName;

    wlayerName = FGDBLaunderName(StringToWString(pszLayerNameIn));
    wlayerName = FGDBEscapeReservedKeywords(wlayerName);
    wlayerName = FGDBEscapeUnsupportedPrefixes(wlayerName);

    // https://desktop.arcgis.com/en/arcmap/latest/manage-data/administer-file-gdbs/file-geodatabase-size-and-name-limits.htm
    // document 160 character limit but
    // https://desktop.arcgis.com/en/arcmap/latest/manage-data/tables/fundamentals-of-adding-and-deleting-fields.htm#GUID-8E190093-8F8F-4132-AF4F-B0C9220F76B3
    // mentions 64. let be optimistic and aim for 160
    constexpr size_t TABLE_NAME_MAX_SIZE = 160;
    if (wlayerName.size() > TABLE_NAME_MAX_SIZE)
        wlayerName.resize(TABLE_NAME_MAX_SIZE);

    /* Ensures uniqueness of layer name */
    int numRenames = 1;
    while ((pParentDataSource->GetLayerByName(
                WStringToString(wlayerName).c_str()) != nullptr) &&
           (numRenames < 10))
    {
        wlayerName = StringToWString(CPLSPrintf(
            "%s_%d",
            WStringToString(wlayerName.substr(0, TABLE_NAME_MAX_SIZE - 2))
                .c_str(),
            numRenames));
        numRenames++;
    }
    while ((pParentDataSource->GetLayerByName(
                WStringToString(wlayerName).c_str()) != nullptr) &&
           (numRenames < 100))
    {
        wlayerName = StringToWString(CPLSPrintf(
            "%s_%d",
            WStringToString(wlayerName.substr(0, TABLE_NAME_MAX_SIZE - 3))
                .c_str(),
            numRenames));
        numRenames++;
    }

    const std::string layerName = WStringToString(wlayerName);
    if (layerName != pszLayerNameIn)
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "Normalized/laundered layer name: '%s' to '%s'",
                 pszLayerNameIn, layerName.c_str());
    }

    std::string table_path = "\\" + std::string(layerName);

    /* Handle the FEATURE_DATASET case */
    if (CSLFetchNameValue(papszOptions, "FEATURE_DATASET") != nullptr)
    {
        std::string feature_dataset =
            CSLFetchNameValue(papszOptions, "FEATURE_DATASET");

        /* Check if FEATURE_DATASET exists. Otherwise create it */
        std::vector<wstring> featuredatasets;
        Geodatabase *gdb = pParentDataSource->GetGDB();
        int bFeatureDataSetExists = FALSE;
        fgdbError hr;
        if (!FAILED(hr = gdb->GetChildDatasets(L"\\", L"Feature Dataset",
                                               featuredatasets)))
        {
            std::wstring feature_dataset_with_slash =
                L"\\" + StringToWString(feature_dataset);
            for (unsigned int i = 0; i < featuredatasets.size(); i++)
            {
                if (featuredatasets[i] == feature_dataset_with_slash)
                    bFeatureDataSetExists = TRUE;
            }
        }

        if (!bFeatureDataSetExists)
        {
            bool rv = CreateFeatureDataset(pParentDataSource, feature_dataset,
                                           poSRS, papszOptions);
            if (!rv)
                return rv;
        }

        table_path = "\\" + feature_dataset + table_path;
        parent_path = "\\" + feature_dataset;
    }

    /* Convert table_path into wstring */
    wtable_path = StringToWString(table_path);
    wparent_path = StringToWString(parent_path);

    /* Over-ride the geometry name if necessary */
    if (CSLFetchNameValue(papszOptions, "GEOMETRY_NAME") != nullptr)
        geometry_name = CSLFetchNameValue(papszOptions, "GEOMETRY_NAME");

    /* Over-ride the OID name if necessary */
    if (CSLFetchNameValue(papszOptions, "FID") != nullptr)
        fid_name = CSLFetchNameValue(papszOptions, "FID");
    else if (CSLFetchNameValue(papszOptions, "OID_NAME") != nullptr)
        fid_name = CSLFetchNameValue(papszOptions, "OID_NAME");

    m_bCreateMultipatch = CPLTestBool(
        CSLFetchNameValueDef(papszOptions, "CREATE_MULTIPATCH", "NO"));

    /* Figure out our geometry type */
    if (eType != wkbNone)
    {
        if (wkbFlatten(eType) == wkbUnknown)
        {
            return GDBErr(-1, "FGDB layers cannot be created with a wkbUnknown "
                              "layer geometry type.");
        }
        if (!OGRGeometryToGDB(eType, &esri_type, &has_z, &has_m))
            return GDBErr(-1, "Unable to map OGR type to ESRI type");

        if (wkbFlatten(eType) == wkbMultiPolygon && m_bCreateMultipatch)
        {
            esri_type = "esriGeometryMultiPatch";
            has_z = true;
        }
        // For TIN and PolyhedralSurface, default to create a multipatch,
        // unless the user explicitly disabled it
        else if ((wkbFlatten(eType) == wkbTIN ||
                  wkbFlatten(eType) == wkbPolyhedralSurface) &&
                 CPLTestBool(CSLFetchNameValueDef(papszOptions,
                                                  "CREATE_MULTIPATCH", "YES")))
        {
            m_bCreateMultipatch = true;
            esri_type = "esriGeometryMultiPatch";
            has_z = true;
        }
    }

    const auto eFlattenType = wkbFlatten(eType);
    const bool bIsLine =
        eFlattenType == wkbLineString || eFlattenType == wkbMultiLineString;
    const bool bIsPolygon =
        eFlattenType == wkbPolygon || eFlattenType == wkbMultiPolygon;

    const bool bCreateShapeLength =
        (bIsLine || bIsPolygon) && !m_bCreateMultipatch &&
        CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "CREATE_SHAPE_AREA_AND_LENGTH_FIELDS", "NO"));
    // Setting a non-default value doesn't work
    const char *pszLengthFieldName =
        CSLFetchNameValueDef(papszOptions, "LENGTH_FIELD_NAME", "Shape_Length");

    const bool bCreateShapeArea =
        bIsPolygon && !m_bCreateMultipatch &&
        CPLTestBool(CSLFetchNameValueDef(
            papszOptions, "CREATE_SHAPE_AREA_AND_LENGTH_FIELDS", "NO"));
    // Setting a non-default value doesn't work
    const char *pszAreaFieldName =
        CSLFetchNameValueDef(papszOptions, "AREA_FIELD_NAME", "Shape_Area");

    m_bLaunderReservedKeywords =
        CPLFetchBool(papszOptions, "LAUNDER_RESERVED_KEYWORDS", true);

    /* XML node */
    CPLXMLNode *xml_xml = CPLCreateXMLNode(nullptr, CXT_Element, "?xml");
    FGDB_CPLAddXMLAttribute(xml_xml, "version", "1.0");
    FGDB_CPLAddXMLAttribute(xml_xml, "encoding", "UTF-8");

    /* First build up a bare-bones feature definition */
    CPLXMLNode *defn_xml =
        CPLCreateXMLNode(nullptr, CXT_Element, "esri:DataElement");
    CPLAddXMLSibling(xml_xml, defn_xml);

    /* Add the attributes to the DataElement */
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xsi",
                            "http://www.w3.org/2001/XMLSchema-instance");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:xs",
                            "http://www.w3.org/2001/XMLSchema");
    FGDB_CPLAddXMLAttribute(defn_xml, "xmlns:esri",
                            "http://www.esri.com/schemas/ArcGIS/10.1");

    /* Need to set this to esri:DEFeatureDataset or esri:DETable */
    FGDB_CPLAddXMLAttribute(
        defn_xml, "xsi:type",
        (eType == wkbNone ? "esri:DETable" : "esri:DEFeatureClass"));

    /* Add in more children */
    CPLCreateXMLElementAndValue(defn_xml, "CatalogPath", table_path.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "Name", layerName.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "ChildrenExpanded", "false");

    /* WKB type of none implies this is a 'Table' otherwise it is a 'Feature
     * Class' */
    std::string datasettype =
        (eType == wkbNone ? "esriDTTable" : "esriDTFeatureClass");
    CPLCreateXMLElementAndValue(defn_xml, "DatasetType", datasettype.c_str());
    CPLCreateXMLElementAndValue(defn_xml, "Versioned", "false");
    CPLCreateXMLElementAndValue(defn_xml, "CanVersion", "false");

    if (CSLFetchNameValue(papszOptions, "CONFIGURATION_KEYWORD") != nullptr)
        CPLCreateXMLElementAndValue(
            defn_xml, "ConfigurationKeyword",
            CSLFetchNameValue(papszOptions, "CONFIGURATION_KEYWORD"));

    /* We might need to make OID optional later, but OGR likes to have a FID */
    CPLCreateXMLElementAndValue(defn_xml, "HasOID", "true");
    CPLCreateXMLElementAndValue(defn_xml, "OIDFieldName", fid_name.c_str());

    /* Add in empty Fields */
    CPLXMLNode *fields_xml = CPLCreateXMLNode(defn_xml, CXT_Element, "Fields");
    FGDB_CPLAddXMLAttribute(fields_xml, "xsi:type", "esri:Fields");
    CPLXMLNode *fieldarray_xml =
        CPLCreateXMLNode(fields_xml, CXT_Element, "FieldArray");
    FGDB_CPLAddXMLAttribute(fieldarray_xml, "xsi:type", "esri:ArrayOfField");

    /* Feature Classes have an implicit geometry column, so we'll add it at
     * creation time */
    CPLXMLNode *srs_xml = nullptr;
    if (eType != wkbNone)
    {
        CPLXMLNode *shape_xml =
            CPLCreateXMLNode(fieldarray_xml, CXT_Element, "Field");
        FGDB_CPLAddXMLAttribute(shape_xml, "xsi:type", "esri:Field");
        CPLCreateXMLElementAndValue(shape_xml, "Name", geometry_name.c_str());
        CPLCreateXMLElementAndValue(shape_xml, "Type", "esriFieldTypeGeometry");
        if (CPLFetchBool(papszOptions, "GEOMETRY_NULLABLE", true))
            CPLCreateXMLElementAndValue(shape_xml, "IsNullable", "true");
        else
            CPLCreateXMLElementAndValue(shape_xml, "IsNullable", "false");
        CPLCreateXMLElementAndValue(shape_xml, "Length", "0");
        CPLCreateXMLElementAndValue(shape_xml, "Precision", "0");
        CPLCreateXMLElementAndValue(shape_xml, "Scale", "0");
        CPLCreateXMLElementAndValue(shape_xml, "Required", "true");
        CPLXMLNode *geom_xml =
            CPLCreateXMLNode(shape_xml, CXT_Element, "GeometryDef");
        FGDB_CPLAddXMLAttribute(geom_xml, "xsi:type", "esri:GeometryDef");
        CPLCreateXMLElementAndValue(geom_xml, "AvgNumPoints", "0");
        CPLCreateXMLElementAndValue(geom_xml, "GeometryType",
                                    esri_type.c_str());
        CPLCreateXMLElementAndValue(geom_xml, "HasM",
                                    (has_m ? "true" : "false"));
        CPLCreateXMLElementAndValue(geom_xml, "HasZ",
                                    (has_z ? "true" : "false"));

        /* Add the SRS if we have one */
        srs_xml = XMLSpatialReference(poSRS, papszOptions);
        if (srs_xml)
            CPLAddXMLChild(geom_xml, srs_xml);
    }

    /* All (?) Tables and Feature Classes will have an ObjectID */
    CPLXMLNode *oid_xml =
        CPLCreateXMLNode(fieldarray_xml, CXT_Element, "Field");
    FGDB_CPLAddXMLAttribute(oid_xml, "xsi:type", "esri:Field");
    CPLCreateXMLElementAndValue(oid_xml, "Name", fid_name.c_str());
    CPLCreateXMLElementAndValue(oid_xml, "Type", "esriFieldTypeOID");
    CPLCreateXMLElementAndValue(oid_xml, "IsNullable", "false");
    CPLCreateXMLElementAndValue(oid_xml, "Length", "12");
    CPLCreateXMLElementAndValue(oid_xml, "Precision", "0");
    CPLCreateXMLElementAndValue(oid_xml, "Scale", "0");
    CPLCreateXMLElementAndValue(oid_xml, "Required", "true");

    /* Add in empty Indexes */
    CPLXMLNode *indexes_xml =
        CPLCreateXMLNode(defn_xml, CXT_Element, "Indexes");
    FGDB_CPLAddXMLAttribute(indexes_xml, "xsi:type", "esri:Indexes");
    CPLXMLNode *indexarray_xml =
        CPLCreateXMLNode(indexes_xml, CXT_Element, "IndexArray");
    FGDB_CPLAddXMLAttribute(indexarray_xml, "xsi:type", "esri:ArrayOfIndex");

    /* CLSID http://forums.arcgis.com/threads/34536?p=118484#post118484 */
    if (eType == wkbNone)
    {
        CPLCreateXMLElementAndValue(defn_xml, "CLSID",
                                    "{7A566981-C114-11D2-8A28-006097AFF44E}");
        CPLCreateXMLElementAndValue(defn_xml, "EXTCLSID", "");
    }
    else
    {
        CPLCreateXMLElementAndValue(defn_xml, "CLSID",
                                    "{52353152-891A-11D0-BEC6-00805F7C4268}");
        CPLCreateXMLElementAndValue(defn_xml, "EXTCLSID", "");
    }

    /* Set the alias for the Feature Class, check if we received an */
    /* explicit one in the options vector. */
    const char *pszLayerAlias = CSLFetchNameValue(papszOptions, "LAYER_ALIAS");
    if (pszLayerAlias != nullptr)
    {
        CPLCreateXMLElementAndValue(defn_xml, "AliasName", pszLayerAlias);
    }
    else if (pszLayerNameIn != layerName)
    {
        CPLCreateXMLElementAndValue(defn_xml, "AliasName", pszLayerNameIn);
    }

    /* Map from OGR WKB type to ESRI type */
    if (eType != wkbNone)
    {
        /* Declare our feature type */
        CPLCreateXMLElementAndValue(defn_xml, "FeatureType", "esriFTSimple");
        CPLCreateXMLElementAndValue(defn_xml, "ShapeType", esri_type.c_str());
        CPLCreateXMLElementAndValue(defn_xml, "ShapeFieldName",
                                    geometry_name.c_str());

        /* Dimensionality */
        CPLCreateXMLElementAndValue(defn_xml, "HasM",
                                    (has_m ? "true" : "false"));
        CPLCreateXMLElementAndValue(defn_xml, "HasZ",
                                    (has_z ? "true" : "false"));

        CPLCreateXMLElementAndValue(defn_xml, "HasSpatialIndex", "true");

        /* These field are required for Arcmap to display aliases correctly */
        if (bCreateShapeArea)
            CPLCreateXMLElementAndValue(defn_xml, "AreaFieldName",
                                        pszAreaFieldName);
        else
            CPLCreateXMLNode(defn_xml, CXT_Element, "AreaFieldName");

        if (bCreateShapeLength)
            CPLCreateXMLElementAndValue(defn_xml, "LengthFieldName",
                                        pszLengthFieldName);
        else
            CPLCreateXMLNode(defn_xml, CXT_Element, "LengthFieldName");

        /* We can't know the extent at this point <Extent xsi:nil='true'/> */
        CPLXMLNode *extn_xml =
            CPLCreateXMLNode(defn_xml, CXT_Element, "Extent");
        FGDB_CPLAddXMLAttribute(extn_xml, "xsi:nil", "true");
    }

    /* Feature Class with known SRS gets an SRS entry */
    if (eType != wkbNone && srs_xml != nullptr)
    {
        CPLAddXMLChild(defn_xml, CPLCloneXMLTree(srs_xml));
    }

    /* Convert our XML tree into a string for FGDB */
    char *defn_str;

    if (CSLFetchNameValue(papszOptions, "XML_DEFINITION") != nullptr)
        defn_str = CPLStrdup(CSLFetchNameValue(papszOptions, "XML_DEFINITION"));
    else
        defn_str = CPLSerializeXMLTree(xml_xml);
    CPLDestroyXMLNode(xml_xml);

    /* TODO, tie this to debugging levels */
    CPLDebug("FGDB", "%s", defn_str);
    // std::cout << defn_str << std::endl;

    /* Create the table. */
    Table *table = new Table;
    Geodatabase *gdb = pParentDataSource->GetGDB();
    fgdbError hr = gdb->CreateTable(defn_str, wparent_path, *table);

    /* Free the XML */
    CPLFree(defn_str);

    /* Check table create status */
    if (FAILED(hr))
    {
        delete table;
        return GDBErr(hr, "Failed at creating table for " + table_path);
    }

    m_papszOptions = CSLDuplicate(papszOptions);

    // Default to YES here assuming ogr2ogr scenario
    m_bBulkLoadAllowed =
        CPLTestBool(CPLGetConfigOption("FGDB_BULK_LOAD", "YES"));

    /* Store the new FGDB Table pointer and set up the OGRFeatureDefn */
    bool bRet =
        FGdbLayer::Initialize(pParentDataSource, table, wtable_path, L"Table");
    if (bRet)
    {
        if (bCreateShapeArea)
        {
            OGRFieldDefn oField(pszAreaFieldName, OFTReal);
            oField.SetDefault("FILEGEODATABASE_SHAPE_AREA");
            bRet &= CreateField(&oField, false) == OGRERR_NONE;
        }
        if (bCreateShapeLength)
        {
            OGRFieldDefn oField(pszLengthFieldName, OFTReal);
            oField.SetDefault("FILEGEODATABASE_SHAPE_LENGTH");
            bRet &= CreateField(&oField, false) == OGRERR_NONE;
        }
    }
    return bRet;
}

/*************************************************************************/
/*                            Initialize()                               */
/* Has ownership of the table as soon as it is called.                   */
/************************************************************************/

bool FGdbLayer::Initialize(FGdbDataSource *pParentDataSource, Table *pTable,
                           const std::wstring &wstrTablePath,
                           const std::wstring &wstrType)
{
    long hr;

    m_pDS = pParentDataSource;  // we never assume ownership of the parent - so
                                // our destructor should not delete

    m_pTable = pTable;

    m_wstrTablePath = wstrTablePath;
    m_wstrType = wstrType;

    wstring wstrQueryName;
    if (FAILED(hr = pParentDataSource->GetGDB()->GetQueryName(wstrTablePath,
                                                              wstrQueryName)))
        return GDBErr(hr, "Failed at getting underlying table name for " +
                              WStringToString(wstrTablePath));

    m_strName = WStringToString(wstrQueryName);

    m_pFeatureDefn = new OGRFeatureDefn(
        m_strName.c_str());  // TODO: Should I "new" an OGR smart pointer -
                             // sample says so, but it doesn't seem right
    SetDescription(m_pFeatureDefn->GetName());
    // as long as we use the same compiler & settings in both the ogr build and
    // this driver, we should be OK
    m_pFeatureDefn->Reference();

    string tableDef;
    if (FAILED(hr = m_pTable->GetDefinition(tableDef)))
        return GDBErr(hr, "Failed at getting table definition for " +
                              WStringToString(wstrTablePath));

    // CPLDebug("FGDB", "tableDef = %s", tableDef.c_str());

    bool abort = false;

    // extract schema information from table
    CPLXMLNode *psRoot = CPLParseXMLString(tableDef.c_str());

    if (psRoot == nullptr)
    {
        CPLError(
            CE_Failure, CPLE_AppDefined, "%s",
            ("Failed parsing GDB Table Schema XML for " + m_strName).c_str());
        return false;
    }

    CPLXMLNode *pDataElementNode =
        psRoot->psNext;  // Move to next field which should be DataElement

    if (pDataElementNode != nullptr && pDataElementNode->psChild != nullptr &&
        pDataElementNode->eType == CXT_Element &&
        EQUAL(pDataElementNode->pszValue, "esri:DataElement"))
    {
        CPLXMLNode *psNode;

        m_bTimeInUTC = CPLTestBool(
            CPLGetXMLValue(pDataElementNode, "IsTimeInUTC", "false"));

        std::string osAreaFieldName;
        std::string osLengthFieldName;
        for (psNode = pDataElementNode->psChild; psNode != nullptr;
             psNode = psNode->psNext)
        {
            if (psNode->eType == CXT_Element && psNode->psChild != nullptr)
            {
                if (EQUAL(psNode->pszValue, "OIDFieldName"))
                {
                    m_strOIDFieldName = CPLGetXMLValue(psNode, nullptr, "");
                }
                else if (EQUAL(psNode->pszValue, "ShapeFieldName"))
                {
                    m_strShapeFieldName = CPLGetXMLValue(psNode, nullptr, "");
                }
                else if (EQUAL(psNode->pszValue, "AreaFieldName"))
                {
                    osAreaFieldName = CPLGetXMLValue(psNode, nullptr, "");
                }
                else if (EQUAL(psNode->pszValue, "LengthFieldName"))
                {
                    osLengthFieldName = CPLGetXMLValue(psNode, nullptr, "");
                }
                else if (EQUAL(psNode->pszValue, "Fields"))
                {
                    if (!GDBToOGRFields(psNode))
                    {
                        abort = true;
                        break;
                    }
                }
            }
        }

        if (!osAreaFieldName.empty())
        {
            const int nIdx =
                m_pFeatureDefn->GetFieldIndex(osAreaFieldName.c_str());
            if (nIdx >= 0)
            {
                m_pFeatureDefn->GetFieldDefn(nIdx)->SetDefault(
                    "FILEGEODATABASE_SHAPE_AREA");
            }
        }

        if (!osLengthFieldName.empty())
        {
            const int nIdx =
                m_pFeatureDefn->GetFieldIndex(osLengthFieldName.c_str());
            if (nIdx >= 0)
            {
                m_pFeatureDefn->GetFieldDefn(nIdx)->SetDefault(
                    "FILEGEODATABASE_SHAPE_LENGTH");
            }
        }

        if (m_strShapeFieldName.empty())
            m_pFeatureDefn->SetGeomType(wkbNone);
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "%s",
                 ("Failed parsing GDB Table Schema XML (DataElement) for " +
                  m_strName)
                     .c_str());
        return false;
    }
    CPLDestroyXMLNode(psRoot);

    if (m_pFeatureDefn->GetGeomFieldCount() != 0)
    {
        m_pFeatureDefn->GetGeomFieldDefn(0)->SetName(
            m_strShapeFieldName.c_str());
        m_pFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(m_pSRS);
    }

    if (abort)
        return false;

    return true;  // AOToOGRFields(ipFields, m_pFeatureDefn,
                  // m_vOGRFieldToESRIField);
}

/************************************************************************/
/*                          ParseGeometryDef()                          */
/************************************************************************/

bool FGdbLayer::ParseGeometryDef(CPLXMLNode *psRoot)
{
    CPLXMLNode *psGeometryDefItem;

    string geometryType;
    bool hasZ = false, hasM = false;
    string wkt, wkid, latestwkid;

    for (psGeometryDefItem = psRoot->psChild; psGeometryDefItem != nullptr;
         psGeometryDefItem = psGeometryDefItem->psNext)
    {
        // loop through all "GeometryDef" elements
        //

        if (psGeometryDefItem->eType == CXT_Element &&
            psGeometryDefItem->psChild != nullptr)
        {
            if (EQUAL(psGeometryDefItem->pszValue, "GeometryType"))
            {
                geometryType = CPLGetXMLValue(psGeometryDefItem, nullptr, "");
            }
            else if (EQUAL(psGeometryDefItem->pszValue, "SpatialReference"))
            {
                ParseSpatialReference(
                    psGeometryDefItem, &wkt, &wkid,
                    &latestwkid);  // we don't check for success because it
                                   // may not be there
            }
            else if (EQUAL(psGeometryDefItem->pszValue, "HasM"))
            {
                if (!strcmp(CPLGetXMLValue(psGeometryDefItem, nullptr, ""),
                            "true"))
                    hasM = true;
            }
            else if (EQUAL(psGeometryDefItem->pszValue, "HasZ"))
            {
                if (!strcmp(CPLGetXMLValue(psGeometryDefItem, nullptr, ""),
                            "true"))
                    hasZ = true;
            }
        }
    }

    OGRwkbGeometryType ogrGeoType;
    if (!GDBToOGRGeometry(geometryType, hasZ, hasM, &ogrGeoType))
        return false;

    m_pFeatureDefn->SetGeomType(ogrGeoType);

    if (wkbFlatten(ogrGeoType) == wkbMultiLineString ||
        wkbFlatten(ogrGeoType) == wkbMultiPoint)
        m_forceMulti = true;

    if (latestwkid.length() > 0 || wkid.length() > 0)
    {
        int bSuccess = FALSE;
        m_pSRS = new OGRSpatialReference();
        m_pSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        CPLPushErrorHandler(CPLQuietErrorHandler);
        if (latestwkid.length() > 0)
        {
            if (m_pSRS->importFromEPSG(atoi(latestwkid.c_str())) == OGRERR_NONE)
            {
                bSuccess = TRUE;
            }
            else
            {
                CPLDebug("FGDB", "Cannot import SRID %s", latestwkid.c_str());
            }
        }
        if (!bSuccess && wkid.length() > 0)
        {
            if (m_pSRS->importFromEPSG(atoi(wkid.c_str())) == OGRERR_NONE)
            {
                bSuccess = TRUE;
            }
            else
            {
                CPLDebug("FGDB", "Cannot import SRID %s", wkid.c_str());
            }
        }
        CPLPopErrorHandler();
        CPLErrorReset();
        if (!bSuccess)
        {
            delete m_pSRS;
            m_pSRS = nullptr;
        }
        else
            return true;
    }

    if (wkt.length() > 0)
    {
        if (!GDBToOGRSpatialReference(wkt, &m_pSRS))
        {
            // report error, but be passive about it
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Failed Mapping ESRI Spatial Reference");
        }
    }
    else
    {
        // report error, but be passive about it
        CPLError(CE_Warning, CPLE_AppDefined, "Empty Spatial Reference");
    }

    return true;
}

/************************************************************************/
/*                        ParseSpatialReference()                       */
/************************************************************************/

bool FGdbLayer::ParseSpatialReference(CPLXMLNode *psSpatialRefNode,
                                      string *pOutWkt, string *pOutWKID,
                                      string *pOutLatestWKID)
{
    *pOutWkt = "";
    *pOutWKID = "";
    *pOutLatestWKID = "";

    CPLXMLNode *psSRItemNode;

    /* Loop through all the SRS elements we want to store */
    for (psSRItemNode = psSpatialRefNode->psChild; psSRItemNode != nullptr;
         psSRItemNode = psSRItemNode->psNext)
    {
        /* The WKID maps (mostly) to an EPSG code */
        if (psSRItemNode->eType == CXT_Element &&
            psSRItemNode->psChild != nullptr &&
            EQUAL(psSRItemNode->pszValue, "WKID"))
        {
            *pOutWKID = CPLGetXMLValue(psSRItemNode, nullptr, "");

            // Needed with FileGDB v1.4 with layers with empty SRS
            if (*pOutWKID == "0")
                *pOutWKID = "";
        }
        /* The concept of LatestWKID is explained in
         * http://resources.arcgis.com/en/help/arcgis-rest-api/index.html#//02r3000000n1000000
         */
        else if (psSRItemNode->eType == CXT_Element &&
                 psSRItemNode->psChild != nullptr &&
                 EQUAL(psSRItemNode->pszValue, "LatestWKID"))
        {
            *pOutLatestWKID = CPLGetXMLValue(psSRItemNode, nullptr, "");
        }
        /* The WKT well-known text can be converted by OGR */
        else if (psSRItemNode->eType == CXT_Element &&
                 psSRItemNode->psChild != nullptr &&
                 EQUAL(psSRItemNode->pszValue, "WKT"))
        {
            *pOutWkt = CPLGetXMLValue(psSRItemNode, nullptr, "");
        }
    }
    return *pOutWkt != "" || *pOutWKID != "";
}

/************************************************************************/
/*                          GDBToOGRFields()                           */
/************************************************************************/

bool FGdbLayer::GDBToOGRFields(CPLXMLNode *psRoot)
{
    m_vOGRFieldToESRIField.clear();

    if (psRoot->psChild == nullptr || psRoot->psChild->psNext == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unrecognized GDB XML Schema");

        return false;
    }

    psRoot = psRoot->psChild->psNext;  // change root to "FieldArray"

    // CPLAssert(ogrToESRIFieldMapping.size() ==
    // pOGRFeatureDef->GetFieldCount());

    CPLXMLNode *psFieldNode;

    for (psFieldNode = psRoot->psChild; psFieldNode != nullptr;
         psFieldNode = psFieldNode->psNext)
    {
        // loop through all "Field" elements
        //

        if (psFieldNode->eType == CXT_Element &&
            psFieldNode->psChild != nullptr &&
            EQUAL(psFieldNode->pszValue, "Field"))
        {

            CPLXMLNode *psFieldItemNode;
            std::string fieldName;
            std::string fieldAlias;
            std::string fieldType;
            int nLength = 0;
            // int nPrecision = 0;
            int bNullable = TRUE;
            std::string osDefault;
            std::string osDomainName;

            // loop through all items in Field element
            //

            for (psFieldItemNode = psFieldNode->psChild;
                 psFieldItemNode != nullptr;
                 psFieldItemNode = psFieldItemNode->psNext)
            {
                if (psFieldItemNode->eType == CXT_Element)
                {
                    const char *pszValue =
                        CPLGetXMLValue(psFieldItemNode, nullptr, "");
                    if (EQUAL(psFieldItemNode->pszValue, "Name"))
                    {
                        fieldName = pszValue;
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "AliasName"))
                    {
                        fieldAlias = pszValue;
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "Type"))
                    {
                        fieldType = pszValue;
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "GeometryDef"))
                    {
                        if (!ParseGeometryDef(psFieldItemNode))
                            return false;  // if we failed parsing the
                                           // GeometryDef, we are done!
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "Length"))
                    {
                        nLength = atoi(pszValue);
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "Precision"))
                    {
                        // nPrecision = atoi(pszValue);
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "IsNullable"))
                    {
                        bNullable = EQUAL(pszValue, "true");
                    }
                    else if (EQUAL(psFieldItemNode->pszValue, "DefaultValue"))
                    {
                        osDefault = pszValue;
                    }
                    // NOTE: when using the GetDefinition() API, the domain name
                    // is set in <Domain><DomainName>, whereas the raw XML is
                    // just <DomainName>
                    else if (EQUAL(psFieldItemNode->pszValue, "Domain"))
                    {
                        osDomainName =
                            CPLGetXMLValue(psFieldItemNode, "DomainName", "");
                    }
                }
            }

            ///////////////////////////////////////////////////////////////////
            // At this point we have parsed everything about the current field

            if (fieldType == "esriFieldTypeGeometry")
            {
                m_strShapeFieldName = fieldName;
                m_pFeatureDefn->GetGeomFieldDefn(0)->SetNullable(bNullable);

                continue;  // finish here for special field - don't add as OGR
                           // fielddef
            }
            else if (fieldType == "esriFieldTypeOID")
            {
                // m_strOIDFieldName = fieldName; // already set by this point

                continue;  // finish here for special field - don't add as OGR
                           // fielddef
            }

            OGRFieldType ogrType;
            OGRFieldSubType eSubType;
            // CPLDebug("FGDB", "name = %s, type = %s", fieldName.c_str(),
            // fieldType.c_str() );
            if (!GDBToOGRFieldType(fieldType, &ogrType, &eSubType))
            {
                // field cannot be mapped, skipping further processing
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Skipping field: [%s] type: [%s] ", fieldName.c_str(),
                         fieldType.c_str());
                continue;
            }

            // TODO: Optimization - modify m_wstrSubFields so it only fetches
            // fields that are mapped

            OGRFieldDefn fieldTemplate(fieldName.c_str(), ogrType);
            if (fieldAlias != fieldName)
            {
                // The SDK generates an alias even with it is not explicitly
                // written
                fieldTemplate.SetAlternativeName(fieldAlias.c_str());
            }
            fieldTemplate.SetSubType(eSubType);
            /* On creation (GDBFieldTypeToWidthPrecision) if string width is 0,
             * we pick up */
            /* 65536 by default to mean unlimited string length, but we don't
             * want */
            /* to advertise such a big number */
            if (ogrType == OFTString && nLength < 65536)
                fieldTemplate.SetWidth(nLength);
            // fieldTemplate.SetPrecision(nPrecision);
            fieldTemplate.SetNullable(bNullable);
            if (!osDefault.empty())
            {
                if (ogrType == OFTString)
                {
                    char *pszTmp =
                        CPLEscapeString(osDefault.c_str(), -1, CPLES_SQL);
                    osDefault = "'";
                    osDefault += pszTmp;
                    CPLFree(pszTmp);
                    osDefault += "'";
                    fieldTemplate.SetDefault(osDefault.c_str());
                }
                else if (ogrType == OFTInteger || ogrType == OFTReal)
                {
#ifdef unreliable
                    /* Disabling this as GDBs and the FileGDB SDK aren't
                     * reliable for numeric values */
                    /* It often occurs that the XML definition in
                     * a00000004.gdbtable doesn't */
                    /* match the default values (in binary) found in the field
                     * definition */
                    /* section of the .gdbtable of the layers themselves */
                    /* The Table::GetDefinition() API of FileGDB doesn't seem to
                     * use the */
                    /* XML definition, but rather the values found in the field
                     * definition */
                    /* section of the .gdbtable of the layers themselves */
                    /* It seems that the XML definition in a00000004.gdbtable is
                     * authoritative */
                    /* in ArcGIS, so we're screwed... */

                    fieldTemplate.SetDefault(osDefault.c_str());
#endif
                }
                else if (ogrType == OFTDateTime)
                {
                    int nYear, nMonth, nDay, nHour, nMinute;
                    float fSecond;
                    if (sscanf(osDefault.c_str(), "%d-%d-%dT%d:%d:%fZ", &nYear,
                               &nMonth, &nDay, &nHour, &nMinute,
                               &fSecond) == 6 ||
                        sscanf(osDefault.c_str(), "'%d-%d-%d %d:%d:%fZ'",
                               &nYear, &nMonth, &nDay, &nHour, &nMinute,
                               &fSecond) == 6)
                    {
                        fieldTemplate.SetDefault(CPLSPrintf(
                            "'%04d/%02d/%02d %02d:%02d:%02d'", nYear, nMonth,
                            nDay, nHour, nMinute, (int)(fSecond + 0.5)));
                    }
                }
            }
            if (!osDomainName.empty())
            {
                fieldTemplate.SetDomainName(osDomainName);
            }

            m_pFeatureDefn->AddFieldDefn(&fieldTemplate);

            m_vOGRFieldToESRIField.push_back(StringToWString(fieldName));
            m_vOGRFieldToESRIFieldType.push_back(fieldType);
            if (ogrType == OFTBinary)
                m_apoByteArrays.push_back(new ByteArray());
        }
    }

    /* Using OpenFileGDB to get reliable default values for integer/real fields
     */
    /* and alias */
    {
        const char *const apszDrivers[] = {"OpenFileGDB", nullptr};
        GDALDataset *poDS = GDALDataset::Open(
            m_pDS->GetFSName(), GDAL_OF_VECTOR, apszDrivers, nullptr, nullptr);
        if (poDS != nullptr)
        {
            OGRLayer *poLyr = poDS->GetLayerByName(GetName());
            if (poLyr)
            {
                const auto poOFGBLayerDefn = poLyr->GetLayerDefn();
                const int nOFGDBFieldCount = poOFGBLayerDefn->GetFieldCount();
                for (int i = 0; i < nOFGDBFieldCount; i++)
                {
                    const OGRFieldDefn *poSrcDefn =
                        poOFGBLayerDefn->GetFieldDefn(i);
                    if ((poSrcDefn->GetType() == OFTInteger ||
                         poSrcDefn->GetType() == OFTReal) &&
                        poSrcDefn->GetDefault() != nullptr)
                    {
                        int nIdxDst = m_pFeatureDefn->GetFieldIndex(
                            poSrcDefn->GetNameRef());
                        if (nIdxDst >= 0)
                            m_pFeatureDefn->GetFieldDefn(nIdxDst)->SetDefault(
                                poSrcDefn->GetDefault());
                    }

                    // XML parsing by the SDK fails when there are special
                    // characters, like &, so fallback to using OpenFileGDB.
                    const char *pszAlternativeName =
                        poSrcDefn->GetAlternativeNameRef();
                    if (pszAlternativeName != nullptr &&
                        pszAlternativeName[0] != '\0' &&
                        strcmp(pszAlternativeName, poSrcDefn->GetNameRef()) !=
                            0)
                    {
                        int nIdxDst = m_pFeatureDefn->GetFieldIndex(
                            poSrcDefn->GetNameRef());
                        if (nIdxDst >= 0)
                            m_pFeatureDefn->GetFieldDefn(nIdxDst)
                                ->SetAlternativeName(pszAlternativeName);
                    }
                }
            }
            GDALClose(poDS);
        }
    }

    return true;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void FGdbLayer::ResetReading()
{
    long hr;

    if (m_pTable == nullptr)
        return;

    EndBulkLoad();

#ifdef WORKAROUND_CRASH_ON_CDF_WITH_BINARY_FIELD
    const auto wstrSubFieldBackup = m_wstrSubfields;
    if (!m_apoByteArrays.empty())
    {
        m_bWorkaroundCrashOnCDFWithBinaryField = CPLTestBool(CPLGetConfigOption(
            "OGR_FGDB_WORKAROUND_CRASH_ON_BINARY_FIELD", "YES"));
        if (m_bWorkaroundCrashOnCDFWithBinaryField)
        {
            m_wstrSubfields = StringToWString(m_strOIDFieldName);
            if (!m_strShapeFieldName.empty() && m_poFilterGeom &&
                !m_poFilterGeom->IsEmpty())
            {
                m_wstrSubfields += StringToWString(", " + m_strShapeFieldName);
            }
        }
    }
#endif

    if (m_poFilterGeom && !m_poFilterGeom->IsEmpty())
    {
        // Search spatial
        // As of beta1, FileGDB only supports bbox searched, if we have GEOS
        // installed, we can do the rest ourselves.

        OGREnvelope ogrEnv;

        m_poFilterGeom->getEnvelope(&ogrEnv);

        // spatial query
        FileGDBAPI::Envelope env(ogrEnv.MinX, ogrEnv.MaxX, ogrEnv.MinY,
                                 ogrEnv.MaxY);

        if (FAILED(hr = m_pTable->Search(m_wstrSubfields, m_wstrWhereClause,
                                         env, true, *m_pEnumRows)))
            GDBErr(hr, "Failed Searching");
    }
    else
    {
        // Search non-spatial
        if (FAILED(hr = m_pTable->Search(m_wstrSubfields, m_wstrWhereClause,
                                         true, *m_pEnumRows)))
            GDBErr(hr, "Failed Searching");
    }

#ifdef WORKAROUND_CRASH_ON_CDF_WITH_BINARY_FIELD
    if (!m_apoByteArrays.empty() && m_bWorkaroundCrashOnCDFWithBinaryField)
        m_wstrSubfields = wstrSubFieldBackup;
#endif

    m_bFilterDirty = false;
}

/************************************************************************/
/*                         SetSpatialFilter()                           */
/************************************************************************/

void FGdbLayer::SetSpatialFilter(OGRGeometry *pOGRGeom)
{
    m_bFilterDirty = true;
    OGRLayer::SetSpatialFilter(pOGRGeom);
}

/************************************************************************/
/*                             ResyncIDs()                              */
/************************************************************************/

void FGdbLayer::ResyncIDs()
{
    if (m_oMapOGRFIDToFGDBFID.empty())
        return;
    if (m_pDS->CloseInternal())
        m_pDS->ReOpen();
}

/************************************************************************/
/*                         SetAttributeFilter()                         */
/************************************************************************/

OGRErr FGdbLayer::SetAttributeFilter(const char *pszQuery)
{
    if (pszQuery != nullptr &&
        CPLString(pszQuery).ifind(GetFIDColumn()) != std::string::npos)
        ResyncIDs();

    m_wstrWhereClause = StringToWString((pszQuery != nullptr) ? pszQuery : "");

    m_bFilterDirty = true;

    return OGRERR_NONE;
}

/************************************************************************/
/*                           OGRFeatureFromGdbRow()                      */
/************************************************************************/

bool FGdbBaseLayer::OGRFeatureFromGdbRow(Row *pRow, OGRFeature **ppFeature)
{
    long hr;

    OGRFeature *pOutFeature = new OGRFeature(m_pFeatureDefn);

    /////////////////////////////////////////////////////////
    // Translate OID
    //

    int32 oid = -1;
    if (FAILED(hr = pRow->GetOID(oid)))
    {
        // this should never happen unless not selecting the OBJECTID
    }
    else
    {
        pOutFeature->SetFID(oid);
    }

    /////////////////////////////////////////////////////////
    // Translate Geometry
    //

    ShapeBuffer gdbGeometry;
    // Row::GetGeometry() will fail with -2147467259 for NULL geometries
    // Row::GetGeometry() will fail with -2147219885 for tables without a
    // geometry field
    if (!m_pFeatureDefn->IsGeometryIgnored() &&
        !FAILED(hr = pRow->GetGeometry(gdbGeometry)))
    {
        OGRGeometry *pOGRGeo = nullptr;

        if ((!GDBGeometryToOGRGeometry(m_forceMulti, &gdbGeometry, m_pSRS,
                                       &pOGRGeo)))
        {
            delete pOutFeature;
            return GDBErr(hr, "Failed to translate FileGDB Geometry to OGR "
                              "Geometry for row " +
                                  string(CPLSPrintf("%d", (int)oid)));
        }

        pOutFeature->SetGeometryDirectly(pOGRGeo);
    }

    //////////////////////////////////////////////////////////
    // Map fields
    //

    int mappedFieldCount = static_cast<int>(m_vOGRFieldToESRIField.size());

    bool foundBadColumn = false;

    for (int i = 0; i < mappedFieldCount; ++i)
    {
        OGRFieldDefn *poFieldDefn = m_pFeatureDefn->GetFieldDefn(i);
        // The IsNull() and GetXXX() API are very slow when there are a
        // big number of fields, for example with Tiger database.
        if (poFieldDefn->IsIgnored())
            continue;

        const wstring &wstrFieldName = m_vOGRFieldToESRIField[i];
        const std::string &strFieldType = m_vOGRFieldToESRIFieldType[i];

        bool isNull = false;

        if (FAILED(hr = pRow->IsNull(wstrFieldName, isNull)))
        {
            GDBErr(hr, "Failed to determine NULL status from column " +
                           WStringToString(wstrFieldName));
            foundBadColumn = true;
            continue;
        }

        if (isNull)
        {
            pOutFeature->SetFieldNull(i);
            continue;
        }

        //
        // NOTE: This switch statement needs to be kept in sync with
        // GDBToOGRFieldType utility function
        //       since we are only checking for types we mapped in that utility
        //       function

        switch (poFieldDefn->GetType())
        {

            case OFTInteger:
            {
                int32 val;

                if (FAILED(hr = pRow->GetInteger(wstrFieldName, val)))
                {
                    int16 shortval;
                    if (FAILED(hr = pRow->GetShort(wstrFieldName, shortval)))
                    {
                        GDBErr(hr,
                               "Failed to determine integer value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }
                    val = shortval;
                }

                pOutFeature->SetField(i, (int)val);
            }
            break;

            case OFTReal:
            {
                if (strFieldType == "esriFieldTypeSingle")
                {
                    float val;

                    if (FAILED(hr = pRow->GetFloat(wstrFieldName, val)))
                    {
                        GDBErr(hr,
                               "Failed to determine float value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }

                    pOutFeature->SetField(i, val);
                }
                else
                {
                    double val;

                    if (FAILED(hr = pRow->GetDouble(wstrFieldName, val)))
                    {
                        GDBErr(hr,
                               "Failed to determine real value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }

                    pOutFeature->SetField(i, val);
                }
            }
            break;
            case OFTString:
            {
                wstring val;
                std::string strValue;

                if (strFieldType == "esriFieldTypeGlobalID")
                {
                    Guid guid;
                    if (FAILED(hr = pRow->GetGlobalID(guid)) ||
                        FAILED(hr = guid.ToString(val)))
                    {
                        GDBErr(hr,
                               "Failed to determine string value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }
                    strValue = WStringToString(val);
                }
                else if (strFieldType == "esriFieldTypeGUID")
                {
                    Guid guid;
                    if (FAILED(hr = pRow->GetGUID(wstrFieldName, guid)) ||
                        FAILED(hr = guid.ToString(val)))
                    {
                        GDBErr(hr,
                               "Failed to determine string value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }
                    strValue = WStringToString(val);
                }
                else if (strFieldType == "esriFieldTypeXML")
                {
                    if (FAILED(hr = pRow->GetXML(wstrFieldName, strValue)))
                    {
                        GDBErr(hr, "Failed to determine XML value for column " +
                                       WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }
                }
                else
                {
                    if (FAILED(hr = pRow->GetString(wstrFieldName, val)))
                    {
                        GDBErr(hr,
                               "Failed to determine string value for column " +
                                   WStringToString(wstrFieldName));
                        foundBadColumn = true;
                        continue;
                    }
                    strValue = WStringToString(val);
                }

                pOutFeature->SetField(i, strValue.c_str());
            }
            break;

            case OFTBinary:
            {
                ByteArray binaryBuf;

                if (FAILED(hr = pRow->GetBinary(wstrFieldName, binaryBuf)))
                {
                    GDBErr(hr, "Failed to determine binary value for column " +
                                   WStringToString(wstrFieldName));
                    foundBadColumn = true;
                    continue;
                }

                pOutFeature->SetField(i, (int)binaryBuf.inUseLength,
                                      (GByte *)binaryBuf.byteArray);
            }
            break;

            case OFTDateTime:
            {
                struct tm val;

                if (FAILED(hr = pRow->GetDate(wstrFieldName, val)))
                {
                    GDBErr(hr, "Failed to determine date value for column " +
                                   WStringToString(wstrFieldName));
                    foundBadColumn = true;
                    continue;
                }

                pOutFeature->SetField(i, val.tm_year + 1900, val.tm_mon + 1,
                                      val.tm_mday, val.tm_hour, val.tm_min,
                                      (float)val.tm_sec,
                                      m_bTimeInUTC ? 100 : 0);
                // Examine test data to figure out how to extract that
            }
            break;

            default:
            {
                if (!m_suppressColumnMappingError)
                {
                    foundBadColumn = true;
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Row id: %d col:%d has unhandled col type (%d). "
                             "Setting to NULL.",
                             (int)oid, (int)i,
                             m_pFeatureDefn->GetFieldDefn(i)->GetType());
                }
            }
        }
    }

    if (foundBadColumn)
        m_suppressColumnMappingError = true;

    *ppFeature = pOutFeature;

    return true;
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *FGdbLayer::GetNextFeature()
{
    if (m_bFilterDirty)
        ResetReading();

    EndBulkLoad();

#ifdef WORKAROUND_CRASH_ON_CDF_WITH_BINARY_FIELD
    if (!m_apoByteArrays.empty() && m_bWorkaroundCrashOnCDFWithBinaryField)
    {
        while (true)
        {
            if (m_pEnumRows == nullptr)
                return nullptr;

            long hr;

            Row rowOnlyOid;

            if (FAILED(hr = m_pEnumRows->Next(rowOnlyOid)))
            {
                GDBErr(hr, "Failed fetching features");
                return nullptr;
            }

            if (hr != S_OK)
            {
                // It's OK, we are done fetching - failure is caught by FAILED
                // macro
                return nullptr;
            }

            int32 oid = -1;
            if (FAILED(hr = rowOnlyOid.GetOID(oid)))
            {
                GDBErr(hr, "Failed to get oid");
                continue;
            }

            EnumRows enumRows;
            OGRFeature *pOGRFeature = nullptr;
            Row rowFull;
            if (GetRow(enumRows, rowFull, oid) != OGRERR_NONE ||
                !OGRFeatureFromGdbRow(&rowFull, &pOGRFeature))
            {
                GDBErr(hr,
                       CPLSPrintf(
                           "Failed translating FGDB row [%d] to OGR Feature",
                           oid));

                // return NULL;
                continue;  // skip feature
            }

            if ((m_poFilterGeom == nullptr ||
                 FilterGeometry(pOGRFeature->GetGeometryRef())))
            {
                return pOGRFeature;
            }
            delete pOGRFeature;
        }
    }
#endif

    OGRFeature *poFeature = FGdbBaseLayer::GetNextFeature();
    if (poFeature)
    {
        std::map<int, int>::iterator oIter =
            m_oMapFGDBFIDToOGRFID.find((int)poFeature->GetFID());
        if (oIter != m_oMapFGDBFIDToOGRFID.end())
        {
            poFeature->SetFID(oIter->second);
        }
    }
    return poFeature;
}

/************************************************************************/
/*                             GetFeature()                             */
/************************************************************************/

OGRFeature *FGdbLayer::GetFeature(GIntBig oid)
{
    // do query to fetch individual row
    EnumRows enumRows;
    Row row;
    if (!CPL_INT64_FITS_ON_INT32(oid) || m_pTable == nullptr)
        return nullptr;

    EndBulkLoad();

    int nFID32 = (int)oid;
    std::map<int, int>::iterator oIter = m_oMapOGRFIDToFGDBFID.find(nFID32);
    if (oIter != m_oMapOGRFIDToFGDBFID.end())
        nFID32 = oIter->second;
    else if (m_oMapFGDBFIDToOGRFID.find(nFID32) != m_oMapFGDBFIDToOGRFID.end())
        return nullptr;

    if (GetRow(enumRows, row, nFID32) != OGRERR_NONE)
        return nullptr;

    OGRFeature *pOGRFeature = nullptr;

    if (!OGRFeatureFromGdbRow(&row, &pOGRFeature))
    {
        return nullptr;
    }
    if (pOGRFeature)
    {
        pOGRFeature->SetFID(oid);
    }

    return pOGRFeature;
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/************************************************************************/

GIntBig FGdbLayer::GetFeatureCount(CPL_UNUSED int bForce)
{
    int32 rowCount = 0;

    if (m_pTable == nullptr)
        return 0;

    EndBulkLoad();

    if (m_poFilterGeom != nullptr || !m_wstrWhereClause.empty())
    {
        ResetReading();
        if (m_pEnumRows == nullptr)
            return 0;

        int nFeatures = 0;
        while (true)
        {
            long hr;

            Row row;

            if (FAILED(hr = m_pEnumRows->Next(row)))
            {
                GDBErr(hr, "Failed fetching features");
                return 0;
            }

            if (hr != S_OK)
            {
                break;
            }

            if (m_poFilterGeom == nullptr)
            {
                nFeatures++;
            }
            else
            {
                ShapeBuffer gdbGeometry;
                if (FAILED(hr = row.GetGeometry(gdbGeometry)))
                {
                    continue;
                }

                OGRGeometry *pOGRGeo = nullptr;
                if (!GDBGeometryToOGRGeometry(m_forceMulti, &gdbGeometry,
                                              m_pSRS, &pOGRGeo) ||
                    pOGRGeo == nullptr)
                {
                    delete pOGRGeo;
                    continue;
                }

                if (FilterGeometry(pOGRGeo))
                {
                    nFeatures++;
                }

                delete pOGRGeo;
            }
        }
        ResetReading();
        return nFeatures;
    }

    long hr;
    if (FAILED(hr = m_pTable->GetRowCount(rowCount)))
    {
        GDBErr(hr, "Failed counting rows");
        return 0;
    }

    return static_cast<int>(rowCount);
}

/************************************************************************/
/*                         GetMetadataItem()                            */
/************************************************************************/

const char *FGdbLayer::GetMetadataItem(const char *pszName,
                                       const char *pszDomain)
{
    if (pszDomain != nullptr && EQUAL(pszDomain, "MAP_OGR_FID_TO_FGDB_FID"))
    {
        if (m_oMapOGRFIDToFGDBFID.find(atoi(pszName)) !=
            m_oMapOGRFIDToFGDBFID.end())
            return CPLSPrintf("%d", m_oMapOGRFIDToFGDBFID[atoi(pszName)]);
    }
    else if (pszDomain != nullptr &&
             EQUAL(pszDomain, "MAP_FGDB_FID_TO_OGR_FID"))
    {
        if (m_oMapFGDBFIDToOGRFID.find(atoi(pszName)) !=
            m_oMapFGDBFIDToOGRFID.end())
            return CPLSPrintf("%d", m_oMapFGDBFIDToOGRFID[atoi(pszName)]);
    }
    return OGRLayer::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                             GetExtent()                              */
/************************************************************************/

OGRErr FGdbLayer::GetExtent(OGREnvelope *psExtent, int bForce)
{
    if (m_pTable == nullptr)
        return OGRERR_FAILURE;

    if (m_poFilterGeom != nullptr || !m_wstrWhereClause.empty() ||
        m_strShapeFieldName.empty())
    {
        const int nFieldCount = m_pFeatureDefn->GetFieldCount();
        int *pabSaveFieldIgnored = new int[nFieldCount];
        for (int i = 0; i < nFieldCount; i++)
        {
            // cppcheck-suppress uninitdata
            pabSaveFieldIgnored[i] =
                m_pFeatureDefn->GetFieldDefn(i)->IsIgnored();
            m_pFeatureDefn->GetFieldDefn(i)->SetIgnored(TRUE);
        }
        OGRErr eErr = OGRLayer::GetExtent(psExtent, bForce);
        for (int i = 0; i < nFieldCount; i++)
        {
            m_pFeatureDefn->GetFieldDefn(i)->SetIgnored(pabSaveFieldIgnored[i]);
        }
        delete[] pabSaveFieldIgnored;
        return eErr;
    }

    long hr;
    Envelope envelope;
    if (FAILED(hr = m_pTable->GetExtent(envelope)))
    {
        GDBErr(hr, "Failed fetching extent");
        return OGRERR_FAILURE;
    }

    psExtent->MinX = envelope.xMin;
    psExtent->MinY = envelope.yMin;
    psExtent->MaxX = envelope.xMax;
    psExtent->MaxY = envelope.yMax;

    if (CPLIsNan(psExtent->MinX) || CPLIsNan(psExtent->MinY) ||
        CPLIsNan(psExtent->MaxX) || CPLIsNan(psExtent->MaxY))
        return OGRERR_FAILURE;

    return OGRERR_NONE;
}

/************************************************************************/
/*                          StartBulkLoad()                             */
/************************************************************************/

void FGdbLayer::StartBulkLoad()
{
    if (!m_pTable)
        return;

    if (m_bBulkLoadInProgress)
        return;

    m_bBulkLoadInProgress = TRUE;
    m_pTable->LoadOnlyMode(true);
    m_pTable->SetWriteLock();
}

/************************************************************************/
/*                           EndBulkLoad()                              */
/************************************************************************/

void FGdbLayer::EndBulkLoad()
{
    if (!m_pTable)
        return;

    if (!m_bBulkLoadInProgress)
        return;

    m_bBulkLoadInProgress = FALSE;
    m_bBulkLoadAllowed = -1; /* so that the configuration option is read the
                                first time we CreateFeature() again */
    m_pTable->LoadOnlyMode(false);
    m_pTable->FreeWriteLock();
}

/* OGRErr FGdbLayer::StartTransaction ()
{
    if ( ! m_pTable )
        return OGRERR_FAILURE;

    m_pTable->LoadOnlyMode(true);
    m_pTable->SetWriteLock();
    return OGRERR_NONE;
} */

/* OGRErr FGdbLayer::CommitTransaction ()
{
    if ( ! m_pTable )
        return OGRERR_FAILURE;

    m_pTable->LoadOnlyMode(false);
    m_pTable->FreeWriteLock();
    return OGRERR_NONE;
} */

/* OGRErr FGdbLayer::RollbackTransaction ()
{
    if ( ! m_pTable )
        return OGRERR_FAILURE;

    m_pTable->LoadOnlyMode(false);
    m_pTable->FreeWriteLock();
    return OGRERR_NONE;
} */

/************************************************************************/
/*                           GetLayerXML()                              */
/* Return XML definition of the Layer as provided by FGDB. Caller must  */
/* free result.                                                         */
/* Not currently used by the driver, but can be used by external code   */
/* for specific purposes.                                               */
/************************************************************************/

OGRErr FGdbLayer::GetLayerXML(char **ppXml)
{
    long hr;
    std::string xml;

    if (m_pTable == nullptr)
        return OGRERR_FAILURE;

    if (FAILED(hr = m_pTable->GetDefinition(xml)))
    {
        GDBErr(hr, "Failed fetching XML table definition");
        return OGRERR_FAILURE;
    }

    *ppXml = CPLStrdup(xml.c_str());
    return OGRERR_NONE;
}

/************************************************************************/
/*                           GetLayerMetadataXML()                      */
/* Return XML metadata for the Layer as provided by FGDB. Caller must  */
/* free result.                                                         */
/* Not currently used by the driver, but can be used by external code   */
/* for specific purposes.                                               */
/************************************************************************/

OGRErr FGdbLayer::GetLayerMetadataXML(char **ppXml)
{
    long hr;
    std::string xml;

    if (m_pTable == nullptr)
        return OGRERR_FAILURE;

    if (FAILED(hr = m_pTable->GetDocumentation(xml)))
    {
        GDBErr(hr, "Failed fetching XML table metadata");
        return OGRERR_FAILURE;
    }

    *ppXml = CPLStrdup(xml.c_str());
    return OGRERR_NONE;
}

/************************************************************************/
/*                           Rename()                                   */
/************************************************************************/

OGRErr FGdbLayer::Rename(const char *pszDstTableName)
{
    if (!TestCapability(OLCRename))
        return OGRERR_FAILURE;

    if (m_pTable == nullptr)
        return OGRERR_FAILURE;

    if (m_pDS->GetLayerByName(pszDstTableName) != nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Layer %s already exists",
                 pszDstTableName);
        return OGRERR_FAILURE;
    }

    long hr = m_pDS->GetGDB()->Rename(m_wstrTablePath, m_wstrType,
                                      StringToWString(pszDstTableName));

    if (FAILED(hr))
    {
        GDBErr(hr, "Failed renaming layer");
        return OGRERR_FAILURE;
    }

    m_strName = pszDstTableName;
    auto strTablePath = WStringToString(m_wstrTablePath);
    m_wstrTablePath =
        StringToWString(strTablePath.substr(0, strTablePath.rfind('\\')) +
                        "\\" + pszDstTableName);
    SetDescription(pszDstTableName);
    m_pFeatureDefn->SetName(pszDstTableName);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int FGdbLayer::TestCapability(const char *pszCap)
{

    if (EQUAL(pszCap, OLCRandomRead))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastFeatureCount))
        return m_poFilterGeom == nullptr && m_wstrWhereClause.empty();

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
        return TRUE;

    else if (EQUAL(pszCap, OLCFastGetExtent))
        return m_poFilterGeom == nullptr && m_wstrWhereClause.empty();

    else if (EQUAL(pszCap, OLCCreateField)) /* CreateField() */
        return m_pDS->GetUpdate();

    else if (EQUAL(pszCap, OLCSequentialWrite)) /* ICreateFeature() */
        return m_pDS->GetUpdate();

    else if (EQUAL(pszCap,
                   OLCStringsAsUTF8)) /* Native UTF16, converted to UTF8 */
        return TRUE;

    else if (EQUAL(pszCap, OLCDeleteFeature)) /* DeleteFeature() */
        return m_pDS->GetUpdate();

    else if (EQUAL(pszCap, OLCRandomWrite)) /* ISetFeature() */
        return m_pDS->GetUpdate();

    else if (EQUAL(pszCap, OLCDeleteField)) /* DeleteField() */
        return m_pDS->GetUpdate();

#ifdef AlterFieldDefn_implemented_but_not_working
    else if (EQUAL(pszCap, OLCAlterFieldDefn)) /* AlterFieldDefn() */
        return m_pDS->GetUpdate();
#endif

    else if (EQUAL(pszCap, OLCRename)) /* Rename() */
        return m_pDS->GetUpdate();

    else if (EQUAL(pszCap,
                   OLCFastSetNextByIndex)) /* TBD FastSetNextByIndex() */
        return FALSE;

    else if (EQUAL(pszCap, OLCTransactions)) /* TBD Start/End Transactions() */
        return FALSE;

    else if (EQUAL(pszCap, OLCIgnoreFields))
        return TRUE;

    else if (EQUAL(pszCap, OLCMeasuredGeometries))
        return TRUE;

    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    else
        return FALSE;
}

/************************************************************************/
/*                           CreateRealCopy()                           */
/************************************************************************/

int FGdbLayer::CreateRealCopy()
{
    CPLAssert(m_bSymlinkFlag);

    // Find the FID of the layer in the system catalog
    char *apszDrivers[2] = {nullptr};
    apszDrivers[0] = (char *)"OpenFileGDB";
    apszDrivers[1] = nullptr;
    const char *pszSystemCatalog =
        CPLFormFilename(m_pDS->GetFSName(), "a00000001.gdbtable", nullptr);
    GDALDataset *poOpenFileGDBDS = (GDALDataset *)GDALOpenEx(
        pszSystemCatalog, GDAL_OF_VECTOR, apszDrivers, nullptr, nullptr);
    if (poOpenFileGDBDS == nullptr || poOpenFileGDBDS->GetLayer(0) == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot open %s with OpenFileGDB driver. Should not happen.",
                 pszSystemCatalog);
        GDALClose(poOpenFileGDBDS);
        return FALSE;
    }

    OGRLayer *poLayer = poOpenFileGDBDS->GetLayer(0);
    CPLString osFilter = "name = '";
    osFilter += GetName();
    osFilter += "'";
    poLayer->SetAttributeFilter(osFilter);
    poLayer->ResetReading();
    OGRFeature *poF = poLayer->GetNextFeature();
    if (poF == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot find filename for layer %s", GetName());
        GDALClose(poOpenFileGDBDS);
        return FALSE;
    }
    int nLayerFID = (int)poF->GetFID();
    delete poF;
    GDALClose(poOpenFileGDBDS);

    if (!m_pDS->CloseInternal(TRUE))
        return FALSE;

    // Create real copies (in .tmp files now) instead of symlinks
    char **papszFiles = VSIReadDir(m_pDS->GetFSName());
    CPLString osBasename(CPLSPrintf("a%08x", nLayerFID));
    int bError = FALSE;
    std::vector<CPLString> aoFiles;
    for (char **papszIter = papszFiles; !bError && papszIter && *papszIter;
         papszIter++)
    {
        if (strncmp(*papszIter, osBasename.c_str(), osBasename.size()) == 0)
        {
            if (CPLCopyFile(
                    CPLFormFilename(m_pDS->GetFSName(), *papszIter, "tmp"),
                    CPLFormFilename(m_pDS->GetFSName(), *papszIter, nullptr)) !=
                0)
            {
                CPLError(CE_Failure, CPLE_AppDefined, "Cannot copy %s",
                         *papszIter);
                bError = TRUE;
            }
            else
                aoFiles.push_back(*papszIter);
        }
    }
    CSLDestroy(papszFiles);

    // Rename the .tmp into normal filenames
    for (size_t i = 0; !bError && i < aoFiles.size(); i++)
    {
        if (VSIUnlink(CPLFormFilename(m_pDS->GetFSName(), aoFiles[i],
                                      nullptr)) != 0 ||
            VSIRename(
                CPLFormFilename(m_pDS->GetFSName(), aoFiles[i], "tmp"),
                CPLFormFilename(m_pDS->GetFSName(), aoFiles[i], nullptr)) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot rename %s.tmp",
                     aoFiles[i].c_str());
            bError = TRUE;
        }
    }

    int bRet = !bError && m_pDS->ReOpen();
    if (bRet)
        m_bSymlinkFlag = FALSE;
    return bRet;
}
