// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

/// \brief write relevant information for muons.
/// \author daiki.sekihata@cern.ch

#include "PWGEM/Dilepton/DataModel/dileptonTables.h"

#include "Common/Core/TableHelper.h"
#include "Common/Core/fwdtrackUtilities.h"
#include "Common/DataModel/CollisionAssociationTables.h"

#include "CCDB/BasicCCDBManager.h"
#include "CommonConstants/PhysicsConstants.h"
#include "DataFormatsParameters/GRPMagField.h"
#include "DetectorsBase/Propagator.h"
#include "Field/MagneticField.h"
#include "Framework/AnalysisDataModel.h"
#include "Framework/AnalysisTask.h"
#include "Framework/DataTypes.h"
#include "Framework/runDataProcessing.h"
#include "GlobalTracking/MatchGlobalFwd.h"
#include "MCHTracking/TrackExtrap.h"
#include "MCHTracking/TrackParam.h"
#include "ReconstructionDataFormats/TrackFwd.h"

#include "Math/SMatrix.h"
#include "Math/Vector4D.h"
#include "TGeoGlobalMagField.h"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace o2;
using namespace o2::soa;
using namespace o2::framework;
using namespace o2::framework::expressions;
using namespace o2::constants::physics;
using namespace o2::aod::fwdtrackutils;

struct skimmerPrimaryMuon {
  using MyCollisions = soa::Join<aod::Collisions, aod::EvSels, aod::EMEvSels>;
  using MyCollisionsWithSWT = soa::Join<MyCollisions, aod::EMSWTriggerInfosTMP>;

  using MyFwdTracks = soa::Join<aod::FwdTracks, aod::FwdTracksCov>; // muon tracks are repeated. i.e. not exclusive.
  using MyFwdTrack = MyFwdTracks::iterator;

  using MyFwdTracksMC = soa::Join<MyFwdTracks, aod::McFwdTrackLabels>;
  using MyFwdTrackMC = MyFwdTracksMC::iterator;

  using MFTTracksMC = soa::Join<o2::aod::MFTTracks, aod::McMFTTrackLabels>;
  using MFTTrackMC = MFTTracksMC::iterator;

  Produces<aod::EMPrimaryMuons> emprimarymuons;
  Produces<aod::EMPrimaryMuonsCov> emprimarymuonscov;

  // Configurables
  Configurable<std::string> ccdburl{"ccdb-url", "http://alice-ccdb.cern.ch", "url of the ccdb repository"};
  Configurable<std::string> grpmagPath{"grpmagPath", "GLO/Config/GRPMagField", "CCDB path of the GRPMagField object"};
  Configurable<std::string> geoPath{"geoPath", "GLO/Config/GeometryAligned", "Path of the geometry file"};
  Configurable<bool> fillQAHistograms{"fillQAHistograms", false, "flag to fill QA histograms"};
  Configurable<float> minPt{"minPt", 0.2, "min pt for muon"};
  Configurable<float> maxPt{"maxPt", 1e+10, "max pt for muon"};
  Configurable<float> minEtaSA{"minEtaSA", -4.0, "min. eta acceptance for MCH-MID"};
  Configurable<float> maxEtaSA{"maxEtaSA", -2.5, "max. eta acceptance for MCH-MID"};
  Configurable<float> minEtaGL{"minEtaGL", -3.6, "min. eta acceptance for MFT-MCH-MID"};
  Configurable<float> maxEtaGL{"maxEtaGL", -2.5, "max. eta acceptance for MFT-MCH-MID"};
  Configurable<float> minRabsGL{"minRabsGL", 27.6, "min. R at absorber end for global muon (min. eta = -3.6)"}; // std::tan(2.f * std::atan(std::exp(- -3.6)) ) * -505.
  Configurable<float> minRabs{"minRabs", 17.6, "min. R at absorber end"};
  Configurable<float> midRabs{"midRabs", 26.5, "middle R at absorber end for pDCA cut"};
  Configurable<float> maxRabs{"maxRabs", 89.5, "max. R at absorber end"};
  Configurable<float> maxDCAxy{"maxDCAxy", 1e+10, "max. DCAxy for global muons"};
  Configurable<float> maxPDCAforLargeR{"maxPDCAforLargeR", 324.f, "max. pDCA for large R at absorber end"};
  Configurable<float> maxPDCAforSmallR{"maxPDCAforSmallR", 594.f, "max. pDCA for small R at absorber end"};
  Configurable<float> maxMatchingChi2MCHMFT{"maxMatchingChi2MCHMFT", 50.f, "max. chi2 for MCH-MFT matching"};
  Configurable<float> maxChi2SA{"maxChi2SA", 1e+6, "max. chi2 for standalone muon"};
  Configurable<float> maxChi2GL{"maxChi2GL", 1e+6, "max. chi2 for global muon"};
  Configurable<bool> refitGlobalMuon{"refitGlobalMuon", true, "flag to refit global muon"};

  o2::ccdb::CcdbApi ccdbApi;
  Service<o2::ccdb::BasicCCDBManager> ccdb;
  int mRunNumber;

