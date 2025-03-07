#!/usr/bin/env pytest
###############################################################################
# $Id$
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test GFF driver
# Author:   Even Rouault, <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2009, Even Rouault <even dot rouault at spatialys.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included
# in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
# OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.
###############################################################################

import gdaltest

from osgeo import gdal

###############################################################################
# Test an extract from a real dataset


def test_gff_1():
    # 12088 = 2048 + 8 * 1255
    gdaltest.download_or_skip(
        "http://sandia.gov/RADAR/complex_data/MiniSAR20050519p0001image008.gff",
        "MiniSAR20050519p0001image008.gff",
        12088,
    )

    tst = gdaltest.GDALTest(
        "GFF",
        "tmp/cache/MiniSAR20050519p0001image008.gff",
        1,
        -1,
        filename_absolute=1,
    )
    gdal.PushErrorHandler("CPLQuietErrorHandler")
    ret = tst.testOpen()
    gdal.PopErrorHandler()
    return ret
