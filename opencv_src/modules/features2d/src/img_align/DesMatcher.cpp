
#include "DesMatcher.h"
#include "Homography.h"
#include "LogUtils.h"
#include <opencv2/features2d.hpp>

using namespace cv;

namespace imgalign
{

namespace
{
  void filterSort(
    TConstMatches& inAllMatches, TMatches& ioGoodMatches,
    size_t minMatchesN, size_t maxMatchesN)
  {
    FUNCLOGTIMEL("DesMatcher::filterSort");

    struct Sort{
      bool operator()(const DMatch& m1, const DMatch& m2){
        return m1.distance < m2.distance;
      }
    } mySort;

    if(ioGoodMatches.size() < minMatchesN) {
      ioGoodMatches.assign(inAllMatches.begin(), inAllMatches.end());
      std::sort(ioGoodMatches.begin(), ioGoodMatches.end(), mySort);
      if(ioGoodMatches.size() > minMatchesN) {
        ioGoodMatches.resize(minMatchesN);
      }
    }
    else if(ioGoodMatches.size() > maxMatchesN) {
      ioGoodMatches.resize(maxMatchesN);
    }
    else {
      std::sort(ioGoodMatches.begin(), ioGoodMatches.end(), mySort);
    }
  }

  void
  getMatchingPoints(
    TConstMatches &inMatches,
    TConstKeyPoints &inKeyPts1, TConstKeyPoints &inKeyPts2,
    TPoints2f &outPts1, TPoints2f &outPts2)
  {
    FUNCLOGTIMEL("DesMatcher::getMatchingPoints");

    for(size_t i = 0; i < inMatches.size(); ++i){

      outPts1.push_back(inKeyPts1[inMatches[i].queryIdx].pt);
      outPts2.push_back(inKeyPts2[inMatches[i].trainIdx].pt);
    }
  }

  void
  getMatchingPoints(const TMatchInfos &inMatchesInfos, TPoints2f &outPts1, TPoints2f &outPts2)
  {
    FUNCLOGTIMEL("DesMatcher::getMatchingPoints");

    for(const auto &rMatchInfo : inMatchesInfos) {

      TConstKeyPoint& kPt1 = std::get<0>(rMatchInfo.first);
      TConstKeyPoint& kPt2 = std::get<1>(rMatchInfo.first);

      outPts1.push_back(kPt1.pt);
      outPts2.push_back(kPt2.pt);
    }
  }

