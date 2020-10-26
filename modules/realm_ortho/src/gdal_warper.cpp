/**
* This file is part of OpenREALM.
*
* Copyright (C) 2018 Alexander Kern <laxnpander at gmail dot com> (Braunschweig University of Technology)
* For more information see <https://github.com/laxnpander/OpenREALM>
*
* OpenREALM is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* OpenREALM is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with OpenREALM. If not, see <http://www.gnu.org/licenses/>.
*/

#include <gdal_warper.h>

#include <opencv2/highgui.hpp>

using namespace realm;

gis::GdalWarper::GdalWarper()
 : _epsg_target(0)
{
  GDALAllRegister();

  // Warping is done in RAM and no data needs to be saved to the disk for now
  _driver = GetGDALDriverManager()->GetDriverByName("MEM");
}

void gis::GdalWarper::setTargetEPSG(int epsg_code)
{
  _epsg_target = epsg_code;
}

CvGridMap::Ptr gis::GdalWarper::warpMap(const CvGridMap &map, uint8_t zone)
{
  //=======================================//
  //
  //      Step 1: Check validity
  //
  //=======================================//

  if (_epsg_target == 0)
    throw(std::runtime_error("Error warping map: Target EPSG was not set!"));

  std::vector<std::string> layer_names = map.getAllLayerNames();
  if (layer_names.size() > 1)
    throw(std::invalid_argument("Error warping map: There is more than one layer in the map. This is currently not supported."));

  //=======================================//
  //
  //      Step 2: Prepare datasets
  //
  //=======================================//

  // Extract data and metadata
  cv::Mat data = map[layer_names[0]];
  io::GDALDatasetMeta* meta = io::computeGDALDatasetMeta(map, zone);

  // Convert the source map into a GDAL dataset
  GDALDataset* dataset_mem_src;
  dataset_mem_src = io::generateMemoryDataset(data, *meta);

  // Get source coordinate system.
  const char *gdal_proj_src = GDALGetProjectionRef(dataset_mem_src);
  CPLAssert(gdal_proj_src != NULL && strlen(gdal_proj_src) > 0);

  // Set target coordinate system
  char *gdal_proj_dst = nullptr;
  OGRSpatialReference oSRS;
  oSRS.importFromEPSG(_epsg_target);
  oSRS.exportToWkt(&gdal_proj_dst);
  CPLAssert(gdal_proj_dst != NULL && strlen(gdal_proj_dst) > 0);

  // Create a transformer that maps from source pixel/line coordinates
  // to destination georeferenced coordinates (not destination
  // pixel line).  We do that by omitting the destination dataset
  // handle (setting it to NULL).
  void *projector = GDALCreateGenImgProjTransformer(dataset_mem_src, gdal_proj_src, NULL, gdal_proj_dst, FALSE, 0, 1);
  CPLAssert( projector != NULL );

  // Get approximate output georeferenced bounds and resolution for file.
  double geoinfo_target[6];
  int warped_cols = 0, warped_rows = 0;
  CPLErr eErr = GDALSuggestedWarpOutput(dataset_mem_src, GDALGenImgProjTransform, projector, geoinfo_target, &warped_cols , &warped_rows  );
  CPLAssert( eErr == CE_None );
  GDALDestroyGenImgProjTransformer(projector);

  // Create the output object.
  GDALDataset* dataset_mem_dst = _driver->Create("", warped_cols , warped_rows , data.channels(), meta->datatype, nullptr );
  CPLAssert( hDstDS != NULL );

  // Write out the projection definition.
  GDALSetProjection(dataset_mem_dst, gdal_proj_dst);
  GDALSetGeoTransform(dataset_mem_dst, geoinfo_target);

  CPLFree(gdal_proj_dst);

  //=======================================//
  //
  //      Step 3: Prepare warping
  //
  //=======================================//

  char** warper_system_options = nullptr;
  warper_system_options = CSLSetNameValue(warper_system_options, "INIT_DEST", "NO_DATA");
  warper_system_options = CSLSetNameValue(warper_system_options, "NUM_THREADS", "ALL_CPUS");

  // Setup warp options.
  GDALWarpOptions *warper_options = GDALCreateWarpOptions();
  warper_options->papszWarpOptions = warper_system_options;
  warper_options->hSrcDS = dataset_mem_src;
  warper_options->hDstDS = dataset_mem_dst;
  warper_options->nBandCount = 0;
  warper_options->nSrcAlphaBand = data.channels();
  warper_options->nDstAlphaBand = data.channels();

  // Establish reprojection transformer.
  warper_options->pTransformerArg = GDALCreateGenImgProjTransformer(
                                       dataset_mem_src,
                                       GDALGetProjectionRef(dataset_mem_src),
                                       dataset_mem_dst,
                                       GDALGetProjectionRef(dataset_mem_dst),
                                       FALSE, 0.0, 1 );

  warper_options->pfnTransformer = GDALGenImgProjTransform;

  //=======================================//
  //
  //      Step 4: Warping
  //
  //=======================================//

  GDALWarpOperation warping;
  warping.Initialize(warper_options);
  warping.ChunkAndWarpImage(0, 0, GDALGetRasterXSize(dataset_mem_dst), GDALGetRasterYSize(dataset_mem_dst));

  int raster_cols = dataset_mem_dst->GetRasterXSize();
  int raster_rows = dataset_mem_dst->GetRasterYSize();
  int raster_channels = dataset_mem_dst->GetRasterCount();

  std::vector<cv::Mat> warped_data_split;
  for(int i = 1; i <= raster_channels; ++i)
  {
    // Save the channel in var not in the vector of Mat
    cv::Mat bckVar(raster_rows, raster_cols, CV_8UC1  );

    GDALRasterBand *poBand = dataset_mem_dst->GetRasterBand(i);

    eErr = poBand->RasterIO(GF_Read, 0, 0, raster_cols, raster_rows, bckVar.data, raster_cols, raster_rows, poBand->GetRasterDataType(), 0, 0);
    CPLAssert( eErr == CE_None );

    warped_data_split.push_back(bckVar);
  }

  cv::Mat warped_data;
  cv::merge(warped_data_split, warped_data);

  GDALDestroyGenImgProjTransformer(warper_options->pTransformerArg );
  GDALDestroyWarpOptions(warper_options );
  delete meta;

  //=======================================//
  //
  //      Step 5: Compute output
  //
  //=======================================//

  double warped_geoinfo[6];
  dataset_mem_dst->GetGeoTransform(warped_geoinfo);

  double warped_resolution = warped_geoinfo[1];

  cv::Rect2d warped_roi;
  warped_roi.x = warped_geoinfo[0];
  warped_roi.y = warped_geoinfo[3] - warped_data.rows * warped_resolution;
  warped_roi.width = warped_data.cols * warped_resolution - warped_resolution;
  warped_roi.height = warped_data.rows * warped_resolution - warped_resolution;

  auto output = std::make_shared<CvGridMap>(warped_roi, warped_resolution);
  output->add("data", warped_data);

  GDALClose(dataset_mem_dst);
  GDALClose(dataset_mem_src);

  return output;
}

void gis::GdalWarper::warpPoints()
{
  /*OGRSpatialReference s_SRS;
    const char* s_WKT = dataset_mem_src->GetProjectionRef();
    s_SRS.importFromWkt(const_cast<char **>(&s_WKT));
    OGRCoordinateTransformation *coordinate_transformation;

    double x = blub[0], y = blub[3];
    coordinate_transformation = OGRCreateCoordinateTransformation(&oSRS, &s_SRS);
    coordinate_transformation->Transform(1, &x, &y);*/
}