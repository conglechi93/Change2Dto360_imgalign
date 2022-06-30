
#include "MultiStitcher.h"
#include "Settings.h"
#include "LogUtils.h"
#include "WarperHelper.h"
#include "Homography.h"
#include "FeatureFactory.h"
#include "ImageUtils.h"

#include "StitchInfoFilter.h"

#include "bundle_adjuster/Helper.h"
#include "bundle_adjuster/motion_estimators.hpp"

#include "opencv2/imgproc/types_c.h"

#include <numeric>
#include <memory>


using namespace cv;
using namespace imgalign::bundle;

namespace imgalign
{

namespace
{
  void logStitchOrder(
    double globalScale,
    const std::vector<cv::Size> &srcImagesSizes,
    const MultiStitcher::TStitchOrder &stitchOrder) {
    
    if(stitchOrder.empty()) return;

    LogUtils::getLog() << "==========================" << std::endl;
    LogUtils::getLog() << "stitchOrder: " << std::endl;
    
    auto logStitchInfo = [&](const StitchInfo *stitchInfo, bool logMats) {

      double w = srcImagesSizes[stitchInfo->srcImageIndex].width;
      double h = srcImagesSizes[stitchInfo->srcImageIndex].height;

      auto vfw = WarperHelper::getFieldOfView(w, h, stitchInfo->matK.at<double>(0, 0));
      auto vfh = WarperHelper::getFieldOfView(w, h, stitchInfo->matK.at<double>(1, 1)) * (h / w);

      LogUtils::getLog()
        << stitchInfo->srcImageIndex << "->" << stitchInfo->dstImageIndex
        << ", confidence: " << stitchInfo->matchInfo.confidence
        << ", matches count all/filtered/outlier/inlier: "
          << stitchInfo->matchInfo.allMatchesCount << "/"
          << stitchInfo->matchInfo.filteredMatchesCount << "/"
          << stitchInfo->matchInfo.outlierMatchesCount << "/"
          << stitchInfo->matchInfo.inlierMatchesCount
        << ", determinant: " << stitchInfo->matchInfo.determinant
        << ", svdConditionNumber: " << stitchInfo->matchInfo.svdConditionNumber
        << ", deltaH/deltaV [°]: " << stitchInfo->deltaH << "/" << stitchInfo->deltaV
        << ", absH/absV [°]: " << stitchInfo->absH << "/" << stitchInfo->absV
        << ", w/h [px]: " << w << "/" << h
        << ", vfw/vfh [°]: " << vfw << "/" << vfh 
        << std::endl;
        if(logMats) {
          LogUtils::logMat("R", stitchInfo->matR);
          LogUtils::logMat("K", stitchInfo->matK);
        }
    };
    
    for(auto it = stitchOrder.begin(); it != stitchOrder.end(); ++it) {
      logStitchInfo(*it, true/*LogUtils::isDebug*/);
      LogUtils::getLog() << "--------------------------" << std::endl;
    }
    
    LogUtils::getLog() << "global scale: " << globalScale << std::endl;
    LogUtils::getLog() << "==========================" << std::endl;
  }

  bool isReasonableSize(int w, int h, int warpedW, int WarpedH, size_t imageIndex) {
    if(warpedW * WarpedH < 16 * w * h) {
      return true;
    }
    
    LogUtils::getLogUserError()
      << "warped image " << imageIndex << ",  unreasonable size: "
      <<  w << "x" << h << " => " << warpedW << "x" << WarpedH
      << ", aborting. "
      << "Advice: "
      << "1: Change bundle adjustment type. "
      << "2: Increase or decrease confidence value. "
      << "3: Try a different surface projection." << std::endl;

    return false;
  }

  void logCameraParams(
    const std::vector<CameraParams> &camParamsV,
    const std::vector<cv::Size> &srcImagesSizes,
    const MultiStitcher::TStitchOrder &stitchOrder) {
    
    std::stringstream sStream;

    for(auto it = stitchOrder.begin(); it != stitchOrder.end(); ++it) {

      size_t srcIndex = (*it)->srcImageIndex;

      double w = srcImagesSizes[srcIndex].width;
      double h = srcImagesSizes[srcIndex].height;
      const auto &camParams = camParamsV[srcIndex];

      auto vh = WarperHelper::getFieldOfView(w, h, camParams.focal);

      sStream << srcIndex << "->" << vh << " ";

      // LogUtils::getLogUserInfo() << "R " << (*it)->srcImageIndex;
      // LogUtils::logMat(camParams.R, LogUtils::getLogUserInfo());
      // LogUtils::getLogUserInfo() << std::endl;
    }
  
    LogUtils::getLog() << "Focals: " << sStream.str() << std::endl;
  }

  void logPairwiseMatches(
    const std::vector<MatchesInfo> &matchesInfoV, bool verbose) {

    std::stringstream sStream;
    sStream << "pairwise matches: " << std::endl;
    
    int count = 0;
    double minConfidence = 100000000;
    double maxConfidence = 0;
    double maxSumDeltaHV = 0;
    for(const auto &mI : matchesInfoV) {

      if(mI.confidence == 0) {
        continue;
      }
      ++count;
      if(minConfidence > mI.confidence) {
        minConfidence = mI.confidence;
      }
      if(maxConfidence < mI.confidence) {
        maxConfidence = mI.confidence;
      }
      if(maxSumDeltaHV < mI.sumDeltaHV) {
        maxSumDeltaHV = mI.sumDeltaHV;
      }
      
      if(verbose) {
        sStream << mI.src_img_idx << "->" << mI.dst_img_idx << ": "
          << mI.confidence << ", "
          << mI.num_all << "/" << mI.num_filtered << "/" << mI.num_outlier << "/" << mI.num_inliers << ", "
          << "t " << mI.H.type() << " " << (mI.H.empty() ? "empty" : "notEmpty")
          << std::endl;
      }
    }

    if(verbose) {
      LogUtils::getLog() << sStream.str();
    }
    LogUtils::getLog()
      << "Pairwise matchesN: " << count << "/" << matchesInfoV.size()
      << ", minC: " << minConfidence
      << ", maxC: " << maxConfidence
      << ", maxHV: " << maxSumDeltaHV
      << std::endl;
  }

  // void dismissDetailedMatchInfoData(std::vector<std::shared_ptr<StitchInfo>> &stitchInfos)
  // {
  //   for(auto spStitchInfo : stitchInfos) {
  //     spStitchInfo->matchInfo.dismissDetailData();
  //   }
  // }

  void setScaledImages(
    const TImages &srcImages, TImages &imagesScaled, std::vector<double> &scaleFactors, int maxPixelsN)
  {
    FUNCLOGTIMEL("MultiStitcher::setScaledImages");

    imagesScaled.resize(srcImages.size());
    scaleFactors.clear();
    size_t imageIndex = 0;
    for(const auto &image : srcImages) {

      Mat srcImageGray;
		  cvtColor(image, srcImageGray, CV_RGBA2GRAY);

      scaleFactors.push_back(
        1.0 / ImageUtils::resize(srcImageGray, imagesScaled[imageIndex], maxPixelsN));

      ++imageIndex;
    }
  }

  void applyCamParams(
    const std::vector<CameraParams> &cameraParamsV,
    bool camEstimateDone,
    bool baDone,
    MultiStitcher::TStitchOrder &rStitchOrder,
    double &rGlobalScale) {

    FUNCLOGTIMEL("MultiStitcher::applyCamParams");

    rGlobalScale = 0.0;

    std::vector<double> focals;
    for(size_t i = 0; i < rStitchOrder.size(); ++i) {
      focals.push_back(cameraParamsV[i].focal);
      if(baDone) {
        cameraParamsV[i].R.convertTo(rStitchOrder[i]->matR, CV_64F);
      }
      else {
        TMat eye = TMat::eye(3, 3, CV_64F);
        eye.copyTo(rStitchOrder[i]->matR);
      }
      cameraParamsV[i].K().convertTo(rStitchOrder[i]->matK, CV_64F);
    }
    if(baDone || camEstimateDone) {
    
      std::sort(focals.begin(), focals.end());
      rGlobalScale = focals.size() % 2 == 1
        ? focals[focals.size() / 2]
        : (focals[focals.size() / 2 - 1] + focals[focals.size() / 2]) * 0.5f;
    }
  }

  bool runBundleAdjust(
    detail::BundleAdjusterBase &ba,
    const std::vector<MatchesInfo> &matchesInfoV,
    const std::vector<ImageFeatures> &imageFeaturesV,
    const std::vector<CameraParams> &cameraParamsV,
    std::vector<CameraParams> &outCameraParamsV) {

    FUNCLOGTIMEL("MultiStitcher::runBundleAdjust");

    Mat_<uchar> refineMask = Mat::zeros(3, 3, CV_8U);
    refineMask(0,0) = 1; // fx
    refineMask(0,1) = 1; // skew
    refineMask(0,2) = 1; // ppx
    refineMask(1,1) = 0; // aspect
    refineMask(1,2) = 1; // ppy
    ba.setRefinementMask(refineMask);

    outCameraParamsV.assign(cameraParamsV.begin(), cameraParamsV.end());
    for(auto &camParams : outCameraParamsV) {
      camParams.R.convertTo(camParams.R, CV_32F);
    }

    bool baSuccess = false;
    try {
      baSuccess = (ba)(imageFeaturesV, matchesInfoV, outCameraParamsV);
    }
    catch(std::exception &e) {
      LogUtils::getLogUserError() << e.what() << std::endl;
      throw e;
    }

    if(!baSuccess) {
      outCameraParamsV.clear();
      return false;
    }

    for(auto &camParams : outCameraParamsV) {
      camParams.R.convertTo(camParams.R, CV_64F);
    }
    return true;
  }