  HistogramRegistry fRegistry{"output", {}, OutputObjHandlingPolicy::AnalysisObject, false, false};
  static constexpr std::string_view muon_types[5] = {"MFTMCHMID/", "MFTMCHMIDOtherMatch/", "MFTMCH/", "MCHMID/", "MCH/"};

  void init(InitContext&)
  {
    ccdb->setURL(ccdburl);
    ccdb->setCaching(true);
    ccdb->setLocalObjectValidityChecking();
    ccdb->setFatalWhenNull(false);
    ccdbApi.init(ccdburl);

    if (fillQAHistograms) {
      addHistograms();
    }
    mRunNumber = 0;
  }

  void initCCDB(aod::BCsWithTimestamps::iterator const& bc)
  {
    if (mRunNumber == bc.runNumber()) {
      return;
    }
    mRunNumber = bc.runNumber();

    std::map<string, string> metadata;
    auto soreor = o2::ccdb::BasicCCDBManager::getRunDuration(ccdbApi, mRunNumber);
    auto ts = soreor.first;
    auto grpmag = ccdbApi.retrieveFromTFileAny<o2::parameters::GRPMagField>(grpmagPath, metadata, ts);
    o2::base::Propagator::initFieldFromGRP(grpmag);
    if (!o2::base::GeometryManager::isGeometryLoaded()) {
      ccdb->get<TGeoManager>(geoPath);
    }
    o2::mch::TrackExtrap::setField();
  }