  void
  sortMatchInfos(TMatchInfos& matchInfos)
  {
    FUNCLOGTIMEL("DesMatcher::sortMatchInfos");

    struct Sort{
      bool operator()(const TMatchInfo& matchInfo1, const TMatchInfo& matchInfo2){
        return matchInfo1.second.distance < matchInfo2.second.distance;
      }
    } mySort;

    std::sort(matchInfos.begin(), matchInfos.end(), mySort);
  }
} 

FilterInfo DesMatcher::filter(TConstMatches& inMatches, TMatches& outMatches) const
{
  FUNCLOGTIMEL("DesMatcher::filter");

  FilterInfo filterInfo;

  filterInfo.maxDist = 0;
	filterInfo.minDist = 100000;

	for(TConstMatches::const_iterator it = inMatches.begin(); it != inMatches.end(); ++it){

		if(it->distance < filterInfo.minDist) filterInfo.minDist = it->distance;
		if(it->distance > filterInfo.maxDist) filterInfo.maxDist = it->distance;
	}

	for(TConstMatches::const_iterator it = inMatches.begin(); it != inMatches.end(); ++it){

		if(it->distance < max(factor * filterInfo.minDist, factor * filterInfo.minDist)){
			outMatches.push_back(*it);
		}
	}

  filterSort(inMatches, outMatches, matchFilterMinMatchesToRetain, matchFilterMaxMatchesToRetain);

	filterInfo.maxDistUsed = outMatches.back().distance;
	filterInfo.maxDistFactorUsed = filterInfo.minDist > 0 ? filterInfo.maxDistUsed / filterInfo.minDist : 0;

	return filterInfo;
}

void DesMatcher::match(TConstMat& inDescriptors1, TConstMat& inDescriptors2, TMatches &outMatches) const
{
  FUNCLOGTIMEL("DesMatcher::match");

  if(  inDescriptors1.size().width == 0
    || inDescriptors1.size().height == 0
    || inDescriptors2.size().width == 0
    || inDescriptors2.size().height == 0) {
    
    return;
  }

  matcher->match(inDescriptors1, inDescriptors2, outMatches);
}

void DesMatcher::match(TConstMat& inQueryDescriptors, TMatches &outMatches) const
{
  FUNCLOGTIMEL("DesMatcher::match");

  if(  descriptors == nullptr
    || descriptors->size().width == 0
    || descriptors->size().height == 0
    || inQueryDescriptors.size().width == 0
    || inQueryDescriptors.size().height == 0) {
    
    return;
  }

  if(matcher->getTrainDescriptors().empty()) {
    throw std::logic_error("Can not match, train descriptors not available");
  }

  matcher->match(inQueryDescriptors, outMatches);
}

MatchInfo DesMatcher::match(
  TransformFinderType tfType,
  TConstMat& inQueryDescriptors,
  TConstKeyPoints &inQueryKeyPoints,
  DataExtractionMode dataExtractionMode) const
{
  FUNCLOGTIMEL("DesMatcher::match");
  
  if(descriptors == nullptr) {
    throw std::logic_error("Can not match, train descriptors not available");
  }
  if(keyPoints == nullptr) {
    throw std::logic_error("Can not match, keypoints not available");
  }

  TMatches matches;
  match(inQueryDescriptors, matches);
  return extractData(dataExtractionMode, tfType, matches, inQueryKeyPoints, *keyPoints);
}

MatchInfo DesMatcher::match(
  TransformFinderType tfType,
  TConstMat& inDescriptors1, TConstMat& inDescriptors2,
  TConstKeyPoints &keyPoints1, TConstKeyPoints &keyPoints2,
  DataExtractionMode dataExtractionMode) const
{
  FUNCLOGTIMEL("DesMatcher::match");

  TMatches matches;
  match(inDescriptors1, inDescriptors2, matches);
  return extractData(dataExtractionMode, tfType, matches, keyPoints1, keyPoints2);
}

MatchInfo DesMatcher::extractData(
  DataExtractionMode dataExtractionMode,
  TransformFinderType tfType,
  TMatches &ioMatches,
  TConstKeyPoints &keyPoints1,
  TConstKeyPoints &keyPoints2) const
{
  FUNCLOGTIMEL("DesMatcher::extractData");

  MatchInfo matchInfo;
  matchInfo.success = false;
  matchInfo.confidence = 0;
  matchInfo.allMatches = ioMatches;

  if(matchInfo.allMatches.empty()) {
    return matchInfo;
  }
  matchInfo.allMatchesCount = matchInfo.allMatches.size();

  matchInfo.filterInfo = filter(matchInfo.allMatches, matchInfo.filteredMatches);
  matchInfo.filteredMatchesCount = matchInfo.filteredMatches.size();
  
  if(dataExtractionMode == DataExtractionMode::eExtended) {
    for(auto it = matchInfo.allMatches.begin(); it != matchInfo.allMatches.end(); ++it) {
      matchInfo.allMatchInfos.push_back(TMatchInfo(
        TKeyPointPair(keyPoints1[it->queryIdx], keyPoints2[it->trainIdx]), *it));
    }
    sortMatchInfos(matchInfo.allMatchInfos);
  }
  else {
    matchInfo.allMatches.clear();
  }
  
  if(dataExtractionMode == DataExtractionMode::eExtended) {
    for(auto it = matchInfo.filteredMatches.begin(); it != matchInfo.filteredMatches.end(); ++it) {
      matchInfo.filteredMatchInfos.push_back(TMatchInfo(
        TKeyPointPair(keyPoints1[it->queryIdx], keyPoints2[it->trainIdx]), *it));
    }
    sortMatchInfos(matchInfo.filteredMatchInfos);
  }

  getMatchingPoints(matchInfo.filteredMatches, keyPoints1, keyPoints2, matchInfo.filteredPts1, matchInfo.filteredPts2);

  TBools mask;
  if(!Homography::getHomography(tfType, matchInfo.filteredPts1, matchInfo.filteredPts2, mask, matchInfo.homography)) {
    return matchInfo;
  }

  if(dataExtractionMode != DataExtractionMode::eExtended) {
    matchInfo.filteredPts1.clear();
    matchInfo.filteredPts2.clear();
  }

  TBools::const_iterator itMask = mask.begin();
  for(TMatches::const_iterator it = matchInfo.filteredMatches.begin();
      it != matchInfo.filteredMatches.end(); ++it, ++itMask){
    
    if(*itMask) {
      matchInfo.inlierMatchInfos.push_back(TMatchInfo(
        TKeyPointPair(keyPoints1[it->queryIdx], keyPoints2[it->trainIdx]), *it));
    }
    else if(dataExtractionMode == DataExtractionMode::eExtended){
      matchInfo.outlierMatchInfos.push_back(TMatchInfo(
        TKeyPointPair(keyPoints1[it->queryIdx], keyPoints2[it->trainIdx]), *it));
    }
  }
  matchInfo.inlierMatchesCount = matchInfo.inlierMatchInfos.size();
  matchInfo.outlierMatchesCount = matchInfo.filteredMatches.size() - matchInfo.inlierMatchesCount;

  if(dataExtractionMode != DataExtractionMode::eExtended) {
    matchInfo.filteredMatches.clear();
  }

  sortMatchInfos(matchInfo.inlierMatchInfos);
  if(dataExtractionMode == DataExtractionMode::eExtended) {
    sortMatchInfos(matchInfo.outlierMatchInfos);
  }

  if(dataExtractionMode != DataExtractionMode::eMin) {
    getMatchingPoints(matchInfo.inlierMatchInfos, matchInfo.inlierPts1, matchInfo.inlierPts2);
  }
  
  //matchInfo.confidence = ((double)matchInfo.inlierMatchInfos.size()) / matchInfo.filteredMatchInfos.size();
  matchInfo.confidence = matchInfo.inlierMatchesCount / (0.8 + 0.3 * matchInfo.filteredMatchesCount);

  matchInfo.success = true;
  matchInfo.determinant = Homography::determinant(matchInfo.homography);
  matchInfo.preservesOrientation = matchInfo.determinant > 0.0;

  matchInfo.svdConditionNumberIsSane = Homography::isSvdConditionNumberSane(
    matchInfo.homography, matchInfo.svdConditionNumber);

  return matchInfo;
}

bool MatchInfo::isHomographyGood() const
{
  //FUNCLOGTIMEL("DesMatcher::isHomographyGood");
  return success
      && determinant > 0.01
      && svdConditionNumberIsSane;
}

MatchInfo
MatchInfo::getInverse() const
{
  FUNCLOGTIMEL("DesMatcher::getInverse");

  MatchInfo matchInfo;

  matchInfo.filterInfo = filterInfo;
  matchInfo.success = success;
  matchInfo.confidence = confidence;
  matchInfo.preservesOrientation = preservesOrientation;
  matchInfo.determinant = determinant;
  matchInfo.svdConditionNumber = svdConditionNumber;
  matchInfo.svdConditionNumberIsSane = svdConditionNumberIsSane;

  matchInfo.allMatchesCount = allMatchesCount;
  matchInfo.filteredMatchesCount = filteredMatchesCount;
  matchInfo.inlierMatchesCount = inlierMatchesCount;
  matchInfo.outlierMatchesCount = outlierMatchesCount;

  auto invertMatches = [&](const TMatches &srcMatches, TMatches &dstMatches) {
    dstMatches.resize(srcMatches.size());
    for(size_t i = 0; i < srcMatches.size(); ++i) {
      dstMatches[i].queryIdx = srcMatches[i].trainIdx;
      dstMatches[i].trainIdx = srcMatches[i].queryIdx;
      dstMatches[i].imgIdx = srcMatches[i].imgIdx;
      dstMatches[i].distance = srcMatches[i].distance;
    }
  };
  auto invertMatchInfos = [&](const TMatchInfos &srcMatchInfos, TMatchInfos &dstMatchInfos) {
    dstMatchInfos.resize(srcMatchInfos.size());
    for(size_t i = 0; i < srcMatchInfos.size(); ++i) {
      
      dstMatchInfos[i].first = std::make_tuple(
        std::get<1>(srcMatchInfos[i].first),
        std::get<0>(srcMatchInfos[i].first));

      dstMatchInfos[i].second.queryIdx = srcMatchInfos[i].second.trainIdx;
      dstMatchInfos[i].second.trainIdx = srcMatchInfos[i].second.queryIdx;
      dstMatchInfos[i].second.imgIdx = srcMatchInfos[i].second.imgIdx;
      dstMatchInfos[i].second.distance = srcMatchInfos[i].second.distance;
    }
  };

  invertMatches(allMatches, matchInfo.allMatches); 
  invertMatches(filteredMatches, matchInfo.filteredMatches); 
  invertMatchInfos(allMatchInfos, matchInfo.allMatchInfos);
  invertMatchInfos(filteredMatchInfos, matchInfo.filteredMatchInfos);
  invertMatchInfos(inlierMatchInfos, matchInfo.inlierMatchInfos);
  invertMatchInfos(outlierMatchInfos, matchInfo.outlierMatchInfos);
  
  matchInfo.filteredPts1.assign(filteredPts2.begin(), filteredPts2.end());
  matchInfo.filteredPts2.assign(filteredPts1.begin(), filteredPts1.end());

  matchInfo.inlierPts1.assign(inlierPts2.begin(), inlierPts2.end());
  matchInfo.inlierPts2.assign(inlierPts1.begin(), inlierPts1.end());

  TMat hInv = homography.inv();
  hInv.copyTo(matchInfo.homography);

  return matchInfo;
}

void MatchInfo::logInfo(bool logH) const
{ 
  LogUtils::getLog() << "confidence: " << confidence << std::endl;
  LogUtils::getLog() << "determinant: " << determinant << std::endl;
  LogUtils::getLog() << "svdConditionNumber: " << svdConditionNumber << std::endl;
  LogUtils::getLog() << "svdConditionNumberIsSane: " << svdConditionNumberIsSane << std::endl;
  LogUtils::getLog() << "allMatchesCount: " << allMatchesCount << std::endl;
  LogUtils::getLog() << "filteredMatchesCount: " << filteredMatchesCount << std::endl;
  LogUtils::getLog() << "outlierMatchesCount: " << outlierMatchesCount << std::endl;
  LogUtils::getLog() << "inlierMatchesCount: " << inlierMatchesCount << std::endl;

  if(logH) {
    LogUtils::logMat("homography", homography);
  }
}

void MatchInfo::dismissDetailData()
{
  FUNCLOGTIMEL("DesMatcher::dismissAdvancedData");

  allMatches.clear();
  //filteredMatches.clear();
  allMatchInfos.clear();
  filteredMatchInfos.clear();
  //inlierMatchInfos.clear();
  outlierMatchInfos.clear();
  filteredPts1.clear();
  filteredPts2.clear();
}

void MatchInfo::getInlierPts(TPoints2f &pts1, TPoints2f &pts2) const
{
  if(!inlierPts1.empty() && !inlierPts2.empty()) {
    pts1.assign(inlierPts1.begin(), inlierPts1.end());
    pts2.assign(inlierPts2.begin(), inlierPts2.end());
  }
  else {
    getMatchingPoints(inlierMatchInfos, pts1, pts2);
  }
}

} // imgalign