  bool
  bundleAdjustRay(
    const std::vector<MatchesInfo> &matchesInfoV,
    const std::vector<ImageFeatures> &imageFeaturesV,
    const std::vector<CameraParams> &cameraParamsV,
    double confidenceThresh,
    std::vector<CameraParams> &outCameraParamsV,
    int maxIterations,
    StitchInfoFilter &sifBundleAdjust,
    StitchInfoFilter &sifCamEstimate,
    StitchInfoFilter &sifComputeOrder)
  {
    FUNCLOGTIMEL("MultiStitcher::bundleAdjustRay");

    std::vector<CameraParams> cameraParamsTmp(cameraParamsV.begin(), cameraParamsV.end());

    bool baSuccess = false;
    for(auto i = 0; i < 2; ++i) {

      Ptr<detail::BundleAdjusterBase> ba = makePtr<detail::BundleAdjusterRay>();
      if(i == 1) {
        ba->reportError = true;
      }
      ba->setConfThresh(confidenceThresh);
      ba->maxIterations = maxIterations;
      baSuccess = runBundleAdjust(
        *ba,
        matchesInfoV, imageFeaturesV, cameraParamsTmp,
        outCameraParamsV);

      if(baSuccess) return true;

      if(i == 0) {

        LogUtils::getLog() << "BA ray failed, re running with inverted focals" << std::endl;

        for(auto &camParams : cameraParamsTmp) {
          camParams.focal = -camParams.focal;
        }
      }
      else {

        if(ba->edgeSrc == -1 || ba->edgeDst == -1) {
          sifBundleAdjust.setDone();
          sifComputeOrder.setDone();
          sifCamEstimate.setDone();
        }
        else {
          sifBundleAdjust.addEdge(ba->edgeSrc, ba->edgeDst);
          sifComputeOrder.addEdge(ba->edgeSrc, ba->edgeDst);
          sifCamEstimate.addEdge(ba->edgeSrc, ba->edgeDst);
        }
      }
    }
    return false;
  }

  bool
  bundleAdjustReproj(
    const std::vector<MatchesInfo> &matchesInfoV,
    const std::vector<ImageFeatures> &imageFeaturesV,
    const std::vector<CameraParams> &cameraParamsV,
    double confidenceThresh,
    std::vector<CameraParams> &outCameraParamsV,
    int maxIterations)
  {
    FUNCLOGTIMEL("MultiStitcher::BundleAdjustReproj");

    Ptr<detail::BundleAdjusterBase> ba = makePtr<detail::BundleAdjusterReproj>();
    ba->setConfThresh(confidenceThresh);
    ba->maxIterations = maxIterations;
    std::vector<CameraParams> baCameraParamsV;
    return runBundleAdjust(
      *ba,
      matchesInfoV, imageFeaturesV, cameraParamsV,
      outCameraParamsV);
  }

  bool
  runCamEstimate(
    const std::vector<MatchesInfo> &matchesInfoV,
    const std::vector<ImageFeatures> &imageFeaturesV,
    std::vector<CameraParams> &outCameraParamsV) {

    FUNCLOGTIMEL("MultiStitcher::camEstimate");

    LogUtils::getLogUserInfo() << "Estimating camera params" << std::endl;

    outCameraParamsV.clear();
    Ptr<detail::Estimator> estimator = makePtr<detail::HomographyBasedEstimator>();

    return (*estimator)(imageFeaturesV, matchesInfoV, outCameraParamsV);
  }
}

MultiStitcher::MultiStitcher(
  std::vector<TMat> &inSrcImages,
  const Settings &inSettings)

