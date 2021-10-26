//
// Created by Nadrino on 22/07/2021.
//

#include <TROOT.h>
#include <Likelihoods.hh>
#include "json.hpp"

#include "Logger.h"
#include "GenericToolbox.h"
#include "GenericToolbox.Root.h"

#include "JsonUtils.h"
#include "GlobalVariables.h"
#include "FitSampleSet.h"


LoggerInit([](){
  Logger::setUserHeaderStr("[FitSampleSet]");
})

FitSampleSet::FitSampleSet() { this->reset(); }
FitSampleSet::~FitSampleSet() { this->reset(); }

void FitSampleSet::reset() {
  _isInitialized_ = false;
  _config_.clear();

  _likelihoodFunctionPtr_ = nullptr;
  _fitSampleList_.clear();
  _dataSetList_.clear();
  _dataEventType_ = DataEventType::Unset;

  _eventByEventDialLeafList_.clear();
}

void FitSampleSet::setConfig(const nlohmann::json &config) {
  _config_ = config;
  while( _config_.is_string() ){
    LogWarning << "Forwarding " << __CLASS_NAME__ << " config: \"" << _config_.get<std::string>() << "\"" << std::endl;
    _config_ = JsonUtils::readConfigFile(_config_.get<std::string>());
  }
}

void FitSampleSet::addEventByEventDialLeafName(const std::string& leafName_){
  if( not GenericToolbox::doesElementIsInVector(leafName_, _eventByEventDialLeafList_) ){
    _eventByEventDialLeafList_.emplace_back(leafName_);
  }
}

void FitSampleSet::initialize() {
  LogWarning << __METHOD_NAME__ << std::endl;

  LogAssert(not _config_.empty(), "_config_ is not set." << std::endl);

  _dataEventType_ = DataEventTypeEnumNamespace::toEnum(
    JsonUtils::fetchValue<std::string>(_config_, "dataEventType"), true
    );

  LogInfo << "Initializing input datasets..." << std::endl;
  auto dataSetListConfig = JsonUtils::fetchValue(_config_, "dataSetList", nlohmann::json());
  LogAssert(not dataSetListConfig.empty(), "No dataSet specified." << std::endl);
  for( const auto& dataSetConfig : dataSetListConfig ){
    _dataSetList_.emplace_back();
    _dataSetList_.back().setConfig(dataSetConfig);
    _dataSetList_.back().initialize();
  }

  LogInfo << "Reading samples definition..." << std::endl;
  auto fitSampleListConfig = JsonUtils::fetchValue(_config_, "fitSampleList", nlohmann::json());
  for( const auto& fitSampleConfig: fitSampleListConfig ){
    _fitSampleList_.emplace_back();
    _fitSampleList_.back().setConfig(fitSampleConfig);
    _fitSampleList_.back().initialize();
  }

  LogInfo << "Creating parallelisable jobs" << std::endl;

  // Fill the bin index inside of each event
  std::function<void(int)> updateSampleEventBinIndexesFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      sample.getMcContainer().updateEventBinIndexes(iThread);
      sample.getDataContainer().updateEventBinIndexes(iThread);
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleEventBinIndexes", updateSampleEventBinIndexesFct);

  // Fill bin event caches
  std::function<void(int)> updateSampleBinEventListFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      sample.getMcContainer().updateBinEventList(iThread);
      sample.getDataContainer().updateBinEventList(iThread);
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleBinEventList", updateSampleBinEventListFct);


  // Histogram fills
  std::function<void(int)> refillMcHistogramsFct = [this](int iThread){
    for( auto& sample : _fitSampleList_ ){
      sample.getMcContainer().refillHistogram(iThread);
      sample.getDataContainer().refillHistogram(iThread);
    }
  };
  std::function<void()> rescaleMcHistogramsFct = [this](){
    for( auto& sample : _fitSampleList_ ){
      sample.getMcContainer().rescaleHistogram();
      sample.getDataContainer().rescaleHistogram();
    }
  };
  GlobalVariables::getParallelWorker().addJob("FitSampleSet::updateSampleHistograms", refillMcHistogramsFct);
  GlobalVariables::getParallelWorker().setPostParallelJob("FitSampleSet::updateSampleHistograms", rescaleMcHistogramsFct);

  _likelihoodFunctionPtr_ = std::shared_ptr<PoissonLLH>(new PoissonLLH);

  _isInitialized_ = true;
}

