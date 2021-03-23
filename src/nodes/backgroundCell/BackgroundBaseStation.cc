//
//                           Simu5G
//
// This file is part of a software released under the license included in file
// "license.pdf". This license can be also found at http://www.ltesimulator.com/
// The above file and the present reference are part of the software itself,
// and cannot be removed from it.
//

#include "nodes/backgroundCell/BackgroundBaseStation.h"

Define_Module(BackgroundBaseStation);

void BackgroundBaseStation::initialize(int stage)
{
    cSimpleModule::initialize(stage);
    if (stage == inet::INITSTAGE_LOCAL)
    {
        // set TX power and angle

        txPower_ = par("txPower");

        std::string txDir = par("txDirection");
        if (txDir.compare(txDirections[OMNI].txDirectionName)==0)
        {
            txDirection_ = OMNI;
            txAngle_ = 0;
        }
        else   // ANISOTROPIC
        {
            txDirection_ = ANISOTROPIC;
            txAngle_ = par("txAngle");
        }

        isNr_ = par("isNr");
        numerologyIndex_ = par("numerologyIndex");
        carrierFrequency_ = par("carrierFrequency").doubleValue();
        numBands_ = par("numBands");

        // initialize band status structures
        bandStatus_[UL].resize(numBands_, 0);
        prevBandStatus_[UL].resize(numBands_, 0);
        bandStatus_[DL].resize(numBands_, 0);
        prevBandStatus_[DL].resize(numBands_, 0);
        ulPrevBandAllocation_.resize(numBands_, 0);
        ulBandAllocation_.resize(numBands_, 0);

        // TODO: if BackgroundBaseStation-interference is disabled, do not send selfMessages
        /* Start TTI tick */
        ttiTick_ = new omnetpp::cMessage("ttiTick_");
        ttiTick_->setSchedulingPriority(1);        // TTI TICK after other messages
        ttiPeriod_ = binder_->getSlotDurationFromNumerologyIndex(numerologyIndex_);
        scheduleAt(NOW + ttiPeriod_, ttiTick_);

        // register to get a notification when position changes
        getParentModule()->subscribe(inet::IMobility::mobilityStateChangedSignal, this);
    }
    if (stage == inet::INITSTAGE_LOCAL+1)
    {
         binder_ = getBinder();

         // add this cell to the binder
         id_ = binder_->addBackgroundBaseStation(this, carrierFrequency_);

         bgTrafficManager_ = check_and_cast<BackgroundTrafficManager*>(getParentModule()->getSubmodule("bgTrafficGenerator")->getSubmodule("manager"));
         bgTrafficManager_->setCarrierFrequency(carrierFrequency_);

         bgChannelModel_ = check_and_cast<BackgroundCellChannelModel*>(getParentModule()->getSubmodule("bgChannelModel"));
         bgChannelModel_->setCarrierFrequency(carrierFrequency_);
    }
}

void BackgroundBaseStation::handleMessage(omnetpp::cMessage *msg)
{
    if (msg->isSelfMessage())
    {
        updateAllocation(UL);
        updateAllocation(DL);

        scheduleAt(NOW + ttiPeriod_, msg);
        return;
    }
}

void BackgroundBaseStation::receiveSignal(cComponent *source, simsignal_t signalID, cObject *obj, cObject *)
{
    if (signalID == inet::IMobility::mobilityStateChangedSignal)
    {
        inet::IMobility *mobility = check_and_cast<inet::IMobility*>(obj);
        pos_ = mobility->getCurrentPosition();
    }
}