  : srcImages(inSrcImages)
  , points(inSrcImages.size())
  , descriptors(inSrcImages.size())
  , settings(inSettings)
{

  FUNCLOGTIMEL("MultiStitcher::MultiStitcher");

  if(inSrcImages.empty()) {
    throw std::logic_error("No src image input");
  }
  
  srcImagesSizes.resize(inSrcImages.size());
  for(size_t i = 0; i < inSrcImages.size(); ++i) {
    srcImagesSizes[i] = inSrcImages[i].size();
  }
}

MultiStitcher::~MultiStitcher()
{
  FUNCLOGTIMEL("MultiStitcher::~MultiStitcher");
  //LogUtils::getLogUserError() << "MultiStitcher::~MultiStitcher" << std::endl;
}

TConstMat &
MultiStitcher::getStitchedImage()
{
  FUNCLOGTIMEL("MultiStitcher::getStitchedImage");
  stitchedImage.stitch(compensateExposure, rectifyPerspective,
    rectifyStretch, maxRectangle, blendType, blendStrength, seamFinderType, !preserveAlphaChannelValue);
  stitchedImage.imageSizeOrig = stitchedImage.image.size();
  WarperHelper::rotateIf(stitchedImage.image, settings.getValue(eMultiStitch_projection));
  return stitchedImage.image;
}

TConstMat &
MultiStitcher::getStitchedImageCurrent(bool update)
{
  FUNCLOGTIMEL("MultiStitcher::getStitchedImageCurrent");

  if(update) {

    int maxPixelsN = currentStitchedImageMaxPixelsN;
    stitchedImage.stitchFast();
    stitchedImage.imageSizeOrig = stitchedImage.image.size();
    ImageUtils::resizeIf(stitchedImage.image, maxPixelsN);

    WarperHelper::rotateIf(stitchedImage.image, settings.getValue(eMultiStitch_projection));
  }
  return stitchedImage.image;
}

const cv::Size &
MultiStitcher::getStitchImageCurrentOrigSize()
{
  FUNCLOGTIMEL("MultiStitcher::getStitchImageCurrentSize");
  return stitchedImage.imageSizeOrig;
}

void
MultiStitcher::releaseStitchedImage()
{
  FUNCLOGTIMEL("MultiStitcher::releaseStitchedImage");
  stitchedImage.image.release();
}

void
MultiStitcher::releaseStitchedData()
{
  FUNCLOGTIMEL("MultiStitcher::releaseStitchedData");

  stitchedImage.stitchedInfos.clear();
}

bool
MultiStitcher::initStiching(
  const std::vector<double> &inFieldsOfView,
  std::vector<int> &outStitchIndices)
{
  FUNCLOGTIMEL("MultiStitcher::initStiching");

  abort = false;

  if(inFieldsOfView.size() != srcImages.size())
  {
    throw std::logic_error("Invalid field of view data");
  }
  fieldsOfView.assign(inFieldsOfView.begin(), inFieldsOfView.end());

  projectionType = (int)settings.getValue(eMultiStitch_projection);
  featureDetectionMaxPixelsN = (int)settings.getValue(eImageCap);
  rectifyPerspective = (bool)settings.getValue(eMultiStitch_rectifyPerspective);
  rectifyStretch = (bool)settings.getValue(eMultiStitch_rectifyStretch);
  camEstimate = (bool)settings.getValue(eMultiStitch_camEstimate);
  bundleAdjustType = settings.getBundleAdjustType();
  colorTransfer = (bool)settings.getValue(eMultiStitch_colorTransfer);
  seamBlend = (bool)settings.getValue(eMultiStitch_seamBlend);
  seamFinderType = settings.getSeamFinderType();
  compensateExposure = (bool)settings.getValue(eMultiStitch_exposureCompensator);
  calcImageOrder = (bool)settings.getValue(eMultiStitch_calcImageOrder);
  confidenceThresh = settings.getValue(eMultiStitch_confidenceThresh);
  blendStrength = settings.getValue(eMultiStitch_blendStrength);
  preserveAlphaChannelValue = (bool)settings.getValue(eMultiStitch_preserveAlphaChannelValue);
  wcType = settings.getWaveCorrectType();
  calcCenterImage = (bool)settings.getValue(eMultiStitch_calcCenterImage);
  currentStitchedImageMaxPixelsN = (int)settings.getValue(eMultiStitch_limitLiveStitchingPreview);
  
  tfType = settings.getTransformFinderType();
  //matcher = FeatureFactory::CreateMatcher(settings);
  matchers.clear();

  blendType = seamBlend
    ? BlendType::BT_MULTIBAND
    : BlendType::BT_NONE;

  confidenceThreshCam = confidenceThresh;
  if(settings.getValue(eMultiStitch_confidenceThreshCamManual) > 0.0) {
    confidenceThreshCam = settings.getValue(eMultiStitch_confidenceThreshCam);
  }

  inputImagesMatchReach = (int)settings.getValue(eMultiStitch_inputImagesMatchReach);
  if(inputImagesMatchReach <= 0) {
    inputImagesMatchReach = std::numeric_limits<int>::max();
  }

  warpFirst = (bool)settings.getValue(eMultiStitch_warpFirst);
  maxRectangle = (bool)settings.getValue(eMultiStitch_maxRectangle);

  if(bundleAdjustType == BundleAdjustType::BAT_NONE ||
     projectionType == (int)eStitch_projectionTypeNone) {

    warpFirst = true;
  }

  if(LogUtils::isDebug) {
    LogUtils::getLog() << "projectionType " << projectionType << std::endl;
    LogUtils::getLog() << "featureDetectionMaxPixelsN " << featureDetectionMaxPixelsN << std::endl;
    LogUtils::getLog() << "rectifyPerspective " << rectifyPerspective << std::endl;
    LogUtils::getLog() << "rectifyStretch " << rectifyStretch << std::endl;
    LogUtils::getLog() << "camEstimate " << camEstimate << std::endl;
    LogUtils::getLog() << "bundleAdjust " << (int)(settings.getValue(eMultiStitch_bundleAdjustType)) << std::endl;
    LogUtils::getLog() << "waveCorrection " << (int)(settings.getValue(eMultiStitch_waveCorrection)) << std::endl;
    LogUtils::getLog() << "colorTransfer " << colorTransfer << std::endl;
    LogUtils::getLog() << "seamBlend " << seamBlend << std::endl;
    LogUtils::getLog() << "compensateExposure " << compensateExposure << std::endl;
    LogUtils::getLog() << "calcImageOrder " << calcImageOrder << std::endl;
    LogUtils::getLog() << "calcCenterImage " << calcCenterImage << std::endl;
    LogUtils::getLog() << "warpFirst " << warpFirst << std::endl;
    LogUtils::getLog() << "confidenceThresh " << confidenceThresh << std::endl;
    LogUtils::getLog() << "confidenceThreshCam " << confidenceThreshCam << std::endl;
    LogUtils::getLog() << "minMatchesToRetain " << (int)settings.getValue(eMatchFilterMinMatchesToRetain) << std::endl;
    LogUtils::getLog() << "seamFinderType " << (int)(settings.getValue(eMultiStitch_seamFinderType)) << std::endl;
    LogUtils::getLog() << "preserveAlphaChannelValue " << preserveAlphaChannelValue << std::endl;
    LogUtils::getLog() << "livePreviewMaxPixelsN " << currentStitchedImageMaxPixelsN << std::endl;
    LogUtils::getLog() << "resultPreviewMaxPixelsN " << (int)(settings.getValue(eMultiStitch_limitResultPreview)) << std::endl;
    LogUtils::getLog() << "liveUpdateCycle " << (int)(settings.getValue(eMultiStitch_liveUpdateCycle)) << std::endl;
    LogUtils::getLog() << "maxRectangle " << maxRectangle << std::endl;
    LogUtils::getLog() << "inputImagesMatchReach " << inputImagesMatchReach << std::endl;
  }

  computeKeyPoints();
  if(!keyPointsComputed)
  {
    return false;
  }

  bool camBasicDataUpToDate = isCamBasicDataUpToDate();
  
  if(!camBasicDataUpToDate) {
    for(auto &stitchInfo : stitchInfos) {
      stitchInfo->resetCamData();
    }
  }

  iiR = std::unique_ptr<InputImagesReach>(new InputImagesReach(inputImagesMatchReach, (int)srcImages.size()));
  sifComputeOrder = std::unique_ptr<StitchInfoFilter>(new SIF_Std(confidenceThresh, iiR.get()));

  if(!camBasicDataUpToDate) {

    centerImageIndex = 0;
    if(calcCenterImage && warpFirst) {
      centerImageIndex = getCenterImage();
    }
    stitchOrder = computeStitchOrder();
    centerImageIndex = stitchOrder[0]->srcImageIndex;

    // if(calcImageOrder) {  
    //   calcAndStoreAllMatches();
    // }
  }

  if(abort) {
    return false;
  }

  if(stitchOrder.size() < 2) {
    LogUtils::getLogUserError() << "Failed, no matches found" << std::endl;
    return false;
  }

  if(!camBasicDataUpToDate) {

    computeAbsRotation(stitchOrder);
    setCamMatrices(stitchOrder);

    std::vector<const StitchInfo *> tempStitchInfos;
    for(auto spStitchInfo : stitchInfos) {
      tempStitchInfos.push_back(spStitchInfo.get());
    }

    switch(bundleAdjustType) {
      case BundleAdjustType::BAT_RAYMINTILES:
      case BundleAdjustType::BAT_REPROJMINTILES:
        sifBundleAdjust = std::unique_ptr<StitchInfoFilter>(new SIF_BestNeighbourOnly(confidenceThresh, iiR.get(), stitchOrder));
        sifCamEstimate = std::unique_ptr<StitchInfoFilter>(new SIF_BestNeighbourOnly(confidenceThresh, iiR.get(), stitchOrder));
        break;
      case BundleAdjustType::BAT_RAYCONFIDENCESTEP:
        sifComputeOrder = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesConfidence(confidenceThresh, iiR.get(), tempStitchInfos));
        sifCamEstimate = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesConfidence(confidenceThresh, iiR.get(), tempStitchInfos)); 
        sifBundleAdjust = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesConfidence(confidenceThresh, iiR.get(), tempStitchInfos));
        break;
      case BundleAdjustType::BAT_RAYBLACKLIST:
        sifBundleAdjust = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesBlacklist(confidenceThresh, iiR.get(), tempStitchInfos.size()));
        sifComputeOrder = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesBlacklist(confidenceThresh, iiR.get(), tempStitchInfos.size()));
        sifCamEstimate = std::unique_ptr<StitchInfoFilter>(new SIF_IgnoreEdgesBlacklist(confidenceThresh, iiR.get(), tempStitchInfos.size()));
        break;
      case BundleAdjustType::BAT_NONE:
      case BundleAdjustType::BAT_REPROJ:
      case BundleAdjustType::BAT_REPROJCAP200:
      case BundleAdjustType::BAT_REPROJNOCAM:
      case BundleAdjustType::BAT_REPROJNOCAMCAP200:
      default: {
        sifBundleAdjust = std::unique_ptr<StitchInfoFilter>(new SIF_Std(confidenceThresh, iiR.get()));
        sifCamEstimate = std::unique_ptr<StitchInfoFilter>(new SIF_Std(confidenceThresh, iiR.get()));
      }
    }

    for(int i = 1; true; ++i) {
      if(camEstimateAndBundleAdjustIf(stitchOrder, globalScale)) {
        break;
      }

      sifComputeOrder->setIteration(i);
      sifBundleAdjust->setIteration(i);
      sifCamEstimate->setIteration(i);

      // if(sifComputeOrder->done()) {
      //   return false;
      // }
      if(sifBundleAdjust == nullptr || sifBundleAdjust->done()) {

        LogUtils::getLogUserError() << "Bundle adjust failed" << std::endl;

        LogUtils::getLogUserError()
          << "Advice: "
          << "1. Increase/decrease confidence value, "
          << "2. Try a difference bundle adjustement type."
          << std::endl;

        return false;
      }
      stitchOrder = computeStitchOrder();
      if(stitchOrder.size() < 2) {
        LogUtils::getLogUserError() << "Failed, no matches found" << std::endl;
        return false;
      }
    }

    lastRunData = std::unique_ptr<LastRunData>(
      new LastRunData(settings, fieldsOfView, stitchOrder, globalScale));
  }
  else {
    applyLastRunData();
  }

  if(   (camEstimate || bundleAdjustType != BundleAdjustType::BAT_NONE)
     && wcType != WaveCorrectType::WCT_NONE) {
    
    waveCorrection(stitchOrder);
  }

  outStitchIndices.clear();
  for(auto *stitchInfo : stitchOrder) {
    outStitchIndices.push_back(stitchInfo->srcImageIndex);
  }

  logStitchOrder(globalScale, srcImagesSizes, stitchOrder);

  if(calcImageOrder && inputImagesMatchReach == std::numeric_limits<int>::max()) {
    descriptors.clear();
  }
  
  stitchedImage.init(
    srcImages[centerImageIndex],
    projectionType,
    stitchOrder,
    warpFirst,
    globalScale == 0.0 ? nullptr : &globalScale);
  
  return true;
}

int
MultiStitcher::stitchAll()
{
  FUNCLOGTIMEL("MultiStitcher::stitchAll");

  for(auto it = stitchOrder.begin(); it != stitchOrder.end(); ++it) {

    if(warpFirst || (projectionType == eStitch_projectionTypeNone && !camEstimate)) {
      if(!stitch(**it)) break;
    }
    else {
      if(!stitch2(**it)) break;
    }
  }
  return (int)stitchedImage.imageIndices.size();
}

bool MultiStitcher::stitchNext(const StitchInfo &stitchInfo)
{
  FUNCLOGTIMEL("MultiStitcher::stitchNext");

  if(warpFirst || (projectionType == eStitch_projectionTypeNone && !camEstimate)) {
    return stitch(stitchInfo);
  }
  else {
    return stitch2(stitchInfo);
  }
}