DataEventType FitSampleSet::getDataEventType() const {
  return _dataEventType_;
}
const std::vector<FitSample> &FitSampleSet::getFitSampleList() const {
  return _fitSampleList_;
}
std::vector<FitSample> &FitSampleSet::getFitSampleList() {
  return _fitSampleList_;
}
std::vector<DataSet> &FitSampleSet::getDataSetList() {
  return _dataSetList_;
}

bool FitSampleSet::empty() const {
  return _fitSampleList_.empty();
}
double FitSampleSet::evalLikelihood() const{

  double llh = 0.;
  for( auto& sample : _fitSampleList_ ){
    double sampleLlh = 0;
    for( int iBin = 1 ; iBin <= sample.getMcContainer().histogram->GetNbinsX() ; iBin++ ){
      sampleLlh += (*_likelihoodFunctionPtr_)(
        sample.getMcContainer().histogram->GetBinContent(iBin),
        sample.getMcContainer().histogram->GetBinError(iBin),
        sample.getDataContainer().histogram->GetBinContent(iBin));
    }
    llh += sampleLlh;
  }

  return llh;
}

void FitSampleSet::loadPhysicsEvents() {
  LogWarning << __METHOD_NAME__ << std::endl;

  LogThrowIf(_dataEventType_ == DataEventType::Unset, "dataEventType not set.");
  LogInfo << "Data events type is set to: " << DataEventTypeEnumNamespace::toString(_dataEventType_) << std::endl;

  int dataSetIndex = -1;
  for( auto& dataSet : _dataSetList_ ){
    dataSetIndex++;

    LogInfo << "Reading dataSet: " << dataSet.getName() << std::endl;

    std::vector<FitSample*> samplesToFillList;
    std::vector<TTreeFormula*> sampleCutFormulaList;
    std::vector<std::string> samplesNames;

    if( not dataSet.isEnabled() ){
      continue;
    }

    auto* chainBuf = dataSet.buildMcChain();
    LogThrowIf(chainBuf == nullptr, "No MC files are available for dataset: " << dataSet.getName());
    delete chainBuf;

    chainBuf = dataSet.buildDataChain();
    LogThrowIf(chainBuf == nullptr and _dataEventType_ == DataEventType::DataFiles,
               "Can't define sample \"" << dataSet.getName() << "\" while in non-Asimov-like fit and no Data files are available" );

    delete chainBuf;

    for( auto& sample : _fitSampleList_ ){
      if( not sample.isEnabled() ) continue;
      if( sample.isDataSetValid(dataSet.getName()) ){
        samplesToFillList.emplace_back(&sample);
        samplesNames.emplace_back(sample.getName());
      }
    }
    if( samplesToFillList.empty() ){
      LogAlert << "No sample is set to use dataset \"" << dataSet.getName() << "\"" << std::endl;
    }
    LogInfo << "Dataset \"" << dataSet.getName() << "\" will populate samples: " << GenericToolbox::parseVectorAsString(samplesNames) << std::endl;

    LogInfo << "Fetching mandatory leaves..." << std::endl;
    for( size_t iSample = 0 ; iSample < samplesToFillList.size() ; iSample++ ){
      // Fit phase space
      for( const auto& bin : samplesToFillList[iSample]->getBinning().getBinsList() ){
        for( const auto& var : bin.getVariableNameList() ){
          dataSet.addRequestedMandatoryLeafName(var);
        }
      }
//        // Cuts // ACTUALLY NOT NECESSARY SINCE THE SELECTION IS DONE INDEPENDENTLY
//        for( int iPar = 0 ; iPar < sampleCutFormulaList.at(iSample)->GetNpar() ; iPar++ ){
//          dataSet.addRequestedMandatoryLeafName(sampleCutFormulaList.at(iSample)->GetParName(iPar));
//        }
    }

    LogInfo << "List of requested leaves: " << GenericToolbox::parseVectorAsString(dataSet.getRequestedLeafNameList()) << std::endl;
    LogInfo << "List of mandatory leaves: " << GenericToolbox::parseVectorAsString(dataSet.getRequestedMandatoryLeafNameList()) << std::endl;

    for( bool isData : {false, true} ){

      TChain* chainPtr{nullptr};
      std::vector<std::string>* activeLeafNameListPtr;

      if( isData and _dataEventType_ == DataEventType::Asimov ){ continue; }
      LogDebug << GET_VAR_NAME_VALUE(isData) << std::endl;

      if( isData ){
        LogInfo << "Reading data files..." << std::endl;
        chainPtr = dataSet.buildDataChain();
        activeLeafNameListPtr = &dataSet.getDataActiveLeafNameList();
      }
      else{
        LogInfo << "Reading MC files..." << std::endl;
        chainPtr = dataSet.buildMcChain();
        activeLeafNameListPtr = &dataSet.getMcActiveLeafNameList();
      }

      if( chainPtr == nullptr or chainPtr->GetEntries() == 0 ){
        continue;
      }

      LogInfo << "Checking the availability of requested leaves..." << std::endl;
      for( auto& requestedLeaf : dataSet.getRequestedLeafNameList() ){
        if( not isData or GenericToolbox::doesElementIsInVector(requestedLeaf, dataSet.getRequestedMandatoryLeafNameList()) ){
          LogThrowIf(chainPtr->GetLeaf(requestedLeaf.c_str()) == nullptr,
                     "Could not find leaf \"" << requestedLeaf << "\" in TChain");
        }

        if( chainPtr->GetLeaf(requestedLeaf.c_str()) != nullptr ){
          activeLeafNameListPtr->emplace_back(requestedLeaf);
        }
      }
      LogInfo << "List of leaves which will be loaded in RAM: "
      << GenericToolbox::parseVectorAsString(*activeLeafNameListPtr) << std::endl;

      LogInfo << "Performing event selection of samples with " << (isData? "data": "mc") << " files..." << std::endl;
      chainPtr->SetBranchStatus("*", true);
      for( auto& sample : samplesToFillList ){
        sampleCutFormulaList.emplace_back(
          new TTreeFormula(
            sample->getName().c_str(),
            sample->getSelectionCutsStr().c_str(),
            chainPtr
          )
        );
        LogThrowIf(sampleCutFormulaList.back()->GetNdim() == 0,
                   "\"" << sample->getSelectionCutsStr() << "\" could not be parsed by the TChain");

        // The TChain will notify the formula that it has to update leaves addresses while swaping TFile
        // Although will have to notify manually since multiple formula are used
        chainPtr->SetNotify(sampleCutFormulaList.back());
      }

      LogDebug << "Enabling only needed branches for sample selection..." << std::endl;
      chainPtr->SetBranchStatus("*", false);
      for( auto* sampleFormula : sampleCutFormulaList ){
        for( int iLeaf = 0 ; iLeaf < sampleFormula->GetNcodes() ; iLeaf++ ){
          chainPtr->SetBranchStatus(sampleFormula->GetLeaf(iLeaf)->GetName(), true);
        }
      }

      Long64_t nEvents = chainPtr->GetEntries();
      // for each event, which sample is active?
      std::vector<std::vector<bool>> eventIsInSamplesList(nEvents, std::vector<bool>(samplesToFillList.size(), true));
      std::vector<size_t> sampleNbOfEvents(samplesToFillList.size(), 0);
      std::string progressTitle = LogWarning.getPrefixString() + "Performing event selection";
      TFile* lastFilePtr{nullptr};
      for( Long64_t iEvent = 0 ; iEvent < nEvents ; iEvent++ ){
        GenericToolbox::displayProgressBar(iEvent, nEvents, progressTitle);
        chainPtr->GetEntry(iEvent);

//        if( lastFilePtr != chainPtr->GetCurrentFile() ){
//          lastFilePtr = chainPtr->GetCurrentFile();
          // update leaves (if a new file has been reached, not doing that makes it crash)
          for( auto& sampleCutFormula : sampleCutFormulaList ){
            LogTrace << GET_VAR_NAME_VALUE(lastFilePtr) << std::endl;
            chainPtr->SetNotify(sampleCutFormula);
            LogTrace << "chain notify" << std::endl;
            sampleCutFormula->Notify();
          }
//        }

        for( size_t iSample = 0 ; iSample < sampleCutFormulaList.size() ; iSample++ ){
//          sampleCutFormulaList.at(iSample)->Notify(); // update leaves (if a new file has been reached, not doing that makes it crash)
          for(int jInstance = 0; jInstance < sampleCutFormulaList.at(iSample)->GetNdata(); jInstance++) {
            if (sampleCutFormulaList.at(iSample)->EvalInstance(jInstance) == 0) {
              // if it doesn't passes the cut
              eventIsInSamplesList.at(iEvent).at(iSample) = false;
              break;
            }
          } // Formula Instances
          if( eventIsInSamplesList.at(iEvent).at(iSample) ){ sampleNbOfEvents.at(iSample)++; }
        } // iSample
      } // iEvent


      // The following lines are necessary since the events might get resized while being in multithread
      // Because std::vector is insuring continuous memory allocation, a resize sometimes
      // lead to the full moving of a vector memory. This is not thread safe, so better ensure
      // the vector won't have to do this by allocating the right event size.
      PhysicsEvent eventBuf;
      eventBuf.setLeafNameListPtr(activeLeafNameListPtr);
      eventBuf.setDataSetIndex(dataSetIndex);
      chainPtr->SetBranchStatus("*", true);
      eventBuf.hookToTree(chainPtr, not isData);
      chainPtr->GetEntry(0); // memory is claimed -> eventBuf has the right size
      // Now the eventBuffer has the right size in memory
      delete chainPtr; // not used anymore

      LogInfo << "Claiming memory for additional events in samples: "
              << GenericToolbox::parseVectorAsString(sampleNbOfEvents) << std::endl;
      std::vector<size_t> sampleIndexOffsetList(samplesToFillList.size(), 0);
      std::vector< std::vector<PhysicsEvent>* > sampleEventListPtrToFill(samplesToFillList.size(), nullptr);

      for( size_t iSample = 0 ; iSample < sampleNbOfEvents.size() ; iSample++ ){
        LogDebug << "Claiming memory for sample #" << iSample << std::endl;
        if( isData ){
          sampleEventListPtrToFill.at(iSample) = &samplesToFillList.at(iSample)->getDataContainer().eventList;
          sampleIndexOffsetList.at(iSample) = sampleEventListPtrToFill.at(iSample)->size();
          samplesToFillList.at(iSample)->getDataContainer().reserveEventMemory(dataSetIndex, sampleNbOfEvents.at(iSample),
                                                                               PhysicsEvent());
        }
        else{
          sampleEventListPtrToFill.at(iSample) = &samplesToFillList.at(iSample)->getMcContainer().eventList;
          sampleIndexOffsetList.at(iSample) = sampleEventListPtrToFill.at(iSample)->size();
          samplesToFillList.at(iSample)->getMcContainer().reserveEventMemory(dataSetIndex, sampleNbOfEvents.at(iSample),
                                                                               PhysicsEvent());
        }
      }

      // Fill function
      ROOT::EnableImplicitMT();
      std::mutex eventOffSetMutex;
      auto fillFunction = [&](int iThread_){

        TChain* threadChain;
        TTreeFormula* threadNominalWeightFormula{nullptr};

        threadChain = isData ? dataSet.buildDataChain() : dataSet.buildMcChain();
        threadChain->SetBranchStatus("*", true);

        if( not isData and not dataSet.getMcNominalWeightFormulaStr().empty() ){
          threadNominalWeightFormula = new TTreeFormula(
              Form("NominalWeightFormula%i", iThread_),
              dataSet.getMcNominalWeightFormulaStr().c_str(),
              threadChain
              );
          threadChain->SetNotify(threadNominalWeightFormula);
        }


        Long64_t nEvents = threadChain->GetEntries();
        PhysicsEvent eventBufThread(eventBuf);
        eventBufThread.hookToTree(threadChain, not isData);
        GenericToolbox::disableUnhookedBranches(threadChain);
        if( threadNominalWeightFormula != nullptr ){
          for( int iLeaf = 0 ; iLeaf < threadNominalWeightFormula->GetNcodes() ; iLeaf++ ){
            threadChain->SetBranchStatus(threadNominalWeightFormula->GetLeaf(iLeaf)->GetName(), true);
          }
        }


//        auto threadSampleIndexOffsetList = sampleIndexOffsetList;
        size_t sampleEventIndex;
        const std::vector<DataBin>* binsListPtr;

        // Loop vars
        int iBin{0};
        size_t iVar{0};
        size_t iSample{0};

        std::string progressTitle = LogInfo.getPrefixString() + "Reading selected events";
        for( Long64_t iEvent = 0 ; iEvent < nEvents ; iEvent++ ){
          if( iEvent % GlobalVariables::getNbThreads() != iThread_ ){ continue; }
          if( iThread_ == 0 ) GenericToolbox::displayProgressBar(iEvent, nEvents, progressTitle);

          bool skipEvent = true;
          for( bool isInSample : eventIsInSamplesList.at(iEvent) ){
            if( isInSample ){
              skipEvent = false;
              break;
            }
          }
          if( skipEvent ) continue;

          threadChain->GetEntry(iEvent);
          eventBufThread.setEntryIndex(iEvent);

          if( threadNominalWeightFormula != nullptr ){
            eventBufThread.setTreeWeight(threadNominalWeightFormula->EvalInstance());
            if( eventBufThread.getTreeWeight() == 0 ) continue;
            eventBufThread.setNominalWeight(eventBufThread.getTreeWeight());
            eventBufThread.resetEventWeight();
          }

          for( iSample = 0 ; iSample < samplesToFillList.size() ; iSample++ ){
            if( eventIsInSamplesList.at(iEvent).at(iSample) ){

              // Reset bin index of the buffer
              eventBufThread.setSampleBinIndex(-1);

              // Has valid bin?
              binsListPtr = &samplesToFillList.at(iSample)->getBinning().getBinsList();

              for( iBin = 0 ; iBin < binsListPtr->size() ; iBin++ ){
                auto& bin = binsListPtr->at(iBin);
                bool isInBin = true;
                for( iVar = 0 ; iVar < bin.getVariableNameList().size() ; iVar++ ){
                  if( not bin.isBetweenEdges(iVar, eventBufThread.getVarAsDouble(bin.getVariableNameList().at(iVar))) ){
                    isInBin = false;
                    break;
                  }
                } // Var
                if( isInBin ){
                  eventBufThread.setSampleBinIndex(int(iBin));
                  break;
                }
              } // Bin

              if( eventBufThread.getSampleBinIndex() == -1 ) {
                // Invalid bin
                break;
              }

//              sampleEventIndex = sampleIndexOffsetList.at(iSample);
//              sampleEventListPtrToFill.at(iSample)->at(sampleEventIndex) = eventBufThread; // copy

              eventOffSetMutex.lock();
              sampleEventIndex = sampleIndexOffsetList.at(iSample)++;
              sampleEventListPtrToFill.at(iSample)->at(sampleEventIndex) = PhysicsEvent(eventBufThread); // copy
              sampleEventListPtrToFill.at(iSample)->at(sampleEventIndex).clonePointerLeaves(); // make sure the pointer leaves aren't pointing toward the TTree basket
              eventOffSetMutex.unlock();
            }
          }
        }
        if( iThread_ == 0 ) GenericToolbox::displayProgressBar(nEvents, nEvents, progressTitle);
        delete threadChain;
        delete threadNominalWeightFormula;
      };

      LogInfo << "Copying selected events to RAM..." << std::endl;
      GlobalVariables::getParallelWorker().addJob(__METHOD_NAME__, fillFunction);
      GlobalVariables::getParallelWorker().runJob(__METHOD_NAME__);
      GlobalVariables::getParallelWorker().removeJob(__METHOD_NAME__);

      LogInfo << "Shrinking event lists..." << std::endl;
      for( size_t iSample = 0 ; iSample < samplesToFillList.size() ; iSample++ ){
        samplesToFillList.at(iSample)->getMcContainer().shrinkEventList(sampleIndexOffsetList.at(iSample));
      }

      LogInfo << "Events have been loaded for " << ( isData ? "data": "mc" )
      << " with dataset: " << dataSet.getName() << std::endl;

    } // isData
  } // data Set

  for( auto& sample : _fitSampleList_ ){
    LogInfo << "Total events loaded from file in \"" << sample.getName() << "\":" << std::endl
    << "-> mc: " << sample.getMcContainer().eventList.size() << " / data: " << sample.getDataContainer().eventList.size() << std::endl;
  }

}
void FitSampleSet::loadAsimovData(){
  if( _dataEventType_ == DataEventType::Asimov ){
    LogWarning << "Asimov data selected: copying MC events..." << std::endl;
    for( auto& sample : _fitSampleList_ ){
      LogInfo << "Copying MC events in sample \"" << sample.getName() << "\"" << std::endl;
      auto& dataEventList = sample.getDataContainer().eventList;
      LogThrowIf(not dataEventList.empty(), "Can't fill Asimov data, dataEventList is not empty.");

      auto& mcEventList = sample.getMcContainer().eventList;
      dataEventList.resize(mcEventList.size());
      for( size_t iEvent = 0 ; iEvent < dataEventList.size() ; iEvent++ ){
        dataEventList[iEvent] = mcEventList[iEvent];
      }
    }
  }
}

void FitSampleSet::updateSampleEventBinIndexes() const{
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleEventBinIndexes");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}
void FitSampleSet::updateSampleBinEventList() const{
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleBinEventList");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}
void FitSampleSet::updateSampleHistograms() const {
  if( _showTimeStats_ ) GenericToolbox::getElapsedTimeSinceLastCallInMicroSeconds(__METHOD_NAME__);
  GlobalVariables::getParallelWorker().runJob("FitSampleSet::updateSampleHistograms");
  if( _showTimeStats_ ) LogDebug << __METHOD_NAME__ << " took: " << GenericToolbox::getElapsedTimeSinceLastCallStr(__METHOD_NAME__) << std::endl;
}