void BackgroundBaseStation::updateAllocation(Direction dir)
{
    EV << "----- BACKGROUND CELL ALLOCATION UPDATE -----" << endl;

    resetAllocation(dir);

    unsigned int b = 0;
    int bgUeIndex;
    int bytesPerBlock;
    MacNodeId bgUeId;

    if (dir == UL)
    {
        std::list<MacNodeId> servedRac;

        // handle RAC
        std::list<int>::const_iterator rit = bgTrafficManager_->getWaitingForRacUesBegin();
        std::list<int>::const_iterator ret = bgTrafficManager_->getWaitingForRacUesEnd();

        for (; rit != ret; ++rit)
        {
            bgUeIndex = *rit;
            bgUeId = BGUE_MIN_ID + bgUeIndex;

            EV << NOW << " BackgroundBaseStation::updateAllocation - dir[" << dirToA(dir) << "] band[" << b << "] - allocated to ue[" << bgUeId << "]" << endl;

            // allocate one block
            bandStatus_[dir][b] = 1;
            ulBandAllocation_[b] = bgUeId;
            b++;

            servedRac.push_back(bgUeId);

            if (b >= numBands_)
            {
                EV << "BackgroundBaseStation::updateAllocation - space ended" << endl;
                EV << "----- END BACKGROUND CELL ALLOCATION UPDATE -----" << endl;
                return;
            }
        }

        while (!servedRac.empty())
        {
            // notify the traffic manager that the RAC for this UE has been served
            bgTrafficManager_->racHandled(servedRac.front());
            servedRac.pop_front();
        }
    }


    // sort backlogged UEs (MaxC/I)
    ScoreList score;
    MacCid bgCid;
    std::list<int>::const_iterator it = bgTrafficManager_->getBackloggedUesBegin(dir);
    std::list<int>::const_iterator et = bgTrafficManager_->getBackloggedUesEnd(dir);
    for (; it != et; ++it)
    {
        bgUeIndex = *it;
        bgUeId = BGUE_MIN_ID + bgUeIndex;

        // the cid for a background UE is a 32bit integer composed as:
        // - the most significant 16 bits are set to the background UE id (BGUE_MIN_ID+index)
        // - the least significant 16 bits are set to 0 (lcid=0)
        bgCid = bgUeId << 16;

        bytesPerBlock = bgTrafficManager_->getBackloggedUeBytesPerBlock(bgUeId, dir);

        ScoreDesc bgDesc(bgCid, bytesPerBlock);
        score.push(bgDesc);
    }

    // Schedule the connections in score order.
    while (!score.empty())
    {
        // Pop the top connection from the list.
        ScoreDesc current = score.top();
        bgUeId = current.x_ >> 16;
        bytesPerBlock = current.score_;

        unsigned int buffer = bgTrafficManager_->getBackloggedUeBuffer(bgUeId, dir);
        unsigned int blocks = ceil((double)buffer/bytesPerBlock);
        unsigned int allocatedBlocks = 0;
        while (b < numBands_ && blocks > 0)
        {
            bandStatus_[dir][b] = 1;
            if (dir == UL)
                ulBandAllocation_[b] = bgUeId;

            EV << NOW << " BackgroundBaseStation::updateAllocation - dir[" << dirToA(dir) << "] band[" << b << "] - allocated to ue[" << bgUeId << "]" << endl;

            blocks--;
            b++;
            allocatedBlocks++;
        }

        unsigned int allocatedBytes = allocatedBlocks * bytesPerBlock;
        bgTrafficManager_->consumeBackloggedUeBytes(bgUeId, allocatedBytes, dir);

        // remove UE from the list
        score.pop();

        if (b >= numBands_)
        {
            // space terminated
            EV << "BackgroundBaseStation::updateAllocation - space ended" << endl;
            break;
        }
    }

    EV << "----- END BACKGROUND CELL ALLOCATION UPDATE -----" << endl;
}

void BackgroundBaseStation::resetAllocation(Direction dir)
{
    for (unsigned int i=0; i < numBands_; i++)
    {
        prevBandStatus_[dir][i] = bandStatus_[dir][i];
        bandStatus_[dir][i] = 0;
        if (dir == UL)
        {
            ulPrevBandAllocation_[i] = ulBandAllocation_[i];
            ulBandAllocation_[i] = 0;
        }
    }

}

TrafficGeneratorBase* BackgroundBaseStation::getBandInterferingUe(int band)
{
    MacNodeId bgUeId = ulBandAllocation_[band];
    return bgTrafficManager_->getTrafficGenerator(bgUeId);
}

TrafficGeneratorBase* BackgroundBaseStation::getPrevBandInterferingUe(int band)
{
    MacNodeId bgUeId = ulPrevBandAllocation_[band];
    return bgTrafficManager_->getTrafficGenerator(bgUeId);
}