const StitchInfo *
MultiStitcher::findNextMatch(const TStitchOrder &rStitchOrder)
{
  FUNCLOGTIMEL("MultiStitcher::findNextMatch");

  if(rStitchOrder.empty()) return nullptr;

  TIndices dstIndices;
  TIndices srcIndices;
  for(size_t i = 0; i < srcImages.size(); ++i) {

    auto it = std::find_if(rStitchOrder.begin(), rStitchOrder.end(),
    [&](const StitchInfo *stitchInfo) {
      return stitchInfo->srcImageIndex == i;
    });

    if(it == rStitchOrder.end()) srcIndices.push_back(i);
    else dstIndices.push_back(i);
  }

  return findNextMatch(dstIndices, srcIndices);
}

const StitchInfo *
MultiStitcher::findNextMatch(
  TConstIndices &dstI,
  TConstIndices &srcI)
{ 
  FUNCLOGTIMEL("MultiStitcher::findNextMatch");

  if(srcI.empty() || dstI.empty()) return nullptr;
  
  const StitchInfo *stitchInfoP = nullptr;
  for(auto dstIndex : dstI) { 
    for(auto srcIndex : srcI) {
      const auto *stitchInfo = getStitchInfo(dstIndex, srcIndex);

      if(sifComputeOrder->pass(*stitchInfo)) {
     
        if(stitchInfoP == nullptr) {
          stitchInfoP = stitchInfo;
          continue;
        }

        if(stitchInfo->matchInfo.confidence > stitchInfoP->matchInfo.confidence) {
          stitchInfoP = stitchInfo;
        }
      }
    }
  }
  return stitchInfoP;
}

bool
MultiStitcher::hasStitchInfo(size_t dstI, size_t srcI)
{
  FUNCLOGTIMEL("MultiStitcher::hasStitchInfo");

  return getStitchInfo(dstI, srcI, false) != nullptr;
}

void
MultiStitcher::calcAndStoreAllMatches()
{
  FUNCLOGTIMEL("MultiStitcher::calcAndStoreAllMatches");

  for(size_t srcI = 0; srcI < srcImages.size(); ++srcI) {
    for(size_t dstI = 0; dstI < srcImages.size(); ++dstI) {
      
      if(abort) return;

      if(srcI == dstI) continue;

      if(!hasStitchInfo(dstI, srcI)) {
     
        LogUtils::getLogUserInfo() << "Matching indirect, "
          << srcI << "->" << dstI
          << std::endl;

        getStitchInfo(dstI, srcI);
      }
    }
  }
}

bool
MultiStitcher::stitch2(const StitchInfo &stitchInfo)
{
  FUNCLOGTIMEL("MultiStitcher::stitch2");

  auto srcIndex = stitchInfo.srcImageIndex;

  auto srcImage = srcImages[srcIndex];
  if(colorTransfer) {
    srcImage = ImageUtils::colorTransfer(srcImages[centerImageIndex], srcImage);
  }
  
  TMat *kMat = &stitchInfo.matK;
  TMat rMat = stitchInfo.matR;

  cv::Point tlCornerRoi(0, 0);
  TMat srcImageProjected;
  TMat mask;
  ImageUtils::createMaskFor(srcImage, mask);
  TMat maskProjected;
  tlCornerRoi = WarperHelper::warpImage(
    projectionType, srcImage, stitchedImage.warpedImage(srcIndex), *kMat, rMat, true, true, globalScale == 0.0 ? nullptr : &globalScale);
  WarperHelper::warpImage(
    projectionType, mask, stitchedImage.warpedMask(srcIndex), *kMat, rMat, false, false, globalScale == 0.0 ? nullptr : &globalScale);
    
  srcImages[srcIndex].release();
  srcImage.release();

  stitchedImage.setCornerRoiXFor(srcIndex, tlCornerRoi.x);
  stitchedImage.setCornerRoiYFor(srcIndex, tlCornerRoi.y);

  stitchedImage.imageIndices.push_back(srcIndex);
  // stitchedImage.imageSize = cv::Size((int)(b - t), (int)(r - l));

  return true;
}

bool
MultiStitcher::stitch(const StitchInfo &stitchInfo)
{
  FUNCLOGTIMEL("MultiStitcher::stitch");

  auto srcIndex = stitchInfo.srcImageIndex;
  auto dstIndex = stitchInfo.dstImageIndex;

  auto fieldOfViewSrc = fieldsOfView[srcIndex];
  auto srcImage = srcImages[srcIndex];
  if(colorTransfer) {
    srcImage = ImageUtils::colorTransfer(srcImages[centerImageIndex], srcImage);
  }
  
  TMat rotMat1, rotMat2;
  WarperHelper::getMatR(0, 0, 0, rotMat1);
  WarperHelper::getMatR(0, 0, 0, rotMat2);

  TMat *kMat1 = nullptr;
  TMat *kMat2 = nullptr;

  rotMat2 = stitchInfo.matR;
  kMat2 = &stitchInfo.matK;

  TPoints2f inlierPts1, inlierPts2;
  stitchInfo.matchInfo.getInlierPts(inlierPts1, inlierPts2);

  TPoints2f ptsWarped1 = stitchedImage.getTransformedPts(
    //stitchInfo.matchInfo.inlierPts1,
    inlierPts1,
    srcImagesSizes[dstIndex],
    dstIndex,
    globalScale == 0.0 ? nullptr : &globalScale);

  TMat homography;
  bool homographyFound = Homography::getHomography(
    ptsWarped1,
    //stitchInfo.matchInfo.inlierPts2, 
    inlierPts2,
    cv::Size(0, 0),
    srcImage.size(),
    kMat1, kMat2,
    0.0,
    fieldOfViewSrc,
    rotMat1,
    rotMat2,
    eStitch_projectionTypeNone,
    projectionType,
    settings.getTransformFinderType(),
    homography,
    globalScale == 0.0 ? nullptr : &globalScale,
    nullptr,
    nullptr);
  
  if(!homographyFound) {
    throw std::logic_error("No homography found on warped images");
  }

  TMat srcImageProjected;
  TMat mask;
  ImageUtils::createMaskFor(srcImage, mask);
  TMat maskProjected;
  if((ParamType)projectionType != eStitch_projectionTypeNone) {
    if(kMat2 != nullptr) {
      WarperHelper::warpImage(
        projectionType, srcImage, srcImageProjected, *kMat2, rotMat2, true, true, globalScale == 0.0 ? nullptr : &globalScale);
      WarperHelper::warpImage(
        projectionType, mask, maskProjected, *kMat2, rotMat2, false, false, globalScale == 0.0 ? nullptr : &globalScale);
    }
    else {
      WarperHelper::warpImage(
        projectionType, srcImage, srcImageProjected, fieldOfViewSrc, rotMat2, true, true);
      WarperHelper::warpImage(
        projectionType, mask, maskProjected, fieldOfViewSrc, rotMat2, false, false);
    }
  }
  else {
    srcImage.copyTo(srcImageProjected);
    mask.copyTo(maskProjected);
  }

  srcImages[srcIndex].release();
  srcImage.release();

  int w1 = stitchedImage.imageSize.width;
  int h1 = stitchedImage.imageSize.height;
  int w2 = srcImageProjected.size().width;
  int h2 = srcImageProjected.size().height;

  {

    TMat homography2;
    homography.copyTo(homography2);

    double tx, ty, t, r, b, l;
    WarperHelper::getBox(w2, h2, homography2, tx, ty, t, r, b, l);

    // Sanity check!
    if(!isReasonableSize(w2, h2, (int)(r - l), (int)(b - t), srcIndex)) {
      return false;
    }

    stitchedImage.setCornerXFor(srcIndex, 0);
    stitchedImage.setCornerYFor(srcIndex, 0);

    if(tx < 0) {
      stitchedImage.setCornerXFor(srcIndex, -tx);
    }
    if(ty < 0) {
      stitchedImage.setCornerYFor(srcIndex, -ty);
    }


    
    Mat alignedImage;
    WarperHelper::warpPerspective(
      srcImageProjected, homography2,
      cv::Size((int)(r - l), (int)(b - t)),
      stitchedImage.warpedImage(srcIndex), true, true);

    WarperHelper::warpPerspective(
      maskProjected, homography2,
      cv::Size((int)(r - l), (int)(b - t)),
      stitchedImage.warpedMask(srcIndex), false, false);

    srcImageProjected.release();
  }
  
  double tx, ty, t, r, b, l;
  WarperHelper::getBox(
    w1, h1, w2, h2,
    homography,
    tx, ty, t, r, b, l);

  stitchedImage.setHomographyFor(srcIndex, homography);
  if(kMat2 != nullptr) stitchedImage.setKMatFor(srcIndex, *kMat2);
  stitchedImage.setRMatFor(srcIndex, rotMat2);

  stitchedImage.addTranslation(tx, ty);
  stitchedImage.imageIndices.push_back(srcIndex);
  stitchedImage.imageSize = cv::Size((int)(b - t), (int)(r - l));
  
  return true;
}