  void addHistograms()
  {
    auto hMuonType = fRegistry.add<TH1>("hMuonType", "muon type", kTH1F, {{5, -0.5f, 4.5f}}, false);
    hMuonType->GetXaxis()->SetBinLabel(1, "MFT-MCH-MID (global muon)");
    hMuonType->GetXaxis()->SetBinLabel(2, "MFT-MCH-MID (global muon other match)");
    hMuonType->GetXaxis()->SetBinLabel(3, "MFT-MCH");
    hMuonType->GetXaxis()->SetBinLabel(4, "MCH-MID");
    hMuonType->GetXaxis()->SetBinLabel(5, "MCH standalone");

    fRegistry.add("MFTMCHMID/hPt", "pT;p_{T} (GeV/c)", kTH1F, {{100, 0.0f, 10}}, false);
    fRegistry.add("MFTMCHMID/hEtaPhi", "#eta vs. #varphi;#varphi (rad.);#eta", kTH2F, {{180, 0, 2 * M_PI}, {60, -5.f, -2.f}}, false);
    fRegistry.add("MFTMCHMID/hEtaPhi_MatchedMCHMID", "#eta vs. #varphi;#varphi (rad.);#eta", kTH2F, {{180, 0, 2 * M_PI}, {60, -5.f, -2.f}}, false);
    fRegistry.add("MFTMCHMID/hDeltaPt_Pt", "#Deltap_{T}/p_{T} vs. p_{T};p_{T}^{gl} (GeV/c);(p_{T}^{sa} - p_{T}^{gl})/p_{T}^{gl}", kTH2F, {{100, 0, 10}, {200, -0.5, +0.5}}, false);
    fRegistry.add("MFTMCHMID/hDeltaEta_Pt", "#Delta#eta vs. p_{T};p_{T}^{gl} (GeV/c);#Delta#eta", kTH2F, {{100, 0, 10}, {200, -0.5, +0.5}}, false);
    fRegistry.add("MFTMCHMID/hDeltaPhi_Pt", "#Delta#varphi vs. p_{T};p_{T}^{gl} (GeV/c);#Delta#varphi (rad.)", kTH2F, {{100, 0, 10}, {200, -0.5, +0.5}}, false);
    fRegistry.add("MFTMCHMID/hSign", "sign;sign", kTH1F, {{3, -1.5, +1.5}}, false);
    fRegistry.add("MFTMCHMID/hNclusters", "Nclusters;Nclusters", kTH1F, {{21, -0.5f, 20.5}}, false);
    fRegistry.add("MFTMCHMID/hNclustersMFT", "NclustersMFT;Nclusters MFT", kTH1F, {{11, -0.5f, 10.5}}, false);
    fRegistry.add("MFTMCHMID/hRatAbsorberEnd", "R at absorber end;R at absorber end (cm)", kTH1F, {{100, 0.0f, 100}}, false);
    fRegistry.add("MFTMCHMID/hPDCA_Rabs", "pDCA vs. Rabs;R at absorber end (cm);p #times DCA (GeV/c #upoint cm)", kTH2F, {{100, 0, 100}, {100, 0.0f, 1000}}, false);
    fRegistry.add("MFTMCHMID/hChi2", "chi2;chi2/ndf", kTH1F, {{100, 0.0f, 10}}, false);
    fRegistry.add("MFTMCHMID/hChi2MFT", "chi2 MFT;chi2 MFT/ndf", kTH1F, {{100, 0.0f, 10}}, false);
    fRegistry.add("MFTMCHMID/hChi2MatchMCHMID", "chi2 match MCH-MID;chi2", kTH1F, {{100, 0.0f, 100}}, false);
    fRegistry.add("MFTMCHMID/hChi2MatchMCHMFT", "chi2 match MCH-MFT;chi2", kTH1F, {{100, 0.0f, 100}}, false);
    fRegistry.add("MFTMCHMID/hDCAxy2D", "DCA x vs. y;DCA_{x} (cm);DCA_{y} (cm)", kTH2F, {{200, -1, 1}, {200, -1, +1}}, false);
    fRegistry.add("MFTMCHMID/hDCAxy2DinSigma", "DCA x vs. y in sigma;DCA_{x} (#sigma);DCA_{y} (#sigma)", kTH2F, {{200, -10, 10}, {200, -10, +10}}, false);
    fRegistry.add("MFTMCHMID/hDCAxy", "DCAxy;DCA_{xy} (cm);", kTH1F, {{100, 0, 1}}, false);
    fRegistry.add("MFTMCHMID/hDCAxyinSigma", "DCAxy in sigma;DCA_{xy} (#sigma);", kTH1F, {{100, 0, 10}}, false);
    fRegistry.addClone("MFTMCHMID/", "MCHMID/");
    fRegistry.add("MFTMCHMID/hDCAxResolutionvsPt", "DCA_{x} vs. p_{T};p_{T} (GeV/c);DCA_{x} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 500}}, false);
    fRegistry.add("MFTMCHMID/hDCAyResolutionvsPt", "DCA_{y} vs. p_{T};p_{T} (GeV/c);DCA_{y} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 500}}, false);
    fRegistry.add("MFTMCHMID/hDCAxyResolutionvsPt", "DCA_{xy} vs. p_{T};p_{T} (GeV/c);DCA_{y} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 500}}, false);
    fRegistry.add("MCHMID/hDCAxResolutionvsPt", "DCA_{x} vs. p_{T};p_{T} (GeV/c);DCA_{x} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 5e+5}}, false);
    fRegistry.add("MCHMID/hDCAyResolutionvsPt", "DCA_{y} vs. p_{T};p_{T} (GeV/c);DCA_{y} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 5e+5}}, false);
    fRegistry.add("MCHMID/hDCAxyResolutionvsPt", "DCA_{xy} vs. p_{T};p_{T} (GeV/c);DCA_{y} resolution (#mum);", kTH2F, {{100, 0, 10.f}, {500, 0, 5e+5}}, false);
  }

  bool isSelected(const float pt, const float eta, const float rAtAbsorberEnd, const float pDCA, const float chi2_per_ndf, const uint8_t trackType, const float dcaXY)
  {
    if (pt < minPt || maxPt < pt) {
      return false;
    }
    if (rAtAbsorberEnd < minRabs || maxRabs < rAtAbsorberEnd) {
      return false;
    }
    if (rAtAbsorberEnd < midRabs ? pDCA > maxPDCAforSmallR : pDCA > maxPDCAforLargeR) {
      return false;
    }

    if (trackType == static_cast<uint8_t>(o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack)) {
      if (eta < minEtaGL || maxEtaGL < eta) {
        return false;
      }
      if (maxDCAxy < dcaXY) {
        return false;
      }
      if (maxChi2GL < chi2_per_ndf) {
        return false;
      }
      if (rAtAbsorberEnd < minRabsGL || maxRabs < rAtAbsorberEnd) {
        return false;
      }
    } else if (trackType == static_cast<uint8_t>(o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack)) {
      if (eta < minEtaSA || maxEtaSA < eta) {
        return false;
      }
      if (maxChi2SA < chi2_per_ndf) {
        return false;
      }
    } else {
      return false;
    }

    return true;
  }

  template <typename TFwdTracks, typename TMFTTracks, typename TCollision, typename TFwdTrack>
  void fillFwdTrackTable(TCollision const& collision, TFwdTrack fwdtrack, const bool isAmbiguous)
  {
    if (fwdtrack.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.chi2MatchMCHMFT() > maxMatchingChi2MCHMFT) {
      return;
    } // Users have to decide the best match between MFT and MCH-MID at analysis level. The same global muon is repeatedly stored.

    if (fwdtrack.chi2MatchMCHMID() < 0.f) { // this should never happen. only for protection.
      return;
    }

    if (fwdtrack.chi2() < 0.f) { // this should never happen. only for protection.
      return;
    }

    o2::dataformats::GlobalFwdTrack propmuonAtPV = propagateMuon(fwdtrack, collision, propagationPoint::kToVertex);
    float pt = propmuonAtPV.getPt();
    float eta = propmuonAtPV.getEta();
    float phi = propmuonAtPV.getPhi();
    o2::math_utils::bringTo02Pi(phi);

    o2::dataformats::GlobalFwdTrack propmuonAtDCA = propagateMuon(fwdtrack, collision, propagationPoint::kToDCA);
    float cXXatDCA = propmuonAtDCA.getSigma2X();
    float cYYatDCA = propmuonAtDCA.getSigma2Y();
    float cXYatDCA = propmuonAtDCA.getSigmaXY();

    float dcaX = propmuonAtDCA.getX() - collision.posX();
    float dcaY = propmuonAtDCA.getY() - collision.posY();
    float dcaXY = std::sqrt(dcaX * dcaX + dcaY * dcaY);
    float rAtAbsorberEnd = fwdtrack.rAtAbsorberEnd(); // this works only for GlobalMuonTrack

    float det = cXXatDCA * cYYatDCA - cXYatDCA * cXYatDCA; // determinanat
    float dcaXYinSigma = 999.f;
    if (det < 0) {
      dcaXYinSigma = 999.f;
    } else {
      dcaXYinSigma = std::sqrt(std::fabs((dcaX * dcaX * cYYatDCA + dcaY * dcaY * cXXatDCA - 2.f * dcaX * dcaY * cXYatDCA) / det / 2.f)); // dca xy in sigma
    }
    float sigma_dcaXY = dcaXY / dcaXYinSigma;

    float pDCA = fwdtrack.p() * dcaXY;
    int nClustersMFT = 0;
    float ptMatchedMCHMID = propmuonAtPV.getPt();
    float etaMatchedMCHMID = propmuonAtPV.getEta();
    float phiMatchedMCHMID = propmuonAtPV.getPhi();
    o2::math_utils::bringTo02Pi(phiMatchedMCHMID);
    // float x = fwdtrack.x();
    // float y = fwdtrack.y();
    // float z = fwdtrack.z();
    // float tgl = fwdtrack.tgl();
    float chi2mft = 0.f;
    uint64_t mftClusterSizesAndTrackFlags = 0;
    int ndf_mchmft = 1;

    if (fwdtrack.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack) {
      // apply r-absorber cut here to minimize the number of calling propagateMuon.
      if (fwdtrack.rAtAbsorberEnd() < minRabsGL || maxRabs < fwdtrack.rAtAbsorberEnd()) {
        return;
      }

      // apply dca cut here to minimize the number of calling propagateMuon.
      if (maxDCAxy < dcaXY) {
        return;
      }

      const auto& mchtrack = fwdtrack.template matchMCHTrack_as<TFwdTracks>(); // MCH-MID
      const auto& mfttrack = fwdtrack.template matchMFTTrack_as<TMFTTracks>(); // MFTsa
      nClustersMFT = mfttrack.nClusters();
      mftClusterSizesAndTrackFlags = mfttrack.mftClusterSizesAndTrackFlags();
      ndf_mchmft = 2.f * (mchtrack.nClusters() + nClustersMFT) - 5.f;
      chi2mft = mfttrack.chi2();
      // chi2mft = mfttrack.chi2() / (2.f * nClustersMFT - 5.f);

      // apply chi2/ndf cut here to minimize the number of calling propagateMuon.
      if (maxChi2GL < fwdtrack.chi2() / ndf_mchmft) {
        return;
      }

      o2::dataformats::GlobalFwdTrack propmuonAtPV_Matched = propagateMuon(mchtrack, collision, propagationPoint::kToVertex);
      ptMatchedMCHMID = propmuonAtPV_Matched.getPt();
      etaMatchedMCHMID = propmuonAtPV_Matched.getEta();
      phiMatchedMCHMID = propmuonAtPV_Matched.getPhi();
      o2::math_utils::bringTo02Pi(phiMatchedMCHMID);

      o2::dataformats::GlobalFwdTrack propmuonAtDCA_Matched = propagateMuon(mchtrack, collision, propagationPoint::kToDCA);
      float dcaX_Matched = propmuonAtDCA_Matched.getX() - collision.posX();
      float dcaY_Matched = propmuonAtDCA_Matched.getY() - collision.posY();
      float dcaXY_Matched = std::sqrt(dcaX_Matched * dcaX_Matched + dcaY_Matched * dcaY_Matched);
      pDCA = mchtrack.p() * dcaXY_Matched;

      if (refitGlobalMuon) {
        eta = mfttrack.eta();
        phi = mfttrack.phi();
        o2::math_utils::bringTo02Pi(phi);
        pt = propmuonAtPV_Matched.getP() * std::sin(2.f * std::atan(std::exp(-eta)));

        // x = mfttrack.x();
        // y = mfttrack.y();
        // z = mfttrack.z();
        // tgl = mfttrack.tgl();
      }
    } else if (fwdtrack.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
      o2::dataformats::GlobalFwdTrack propmuonAtRabs = propagateMuon(fwdtrack, collision, propagationPoint::kToRabs); // this is necessary only for MuonStandaloneTrack
      float xAbs = propmuonAtRabs.getX();
      float yAbs = propmuonAtRabs.getY();
      rAtAbsorberEnd = std::sqrt(xAbs * xAbs + yAbs * yAbs); // Redo propagation only for muon tracks // propagation of MFT tracks alredy done in reconstruction
    } else {
      return;
    }

    if (!isSelected(pt, eta, rAtAbsorberEnd, pDCA, fwdtrack.chi2() / ndf_mchmft, fwdtrack.trackType(), dcaXY)) {
      return;
    }

    float dpt = (ptMatchedMCHMID - pt) / pt;
    float deta = etaMatchedMCHMID - eta;
    float dphi = phiMatchedMCHMID - phi;
    o2::math_utils::bringToPMPi(dphi);

    bool isAssociatedToMPC = fwdtrack.collisionId() == collision.globalIndex();
    // LOGF(info, "isAmbiguous = %d, isAssociatedToMPC = %d, fwdtrack.globalIndex() = %d, fwdtrack.collisionId() = %d, collision.globalIndex() = %d", isAmbiguous, isAssociatedToMPC, fwdtrack.globalIndex(), fwdtrack.collisionId(), collision.globalIndex());

    emprimarymuons(collision.globalIndex(), fwdtrack.globalIndex(), fwdtrack.matchMFTTrackId(), fwdtrack.matchMCHTrackId(), fwdtrack.trackType(),
                   pt, eta, phi, fwdtrack.sign(), dcaX, dcaY, cXXatDCA, cYYatDCA, cXYatDCA, ptMatchedMCHMID, etaMatchedMCHMID, phiMatchedMCHMID,
                   // x, y, z, tgl,
                   fwdtrack.nClusters(), pDCA, rAtAbsorberEnd, fwdtrack.chi2(), fwdtrack.chi2MatchMCHMID(), fwdtrack.chi2MatchMCHMFT(),
                   fwdtrack.mchBitMap(), fwdtrack.midBitMap(), fwdtrack.midBoards(), mftClusterSizesAndTrackFlags, chi2mft, isAssociatedToMPC, isAmbiguous);

    const auto& fwdcov = propmuonAtPV.getCovariances(); // covatiant matrix at PV
    emprimarymuonscov(
      fwdcov(0, 0),
      fwdcov(0, 1), fwdcov(1, 1),
      fwdcov(2, 0), fwdcov(2, 1), fwdcov(2, 2),
      fwdcov(3, 0), fwdcov(3, 1), fwdcov(3, 2), fwdcov(3, 3),
      fwdcov(4, 0), fwdcov(4, 1), fwdcov(4, 2), fwdcov(4, 3), fwdcov(4, 4));

    // See definition DataFormats/Reconstruction/include/ReconstructionDataFormats/TrackFwd.h
    // Covariance matrix of track parameters, ordered as follows:
    //  <X,X>         <Y,X>           <PHI,X>       <TANL,X>        <INVQPT,X>
    //  <X,Y>         <Y,Y>           <PHI,Y>       <TANL,Y>        <INVQPT,Y>
    // <X,PHI>       <Y,PHI>         <PHI,PHI>     <TANL,PHI>      <INVQPT,PHI>
    // <X,TANL>      <Y,TANL>       <PHI,TANL>     <TANL,TANL>     <INVQPT,TANL>
    // <X,INVQPT>   <Y,INVQPT>     <PHI,INVQPT>   <TANL,INVQPT>   <INVQPT,INVQPT>

    if (fillQAHistograms) {
      fRegistry.fill(HIST("hMuonType"), fwdtrack.trackType());
      if (fwdtrack.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack) {
        fRegistry.fill(HIST("MFTMCHMID/hPt"), pt);
        fRegistry.fill(HIST("MFTMCHMID/hEtaPhi"), phi, eta);
        fRegistry.fill(HIST("MFTMCHMID/hEtaPhi_MatchedMCHMID"), phiMatchedMCHMID, etaMatchedMCHMID);
        fRegistry.fill(HIST("MFTMCHMID/hDeltaPt_Pt"), pt, dpt);
        fRegistry.fill(HIST("MFTMCHMID/hDeltaEta_Pt"), pt, deta);
        fRegistry.fill(HIST("MFTMCHMID/hDeltaPhi_Pt"), pt, dphi);
        fRegistry.fill(HIST("MFTMCHMID/hSign"), fwdtrack.sign());
        fRegistry.fill(HIST("MFTMCHMID/hNclusters"), fwdtrack.nClusters());
        fRegistry.fill(HIST("MFTMCHMID/hNclustersMFT"), nClustersMFT);
        fRegistry.fill(HIST("MFTMCHMID/hPDCA_Rabs"), rAtAbsorberEnd, pDCA);
        fRegistry.fill(HIST("MFTMCHMID/hRatAbsorberEnd"), rAtAbsorberEnd);
        fRegistry.fill(HIST("MFTMCHMID/hChi2"), fwdtrack.chi2() / ndf_mchmft);
        fRegistry.fill(HIST("MFTMCHMID/hChi2MFT"), chi2mft);
        fRegistry.fill(HIST("MFTMCHMID/hChi2MatchMCHMID"), fwdtrack.chi2MatchMCHMID());
        fRegistry.fill(HIST("MFTMCHMID/hChi2MatchMCHMFT"), fwdtrack.chi2MatchMCHMFT());
        fRegistry.fill(HIST("MFTMCHMID/hDCAxy2D"), dcaX, dcaY);
        fRegistry.fill(HIST("MFTMCHMID/hDCAxy2DinSigma"), dcaX / std::sqrt(cXXatDCA), dcaY / std::sqrt(cYYatDCA));
        fRegistry.fill(HIST("MFTMCHMID/hDCAxy"), dcaXY);
        fRegistry.fill(HIST("MFTMCHMID/hDCAxyinSigma"), dcaXYinSigma);
        fRegistry.fill(HIST("MFTMCHMID/hDCAxResolutionvsPt"), pt, std::sqrt(cXXatDCA) * 1e+4); // convert cm to um
        fRegistry.fill(HIST("MFTMCHMID/hDCAyResolutionvsPt"), pt, std::sqrt(cYYatDCA) * 1e+4); // convert cm to um
        fRegistry.fill(HIST("MFTMCHMID/hDCAxyResolutionvsPt"), pt, sigma_dcaXY * 1e+4);        // convert cm to um
      } else if (fwdtrack.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
        fRegistry.fill(HIST("MCHMID/hPt"), pt);
        fRegistry.fill(HIST("MCHMID/hEtaPhi"), phi, eta);
        fRegistry.fill(HIST("MCHMID/hEtaPhi_MatchedMCHMID"), phiMatchedMCHMID, etaMatchedMCHMID);
        fRegistry.fill(HIST("MCHMID/hDeltaPt_Pt"), pt, dpt);
        fRegistry.fill(HIST("MCHMID/hDeltaEta_Pt"), pt, deta);
        fRegistry.fill(HIST("MCHMID/hDeltaPhi_Pt"), pt, dphi);
        fRegistry.fill(HIST("MCHMID/hSign"), fwdtrack.sign());
        fRegistry.fill(HIST("MCHMID/hNclusters"), fwdtrack.nClusters());
        fRegistry.fill(HIST("MCHMID/hNclustersMFT"), nClustersMFT);
        fRegistry.fill(HIST("MCHMID/hPDCA_Rabs"), rAtAbsorberEnd, pDCA);
        fRegistry.fill(HIST("MCHMID/hRatAbsorberEnd"), rAtAbsorberEnd);
        fRegistry.fill(HIST("MCHMID/hChi2"), fwdtrack.chi2());
        fRegistry.fill(HIST("MCHMID/hChi2MFT"), chi2mft);
        fRegistry.fill(HIST("MCHMID/hChi2MatchMCHMID"), fwdtrack.chi2MatchMCHMID());
        fRegistry.fill(HIST("MCHMID/hChi2MatchMCHMFT"), fwdtrack.chi2MatchMCHMFT());
        fRegistry.fill(HIST("MCHMID/hDCAxy2D"), dcaX, dcaY);
        fRegistry.fill(HIST("MCHMID/hDCAxy2DinSigma"), dcaX / std::sqrt(cXXatDCA), dcaY / std::sqrt(cYYatDCA));
        fRegistry.fill(HIST("MCHMID/hDCAxy"), dcaXY);
        fRegistry.fill(HIST("MCHMID/hDCAxyinSigma"), dcaXYinSigma);
        fRegistry.fill(HIST("MCHMID/hDCAxResolutionvsPt"), pt, std::sqrt(cXXatDCA) * 1e+4); // convert cm to um
        fRegistry.fill(HIST("MCHMID/hDCAyResolutionvsPt"), pt, std::sqrt(cYYatDCA) * 1e+4); // convert cm to um
        fRegistry.fill(HIST("MCHMID/hDCAxyResolutionvsPt"), pt, sigma_dcaXY * 1e+4);        // convert cm to um
      }
    }
  }

  SliceCache cache;

  Preslice<aod::FwdTracks> perCollision = o2::aod::fwdtrack::collisionId;
  Preslice<aod::FwdTrackAssoc> fwdtrackIndicesPerCollision = aod::track_association::collisionId;
  PresliceUnsorted<aod::FwdTrackAssoc> fwdtrackIndicesPerFwdTrack = aod::track_association::fwdtrackId;

  void processRec_SA(MyCollisions const& collisions, MyFwdTracks const& fwdtracks, aod::MFTTracks const&, aod::BCsWithTimestamps const&)
  {
    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);

      if (!collision.isSelected()) {
        continue;
      }

      const auto& fwdtracks_per_coll = fwdtracks.sliceBy(perCollision, collision.globalIndex());
      for (const auto& fwdtrack : fwdtracks_per_coll) {
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracks, aod::MFTTracks>(collision, fwdtrack, false);
      } // end of fwdtrack loop
    } // end of collision loop
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processRec_SA, "process reconstructed info", false);

  void processRec_TTCA(MyCollisions const& collisions, MyFwdTracks const& fwdtracks, aod::MFTTracks const&, aod::BCsWithTimestamps const&, aod::FwdTrackAssoc const& fwdtrackIndices)
  {
    std::unordered_map<int64_t, bool> mapAmb; // fwdtrack.globalIndex() -> bool isAmb;
    for (const auto& fwdtrack : fwdtracks) {
      const auto& fwdtrackIdsPerFwdTrack = fwdtrackIndices.sliceBy(fwdtrackIndicesPerFwdTrack, fwdtrack.globalIndex());
      mapAmb[fwdtrack.globalIndex()] = fwdtrackIdsPerFwdTrack.size() > 1;
      // LOGF(info, "fwdtrack.globalIndex() = %d, ntimes = %d, isAmbiguous = %d", fwdtrack.globalIndex(), fwdtrackIdsPerFwdTrack.size(), mapAmb[fwdtrack.globalIndex()]);
    } // end of fwdtrack loop

    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);

      if (!collision.isSelected()) {
        continue;
      }

      const auto& fwdtrackIdsThisCollision = fwdtrackIndices.sliceBy(fwdtrackIndicesPerCollision, collision.globalIndex());
      for (const auto& fwdtrackId : fwdtrackIdsThisCollision) {
        const auto& fwdtrack = fwdtrackId.template fwdtrack_as<MyFwdTracks>();
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracks, aod::MFTTracks>(collision, fwdtrack, mapAmb[fwdtrack.globalIndex()]);
      } // end of fwdtrack loop
    } // end of collision loop
    mapAmb.clear();
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processRec_TTCA, "process reconstructed info", false);

  void processRec_SA_SWT(MyCollisionsWithSWT const& collisions, MyFwdTracks const& fwdtracks, aod::MFTTracks const&, aod::BCsWithTimestamps const&)
  {
    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);

      if (!collision.isSelected()) {
        continue;
      }

      if (collision.swtaliastmp_raw() == 0) {
        continue;
      }

      const auto& fwdtracks_per_coll = fwdtracks.sliceBy(perCollision, collision.globalIndex());
      for (const auto& fwdtrack : fwdtracks_per_coll) {
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracks, aod::MFTTracks>(collision, fwdtrack, false);
      } // end of fwdtrack loop
    } // end of collision loop
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processRec_SA_SWT, "process reconstructed info only with standalone", false);

  void processRec_TTCA_SWT(MyCollisionsWithSWT const& collisions, MyFwdTracks const& fwdtracks, aod::MFTTracks const&, aod::BCsWithTimestamps const&, aod::FwdTrackAssoc const& fwdtrackIndices)
  {
    std::unordered_map<int64_t, bool> mapAmb; // fwdtrack.globalIndex() -> bool isAmb;
    for (const auto& fwdtrack : fwdtracks) {
      const auto& fwdtrackIdsPerFwdTrack = fwdtrackIndices.sliceBy(fwdtrackIndicesPerFwdTrack, fwdtrack.globalIndex());
      mapAmb[fwdtrack.globalIndex()] = fwdtrackIdsPerFwdTrack.size() > 1;
      // LOGF(info, "fwdtrack.globalIndex() = %d, ntimes = %d, isAmbiguous = %d", fwdtrack.globalIndex(), fwdtrackIdsPerFwdTrack.size(), mapAmb[fwdtrack.globalIndex()]);
    } // end of fwdtrack loop

    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);
      if (!collision.isSelected()) {
        continue;
      }
      if (collision.swtaliastmp_raw() == 0) {
        continue;
      }

      const auto& fwdtrackIdsThisCollision = fwdtrackIndices.sliceBy(fwdtrackIndicesPerCollision, collision.globalIndex());
      for (const auto& fwdtrackId : fwdtrackIdsThisCollision) {
        const auto& fwdtrack = fwdtrackId.template fwdtrack_as<MyFwdTracks>();
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracks, aod::MFTTracks>(collision, fwdtrack, mapAmb[fwdtrack.globalIndex()]);
      } // end of fwdtrack loop
    } // end of collision loop
    mapAmb.clear();
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processRec_TTCA_SWT, "process reconstructed info", false);

  void processMC_SA(soa::Join<MyCollisions, aod::McCollisionLabels> const& collisions, MyFwdTracksMC const& fwdtracks, MFTTracksMC const&, aod::BCsWithTimestamps const&)
  {
    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);
      if (!collision.isSelected()) {
        continue;
      }
      if (!collision.has_mcCollision()) {
        continue;
      }

      const auto& fwdtracks_per_coll = fwdtracks.sliceBy(perCollision, collision.globalIndex());
      for (const auto& fwdtrack : fwdtracks_per_coll) {
        if (!fwdtrack.has_mcParticle()) {
          continue;
        }
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracksMC, MFTTracksMC>(collision, fwdtrack, false);
      } // end of fwdtrack loop
    } // end of collision loop
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processMC_SA, "process reconstructed and MC info", false);

  void processMC_TTCA(soa::Join<MyCollisions, aod::McCollisionLabels> const& collisions, MyFwdTracksMC const& fwdtracks, MFTTracksMC const&, aod::BCsWithTimestamps const&, aod::FwdTrackAssoc const& fwdtrackIndices)
  {
    std::unordered_map<int64_t, bool> mapAmb; // fwdtrack.globalIndex() -> bool isAmb;
    for (const auto& fwdtrack : fwdtracks) {
      const auto& fwdtrackIdsPerFwdTrack = fwdtrackIndices.sliceBy(fwdtrackIndicesPerFwdTrack, fwdtrack.globalIndex());
      mapAmb[fwdtrack.globalIndex()] = fwdtrackIdsPerFwdTrack.size() > 1;
      // LOGF(info, "fwdtrack.globalIndex() = %d, ntimes = %d, isAmbiguous = %d", fwdtrack.globalIndex(), fwdtrackIdsPerFwdTrack.size(), mapAmb[fwdtrack.globalIndex()]);
    } // end of fwdtrack loop

    for (const auto& collision : collisions) {
      const auto& bc = collision.template bc_as<aod::BCsWithTimestamps>();
      initCCDB(bc);
      if (!collision.isSelected()) {
        continue;
      }
      if (!collision.has_mcCollision()) {
        continue;
      }

      const auto& fwdtrackIdsThisCollision = fwdtrackIndices.sliceBy(fwdtrackIndicesPerCollision, collision.globalIndex());
      for (const auto& fwdtrackId : fwdtrackIdsThisCollision) {
        const auto& fwdtrack = fwdtrackId.template fwdtrack_as<MyFwdTracksMC>();
        if (!fwdtrack.has_mcParticle()) {
          continue;
        }
        if (fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack && fwdtrack.trackType() != o2::aod::fwdtrack::ForwardTrackTypeEnum::MuonStandaloneTrack) {
          continue;
        }
        fillFwdTrackTable<MyFwdTracksMC, MFTTracksMC>(collision, fwdtrack, mapAmb[fwdtrack.globalIndex()]);
      } // end of fwdtrack loop
    } // end of collision loop
    mapAmb.clear();
  }
  PROCESS_SWITCH(skimmerPrimaryMuon, processMC_TTCA, "process reconstructed and MC info", false);

  void processDummy(aod::Collisions const&) {}
  PROCESS_SWITCH(skimmerPrimaryMuon, processDummy, "process dummy", true);
};
struct associateAmbiguousMuon {
  Produces<aod::EMAmbiguousMuonSelfIds> em_amb_muon_ids;

