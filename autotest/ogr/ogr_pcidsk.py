#!/usr/bin/env pytest
###############################################################################
# $Id$
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test PCIDSK driver functionality.
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2011, Even Rouault <even dot rouault at spatialys.com>
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
import ogrtest
import pytest

from osgeo import gdal, ogr, osr

wkts = [
    ("POINT (0 1 2)", "points", 0),
    ("LINESTRING (0 1 2,3 4 5)", "lines", 0),
    ("POINT (0 1 2)", "points2", 4326),
    ("LINESTRING (0 1 2,3 4 5)", "lines2", 32631),
]

###############################################################################
# Test creation


def test_ogr_pcidsk_1():

    ogr_drv = ogr.GetDriverByName("PCIDSK")
    if ogr_drv is None:
        pytest.skip()

    ds = ogr_drv.CreateDataSource("tmp/ogr_pcidsk_1.pix")

    lyr = ds.CreateLayer("nothing", geom_type=ogr.wkbNone)
    feat = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(feat)

    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    assert feat is not None

    lyr = ds.CreateLayer("fields", geom_type=ogr.wkbNone)
    lyr.CreateField(ogr.FieldDefn("strfield", ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn("intfield", ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn("realfield", ogr.OFTReal))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField(0, "foo2")
    feat.SetField(1, 1)
    feat.SetField(2, 3.45)
    lyr.CreateFeature(feat)

    feat.SetField(0, "foo")
    lyr.SetFeature(feat)

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField(0, "bar")
    lyr.CreateFeature(feat)

    assert lyr.GetFeatureCount() == 2

    lyr.DeleteFeature(1)

    assert lyr.GetFeatureCount() == 1

    lyr.ResetReading()
    feat = lyr.GetNextFeature()
    assert feat is not None
    assert feat.GetField(0) == "foo"
    assert feat.GetField(1) == 1
    assert feat.GetField(2) == 3.45

    for (wkt, layername, epsgcode) in wkts:
        geom = ogr.CreateGeometryFromWkt(wkt)
        if epsgcode != 0:
            srs = osr.SpatialReference()
            srs.ImportFromEPSG(epsgcode)
        else:
            srs = None
        lyr = ds.CreateLayer(layername, geom_type=geom.GetGeometryType(), srs=srs)
        feat = ogr.Feature(lyr.GetLayerDefn())
        feat.SetGeometry(geom)
        lyr.CreateFeature(feat)

        lyr.ResetReading()
        feat = lyr.GetNextFeature()
        assert feat is not None, layername
        if feat.GetGeometryRef().ExportToWkt() != wkt:
            feat.DumpReadable()
            pytest.fail(layername)

    ds = None


###############################################################################
# Test reading


def test_ogr_pcidsk_2():

    ogr_drv = ogr.GetDriverByName("PCIDSK")
    if ogr_drv is None:
        pytest.skip()

    ds = ogr.Open("tmp/ogr_pcidsk_1.pix")
    assert ds.GetLayerCount() == 2 + len(wkts)

    lyr = ds.GetLayerByName("nothing")
    assert lyr.GetGeomType() == ogr.wkbNone
    feat = lyr.GetNextFeature()
    assert feat is not None

    lyr = ds.GetLayerByName("fields")
    feat = lyr.GetNextFeature()
    assert feat is not None
    assert feat.GetField(0) == "foo"
    assert feat.GetField(1) == 1
    assert feat.GetField(2) == 3.45

    for (wkt, layername, epsgcode) in wkts:
        geom = ogr.CreateGeometryFromWkt(wkt)
        lyr = ds.GetLayerByName(layername)
        assert lyr.GetGeomType() == geom.GetGeometryType(), layername

        srs = lyr.GetSpatialRef()
        if epsgcode != 0:
            ref_srs = osr.SpatialReference()
            ref_srs.ImportFromEPSG(epsgcode)
            assert srs is not None and ref_srs.IsSame(srs) == 1, layername

        feat = lyr.GetNextFeature()
        assert feat is not None, layername
        if feat.GetGeometryRef().ExportToWkt() != wkt:
            feat.DumpReadable()
            pytest.fail(layername)

    ds = None


###############################################################################
# Check with test_ogrsf


def test_ogr_pcidsk_3():

    import test_cli_utilities

    if test_cli_utilities.get_test_ogrsf_path() is None:
        pytest.skip()

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    ret = gdaltest.runexternal(
        test_cli_utilities.get_test_ogrsf_path() + " tmp/ogr_pcidsk_1.pix"
    )

    if ret.find("ERROR: The feature was not deleted") != -1:
        # Expected fail for now
        print("ERROR: The feature was not deleted")
        ret = ret.replace(
            "ERROR: The feature was not deleted", "ARGHH: The feature was not deleted"
        )
        ret = ret.replace(
            "ERROR: Attempt to restore feature failed",
            "ARGHH: Attempt to restore feature failed",
        )
    if ret.find("ERROR") == ret.find("ERROR ret code = 1"):
        ret = ret.replace("ERROR ret code = 1", "")
    assert "INFO" in ret
    assert "ERROR" not in ret


###############################################################################
# Test that we cannot open a raster only pcidsk in read-only mode


def test_ogr_pcidsk_4():

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    if gdal.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    ds = ogr.Open("../gdrivers/data/utm.pix")
    assert ds is None
    ds = None


###############################################################################
# Test that we can open a raster only pcidsk in update mode


def test_ogr_pcidsk_5():

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    if gdal.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    ds = ogr.Open("../gdrivers/data/pcidsk/utm.pix", update=1)
    assert ds is not None
    ds = None


###############################################################################


def test_ogr_pcidsk_add_field_to_non_empty_layer():

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    tmpfile = "/vsimem/tmp.pix"
    ds = ogr.GetDriverByName("PCIDSK").CreateDataSource(tmpfile)
    lyr = ds.CreateLayer("foo")
    lyr.CreateField(ogr.FieldDefn("foo", ogr.OFTString))
    f = ogr.Feature(lyr.GetLayerDefn())
    f["foo"] = "bar"
    lyr.CreateFeature(f)
    f = None
    with gdaltest.error_handler():
        assert lyr.CreateField(ogr.FieldDefn("bar", ogr.OFTString)) != 0
    f = ogr.Feature(lyr.GetLayerDefn())
    f["foo"] = "bar2"
    lyr.CreateFeature(f)
    f = None
    ds = None

    ogr.GetDriverByName("PCIDSK").DeleteDataSource(tmpfile)


###############################################################################


def test_ogr_pcidsk_too_many_layers():

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    tmpfile = "/vsimem/tmp.pix"
    ds = ogr.GetDriverByName("PCIDSK").CreateDataSource(tmpfile)
    for i in range(1023):
        ds.CreateLayer("foo%d" % i)
    with gdaltest.error_handler():
        assert ds.CreateLayer("foo") is None
    ds = None

    ogr.GetDriverByName("PCIDSK").DeleteDataSource(tmpfile)


###############################################################################
# Check a polygon layer


def test_ogr_pcidsk_online_1():

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    gdaltest.download_or_skip(
        "http://download.osgeo.org/gdal/data/pcidsk/sdk_testsuite/polygon.pix",
        "polygon.pix",
    )

    ds = ogr.Open("tmp/cache/polygon.pix")
    assert ds is not None

    lyr = ds.GetLayer(0)
    assert lyr is not None

    feat = lyr.GetNextFeature()
    assert feat is not None

    geom = "POLYGON ((479819.84375 4765180.5 0,479690.1875 4765259.5 0,479647.0 4765369.5 0,479730.375 4765400.5 0,480039.03125 4765539.5 0,480035.34375 4765558.5 0,480159.78125 4765610.5 0,480202.28125 4765482.0 0,480365.0 4765015.5 0,480389.6875 4764950.0 0,480133.96875 4764856.5 0,480080.28125 4764979.5 0,480082.96875 4765049.5 0,480088.8125 4765139.5 0,480059.90625 4765239.5 0,480019.71875 4765319.5 0,479980.21875 4765409.5 0,479909.875 4765370.0 0,479859.875 4765270.0 0,479819.84375 4765180.5 0))"
    if ogrtest.check_feature_geometry(feat, geom) != 0:
        feat.DumpReadable()
        pytest.fail()


###############################################################################
# Check a polygon layer


def test_ogr_pcidsk_online_2():

    import test_cli_utilities

    if test_cli_utilities.get_test_ogrsf_path() is None:
        pytest.skip()

    if ogr.GetDriverByName("PCIDSK") is None:
        pytest.skip()

    gdaltest.download_or_skip(
        "http://download.osgeo.org/gdal/data/pcidsk/sdk_testsuite/polygon.pix",
        "polygon.pix",
    )

    ret = gdaltest.runexternal(
        test_cli_utilities.get_test_ogrsf_path() + " -ro tmp/cache/polygon.pix"
    )

    assert ret.find("INFO") != -1 and ret.find("ERROR") == -1


###############################################################################
# Cleanup


def test_ogr_pcidsk_cleanup():

    gdal.Unlink("tmp/ogr_pcidsk_1.pix")