void
MultiStitcher::computeKeyPoints()
{
  FUNCLOGTIMEL("MultiStitcher::computeKeyPoints");

  if(keyPointsComputed)
  {
    return;
  }

  setScaledImages(srcImages, srcImagesScaled, scaleFactors, featureDetectionMaxPixelsN);
  try {
    cv::Ptr<cv::Feature2D> featureDet;
    cv::Ptr<cv::Feature2D> featureDes;

    FeatureFactory::CreateFeatureDetector(featureDet, settings);
    FeatureFactory::CreateDescriptorComputer(featureDes, settings);

    size_t imageIndex = 0;
    keyPoints.resize(srcImagesScaled.size());
    descriptors.resize(srcImagesScaled.size());
    for(auto &image : srcImagesScaled) {

      if(abort) {
        srcImagesScaled.clear();
        return;
      }

      featureDet->detect(image, keyPoints[imageIndex]);
      featureDes->compute(image, keyPoints[imageIndex], descriptors[imageIndex]);

      //auto ptIndex = 0;
      //points[imageIndex].resize(keyPoints[imageIndex].size());
      for(auto &keyPoint : keyPoints[imageIndex]) {
        keyPoint.pt.x *= scaleFactors[imageIndex];
        keyPoint.pt.y *= scaleFactors[imageIndex];
        //points[imageIndex][ptIndex].x = keyPoint.pt.x;
        //points[imageIndex][ptIndex].y = keyPoint.pt.y;

        //++ptIndex;
      }
      ++imageIndex;

      LogUtils::getLogUserInfo() << "keypoints image "
        << imageIndex << "/" << srcImagesScaled.size() << std::endl;
    }
    keyPointsComputed = true;
    srcImagesScaled.clear();
  }
  catch(...) {
    srcImagesScaled.clear();
  }
}

const StitchInfo *
MultiStitcher::getStitchInfo(size_t dstI, size_t srcI, bool createIf)
{
  //FUNCLOGTIMEL("MultiStitcher::getStitchInfo");

  if(dstI >= srcImages.size() || srcI >= srcImages.size()) {
    throw std::logic_error("invalid image index");
  }
  
  auto findStitchInfo = [this](size_t srcI, size_t dstI) {
    auto it = std::find_if(stitchInfos.begin(), stitchInfos.end(),
      [&](const std::shared_ptr<StitchInfo> stitchInfo) {
      return stitchInfo->srcImageIndex == srcI
          && stitchInfo->dstImageIndex == dstI;
    });
    if(it != stitchInfos.end()) {
      return *it;
    }
    return std::shared_ptr<imgalign::StitchInfo>(nullptr);
  };

  auto spStitchInfo = findStitchInfo(srcI, dstI);
  std::shared_ptr<imgalign::StitchInfo> spStitchInfoInverse(nullptr);
  if(spStitchInfo != nullptr) {
    if(spStitchInfo->matched) return spStitchInfo.get();
  }
  else if(!createIf) {
    return nullptr;
  }
  else {
    spStitchInfo = std::make_shared<StitchInfo>(srcI, dstI);
    spStitchInfoInverse = std::make_shared<StitchInfo>(dstI, srcI);
    stitchInfos.push_back(spStitchInfo);
    stitchInfos.push_back(spStitchInfoInverse);
  }

  if(srcI == dstI) return spStitchInfo.get();
  if(iiR != nullptr && !iiR->insideReach((int)srcI, (int)dstI)) {
    return spStitchInfo.get();
  }

  if(descriptors.empty()) {
    throw std::logic_error("Can not match, descriptors not available");
  }
  if(keyPoints.empty()) {
    throw std::logic_error("Can not match, key points not available");
  }
  
  const auto &matcher = getMatcher(srcI, descriptors[srcI], keyPoints[srcI]);

  spStitchInfo->matchInfo = matcher.match(
    tfType, descriptors[dstI],
    keyPoints[dstI],
    warpFirst ? DataExtractionMode::eBasic : DataExtractionMode::eMin);
  spStitchInfo->matched = true;

  if(spStitchInfoInverse == nullptr) {
    spStitchInfoInverse = findStitchInfo(dstI, srcI);
    if(spStitchInfoInverse == nullptr) {
      throw std::logic_error("No inverse stitchinfo found");
    }
  }
  spStitchInfoInverse->matched = true;

  if(spStitchInfo->matchInfo.success) {
    spStitchInfoInverse->matchInfo = spStitchInfo->matchInfo.getInverse();
  }

  if(spStitchInfo->matchInfo.isHomographyGood()) {
    computeRelativeRotation(*spStitchInfo);
    spStitchInfoInverse->deltaH = -spStitchInfo->deltaH;
    spStitchInfoInverse->deltaV = -spStitchInfo->deltaV;
  }

  if(LogUtils::isDebug) {
    LogUtils::getLog() << "Match " << srcI << "->" << dstI << ", c: "
      << spStitchInfo->matchInfo.confidence << ", d: "
      << spStitchInfo->matchInfo.determinant <<  std::endl;
  }
  
  return spStitchInfo.get();
}

size_t MultiStitcher::getCenterImage()
{
  FUNCLOGTIMEL("MultiStitcher::getCenterImage");

  if(srcImages.empty()) {
    throw std::logic_error("No image data available");
  }

  if(srcImages.size() <= 2) {
    return 0;
  }

  auto _stitchOrder = computeStitchOrder(srcImages.size() / 2);
 
  struct Info {
    double absH = 0.0;
    double absV = 0.0;
  };
  std::vector<Info> infos(srcImages.size());

  for(auto it = _stitchOrder.begin(); it != _stitchOrder.end(); ++it) {

    auto dstI = (*it)->dstImageIndex;
    auto srcI = (*it)->srcImageIndex;

    infos[srcI].absH = infos[dstI].absH + (*it)->deltaH;
    infos[srcI].absV = infos[dstI].absV + (*it)->deltaV;
  }
  double minH, maxH, minV, maxV;
  minH = minV = std::numeric_limits<double>::max();
  maxH = maxV = -std::numeric_limits<double>::max();
  for(auto it = infos.begin(); it != infos.end(); ++it) {
    
    if(it->absH < minH) minH = it->absH;
    if(it->absH > maxH) maxH = it->absH;
    if(it->absV < minV) minV = it->absV;
    if(it->absV > maxV) maxV = it->absV;
  }

  double centerH = 0.5 * (maxH + minH);
  double centerV = 0.5 * (maxV + minV);

  size_t centerIndex = 0;
  size_t index = 0;
  double deviation = std::numeric_limits<double>::max();
  for(auto it = infos.begin(); it != infos.end(); ++it) {
    auto diffH = std::abs(it->absH - centerH);
    auto diffV = std::abs(it->absV - centerV);

    if(std::abs(diffH) + std::abs(diffV) < deviation) {
      deviation = std::abs(diffH) + std::abs(diffV);
      centerIndex = index;
    }
    ++index;
  }

  LogUtils::getLogUserInfo() << "Starting with image: " << centerIndex << std::endl;

  return centerIndex;
}

MultiStitcher::TStitchOrder MultiStitcher::computeStitchOrder(size_t startIndex)
{
  FUNCLOGTIMEL("MultiStitcher::computeStitchOrder");

  if(srcImages.empty()) {
    throw std::logic_error("No image data available");
  }

  TStitchOrder _stitchOrder;
  _stitchOrder.push_back(getStitchInfo(startIndex, startIndex));

  if(srcImages.size() < 2) {
    return _stitchOrder;
  }
  
  if(!calcImageOrder) {
    LogUtils::getLog() << "Matching images, using input image order as stitching order" << std::endl;
    
    size_t lastDstSuccess = startIndex;

    int matchesFoundN = 0;

    auto calcNext = [&](int srcIndex) {

      size_t dstIndex = lastDstSuccess;
      auto *stitchInfo = getStitchInfo(dstIndex, srcIndex);
      if(sifComputeOrder->pass(*stitchInfo)) {
       
        lastDstSuccess = srcIndex;
        _stitchOrder.push_back(stitchInfo);

        LogUtils::getLogUserInfo() << "Matching, found "
          << stitchInfo->srcImageIndex << "->" << stitchInfo->dstImageIndex << ", "
          << ++matchesFoundN << "/" << srcImages.size() - 1
          << std::endl;
      }
    };
    
    for(size_t i = startIndex; i < srcImages.size() - 1; ++i){
      if(abort) {
        _stitchOrder.clear();
        return _stitchOrder;
      }
      calcNext(i + 1);
    }
    lastDstSuccess = startIndex;
    for(int i = startIndex; i > 0; --i){
      if(abort) {
        _stitchOrder.clear();
        return _stitchOrder;
      }
      calcNext(i - 1);
    }
  }
  else {
    LogUtils::getLog() << "Matching images and computing stitching order" << std::endl;
    
    int matchesFound = 0;
    int totalMatchesToFind = srcImages.size() - 1;

    while(true) {

      if(abort) {
        _stitchOrder.clear();
        return _stitchOrder;
      }

      const auto *stitchInfo = findNextMatch(_stitchOrder);
      if(stitchInfo == nullptr) break;
      _stitchOrder.push_back(stitchInfo);

      LogUtils::getLogUserInfo() << "Matching, found "
          << stitchInfo->srcImageIndex << "->" << stitchInfo->dstImageIndex << ", "
          << ++matchesFound << "/" << totalMatchesToFind
          << std::endl;
    }
    
  }
  LogUtils::getLog() << "done" << std::endl;
  return _stitchOrder;
}