  SliceCache cache;
  PresliceUnsorted<aod::EMPrimaryMuons> perTrack = o2::aod::emprimarymuon::fwdtrackId;
  std::vector<int> ambmuon_self_Ids;

  void process(aod::EMPrimaryMuons const& muons)
  {
    for (const auto& muon : muons) {
      auto muons_with_same_trackId = muons.sliceBy(perTrack, muon.fwdtrackId());
      ambmuon_self_Ids.reserve(muons_with_same_trackId.size());
      for (const auto& amb_muon : muons_with_same_trackId) {
        if (amb_muon.globalIndex() == muon.globalIndex()) { // don't store myself.
          continue;
        }
        ambmuon_self_Ids.emplace_back(amb_muon.globalIndex());
      }
      em_amb_muon_ids(ambmuon_self_Ids);
      ambmuon_self_Ids.clear();
      ambmuon_self_Ids.shrink_to_fit();
    }
  }
};
struct associateSameMFT {
  Produces<aod::EMGlobalMuonSelfIds> em_same_mft_ids;

  SliceCache cache;
  PresliceUnsorted<aod::EMPrimaryMuons> perMFTTrack = o2::aod::emprimarymuon::mfttrackId;
  std::vector<int> self_Ids;

  void process(aod::EMPrimaryMuons const& muons)
  {
    for (const auto& muon : muons) {
      if (muon.trackType() == o2::aod::fwdtrack::ForwardTrackTypeEnum::GlobalMuonTrack) {
        auto muons_with_same_mfttrackId = muons.sliceBy(perMFTTrack, muon.mfttrackId());
        self_Ids.reserve(muons_with_same_mfttrackId.size());
        for (const auto& global_muon : muons_with_same_mfttrackId) {
          if (global_muon.globalIndex() == muon.globalIndex()) { // don't store myself.
            continue;
          }
          if (global_muon.collisionId() == muon.collisionId()) {
            self_Ids.emplace_back(global_muon.globalIndex());
          }
        }
        em_same_mft_ids(self_Ids);
        self_Ids.clear();
        self_Ids.shrink_to_fit();
      } else {
        em_same_mft_ids(std::vector<int>{}); // empty for standalone muons
      }
    } // end of muon loop
  }
};
WorkflowSpec defineDataProcessing(ConfigContext const& cfgc)
{
  return WorkflowSpec{
    adaptAnalysisTask<skimmerPrimaryMuon>(cfgc, TaskName{"skimmer-primary-muon"}),
    adaptAnalysisTask<associateAmbiguousMuon>(cfgc, TaskName{"associate-ambiguous-muon"}),
    adaptAnalysisTask<associateSameMFT>(cfgc, TaskName{"associate-same-mft"})};
}