MultiStitcher::TStitchOrder
MultiStitcher::computeStitchOrder()
{
  FUNCLOGTIMEL("MultiStitcher::computeStitchOrder");

  auto _stitchOrder = computeStitchOrder(centerImageIndex);
  
  auto failIndex = centerImageIndex;
  for(size_t i = 0; i < srcImages.size() && _stitchOrder.size() < 2; ++i) {

    if(abort) {
      _stitchOrder.clear();
      return _stitchOrder;
    }

    if(i == centerImageIndex) continue;

    LogUtils::getLog()
      << "Failed to compute stitchOrder with start index " << failIndex
      << " retrying with index " << i << std::endl; 

    _stitchOrder = computeStitchOrder(i);
    failIndex = i;
  }
  return _stitchOrder;
}

void
MultiStitcher::computeRelativeRotation(TStitchOrder &rStitchOrder)
{
  FUNCLOGTIMEL("MultiStitcher::computeRelativeRotation");

  for(auto it = rStitchOrder.begin(); it != rStitchOrder.end(); ++it) {
    computeRelativeRotation(**it);
  }
}

void
MultiStitcher::computeRelativeRotation(
  std::vector<std::shared_ptr<StitchInfo>> &rStitchInfos)
{
  FUNCLOGTIMEL("MultiStitcher::computeRelativeRotation");

  for(auto it = rStitchInfos.begin(); it != rStitchInfos.end(); ++it) {
    computeRelativeRotation(*((*it).get()));
  }
}

void
MultiStitcher::computeRelativeRotation(const StitchInfo &stitchInfo)
{
  FUNCLOGTIMEL("MultiStitcher::computeRelativeRotation");

  if(stitchInfo.srcImageIndex == stitchInfo.dstImageIndex) {
    stitchInfo.deltaH = 0.0;
    stitchInfo.deltaV = 0.0;
    return;
  }

  auto w1 = srcImagesSizes[stitchInfo.dstImageIndex].width;
  auto h1 = srcImagesSizes[stitchInfo.dstImageIndex].height;
  auto w2 = srcImagesSizes[stitchInfo.srcImageIndex].width;
  auto h2 = srcImagesSizes[stitchInfo.srcImageIndex].height;
 
  WarperHelper::getRelativeRotation(
    w1, h1, w2, h2,
    fieldsOfView[stitchInfo.dstImageIndex],
    stitchInfo.matchInfo.homography,
    stitchInfo.deltaH, stitchInfo.deltaV);
}

void
MultiStitcher::computeAbsRotation(TStitchOrder &rStitchOrder)
{
  FUNCLOGTIMEL("MultiStitcher::computeAbsRotation");

  if(rStitchOrder.size() < 2) return;

  double minH, maxH, minV, maxV;
  minH = minV = std::numeric_limits<double>::max();
  maxH = maxV = -std::numeric_limits<double>::max();

  auto itLast = rStitchOrder.begin();
  for(auto it = ++rStitchOrder.begin(); it != rStitchOrder.end(); ++it, ++itLast) {

    double absH = (*itLast)->absH + (*it)->deltaH;
    double absV = (*itLast)->absV + (*it)->deltaV;

    (*it)->absH = absH;
    (*it)->absV = absV;

    WarperHelper::getMatR(absH, absV, 0, (*it)->matR);

    if(absH < minH) minH = absH;
    if(absH > maxH) maxH = absH;
    if(absV < minV) minV = absV;
    if(absV > maxV) maxV = absV;
  }

  estimatedFieldOfViewH = maxH - minH;
  estimatesFieldOfViewV = maxV - minV;
}

void
MultiStitcher::setCamMatrices(TStitchOrder &rStitchOrder)
{
  FUNCLOGTIMEL("MultiStitcher::setCamMatrices");

  for(auto it = rStitchOrder.begin(); it != rStitchOrder.end(); ++it) {

    size_t srcIndex = (*it)->srcImageIndex;
    auto w = srcImagesSizes[srcIndex].width;
    auto h = srcImagesSizes[srcIndex].height;
    // auto w = srcImages[srcIndex].size().width;
    // auto h = srcImages[srcIndex].size().height;

    auto fLenPx = WarperHelper::getFocalLengthPx(w, h, fieldsOfView[srcIndex]);
    WarperHelper::getMatK(w, h, fLenPx, (*it)->matK);
  }
}

bool
MultiStitcher::camEstimateAndBundleAdjustIf(
  TStitchOrder &rStitchOrder,
  double &rGlobalScale)
{
  FUNCLOGTIMEL("MultiStitcher::camEstimateAndBundleAdjust");

  rGlobalScale = 0.0;

  if(!camEstimate && bundleAdjustType == BundleAdjustType::BAT_NONE) {
    return true;
  }

  std::vector<const StitchInfo *> tempStitchInfos(stitchInfos.size());
  for(size_t i = 0; i < stitchInfos.size(); ++i) {
    tempStitchInfos[i] = stitchInfos[i].get();
  }

  bool baSuccess = false;
  std::vector<CameraParams> baCamParamsV;
  std::vector<MatchesInfo> matchesInfoV;
  std::vector<CameraParams> cameraParamsV;
  std::vector<ImageFeatures> imageFeaturesV;

  Helper::getData(keyPoints, descriptors, srcImagesSizes, fieldsOfView,
    rStitchOrder, cameraParamsV, imageFeaturesV);

  LogUtils::getLogUserInfo() << "Matches CamEstimate" << std::endl;
  Helper::getMatchesInfo(rStitchOrder, tempStitchInfos,
    *sifCamEstimate, matchesInfoV);

  logPairwiseMatches(matchesInfoV, false);
  logCameraParams(cameraParamsV, srcImagesSizes, rStitchOrder);

  if(camEstimate &&
     bundleAdjustType != BundleAdjustType::BAT_REPROJNOCAM &&
     bundleAdjustType != BundleAdjustType::BAT_REPROJNOCAMCAP200) {

    try {
      std::vector<CameraParams> estimatedCamParamsV;
      if(runCamEstimate(matchesInfoV, imageFeaturesV, estimatedCamParamsV)) {
        
        cameraParamsV.assign(estimatedCamParamsV.begin(), estimatedCamParamsV.end());
        logCameraParams(cameraParamsV, srcImagesSizes, rStitchOrder);
      }
      else {
        LogUtils::getLogUserError() << "Inital cam estimation failed" << std::endl;
        LogUtils::getLogUserInfo() << "Try to decrease confidence values." << std::endl;
        return false;
      }
    }
    catch(std::exception &e) {
      LogUtils::getLog() << e.what() << std::endl;
      LogUtils::getLogUserError() << "Inital cam estimation failed" << std::endl;
      LogUtils::getLogUserInfo() << "Try to decrease confidence values." << std::endl;
      return false;
    }
  }

  LogUtils::getLogUserInfo() << "Matches sifBundleAdjust" << std::endl;
  Helper::getMatchesInfo(rStitchOrder, tempStitchInfos,
    *sifBundleAdjust, matchesInfoV);
  
  switch(bundleAdjustType) {
    case BundleAdjustType::BAT_NONE:
      applyCamParams(cameraParamsV, camEstimate, false, rStitchOrder, rGlobalScale);
      return true;
    case BundleAdjustType::BAT_REPROJ:
    case BundleAdjustType::BAT_REPROJMINTILES:
    case BundleAdjustType::BAT_REPROJNOCAM:
      LogUtils::getLogUserInfo() << "Run BA reprojection" << std::endl;
      baSuccess = bundleAdjustReproj(matchesInfoV, imageFeaturesV, cameraParamsV, confidenceThresh, baCamParamsV, -1);
      break;
    case BundleAdjustType::BAT_REPROJCAP200:
    case BundleAdjustType::BAT_REPROJNOCAMCAP200:
      LogUtils::getLogUserInfo() << "Run BA reprojection cap" << std::endl;
      baSuccess = bundleAdjustReproj(matchesInfoV, imageFeaturesV, cameraParamsV, confidenceThresh, baCamParamsV, 200);
      break;
    case BundleAdjustType::BAT_RAY:
    case BundleAdjustType::BAT_RAYBLACKLIST:
    case BundleAdjustType::BAT_RAYCONFIDENCESTEP:
    case BundleAdjustType::BAT_RAYMINTILES:
      LogUtils::getLogUserInfo() << "Run BA ray" << std::endl;
      baSuccess = bundleAdjustRay(matchesInfoV, imageFeaturesV, cameraParamsV,
        confidenceThresh, baCamParamsV, -1, *sifBundleAdjust, *sifCamEstimate, *sifComputeOrder);
      break;
    default:{
    }
  }
  
  if(baSuccess) {
    applyCamParams(baCamParamsV, camEstimate, true, rStitchOrder, rGlobalScale);
    logCameraParams(baCamParamsV, srcImagesSizes, rStitchOrder);
  }
  else {
    // LogUtils::getLogUserError() << "Bundle adjust failed" << std::endl;

    // LogUtils::getLogUserError()
    //   << "Advice: "
    //   << "1. Increase/decrease confidence value, "
    //   << "2. Try a difference bundle adjustement type."
    //   << std::endl;

    applyCamParams(cameraParamsV, camEstimate, false, rStitchOrder, rGlobalScale);
  }
  return baSuccess;
}

void MultiStitcher::waveCorrection(TStitchOrder &rStitchOrder)
{
  FUNCLOGTIMEL("MultiStitcher::waveCorrection");
  
  if(wcType == WaveCorrectType::WCT_NONE) return;
  
  bool horizonal = wcType == WaveCorrectType::WCT_H;
  if(wcType == WaveCorrectType::WCT_V) {
    horizonal = false;
  }
  else if(wcType == WaveCorrectType::WCT_AUTO) {
    horizonal = estimatedFieldOfViewH >= estimatesFieldOfViewV;
  }

  LogUtils::getLogUserInfo() << "Wave correction "
    << (horizonal ? "horizontal" : "vertical") << std::endl;

  std::vector<TMat> rMats(rStitchOrder.size());
  size_t i = 0;
  for(auto *stitchInfo : rStitchOrder) {
    stitchInfo->matR.convertTo(rMats[i], CV_32F);
    ++i;
  }
  
  WarperHelper::waveCorrect(rMats, horizonal);

  i = 0;
  for(auto *stitchInfo : rStitchOrder) {
    rMats[i].convertTo(stitchInfo->matR, CV_64F);
    ++i;
  }
}

const MultiStitcher::TStitchOrder &
MultiStitcher::getStitchOrder() const
{
  FUNCLOGTIMEL("MultiStitcher::getStitchOrder");
  return stitchOrder;
}

void
MultiStitcher::applyLastRunData()
{
  FUNCLOGTIMEL("MultiStitcher::applyLastRunData");
  if(lastRunData == nullptr) {
    throw std::logic_error("applyLastRunData, no data available");
  }
  if(lastRunData->stitchOrderMatKs.size() != stitchOrder.size()) {
    throw std::logic_error("applyLastRunData, invalid data");
  }

  for(size_t i = 0; i < stitchOrder.size(); ++i) {
    lastRunData->stitchOrderMatRs[i].copyTo(stitchOrder[i]->matR);
    lastRunData->stitchOrderMatKs[i].copyTo(stitchOrder[i]->matK);
  }

  lastRunData->globalScale = globalScale;

  if(abort) lastRunData->aborted = true;
}

bool
MultiStitcher::isCamBasicDataUpToDate()
{
  FUNCLOGTIMEL("MultiStitcher::isCamBasicDataUpToDate");

  if(lastRunData == nullptr) return false;

  if(settings.getValue(eMultiStitch_bundleAdjustType) !=
    lastRunData->settings.getValue(eMultiStitch_bundleAdjustType)) return false;
  if(settings.getValue(eMultiStitch_camEstimate) !=
    lastRunData->settings.getValue(eMultiStitch_camEstimate)) return false;
  if(settings.getValue(eMultiStitch_calcImageOrder) !=
    lastRunData->settings.getValue(eMultiStitch_calcImageOrder)) return false;
  if(settings.getValue(eMultiStitch_calcCenterImage) !=
    lastRunData->settings.getValue(eMultiStitch_calcCenterImage)) return false;
  if(settings.getValue(eMultiStitch_confidenceThresh) !=
    lastRunData->settings.getValue(eMultiStitch_confidenceThresh)) return false;
  if(settings.getValue(eMultiStitch_confidenceThreshCam) !=
    lastRunData->settings.getValue(eMultiStitch_confidenceThreshCam)) return false;
  if(settings.getValue(eMultiStitch_inputImagesMatchReach) !=
    lastRunData->settings.getValue(eMultiStitch_inputImagesMatchReach)) return false;
  

  if(fieldsOfView.size() != lastRunData->fieldsOfView.size()) return false;
  for(size_t i = 0; i < fieldsOfView.size(); ++i) {
    if(fieldsOfView[i] != lastRunData->fieldsOfView[i]) return false;
  }

  return true;
}

void MultiStitcher::signalAbort()
{
  FUNCLOGTIMEL("MultiStitcher::signalAbort");
  abort = true;
}

const DesMatcher *MultiStitcher::getMatcher(int dstIndex)
{
  FUNCLOGTIMEL("MultiStitcher::getMatcher");

  auto it = matchers.find(dstIndex);
  if(it == matchers.end()) {
    return nullptr;
  }
  return &(it->second);
}

const DesMatcher &MultiStitcher::getMatcherRef(int dstIndex)
{
  FUNCLOGTIMEL("MultiStitcher::getMatcher");
  auto it = matchers.find(dstIndex);
  if(it == matchers.end()) {

    return matchers.insert(std::make_pair(
      dstIndex,
      FeatureFactory::CreateMatcher(settings))).first->second;

  }
  return it->second;

}

const DesMatcher *MultiStitcher::createMatcher(
  int dstIndex, TConstMat& inDescriptors, TConstKeyPoints &inKeyPoints)
{
  FUNCLOGTIMEL("MultiStitcher::createMatcher");

  if(getMatcher(dstIndex) != nullptr) {
    return nullptr;
  }

  return &(matchers.insert(std::make_pair(
      dstIndex,
      FeatureFactory::CreateMatcher(settings, &inDescriptors, &inKeyPoints))).first->second);
}

const DesMatcher &MultiStitcher::getMatcher(
  int dstIndex, TConstMat& inDescriptors, TConstKeyPoints &inKeyPoints)
{
  FUNCLOGTIMEL("MultiStitcher::getMatcher");

  auto *matcher = getMatcher(dstIndex);
  if(matcher != nullptr) {
    return *matcher;
  }

  return matchers.insert(std::make_pair(
      dstIndex,
      FeatureFactory::CreateMatcher(settings, &inDescriptors, &inKeyPoints))).first->second;
}


StitchedImage::StitchedInfo::StitchedInfo()
{
  FUNCLOGTIMEL("MultiStitcher::StitchedInfo");

  TMat eye = TMat::eye(3, 3, CV_64F);
  eye.copyTo(homography);
  eye.copyTo(rMat);
  eye.copyTo(kMat);
  tx = ty = 0.0;
  tlCorner.x = tlCorner.y = tlCornerRoi.x = tlCornerRoi.y = 0;
}

void
StitchedImage::init(
  const TMat &inImage,
  int inProjType,
  std::vector<const StitchInfo *> &stitchOrder,
  bool warpFirst,
  double *globalScale)
{
  FUNCLOGTIMEL("StitchedImage::init");
  auto startIndex = stitchOrder[0]->srcImageIndex;
  projType = inProjType;
  imageIndices.resize(1);
  imageIndices[0] = startIndex;

  stitchedInfos.clear();
  for(const auto *stitchInfo : stitchOrder) {
    stitchedInfos.insert(std::make_pair(stitchInfo->srcImageIndex, StitchedInfo()));
  }

  auto &stitchedInfo = stitchedInfos[startIndex];
  stitchOrder[0]->matR.copyTo(stitchedInfo.rMat);
  stitchOrder[0]->matK.copyTo(stitchedInfo.kMat);

  cv::Point tlCornerRoi(0, 0);
  TMat mask;
  ImageUtils::createMaskFor(inImage, mask);
  if(projType != eStitch_projectionTypeNone) {
    tlCornerRoi = WarperHelper::warpImage(projType, inImage, stitchedInfo.warpedImage, stitchedInfo.kMat, stitchedInfo.rMat, true, true, globalScale);
    WarperHelper::warpImage(projType, mask, stitchedInfo.warpedMask, stitchedInfo.kMat, stitchedInfo.rMat, false, false, globalScale);
  }
  else {
    inImage.copyTo(stitchedInfo.warpedImage);
    mask.copyTo(stitchedInfo.warpedMask);
  }

  if(!warpFirst) {
    setCornerRoiXFor(startIndex, tlCornerRoi.x);
    setCornerRoiYFor(startIndex, tlCornerRoi.y);
  }

  imageSize = stitchedInfo.warpedImage.size();
}

void
StitchedImage::addTranslation(double tx, double ty)
{
  FUNCLOGTIMEL("StitchedImage::addTranslation");

  for(auto i : imageIndices) {

    auto &stitchedInfo = stitchedInfos[i];
    stitchedInfo.tx += tx;
    stitchedInfo.ty += ty;
  }
}

TPoints2f
StitchedImage::getTransformedPts(
  TConstPoints2f &pts,
  const cv::Size &size,
  size_t inIndex,
  double *globalScale) const
{
  FUNCLOGTIMEL("StitchedImage::getTransformedPts");

  if(std::find_if(imageIndices.begin(), imageIndices.end(), [&](const size_t &index) {
    return index == inIndex;
  }) == imageIndices.end()) {
    LogUtils::getLog() << "error index: " << inIndex << std::endl;
    throw std::logic_error("Stitched image does not contain an image with the given index");
  }

  TPoints2f ptsWarped(pts.begin(), pts.end());
  auto it = stitchedInfos.find(inIndex);
  const StitchedInfo &stitchedInfo = it->second;

  WarperHelper::warpPoints(
    size.width,
    size.height,
    stitchedInfo.kMat,
    stitchedInfo.rMat,
    projType,
    ptsWarped,
    globalScale);

  ptsWarped = WarperHelper::transformPtsf(ptsWarped, stitchedInfo.homography);

  for(auto &pt : ptsWarped) {
    pt.x += stitchedInfo.tx;
    pt.y += stitchedInfo.ty;
  }

  return ptsWarped;
}

void StitchedImage::createMaskFor(size_t imageIndex)
{
  FUNCLOGTIMEL("StitchedImage::createMaskFor");

  auto &stitchedInfo = stitchedInfos[imageIndex];

  stitchedInfo.warpedMask = TMat::zeros(stitchedInfo.warpedImage.size(), CV_8UC1);

  auto itMask = stitchedInfo.warpedMask.begin<uint8_t>();
  auto imageBegin = stitchedInfo.warpedImage.begin<cv::Vec4b>();
  auto imageEnd = stitchedInfo.warpedImage.end<cv::Vec4b>();

  for(auto it = imageBegin; it != imageEnd; ++it, ++itMask) {
    // if((*it)[3] > 0) {
    //   *itMask = 255;
    // }
    if((*it)[3] == 255) {
      *itMask = (*it)[3];
    }
  }
}

void StitchedImage::setMaskFor(size_t imageIndex, TMat mat)
{
  FUNCLOGTIMEL("StitchedImage::setMaskFor");

  stitchedInfos[imageIndex].warpedMask = mat;
}

void
StitchedImage::setHomographyFor(size_t imageIndex, TConstMat &h)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  h.copyTo(stitchedInfo.homography);
}
void
StitchedImage::setKMatFor(size_t imageIndex, TConstMat &k)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  k.copyTo(stitchedInfo.kMat);
}
void
StitchedImage::setRMatFor(size_t imageIndex, TConstMat &r)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  r.copyTo(stitchedInfo.rMat);
}
void
StitchedImage::setXTranslationFor(size_t imageIndex, double tx)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.tx = tx;
}
void
StitchedImage::setYTranslationFor(size_t imageIndex, double ty)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.ty = ty;
}
void
StitchedImage::setCornerXFor(size_t imageIndex, double x)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.tlCorner.x = x;
}
void
StitchedImage::setCornerYFor(size_t imageIndex, double y)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.tlCorner.y = y;
}

void
StitchedImage::setCornerRoiXFor(size_t imageIndex, double x)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.tlCornerRoi.x = x;
}
void
StitchedImage::setCornerRoiYFor(size_t imageIndex, double y)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  stitchedInfo.tlCornerRoi.y = y;
}

TMat&
StitchedImage::warpedImage(size_t imageIndex)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  return stitchedInfo.warpedImage;
}

TMat&
StitchedImage::warpedMask(size_t imageIndex)
{
  auto &stitchedInfo = stitchedInfos[imageIndex];
  return stitchedInfo.warpedMask;
}

void
StitchedImage::stitchFast()
{
  FUNCLOGTIMEL("StitchedImage::stitchFast");

  std::vector<cv::Point> tlCornersI;
  std::vector<TMat> images;
  std::vector<TMat> masks;

  for(auto i : imageIndices) {

    auto &stitchedInfo = stitchedInfos[i];

    tlCornersI.push_back(Point(
      stitchedInfo.tx + stitchedInfo.tlCorner.x + stitchedInfo.tlCornerRoi.x,
      stitchedInfo.ty + stitchedInfo.tlCorner.y + stitchedInfo.tlCornerRoi.y));
    
    images.push_back(stitchedInfo.warpedImage);
    masks.push_back(stitchedInfo.warpedMask);
  }

  TMat tempImage;
  ImageUtils::stitchFast(
    images, masks, tlCornersI, tempImage);

  ImageUtils::crop(tempImage, image);
}


void
StitchedImage::stitch(
  bool compensateExposure,
  bool rectifyPerspective,
  bool rectifyStretch,
  bool maxRectangle, 
  BlendType blendType,
  double blendStrength,
  SeamFinderType seamFinderType,
  bool saveMemory)
{
  FUNCLOGTIMEL("StitchedImage::stitch");

  std::vector<cv::Point> tlCornersI;
  std::vector<cv::Point> tlCornersWarpedImage;
  std::vector<TMat> images;
  std::vector<TMat> masks;

  for(auto i : imageIndices) {

    auto &stitchedInfo = stitchedInfos[i];

    tlCornersI.push_back(Point(
      stitchedInfo.tx + stitchedInfo.tlCorner.x + stitchedInfo.tlCornerRoi.x,
      stitchedInfo.ty + stitchedInfo.tlCorner.y + stitchedInfo.tlCornerRoi.y));

    images.push_back(stitchedInfo.warpedImage);
    masks.push_back(stitchedInfo.warpedMask);
  }
  
  TMat tempImage;
  ImageUtils::stitch(
    images, masks, tlCornersI,
    compensateExposure,
    blendType,
    blendStrength,
    seamFinderType,
    saveMemory,
    tempImage);

  images.clear();
  masks.clear();

  for(auto i : imageIndices) {
    stitchedInfos[i].warpedImage.release();
    stitchedInfos[i].warpedMask.release();
  }

  LogUtils::getLogUserInfo() << "Cropping" << std::endl;

  TMat dstImage;
  ImageUtils::crop(tempImage, dstImage);
  tempImage = dstImage;
  
  if(rectifyPerspective) {
    LogUtils::getLogUserInfo() << "Rectifying perspective" << std::endl;
  }
  if(!rectifyPerspective || !WarperHelper::rectifyPerspective(tempImage, dstImage)) {
    dstImage = tempImage;
  }
  tempImage = dstImage;

  if(rectifyPerspective && rectifyStretch) {
    LogUtils::getLogUserInfo() << "Rectifying stretch" << std::endl;
    WarperHelper::rectifyStretch(tempImage, dstImage);
    tempImage = dstImage;
  }

  if(maxRectangle) {
    LogUtils::getLogUserInfo() << "Cropping to max rectangle" << std::endl;
    auto maxRectRoi = ImageUtils::maxRect(tempImage);

    LogUtils::getLogUserInfo() << "l/r/t/b "
      << maxRectRoi.tl().x << "/" << maxRectRoi.br().x << "/"
      << maxRectRoi.tl().y << "/" << maxRectRoi.br().y
      << std::endl;

    TMat matRoi(tempImage, maxRectRoi);
    tempImage = matRoi;
  }
  image = tempImage;
}

StitchInfo::StitchInfo(size_t srcI, size_t dstI)
  : srcImageIndex(srcI)
  , dstImageIndex(dstI)
  , matched(false)
{
  FUNCLOGTIMEL("StitchInfo::StitchInfo");
  //LogUtils::getLogUserInfo() << "StitchInfo::StitchInfo, srcIndex: " << srcImageIndex << std::endl;
  resetCamData();
}

void StitchInfo::resetCamData()
{
  FUNCLOGTIMEL("StitchInfo::resetCamData");

  absH = 0.0;
  absV = 0.0;
  TMat eye = TMat::eye(3, 3, CV_64F);
  eye.copyTo(matR);
  eye.copyTo(matK);
}

LastRunData::LastRunData(
  const Settings &inSettings,
  const std::vector<double> inFieldsOfView,
  const std::vector<const StitchInfo *> &inStitchOrder,
  double inGlobalScale)
{
  FUNCLOGTIMEL("LastRunData::LastRunData");

  settings = inSettings;
  fieldsOfView.assign(inFieldsOfView.begin(), inFieldsOfView.end());

  stitchOrderMatKs.resize(inStitchOrder.size());
  stitchOrderMatRs.resize(inStitchOrder.size());
  
  for(size_t i = 0; i < inStitchOrder.size(); ++i) {
    inStitchOrder[i]->matK.copyTo(stitchOrderMatKs[i]);
    inStitchOrder[i]->matR.copyTo(stitchOrderMatRs[i]);
  }

  globalScale = inGlobalScale;
}


StitchedImage::~StitchedImage()
{
  FUNCLOGTIMEL("StitchedImage::~StitchedImage");
  //LogUtils::getLogUserInfo() << "StitchedImage::~StitchedImage " << std::endl;
}
StitchInfo::~StitchInfo()
{
  FUNCLOGTIMEL("StitchInfo::~StitchInfo");
  //LogUtils::getLogUserInfo() << "StitchInfo::~StitchInfo, srcIndex: " << srcImageIndex << std::endl;
}

StitchedImage::StitchedInfo::~StitchedInfo()
{
  FUNCLOGTIMEL("StitchedImage::StitchedInfo::~StitchedInfo()");
  //LogUtils::getLogUserInfo() << "StitchedImage::StitchedInfo::~StitchedInfo() " << std::endl;
}

} // imgalign